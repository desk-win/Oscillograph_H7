/* ===========================================================================
 * CM7 侧 SD 卡存储 - 跳变记录表存成 CSV
 *
 * 四件事 (对应 sd.h 里那四个入口):
 *   初始化  SD_Init()        f_mount: 认卡 + 挂卷
 *   创建    SD_Create()      f_open(FA_CREATE_ALWAYS | FA_WRITE) + 写表头
 *   写入    SD_WriteRecs()   跳变表 -> 行 -> 攒够一批 f_write -> f_sync
 *   删除    SD_Delete()      f_unlink
 *
 * 数据流 (整条链路上谁在哪儿, 见 sd.h 的文件头):
 *   logic_trace.c 的 LOG_REC_BUF (ts + 8 路完整电平)
 *     -> LOG_Trace_BorrowRecs() 借出整表 (借用期间采集不会重开)
 *     -> 这里转成 CSV 行写卡
 *     -> LOG_Trace_ReleaseRecs() 还回去
 *   这一层只吃 log_rec_t 数组, 不碰采集的静态变量, 所以调用方自己决定什么时候存
 *   (界面上的"存卡"按钮 -> 任务里借表 -> 存 -> 还表)。
 *
 * 为什么要自己攒一个行缓冲, 而不是一行一次 f_write:
 *   一次采集最多 512 KB 记录 (= LOG_REC_MAX 65536 条), 转成 CSV 最坏五十多万行。
 *   一行一次 f_write 就是五十多万次调用, 每次都过一遍 FatFs 的扇区缓冲 (512 B),
 *   慢得没意义。s_buf 攒到 SD_WRITE_CHUNK 才交一次, 调用次数降三个数量级。
 *   写入不是实时路径 (写卡能到 MB/s 级, 跳变表撑死几 MB), 但慢到几十秒也很难受。
 *
 * 时间戳: 默认写采样序号 (log_rec_t.ts), 见 sd.h 的 SD_TS_AS_NANOSECONDS。
 *   序号除以 fs 就是秒。不默认换成 ns 是因为原始序号无损、不用除法, 而看的人拿
 *   实测采样率一乘就回去了。
 *
 * 丢帧 (LOG_REC_GAP): 表里某条带这个标记, 说明它之前有半个帧没被扫到, 那半帧里
 *   可能藏着跳变。CSV 上的表现是"某一路在两条记录之间就变了, 但没有对应的那一行"
 *   (电平本身是对的: 记录存的是变化之后的完整电平)。这儿不给它加第三列去标记
 *   (格式就是一个通道两列), 只把条数记在 sd_diag_gaps 里。
 *
 * 为什么直接用 CubeMX 生成的 SDFatFS / SDFile / SDPath (fatfs.c) 而不是自己再开一套:
 *   一个卷只挂一次。fatfs.c 里那两个对象就是给这一卷准备的, 而且 MX_FATFS_Init()
 *   已经把 SD_Driver 链到 SDPath 上了 (那一步是纯内存操作, 在调度器起来之前做没事)。
 *
 * 两条使用上的硬约束:
 *   1. **只能在任务里调** (osKernelStart() 之后)。sd_diskio.c 的 SD_initialize()
 *      先看 osKernelGetState() == osKernelRunning, 没起调度器直接 STA_NOINIT;
 *      _FS_REENTRANT = 1 下 f_mount 还要建 CMSIS-RTOS2 信号量。
 *   2. **不要从中断里调**。FatFs 的重入保护是 RTOS 互斥量, 中断里拿不到。
 *   下面那组静态量 (s_buf / s_buf_len / s_file_open / s_mounted) 也按"同一时刻只有
 *   一个任务在存卡"来用 —— 存卡是"点一下存一帧"的动作, 不是并发路径。
 *
 * 调用点是 CM7/Mylibrary/Src/logic_save.c 里的存卡任务: 借出跳变表 → 建文件 →
 * 写 → 收尾。界面上的 Save 只发一个请求进来 (见 logic_save.h)。
 * ========================================================================= */

#include "sd.h"
#include "fatfs.h"          /* SDFatFS / SDFile / SDPath (CubeMX 生成), 顺带 ff.h */
#include "sdmmc.h"          /* hsd1: 轮询模式那条路要用 */
#include <stdint.h>
#include <stdio.h>          /* snprintf: 只在 SD_MakeName 里给文件名补零 */
#include <string.h>         /* memcpy */

/* ===========================================================================
 * 磁盘 I/O: 把 sd_diskio.c 那条 DMA 路换成轮询 (见 sd.h 的 SD_DISKIO_POLLING)
 *
 * sd_diskio.c 是 ST 的 "dma_rtos" 模板: 它调 BSP_SD_ReadBlocks_DMA /
 * WriteBlocks_DMA, 然后等 SDMMC 中断里投出来的一条队列消息。这两个 BSP 接口在
 * bsp_driver_sd.c 里是 __weak 的 (头文件里也写了 "can be modified in case the
 * current settings need to be changed"), 所以这里给一份强定义就把整条路换掉了。
 *
 *   数据搬运: ST 的 DMA 版让 SDMMC 的 IDMA 直接写调用方的缓冲。FatFs 的工作缓冲
 *            (SDFatFS.win / SDFile.buf) 在 .bss = DTCM (0x20000000), 而 SDMMC 的
 *            IDMA 够不到 DTCM, 于是传输报错、完成消息永远不来、sd_diskio 干等到
 *            SD_TIMEOUT (30 秒)、FatFs 最后报 FR_DISK_ERR = 1。
 *            下面这两份走 HAL 的轮询接口 (HAL_SD_ReadBlocks / WriteBlocks): CPU 从
 *            SDMMC 的 FIFO 里搬数据, 全程不使能 IDMA, 所以 DTCM 缓冲没问题。
 *   完成消息: 轮询接口是阻塞的, 返回时传输已经做完, 所以在返回前直接调用
 *            sd_diskio.c 暴露出来的 BSP_SD_ReadCpltCallback() / WriteCpltCallback()
 *            把那条消息补上, 对上层 (SD_read / SD_write) 完全等价。
 *
 * 为什么不用 HAL 的 interrupt 模式: 那条路要 CPU 在 FIFO 半满时进中断搬数据,
 * 一次几十 KB 就是几千次中断, 比轮询还慢也更吵。
 * ========================================================================= */

#if SD_DISKIO_POLLING

/* 单次传输的超时 (ms)。sd_diskio 自己那层超时是 30 秒, 这里给 5 秒: 卡真卡住了就早点
 * 报错, 别把存卡任务按在那儿。一次几十 KB 的传输在这个时钟下也就几十毫秒。 */
#define SD_BSP_TIMEOUT_MS  5000u

uint8_t BSP_SD_ReadBlocks_DMA(uint32_t *pData, uint32_t ReadAddr, uint32_t NumOfBlocks)
{
    if (HAL_SD_ReadBlocks(&hsd1, (uint8_t *)pData, ReadAddr, NumOfBlocks,
                          SD_BSP_TIMEOUT_MS) != HAL_OK) {
        return MSD_ERROR;
    }
    BSP_SD_ReadCpltCallback();      /* 等价于中断里那条"读完了" */
    return MSD_OK;
}

uint8_t BSP_SD_WriteBlocks_DMA(uint32_t *pData, uint32_t WriteAddr, uint32_t NumOfBlocks)
{
    if (HAL_SD_WriteBlocks(&hsd1, (const uint8_t *)pData, WriteAddr, NumOfBlocks,
                           SD_BSP_TIMEOUT_MS) != HAL_OK) {
        return MSD_ERROR;
    }
    BSP_SD_WriteCpltCallback();
    return MSD_OK;
}

#endif /* SD_DISKIO_POLLING */

/* ---- 诊断 (sd.h 里 extern 过了): 只记数, 不打印 ---- */
volatile uint32_t sd_diag_rows;
volatile uint32_t sd_diag_gaps;
volatile uint32_t sd_diag_fres;

/* 逻辑通道 1..8 -> GPIOG 位号。跟 sharedconfig.h 的 LOG_CH_BIT 是同一张表, 这儿
 * 抄一份是因为 CSV 的列序要按它走 (第 1 对列 = CH1 = bit2 ... 第 8 对 = CH8 = bit13)。 */
static const uint8_t s_ch_bit[SD_CH_COUNT] = LOG_CH_BIT;

/* 表头。16 列列名写死在这儿是故意的: 列名是给人看的, 按 SD_CH_COUNT 生成反而绕。
 * 改动通道数的时候这里和 s_ch_bit 一起改。 */
static const char s_header[] =
    "CH1_T,CH1_V,CH2_T,CH2_V,CH3_T,CH3_V,CH4_T,CH4_V,"
    "CH5_T,CH5_V,CH6_T,CH6_V,CH7_T,CH7_V,CH8_T,CH8_V\r\n";

/* 一行最长 = 20 位时间戳 (纳秒档) + 15 个逗号 + ",1" + "\r\n" = 39, 取 48。 */
#define SD_LINE_MAX  48u

/* ---- 状态 ---- */
static uint8_t  s_mounted;      /* SD_Init() 成功挂上过 */
static uint8_t  s_file_open;    /* SDFile 正开着 */
static uint32_t s_buf_len;      /* s_buf 里攒了多少字节 */
static char     s_buf[SD_WRITE_CHUNK];

/* ===========================================================================
 * 行缓冲: 攒够一批交给一次 f_write
 * ========================================================================= */

/* 把 s_buf 里攒的字节写下去。返回 SD_OK / SD_ERR_WRITE。
 * 先清长度再写: 不管写没写成, 这段都不能留在缓冲里再写第二遍。 */
static uint8_t buf_flush(void)
{
    const uint32_t n = s_buf_len;
    UINT bw = 0u;
    FRESULT fr;

    if (n == 0u) {
        return SD_OK;
    }
    s_buf_len = 0u;

    if (!s_file_open) {
        return SD_ERR_STATE;        /* 文件没开着, 攒的东西没有去处 */
    }

    fr = f_write(&SDFile, s_buf, n, &bw);
    if (fr != FR_OK || (uint32_t)bw != n) {
        sd_diag_fres = (uint32_t)fr;
        return SD_ERR_WRITE;        /* 短写也算失败: 卡满了 / 写保护 / 掉卡 */
    }
    return SD_OK;
}

/* 往行缓冲里塞 n 字节。n 最多一行 (SD_LINE_MAX), 一定比 SD_WRITE_CHUNK 小 */
static uint8_t buf_put(const char *src, uint32_t n)
{
    if (n > SD_WRITE_CHUNK) {
        return SD_ERR_PARAM;
    }
    if (s_buf_len + n > SD_WRITE_CHUNK) {
        const uint8_t rc = buf_flush();
        if (rc != SD_OK) {
            return rc;
        }
    }
    memcpy(&s_buf[s_buf_len], src, n);
    s_buf_len += n;
    return SD_OK;
}

/* ===========================================================================
 * 拼一行
 * ========================================================================= */

/* uint64 -> 十进制, 写进 dst (至少 20 字节), 返回长度。
 * 手写而不是 snprintf: 这是每一行都要走一遍的地方 (几十万次), 不值得为了省这几行
 * 去让 newlib 的格式化代码在热路径上跑。 */
static uint32_t u64_dec(char *dst, uint64_t v)
{
    char tmp[20];
    uint32_t n = 0u;

    do {
        tmp[n++] = (char)('0' + (uint32_t)(v % 10u));
        v /= 10u;
    } while (v != 0u);

    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = tmp[n - 1u - i];
    }
    return n;
}

/* 时间戳那一列的值: 默认原样采样序号; SD_TS_AS_NANOSECONDS = 1 时换算成 ns
 * (ts / fs = 秒)。fs_hz 是 CM4 用 DWT 实测发布出来的 (LOG_CTRL->fs_hz), 不是标称
 * 20 MHz 那个数; 还没发布出来 (0) 就退回标称值, 总比除零强。 */
#if SD_TS_AS_NANOSECONDS
static uint64_t ts_value(uint32_t ts, uint32_t fs_hz)
{
    const uint32_t fs = (fs_hz != 0u) ? fs_hz : 20000000u;

    return ((uint64_t)ts * 1000000000u) / (uint64_t)fs;
}
#else
static uint64_t ts_value(uint32_t ts, uint32_t fs_hz)
{
    (void)fs_hz;    /* 这一档用不到; 参数留着是为了调用点上两档长得一样 */
    return (uint64_t)ts;
}
#endif

/* 拼一行: 16 列, 只有第 ch 路 (0..7) 的那两列有值 (ts, val), 其余留空。
 *     ch1: "0,0,,,,,,,,,,,,,,\r\n"      ch2: ",,0,1,,,,,,,,,,,,\r\n"
 * 每一路都是"逗号 + 时间戳 + 逗号 + 电平"这一对, 空的那路这两个位置就都空着
 * (所以是连续两个逗号), 这样列数才是 SD_CH_COUNT * 2。
 * 返回这一行的字节数。out 至少 SD_LINE_MAX 字节。 */
static uint32_t row_build(char *out, uint64_t ts, uint32_t ch, uint32_t val)
{
    uint32_t n = 0u;

    for (uint32_t c = 0u; c < SD_CH_COUNT; c++) {
        if (c != 0u) {
            out[n++] = ',';
        }
        if (c == ch) {
            n += u64_dec(&out[n], ts);
        }
        out[n++] = ',';
        if (c == ch) {
            out[n++] = (char)('0' + (val & 1u));
        }
    }
    out[n++] = '\r';
    out[n++] = '\n';
    return n;
}

/* ===========================================================================
 * 初始化 / 挂载
 * ========================================================================= */

uint8_t SD_Init(void)
{
    FRESULT fr;

    /* 先把上一次的开着的东西收干净: 卷都要重挂了, 句柄留着就是悬空的 */
    if (s_file_open) {
        (void)SD_Close();
    }

    /* 已经挂过就先摘掉 (f_mount 传 NULL 就是卸载这一卷)。插拔过卡、换过卡、或者
     * 上一次挂载失败之后, FatFs 里可能还留着旧状态, 重挂一遍最省事。 */
    if (s_mounted) {
        (void)f_mount(0, SDPath, 0);
        s_mounted = 0u;
    }

    fr = f_mount(&SDFatFS, SDPath, 1);
    if (fr != FR_OK) {
        sd_diag_fres = (uint32_t)fr;    /* FR_NOT_READY = 没插卡/没认到;
                                         * FR_NO_FILESYSTEM = 卡没格式化 */
        return SD_ERR_MOUNT;
    }

    s_mounted = 1u;
    return SD_OK;
}

uint8_t SD_IsReady(void)
{
    return (s_mounted != 0u) ? (uint8_t)1u : (uint8_t)0u;
}

/* ===========================================================================
 * 创建 / 写入 / 收尾 / 删除
 * ========================================================================= */

uint8_t SD_Create(const char *name)
{
    const UINT hdr_len = (UINT)(sizeof(s_header) - 1u);
    UINT bw = 0u;
    FRESULT fr;

    if (name == 0) {
        return SD_ERR_PARAM;
    }
    if (!s_mounted) {
        return SD_ERR_STATE;        /* 先 SD_Init() */
    }

    /* 上一次还开着就先收掉: 重复存卡 (存完一帧又存一帧) 不能把句柄漏在外面 */
    if (s_file_open) {
        (void)SD_Close();
    }

    /* FA_CREATE_ALWAYS: 没有就新建, 有就截断成 0 字节 —— 一份采集一个文件,
     * 不留上一次的内容。 */
    fr = f_open(&SDFile, name, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        sd_diag_fres = (uint32_t)fr;
        return SD_ERR_OPEN;
    }
    s_file_open = 1u;
    s_buf_len   = 0u;

    /* 表头直接写, 不走行缓冲 (它只在开头一次)。写失败也要把文件收掉, 别留个半开的
     * 句柄在那儿。 */
    fr = f_write(&SDFile, s_header, hdr_len, &bw);
    if (fr != FR_OK || bw != hdr_len) {
        sd_diag_fres = (uint32_t)fr;
        (void)SD_Close();
        return SD_ERR_WRITE;
    }
    return SD_OK;
}

uint8_t SD_WriteRecs(const log_rec_t *recs, uint32_t count, uint32_t fs_hz)
{
    char     line[SD_LINE_MAX];
    uint32_t rows = 0u;
    uint32_t gaps = 0u;
    uint32_t prev = 0u;             /* 上一条记录的完整电平 */
    uint8_t  rc   = SD_OK;

    if (recs == 0 || count == 0u) {
        return SD_ERR_PARAM;
    }
    if (!s_file_open) {
        return SD_ERR_STATE;        /* 先 SD_Create() */
    }

    for (uint32_t i = 0u; i < count; i++) {
        const uint32_t smp = (uint32_t)recs[i].smp & LOG_PIN_MASK;
        const uint64_t ts  = ts_value(recs[i].ts, fs_hz);
        uint32_t chg;

        if ((recs[i].flags & LOG_REC_GAP) != 0u) {
            gaps++;
        }

        /* 哪几路跳了 = 跟上一回的完整电平异或。记录里存的是"变化之后的完整电平",
         * 不是变化遮罩 (见 sharedconfig.h 的 log_rec_t), 所以要自己算。
         * 第 0 条是起点 (合上开关那一刹的八路电平, ts 恒为 0), 八路各写一行:
         * 没有它, 后面每一路的第一个 0/1 就没有参照。 */
        chg  = (i == 0u) ? LOG_PIN_MASK : (smp ^ prev);
        prev = smp;

        for (uint32_t c = 0u; c < SD_CH_COUNT; c++) {
            const uint32_t bit = s_ch_bit[c];

            if ((chg & (1u << bit)) == 0u) {
                continue;
            }
            {
                const uint32_t n = row_build(line, ts, c, (smp >> bit) & 1u);

                rc = buf_put(line, n);
                if (rc != SD_OK) {
                    goto done;
                }
            }
            rows++;
        }
    }

    /* 末尾没攒满的那一段也要下去 */
    rc = buf_flush();
    if (rc != SD_OK) {
        goto done;
    }

    /* f_sync: 把 FatFs 的扇区缓冲和文件的目录项都刷到卡上。刷完这一下, 卡上的
     * 文件就是完整的, 这时候拔卡/断电丢的也只是"还没写的下一次"。 */
    {
        const FRESULT fr = f_sync(&SDFile);
        if (fr != FR_OK) {
            sd_diag_fres = (uint32_t)fr;
            rc = SD_ERR_WRITE;
        }
    }

done:
    sd_diag_rows = rows;
    sd_diag_gaps = gaps;
    return rc;
}

uint8_t SD_Close(void)
{
    uint8_t rc = SD_OK;
    FRESULT fr;

    if (!s_file_open) {
        return SD_OK;               /* 幂等: 没开着就是已经收好了 */
    }

    /* 还攒着的那一段先下去 (正常路径上 SD_WriteRecs 已经刷过了) */
    if (buf_flush() != SD_OK) {
        rc = SD_ERR_WRITE;
    }

    /* f_close 里自己会 f_sync 一次, 所以不用再单独调 */
    fr = f_close(&SDFile);
    s_file_open = 0u;
    if (fr != FR_OK) {
        sd_diag_fres = (uint32_t)fr;
        rc = SD_ERR_WRITE;
    }
    return rc;
}

uint8_t SD_Delete(const char *name)
{
    FRESULT fr;

    if (name == 0) {
        return SD_ERR_PARAM;
    }
    if (!s_mounted) {
        return SD_ERR_STATE;        /* 先 SD_Init() */
    }

    /* 要删的正好是开着的那个文件, 先把句柄还回去: 文件在 FatFs 里删掉了, 但那个
     * FIL 里的目录项位置还指着已经不存在的条目。 */
    if (s_file_open) {
        (void)SD_Close();
    }

    fr = f_unlink(name);
    if (fr != FR_OK) {
        sd_diag_fres = (uint32_t)fr;    /* 文件不存在也是 FR_NO_FILE */
        return SD_ERR_UNLINK;
    }
    return SD_OK;
}

/* ===========================================================================
 * 文件名 / 返回值字符串
 * ========================================================================= */

void SD_MakeName(char *out, uint32_t index)
{
    if (out == 0) {
        return;
    }
    /* 8.3 短名 (_USE_LFN = 0): "LOG0001.CSV"。用 snprintf 就是为了补零那一下,
     * 一次存卡只调一次, 不心疼。 */
    (void)snprintf(out, SD_NAME_MAX, "%s%04u.%s",
                   SD_NAME_PREFIX, (unsigned)(index % 10000u), SD_NAME_EXT);
}

/* 'a'..'z' → 'A'..'Z', 其余原样。只用来比文件名 (卡上别处建的名字可能是小写) */
static char sd_up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

uint32_t SD_NextIndex(void)
{
    DIR dir;
    FILINFO fno;
    uint32_t max = 0u;

    if (f_opendir(&dir, SDPath) != FR_OK) {
        return 1u;
    }

    for (;;) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') {
            break;
        }
        if ((fno.fattrib & AM_DIR) != 0u) {
            continue;                       /* 目录不算 */
        }

        /* 形状: "LOG" + 4 位数字 + '.' + "CSV" (_USE_LFN = 0, 只能是 8.3) */
        uint32_t k;
        for (k = 0u; k < 3u; k++) {
            if (sd_up(fno.fname[k]) != SD_NAME_PREFIX[k]) { break; }
        }
        if (k != 3u) { continue; }

        uint32_t idx = 0u;
        for (k = 0u; k < 4u; k++) {
            const char c = fno.fname[3u + k];
            if (c < '0' || c > '9') { break; }
            idx = idx * 10u + (uint32_t)(c - '0');
        }
        if (k != 4u) { continue; }

        if (fno.fname[7] != '.') { continue; }
        if (sd_up(fno.fname[8])  != SD_NAME_EXT[0]) { continue; }
        if (sd_up(fno.fname[9])  != SD_NAME_EXT[1]) { continue; }
        if (sd_up(fno.fname[10]) != SD_NAME_EXT[2]) { continue; }
        if (fno.fname[11] != '\0') { continue; }

        if (idx > max) {
            max = idx;
        }
    }

    (void)f_closedir(&dir);

    /* 编到 9999 之后回到 1 重新找 (编号只有四位, 和 SD_MakeName 一个口径) */
    return (max >= 9999u) ? 1u : (max + 1u);
}

const char *SD_StrErr(uint8_t res)
{
    switch (res) {
    case SD_OK:         return "OK";
    case SD_ERR_PARAM:  return "PARAM";
    case SD_ERR_MOUNT:  return "MOUNT";
    case SD_ERR_OPEN:   return "OPEN";
    case SD_ERR_WRITE:  return "WRITE";
    case SD_ERR_UNLINK: return "UNLINK";
    case SD_ERR_STATE:  return "STATE";
    default:            return "?";
    }
}
