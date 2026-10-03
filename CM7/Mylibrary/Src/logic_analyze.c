/* ===========================================================================
 * CM7 侧逻辑分析仪 —— 协议解码 (跳变表 → 一串帧)
 *
 * 接口和设计约束都在 logic_analyze.h 的文件头, 这里只讲实现上几个要点。
 *
 * ── 为什么在跳变表上解码 ──
 * log_rec_t 是"变化之后的完整电平", 两个跳变之间电平恒定 —— 对二值数字线来说
 * 这张表是无损的, 任意采样序号上的电平都能精确还原。见 rec_level_at()。
 * 原始采样缓冲做不到这件事: 32 KB 双缓冲早被 DMA 绕圈盖掉了。
 *
 * ── 时间轴 ──
 * 全部用采样序号, 不用微秒也不碰 DWT —— 位宽 = fs_hz / baud 是个定点数,
 * 用到的地方一律 Q16 (低 16 位是小数)。一个位累积到第十位误差也只有 10/65536
 * 个采样点, 远小于半个位。
 *
 * 注意 (start_ts << 16) 必须用 uint64: 采样序号最大到 2000 万 (空闲超时是 1 秒
 * @20 MSa/s), 左移 16 位就是 1.3e12, 32 位装不下。
 *
 * ── 这一层不碰什么 ──
 * 不碰 TouchGFX, 不碰任何外设寄存器, 不碰 logic_trace.c 的状态 —— 只读它交出来
 * 的记录表。所以以后要把它搬到独立 RTOS 任务里, 改的只是"谁调 LOG_Analyze_Run"。
 * ========================================================================= */

#include "logic_analyze.h"
#include "logic_trace.h"
#include <stdint.h>

/* DWT 周期计数器 (0xE0001004), 和 osc_probe.h 里 OSC_PROBE_CYC 是同一个地址。
 *
 * 这里自己写一份而不是 include osc_probe.h: 那份探针的文件头写着"调完请把这份
 * 一起删掉", 解码这一层没有理由跟着它一起消失。
 *
 * 计数器由 OSC_Probe_Init() (Model 构造函数里调) 解锁。哪天探针真被删了, 这个
 * 读数会恒为 0 —— 只影响 log_ana_diag_cyc 这一列诊断, 不影响解码本身。 */
#define LOG_ANA_CYC (*(volatile uint32_t *)0xE0001004u)

/* 逻辑通道号 1..8 → GPIOG 的位号。和 logic_trace.c 用的是同一张表
 * (sharedconfig.h 的 LOG_CH_BIT), 这里只是把它摊成一个数组好按下标取。 */
static const uint8_t s_ch_bit[8] = LOG_CH_BIT;

/* ---- 配置与结果 ---- */

static log_ana_cfg_t  s_cfg;
static log_ana_item_t s_items[LOG_ANA_MAX_ITEMS];
static uint32_t       s_count;

/* ---- 诊断 ---- */
volatile uint32_t log_ana_diag_items;
volatile uint32_t log_ana_diag_cyc;
volatile uint32_t log_ana_diag_gap;
volatile uint32_t log_ana_diag_trunc;

/* ===========================================================================
 * 跳变表查询
 * ========================================================================= */

/**
*   @brief 记录表里"某个采样序号上的电平" (只取 mask 那一位)
*
*   @param r    记录表 / @param n 条数 / @param ts 采样序号 / @param mask 通道掩码
*   @return 该时刻那一路上是高 (1) 还是低 (0)
*
*   @note  两个记录之间电平恒定, 所以只要找到"最后一条 ts <= 查询值"的记录就行。
*          记录 0 的 ts 恒为 0 (LOG_Trace_Start 写下去的起点电平), 所以任意
*          ts >= 0 都有解, 不用处理"查早了"。
*
*          骨架阶段用二分。帧内每个采样点查一次, 记录满 (65536 条) 时估算要
*          ~27 ms。要提速就换游标: 帧内的查询时刻是单调递增的, 而且帧与帧之间
*          也是, 所以一个只往前挪的游标就能把这一串二分摊平。
*/
static uint32_t rec_level_at(const log_rec_t *r, uint32_t n, uint32_t ts,
                             uint16_t mask)
{
    if (n == 0u) {
        return 1u;              /* 一条记录都没有: 当空闲高电平 */
    }
    if (ts <= r[0].ts) {
        return (uint32_t)(r[0].smp & mask);
    }

    uint32_t lo = 0u;
    uint32_t hi = n - 1u;       /* 不变式: r[lo].ts <= ts */
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1u) / 2u;
        if (r[mid].ts <= ts) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    return (uint32_t)(r[lo].smp & mask);
}

/* ===========================================================================
 * UART
 *
 * 帧结构 (空闲是高):
 *
 *      ┌─────┬───┬───┬─ ─ ─┬────┬──────┐
 *      │start│D0 │D1 │ ... │ Dn │parity│stop│
 *      └─────┴───┴───┴─ ─ ─┴────┴──────┘
*        0    1   2         n+1  n+2    n+3      (单位: 位, 从起始位下降沿起算)
*
* 起始位 = 一个下降沿; 数据位 LSB 先; 停止位必须是高。
* 采样点一律取每一位的**中心**, 这样位宽有 ±半位的容差。
* ========================================================================= */

/**
*   @brief 在跳变表上解一路 UART
*
*   @param recs / nrec  记录表和条数 (调用方保证采集已停, 表是冻结的)
*   @param fs_hz        实测采样率, 位宽 = fs_hz / baud
*   @param c            这一路的配置
*   @param out / cap    输出数组和容量
*   @param n_gap        [out] 因为帧头撞上丢帧区间而放弃的帧数
*   @param n_trunc      [out] 因为数组满了没存下来的帧数 (非 0 = 结果被截断)
*   @return 存进 out 的帧数
*/
static uint32_t uart_decode(const log_rec_t *recs, uint32_t nrec, uint32_t fs_hz,
                            const log_ana_uart_cfg_t *c,
                            log_ana_item_t *out, uint32_t cap,
                            uint32_t *n_gap, uint32_t *n_trunc)
{
    /* ---- 把配置规范化 ----
     * 这几个值是从设置屏的候选值表里翻过来的, 正常情况下一定合法; 但这一层
     * 不假设上游没写坏 —— 越界一律兜到默认档, 免得后面拿它当除数或者移位。 */
    const uint32_t baud = (c->baud != 0u) ? c->baud : 9600u;
    const uint32_t nbits = (c->data_bits == 7u) ? 7u : 8u;
    const uint32_t stop_x10 = (c->stop_x10 == 15u || c->stop_x10 == 20u)
                            ? c->stop_x10 : 10u;
    const uint32_t has_par = (c->parity != LOG_UART_PARITY_NONE) ? 1u : 0u;
    const uint32_t ch = (c->rx_ch < 8u) ? c->rx_ch : 0u;
    const uint16_t mask = (uint16_t)(1u << s_ch_bit[ch]);

    /* ---- 位宽, Q16 定点 ----
     * 20 MSa/s、9600 baud → 2083.33 个采样点/位 → Q16 约 1.37e8, uint32 装得下。
     * fs_hz < baud 时它取整成 0: 一位还不到一个采样点, 物理上解不了 (奈奎斯特),
     * 直接认输而不是拿 0 当步长在死循环里转。 */
    const uint32_t tbit_q16 = (uint32_t)(((uint64_t)fs_hz << 16) / (uint64_t)baud);
    if (tbit_q16 == 0u) {
        return 0u;
    }

    /* 一整帧有多长 (单位: 1/10 位) —— 用来把扫描位置跳到帧尾之后。 */
    const uint32_t total_x10 = 10u + nbits * 10u + has_par * 10u + stop_x10;

    uint32_t out_n = 0u;
    uint32_t gap_n = 0u;
    uint32_t trunc_n = 0u;

    /* 从 1 开始: 记录 0 是"合上开关那一刻的电平", 不是跳变, 拿它当起始位是错的。 */
    uint32_t i = 1u;
    while (i < nrec) {
        const uint32_t cur  = (uint32_t)(recs[i].smp & mask);
        const uint32_t prev = (uint32_t)(recs[i - 1u].smp & mask);

        /* 起始位 = 一个下降沿 (之前是高, 现在是低)。 */
        if (cur != 0u || prev == 0u) {
            i++;
            continue;
        }

        /* 帧头撞上丢帧区间: 那之前丢过整整几毫秒的样本, 这段时间里的跳变根本
         * 没被记下来 —— 拿它当帧头, 整帧的位置就是错的。放弃这一帧。 */
        if ((recs[i].flags & LOG_REC_GAP) != 0u) {
            gap_n++;
            i++;
            continue;
        }

        const uint32_t start_ts = recs[i].ts;
        uint64_t p = ((uint64_t)start_ts << 16) + tbit_q16 / 2u;    /* 起始位中心 */

        /* 半个位之后还是低, 才算真的起始位 —— 挡掉窄毛刺引起的假起始位。 */
        if (rec_level_at(recs, nrec, (uint32_t)(p >> 16), mask) != 0u) {
            i++;
            continue;
        }

        p += tbit_q16;              /* 挪到第 0 个数据位的中心 */

        uint32_t byte = 0u;
        for (uint32_t k = 0u; k < nbits; k++) {
            if (rec_level_at(recs, nrec, (uint32_t)(p >> 16), mask) != 0u) {
                byte |= (1u << k);  /* LSB 先 */
            }
            p += tbit_q16;
        }

        uint32_t par_bit = 0u;
        if (has_par != 0u) {
            par_bit = rec_level_at(recs, nrec, (uint32_t)(p >> 16), mask);
            p += tbit_q16;
        }

        /* 停止位必须是高。不是高就是帧错 —— 丢掉, 从下一个下降沿重来。
         * 帧内某个数据 0 被误当成起始位时, 正是在这里被挡下来的。 */
        if (rec_level_at(recs, nrec, (uint32_t)(p >> 16), mask) == 0u) {
            i++;
            continue;
        }

        /* ---- 校验位 ----
         * 对不上只打个标记, **不丢帧** —— 用户多半就是想看看线上到底传了什么,
         * 把有问题的字节藏起来反而更难查。 */
        uint8_t flags = 0u;
        if (has_par != 0u) {
            uint32_t ones = 0u;
            for (uint32_t k = 0u; k < nbits; k++) {
                ones += (byte >> k) & 1u;
            }
            /* 奇校验: 数据位 + 校验位里 1 的总数要为奇 → 数据位有偶数个 1 时期望校验位是 1 */
            const uint32_t want = (c->parity == LOG_UART_PARITY_ODD)
                                ? ((ones & 1u) == 0u)
                                : ((ones & 1u) != 0u);
            if (want != par_bit) {
                flags |= LOG_ANA_IT_ERR;
            }
        }

        /* ---- 跳到帧尾之后的第一个记录 ----
         * 不跳的话, 帧内某个数据 0 会被当成新的起始位, 一个字节解成好几个。
         *
         * 顺便扫一遍这帧跨过的记录: 中间撞上丢帧区间的帧内容不可信, 但还是存
         * 下来打标记 —— "这里有问题"比"这里凭空少一段"有用。 */
        const uint32_t end_ts = (uint32_t)((((uint64_t)start_ts << 16)
                               + ((uint64_t)tbit_q16 * total_x10) / 10u) >> 16);
        for (; i < nrec && recs[i].ts < end_ts; i++) {
            if ((recs[i].flags & LOG_REC_GAP) != 0u) {
                flags |= LOG_ANA_IT_GAP;
            }
        }

        if (out_n < cap) {
            log_ana_item_t *it = &out[out_n];
            out_n++;

            it->ts    = start_ts;
            it->proto = LOG_ANA_UART;
            it->flags = flags;
            it->len   = 1u;
            it->aux   = 0u;

            /* 整片清零再写第一个字节: 现在只有 data[0] 有用, 但清干净之后
             * 以后加 I2C/SPI/CAN 时不用再回头想"剩下那几个字节是不是脏的"。 */
            for (uint32_t k = 0u; k < LOG_ANA_ITEM_MAX; k++) {
                it->data[k] = 0u;
            }
            it->data[0] = (uint8_t)byte;
        } else {
            trunc_n++;
        }
    }

    if (n_gap   != 0) { *n_gap   = gap_n; }
    if (n_trunc != 0) { *n_trunc = trunc_n; }
    return out_n;
}

/* ===========================================================================
 * I2C
 *
 * 两根开漏线, 空闲都是高。帧界是"SCL 高的时候 SDA 变" —— 数据段里 SDA 只允许
 * 在 SCL 低的时候变, 所以这两个组合不可能出现在数据里:
 *
 *     起始位 = SCL 高时 SDA 下降 (S)
 *     停止位 = SCL 高时 SDA 上升 (P)
 *
 * 一个字节: 8 位 + 1 个应答位
 *
 *      SCL ─┐ ┌─┐ ┌─┐ ┌─ ─ ─ ─┐ ┌─┐ ┌─ ─ ─┐ ┌─┐ ┌──
 *           └─┘ └─┘ └─┘        └─┘ └─      └─┘ └─
 *      SDA ──╲___╱‾‾‾╲___ ... ___╱‾‾‾╲_______╱‾‾‾‾‾
 *             D7   D6   ...    D0    A(应答)        P
 *           MSB 先
 *
 * **不需要波特率**: 时钟线本身就在采集里, 每个位边界都是一个 SCL 边沿。这也就
 * 是 I2C 在设置屏上一行配置都没有的原因 —— 它没什么可配的。
 *
 * 顺带白送一个好处: **时钟拉伸天然就对**。从机把 SCL 拉住不放的时候, 我们本来
 * 就是在等边沿, 不是在数时间, 拉多久都不影响。
 *
 * 采样点取 SCL 的**上升沿**那一刻就够了: 数据在 SCL 低电平期间就已经建立好,
 * 上升沿时 SDA 一定是这一位的值, 不用再往后等半个周期。
 *
 * 一段 (起始位到停止位之间) 的第一个字节是地址: 高 7 位是地址, 最低位 1 = 读。
 * 重复起始之后又是一个新的地址字节 —— 这里是这么处理的: 每个起始/停止位都把
 * 状态整个复位, 所以重复起始自然就落在"新的一段"上。
 * ========================================================================= */

/**
*   @brief 在跳变表上解 I2C (CH1 = SCL, CH2 = SDA)
*
*   @param recs / nrec  记录表和条数 (调用方保证采集已停, 表是冻结的)
*   @param out / cap    输出数组和容量
*   @param n_gap        [out] 打了 GAP 标记的条目数
*   @param n_trunc      [out] 因为数组满了没存下来的字节数
*   @return 存进 out 的字节数 (一个字节一条, 地址也是其中一条)
*/
static uint32_t i2c_decode(const log_rec_t *recs, uint32_t nrec,
                           log_ana_item_t *out, uint32_t cap,
                           uint32_t *n_gap, uint32_t *n_trunc)
{
    /* 接线约定: I2C 用 CH1 = SCL, CH2 = SDA (见 Logic_SettingView.cpp 的通道表)。 */
    const uint16_t scl_msk = (uint16_t)(1u << s_ch_bit[0]);
    const uint16_t sda_msk = (uint16_t)(1u << s_ch_bit[1]);

    uint32_t out_n   = 0u;
    uint32_t gap_n   = 0u;
    uint32_t trunc_n = 0u;

    /* ---- 一段 (起始位~停止位) 内部的状态 ---- */
    uint32_t in_msg  = 0u;      /* 在不在两帧之间: 没有起始位就不认任何位 */
    uint32_t addr7   = LOG_ANA_I2C_NOADDR;
    uint32_t is_addr = 1u;      /* 这一段的第一个字节是地址 */
    uint32_t bitcnt  = 0u;      /* 这个字节已经收了几位 (0..7), 8 = 等下一位应答 */
    uint32_t byte    = 0u;
    uint32_t byte_ts = 0u;      /* 这个字节第一位所在的采样序号 */
    uint32_t saw_gap = 0u;      /* 这一段里撞见过丢帧区间 */

    /* 从 1 开始: 记录 0 是"合上开关那一刻的电平", 不是跳变。 */
    uint32_t i = 1u;
    while (i < nrec) {
        const uint16_t cur  = recs[i].smp;
        const uint16_t prev = recs[i - 1u].smp;

        const uint32_t scl     = (cur  & scl_msk) != 0u;
        const uint32_t sda     = (cur  & sda_msk) != 0u;
        const uint32_t scl_pre = (prev & scl_msk) != 0u;
        const uint32_t sda_pre = (prev & sda_msk) != 0u;

        /* ---- SCL 高的时候 SDA 变 = 起始位或者停止位 ----
         *
         * 这一条要排在最前面: 它和下面的"SCL 上升沿"是互斥的 (这里 SCL 一直是
         * 高, 没有上升沿), 但排前面读起来顺 —— 帧界优先于位。
         *
         * 两个都当消息边界处理, 状态整个复位。起始位 (SDA 下降) 不用特别记住 ——
         * 复位之后下面就开始收新的地址字节了。 */
        if (scl != 0u && scl_pre != 0u && sda != sda_pre) {
            /* SDA 下降 = 起始位, SDA 上升 = 停止位。只有起始位之后才有字节可收。 */
            in_msg  = (sda_pre != 0u && sda == 0u) ? 1u : 0u;
            addr7   = LOG_ANA_I2C_NOADDR;
            is_addr = 1u;
            bitcnt  = 0u;
            byte    = 0u;

            /* 丢帧标记说的是"**这一条之前**有样本没被扫过", 而起始位自己是一个
             * 位置精确的真实边沿 —— 它之后的内容是可信的, 所以这里从 0 重新起算,
             * 不把起始位之前那段缺口算到这一段头上。 */
            saw_gap = 0u;
            i++;
            continue;
        }

        /* 段内撞上丢帧区间: 后面的位位置全不可信了, 但还是接着解、给内容打标记
         * ——"这里有问题"比"这里凭空少一段"有用。 */
        if (in_msg != 0u && (recs[i].flags & LOG_REC_GAP) != 0u) {
            saw_gap = 1u;
        }

        /* ---- SCL 上升沿 = 收一位 ---- */
        if (in_msg != 0u && scl != 0u && scl_pre == 0u) {
            if (bitcnt < 8u) {
                if (bitcnt == 0u) {
                    byte_ts = recs[i].ts;   /* 一个字节从它的第一位开始算 */
                }
                byte = (byte << 1) | sda;   /* MSB 先 (UART 是 LSB 先, 别抄错) */
                bitcnt++;
            } else {
                /* 第 9 个上升沿 = 应答位: 0 = ACK, 1 = NACK。
                 * 到这儿一个字节才算完整 —— 应答位是跟着这个字节走的, 所以要等
                 * 到看见它才把字节交出去。 */
                uint8_t flags = 0u;
                if (saw_gap != 0u) { flags |= LOG_ANA_IT_GAP; }
                if (sda != 0u)     { flags |= LOG_ANA_IT_I2C_NACK; }
                if (is_addr != 0u) {
                    flags |= LOG_ANA_IT_I2C_ADDR;
                    if ((byte & 1u) != 0u) {
                        flags |= LOG_ANA_IT_I2C_RD;
                    }
                    addr7 = (byte >> 1) & 0x7Fu;
                }

                if (out_n < cap) {
                    log_ana_item_t *it = &out[out_n];
                    out_n++;

                    it->ts    = byte_ts;
                    it->proto = LOG_ANA_I2C;
                    it->flags = flags;
                    it->len   = 1u;
                    /* 地址字节自己的 aux 就是它代表的地址; 数据字节的 aux 是它
                     * 归属的那个地址 —— 显示时按地址分行的依据。 */
                    it->aux   = (uint8_t)addr7;
                    it->data[0] = (uint8_t)byte;

                    /* 整片清掉: 现在只用 data[0], 但和 uart_decode 那边一样,
                     * 清干净之后以后加协议时不用回头想"剩下几个字节脏不脏"。 */
                    for (uint32_t k = 1u; k < LOG_ANA_ITEM_MAX; k++) {
                        it->data[k] = 0u;
                    }
                } else {
                    trunc_n++;
                }

                if (saw_gap != 0u) {
                    gap_n++;
                }

                bitcnt  = 0u;
                byte    = 0u;
                is_addr = 0u;       /* 一段里只有第一个字节是地址 */
            }
        }

        i++;
    }

    if (n_gap   != 0) { *n_gap   = gap_n; }
    if (n_trunc != 0) { *n_trunc = trunc_n; }
    return out_n;
}

/* ===========================================================================
 * 采样率
 * ========================================================================= */

/**
*   @brief 当前采样率 (Hz)
*
*   @note  必须用 CM4 实测发布的 LOG_CTRL->fs_hz, 不能用标称值: DMA 漏请求时
*          实际采样率会低于 240e6/(ARR+1), 拿标称值算位宽会一路偏下去, 一帧
*          十几个位之后就解错了。
*
*          fs_hz 还是 0 (CM4 在这一次上电里还没跑过一轮采集) 时才退到标称值。
*          返回 0 = 拿不到, 调用方放弃这一轮。
*/
static uint32_t decode_fs_hz(void)
{
    uint32_t fs = LOG_CTRL->fs_hz;

    if (fs == 0u) {
        fs = LOG_TIM2_CLK_HZ / (LOG_SAMPLE_ARR_DEFAULT + 1u);   /* 240e6 / 12 = 20 MHz */
    }
    return fs;
}

/* ===========================================================================
 * 公开接口
 * ========================================================================= */

void LOG_Analyze_Init(void)
{
    s_count = 0u;

    /* 默认值必须和设置屏那边**开机时显示的那一份**一模一样, 否则会出现
     * "界面写着 7 位、解码按 8 位算"这种没人查得出来的错。
     *
     * 设置屏开机是"候选值列表里第 0 个" (Model 里 logicCfgVal 全清 0), 也就是
     * VAL_UART_BAUD[0] / VAL_UART_STOP[0] / VAL_UART_DATA[0] / VAL_UART_PARITY[0]
     * —— 注意 VAL_UART_DATA 是 { T_VAL7, T_VAL8 }, 所以默认是 **7 位数据**。
     * 想改成 8N1 的话改设置屏那张表的顺序 (Logic_SettingView.cpp), 这里跟着改。 */
    s_cfg.proto    = LOG_ANA_UART;
    s_cfg.rsvd[0]  = 0u;
    s_cfg.rsvd[1]  = 0u;
    s_cfg.rsvd[2]  = 0u;

    s_cfg.u.uart.baud      = 9600u;
    s_cfg.u.uart.data_bits = 7u;
    s_cfg.u.uart.stop_x10  = 10u;
    s_cfg.u.uart.parity    = LOG_UART_PARITY_NONE;
    s_cfg.u.uart.rx_ch     = 0u;    /* 接线约定: UART 的 RX 进 CH1 */
}

void LOG_Analyze_SetConfig(const log_ana_cfg_t *cfg)
{
    /* 越界就整个丢掉, 配置保持原样 —— 不半途改一半。 */
    if (cfg == 0 || cfg->proto >= LOG_ANA_PROTO_COUNT) {
        return;
    }
    s_cfg = *cfg;
}

void LOG_Analyze_GetConfig(log_ana_cfg_t *out)
{
    if (out != 0) {
        *out = s_cfg;
    }
}

uint8_t LOG_Analyze_Ready(void)
{
    /* 判据就是 GetRecs 那三个前提 (采集停了 + 记录表冻结 + 真有跳变可解), 所以
     * 直接问它 —— 不另外抄一遍条件, 抄了迟早会和 Run 里那句跑偏。
     * 三个出参都不看, 传 0 进去, 它自己会判空。 */
    return LOG_Trace_GetRecs(0, 0, 0);
}

uint32_t LOG_Analyze_Run(void)
{
    const uint32_t t0 = LOG_ANA_CYC;

    /* 先全部清掉: 这一轮解不出东西的话, 结果就该是空的 —— 留着上一次的会让
     * 用户以为"这一轮也解出来了"。 */
    s_count = 0u;
    log_ana_diag_items = 0u;
    log_ana_diag_gap   = 0u;
    log_ana_diag_trunc = 0u;

    const log_rec_t *recs = 0;
    uint32_t nrec = 0u;

    /* GetRecs 把三个前提一起判了: 采集停了 + 记录表冻结 + 真的有跳变
     * (只有记录 0 那条起点电平的话, 没有任何可解的东西)。 */
    if (LOG_Trace_GetRecs(&recs, &nrec, 0) != 0u) {
        uint32_t n_gap   = 0u;
        uint32_t n_trunc = 0u;

        if (s_cfg.proto == LOG_ANA_UART) {
            /* UART 是异步的 (线上没时钟), 位宽得自己算 —— 那是唯一要采样率的
             * 一路, 所以 fs_hz 也在这一支里取。 */
            const uint32_t fs_hz = decode_fs_hz();

            if (fs_hz != 0u) {
                s_count = uart_decode(recs, nrec, fs_hz, &s_cfg.u.uart,
                                      s_items, LOG_ANA_MAX_ITEMS, &n_gap, &n_trunc);
            }
        } else if (s_cfg.proto == LOG_ANA_I2C) {
            /* I2C 是同步的 —— 时钟线就在采集里, 用不上采样率, 也没有可配项。 */
            s_count = i2c_decode(recs, nrec, s_items, LOG_ANA_MAX_ITEMS,
                                 &n_gap, &n_trunc);
        }
        /* SPI / CAN 还没实现 —— 解出 0 条, 界面就是空的。加协议时在这里加一个
         * 分支, 其余通路 (Model / View / 格式化) 基本不用动。 */

        log_ana_diag_gap   = n_gap;
        log_ana_diag_trunc = n_trunc;
    }

    log_ana_diag_items = s_count;
    log_ana_diag_cyc   = LOG_ANA_CYC - t0;
    return s_count;
}

const log_ana_item_t *LOG_Analyze_GetItems(uint32_t *count)
{
    if (count != 0) {
        *count = s_count;
    }
    return s_items;
}

void LOG_Analyze_Clear(void)
{
    s_count = 0u;
}
