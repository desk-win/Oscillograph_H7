#ifndef __SD_H
#define __SD_H

#include <stdint.h>

/* 这里要用到 log_rec_t (跳变点记录) 和 LOG_REC_GAP, 都在 sharedconfig.h 里 ——
 * 不 include 它, 光写 log_rec_t 是 "'log_rec_t' does not name a type"。
 * 这个头会被 C 和 C++ 两边 include (TouchGFX 的 Model 是 C++), 所以下面要有
 * extern "C" 守卫, 否则链接期名字修饰对不上, 报 undefined reference。 */
#include "sharedconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * SD 卡存储 —— 把逻辑分析仪的跳变点表存成 CSV
 *
 * 数据从哪来:
 *   logic_trace.c 里那张跳变记录表 (LOG_REC_BUF, D1 AXI SRAM, 一条 8 B:
 *   ts = 采样序号, smp = 那一刻 8 路的完整电平, bit 映射同 LOG_CH_BIT)。
 *   要存卡的时候 LOG_Trace_BorrowRecs() 借出整表 (借用期间采集不会重开),
 *   存完 LOG_Trace_ReleaseRecs()。这一层不碰 logic_trace 的静态变量, 只吃那张表,
 *   所以它跟逻辑分析仪是解耦的 —— 只依赖 sharedconfig.h 的 log_rec_t。
 *
 * 为什么在 CM7 这一侧:
 *   SDMMC1 + FatFs 都挂在 CM7 (CM7/Core/Src/sdmmc.c, CM7/FATFS/...), 跳变表也在
 *   CM7 手里。CM4 那边只管采样, 不碰卡。
 *
 * 四个入口 (就是初始化 / 创建 / 写入 / 删除):
 *   SD_Init()       初始化: f_mount, 认卡 + 挂卷
 *   SD_Create()     创建:   新建 (或清空) 一个 CSV, 写表头
 *   SD_WriteRecs()  写入:   整张跳变表 → CSV 的行
 *   SD_Delete()     删除:   f_unlink
 *
 * CSV 长什么样 (8 路 → 16 列, 一个通道两列: 时间戳 + 电平):
 *
 *     CH1_T,CH1_V,CH2_T,CH2_V,CH3_T,CH3_V,CH4_T,CH4_V,
 *     CH5_T,CH5_V,CH6_T,CH6_V,CH7_T,CH7_V,CH8_T,CH8_V
 *     0,0,,,,,,,,,,,,,,          <- 起点: ts = 0 时 CH1 = 0
 *     ,,0,1,,,,,,,,,,,,          <- 起点: ts = 0 时 CH2 = 1
 *     12345,1,,,,,,,,,,,,,,      <- CH1 在采样序号 12345 上跳到 1
 *     ,,23456,0,,,,,,,,,,,,      <- CH2 在采样序号 23456 上跳到 0
 *     ...
 *
 * 一行 = 一次跳变: 哪个通道跳了, 就把它那两列填上 (时间戳, 跳变后的电平 0/1),
 * 另外 14 列留空。所以每一路单独看都是"竖着往下排"的一列时间戳 + 一列电平,
 * 直接丢进 Excel 画阶梯图就是波形。
 * ts = 0 那几行是起点: 合上开关那一刹 8 路各是什么电平, 八路各写一行 (上面只画了
 * 前两路)。没有它,
 * 后面每一路的第一个 0/1 就没有参照 (记录存的是"变化之后的完整电平",
 * 只看跳变推不出电平 —— 见 sharedconfig.h 里 log_rec_t 那一段)。
 *
 * 时间戳写什么: 默认就是**采样序号** (log_rec_t.ts, 无损, 一秒的采集也就 2e7)。
 *   "采样序号 / fs_hz = 秒"。想直接写真实时间 (ns) 就把下面的
 *   SD_TS_AS_NANOSECONDS 打开, 那时才用得上 fs_hz。
 *
 * 前置条件 (前两条不满足, f_mount 一定失败):
 *   1. main.c 里跑过 MX_SDMMC1_SD_Init() + MX_FATFS_Init() (现在就是,
 *      MX_FATFS_Init 把 SD_Driver 链到 SDPath 上, 那一步是纯内存操作,
 *      在 osKernelStart() 之前做没问题)。
 *   2. **必须在 osKernelStart() 之后、从任务里调**: sd_diskio.c 的 SD_initialize()
 *      头一条就是 osKernelGetState() == osKernelRunning, 没起调度器它直接返回
 *      STA_NOINIT; 而且 _FS_REENTRANT = 1, f_mount 内部要建一个 CMSIS-RTOS2
 *      信号量 (也是从 FreeRTOS 堆里出)。所以别放进 main.c 的 USER CODE BEGIN 2。
 *   3. 文件系统是 8.3 短名 (_USE_LFN = 0): 名字最多 8 + 3 个字符, 用
 *      SD_MakeName() 生成 "LOG0001.CSV" 这种, 别自己拼长名字。
 *
 * 第一次真用这张卡, 建议先写一版测一版:
 *   SDFatFS / SDFile 跟别的全局变量一样落在 .bss → DTCM (0x20000000), 而写卡时
 *   是 SDMMC 的 IDMA 去读这片内存。本工程 docs/log_plan.md 里记着 "TCM: 任何
 *   DMA 引擎都访问不到"。要是实测挂载成功、写下去读回来却是空文件或 0xFF, 就把
 *   fatfs.c 里的 SDFatFS / SDFile 搬到 D2 或 AXI SRAM (落在缓存区的话还得打开
 *   sd_diskio.c 里注掉的那行 ENABLE_SD_DMA_CACHE_MAINTENANCE)。这条只影响硬件
 *   实测, 跟下面这些函数的逻辑无关。
 * ========================================================================= */

/* ---- 返回值 ----
 * 每个函数都返回这几个之一。区分不了的细节 (没卡 / 卡没格式化 / 写保护) 落在
 * sd_diag_fres 里, 那是 FatFs 给的原始 FRESULT, 对照 ff.h 的枚举看。 */
#define SD_OK          0u   /* 成功 */
#define SD_ERR_PARAM   1u   /* 名字或记录表给的是空指针 */
#define SD_ERR_MOUNT   2u   /* f_mount 失败: 没插卡 / 卡没格式化 / 调度器还没起 */
#define SD_ERR_OPEN    3u   /* f_open 失败 */
#define SD_ERR_WRITE   4u   /* 短写 / f_sync / f_close 失败 */
#define SD_ERR_UNLINK  5u   /* f_unlink 失败 (文件不存在时也是这个, FatFs 给 FR_NO_FILE) */
#define SD_ERR_STATE   6u   /* 状态不对: 没 Init 就 Create / 没 Create 就 Write */

/* ---- 文件名 ----
 * _USE_LFN = 0, 只有 8.3。SD_MakeName() 生成 "LOG0001.CSV" = 11 个字符, 加上
 * 结尾的 '\0' 是 12, 调用方按这个开数组。 */
#define SD_NAME_MAX     12u
#define SD_NAME_PREFIX  "LOG"
#define SD_NAME_EXT     "CSV"

/* 时间戳那一列写什么。0 = 采样序号 (原始值, 默认); 1 = 纳秒 (ts * 1e9 / fs_hz)。 */
#define SD_TS_AS_NANOSECONDS  0

/* 攒多少字节的行才交给一次 f_write。写卡是"归档一帧"这种低频动作, 不是实时流,
 * 但一次采集最坏也有五十多万行 (65536 条记录 × 8 路), 一行一次 f_write 是五十多万
 * 次调用 + 五十多万次扇区缓冲搬运, 慢得没意义。512 的整数倍, 让 FatFs 的扇区缓冲
 * (512 B) 正好整数倍地搬。 */
#define SD_WRITE_CHUNK  2048u

/* 逻辑分析仪的通道数。列数是它的两倍 (每路一列时间戳 + 一列电平)。 */
#define SD_CH_COUNT     8u

/* ---- 磁盘 I/O 走哪条路 ----
 *
 * 0 = ST 模板那条 DMA 模式: 用 sd_diskio.c 里的 BSP_SD_Read/WriteBlocks_DMA, 让
 *     SDMMC 的 IDMA 直接往调用方的缓冲写, 完成后由 SDMMC 中断投一条队列消息。
 * 1 = 轮询模式 (默认): sd.c 把那两个 __weak 的 DMA 接口换成轮询实现 —— 数据由 CPU
 *     从 SDMMC 的 FIFO 搬, 完成消息在返回前自己投一条, 其它逻辑一行不动。
 *
 * 为什么默认走 1:
 *   FatFs 的工作缓冲 (SDFatFS / SDFile) 跟别的全局变量一样落在 .bss = DTCM
 *   (0x20000000), 而 SDMMC 的 IDMA 够不到 DTCM —— DMA 模式下传输直接报错、完成中断
 *   不来, sd_diskio 干等到 SD_TIMEOUT (30 秒) 才超时, FatFs 报 FR_DISK_ERR = 1。
 *   轮询模式是 CPU 搬数据, 根本不使能 IDMA (见 stm32h7xx_hal_sd.c 里
 *   HAL_SD_ReadBlocks 的 SDMMC_ReadFIFO 循环), 所以 DTCM 缓冲没问题。
 *   实测: 开机自检里把同一把扇区读进 DTCM 缓冲是好的, 走 diskio 那条 DMA 路就是
 *   超时 —— 差别只在"谁来搬"。
 *
 * 代价: 传输期间 CPU 忙着等 FIFO (存卡任务优先级低, 界面任务优先级更高, 所以界面
 *   照跑)。归档一帧这种低频写入完全够用。
 * 想回 ST 那条 DMA 路: 把这里改成 0, 前提是先把 SDFatFS / SDFile 搬到 D2 或 AXI
 *   SRAM 并配好 MPU 属性 (落在缓存区还要处理 cache 一致性)。 */
#define SD_DISKIO_POLLING  1

/**
 * 初始化 (初始化): 把 SD 卷挂上 —— f_mount(&SDFatFS, SDPath, 1)。
 * **必须在 osKernelStart() 之后、从任务里调** (理由见文件头上面的前置条件 2)。
 * 插拔卡之后可以再调一次: 它会先把上次的卷摘掉再重新挂, 重新认卡 (所以卡拔了又
 * 插上、或者换了一张卡, 都走这一条)。
 * 只挂卷, 不碰文件。返回 SD_OK / SD_ERR_MOUNT。
 */
uint8_t SD_Init(void);

/**
 * 卷挂上了没有 (给界面判断要不要显示"没卡"用)。
 * 0 = 还没 Init 成功过。注意它记的是"上次 SD_Init 的结果": 挂上之后把卡拔了,
 * 它不会自己变 0 —— 要重新认卡就再调一次 SD_Init()。
 */
uint8_t SD_IsReady(void);

/**
 * 创建 (创建): 新建 (同名文件则清空) 一个 CSV 并写表头, 文件保持打开。
 * name 用 SD_MakeName() 生成, 或者自己给个 8.3 的短名 —— 不带盘符, 盘符在
 * SDPath 里 ("0:/")。上一次还开着文件的话先替它收掉 (重复存卡不会漏句柄)。
 * 接着调 SD_WriteRecs() 写数据, 最后一次写完调 SD_Close() 收尾。
 * 返回 SD_OK / SD_ERR_PARAM / SD_ERR_STATE / SD_ERR_OPEN / SD_ERR_WRITE。
 */
uint8_t SD_Create(const char *name);

/**
 * 写入 (写入): 把整张跳变记录表写成 CSV 的行 (表头后面的部分)。
 * recs / count 就是 LOG_Trace_BorrowRecs() 借出来的那一对 —— count 是**有效条数**
 * 不是容量, 记录 0 (ts = 0 的起点) 必须含在里面。
 * fs_hz 只在 SD_TS_AS_NANOSECONDS = 1 时用得上 (传 LOG_CTRL->fs_hz 就行),
 * 传 0 的话按标称 20 MSa/s 算。
 * 写完会 f_sync 一次, FatFs 的扇区缓冲和目录项都刷到卡上 —— 这时候拔卡也不会
 * 丢数据, 文件还是完整的 (只是句柄没还回去)。文件保持打开, 想接着追加就再调一次;
 * 收工调 SD_Close()。这个函数不碰采集: 记录表是调用方借好的, 借用期间采集本来
 * 就不会重开 (见 logic_trace.h 的 BorrowRecs)。
 * 返回 SD_OK / SD_ERR_PARAM / SD_ERR_STATE / SD_ERR_WRITE。
 * 写了多少行、表里有多少条带丢帧标记的记录, 落在 sd_diag_rows / sd_diag_gaps。
 */
uint8_t SD_WriteRecs(const log_rec_t *recs, uint32_t count, uint32_t fs_hz);

/**
 * 收尾: f_sync + f_close, 把文件句柄还回去。幂等 —— 没开着文件就直接返回 SD_OK。
 * 返回 SD_OK / SD_ERR_WRITE。
 */
uint8_t SD_Close(void);

/**
 * 删除 (删除): f_unlink(name), 把卡上的文件删掉 (同名覆盖前先删也行, 但
 * SD_Create 的 FA_CREATE_ALWAYS 本来就会截断, 不必多这一步)。
 * 要删的正好是打开着的那个文件的话, 先替它收掉。
 * 返回 SD_OK / SD_ERR_PARAM / SD_ERR_STATE / SD_ERR_UNLINK。
 */
uint8_t SD_Delete(const char *name);

/**
 * 生成文件名 "LOG0001.CSV"。out 要能装 SD_NAME_MAX 个字节。
 * index 超过 9999 会回绕 (只取低四位); 起始值由调用方定 —— 从 1 开始最顺眼,
 * 也就是 LOG0001.CSV 起。
 */
void SD_MakeName(char *out, uint32_t index);

/**
 * 扫一遍根目录里已有的 "LOGnnnn.CSV", 返回**下一个可用序号** (最大编号 + 1)。
 * 目录空 / 打不开 / 没有同形状的名字就返回 1; 编到 9999 之后也回到 1。
 *
 * 存卡任务在第一次挂卷之后调一次, 之后自己顺着推 —— 关机重开、或者换了张已经
 * 有 LOG0007.CSV 的卡, 都不会覆盖掉旧文件。只在任务里调。
 */
uint32_t SD_NextIndex(void);

/**
 * 返回值的短字符串 ("OK" / "MOUNT" / ...), 给界面上那行状态显示用。
 * 越界返回 "?"。
 */
const char *SD_StrErr(uint8_t res);

/* ---- 诊断 (跟别的模块一个路子: 只记数, 不打印) ---- */

/** 最近一次 SD_WriteRecs 写下的行数 (不含表头) */
extern volatile uint32_t sd_diag_rows;
/** 最近一次 SD_WriteRecs 看到几条带 LOG_REC_GAP 的记录。非 0 说明那一帧之前有半帧
 * 没扫到, 那半帧里的跳变就少了几行 (电平还是对的 —— 记录存的是变化后的完整电平)。
 * 对应着去看 CM4 的 log_diag_gap_recs。 */
extern volatile uint32_t sd_diag_gaps;
/** 最近一次失败时 FatFs 给的原始 FRESULT (成功的那次不更新), 对照 ff.h 看 */
extern volatile uint32_t sd_diag_fres;

/* 这几个入口已经进了 CM7/CMakeLists.txt 的 target_sources。调用点只有一个:
 * CM7/Mylibrary/Src/logic_save.c 的存卡任务 (界面上的 Save 只发请求)。 */

#ifdef __cplusplus
}
#endif

#endif /* __SD_H */
