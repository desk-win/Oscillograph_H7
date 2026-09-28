/* ===========================================================================
 * CM7 侧逻辑分析仪 —— 读半帧 → 找跳变 → 写记录 → 抽成列
 *
 * 完整设计见 docs/log_plan.md §8。几条要点先写在这儿:
 *
 * ── 记录区为什么不需要 __DMB / 发布序号那一套 ──
 * D1 AXI SRAM (0x24000000) 是 M7 独占的: M7 写、M7 读, 缓存就是唯一真相。
 * MPU 非缓存区域只在"这块内存有本核之外的写者"时才需要, 这里不满足那个
 * 条件 —— 而 CM4 发布的 frame_seq 是另一回事, 那一处要 __DMB (见下面)。
 *
 * ── 时间戳为什么不用 DWT ──
 * M7 是在跳变发生**之后**才看到它的 (DMA 先写进 D2, M7 之后才去读那半帧)。
 * 20 MSa/s、16 KB 半帧下, 从跳变发生到被读到延迟 0～409.6 µs, 且随跳变落在
 * 半帧里的位置而变。在读到的那一刻读 DWT->CYCCNT, 得到的不是跳变的时刻。
 * 所以时间戳用**采样序号** (半帧序号 × 8192 + 半帧内偏移): 零成本、无延迟、
 * 丢帧也照样准。DWT 这次不用在 M7 上, 它归 CM4 用来实测采样率。
 *
 * ── 为什么分两个入口 ──
 * 消费这一侧拆成了两半, 分界就是"有没有硬期限":
 *
 *   LOG_Trace_OnHalfFrame()  中断上下文, CM4 每完成一个半帧叫一次
 *                            扫那一格 + 写记录 + 判满/超时。**有**期限:
 *                            409.6 µs 之后 DMA 就绕回来盖掉这一格了。
 *   LOG_Trace_Poll()         每个 tick 一次 (TouchGFX 的 Model::tick)
 *                            记录 → 520 列 + LED + 启动门。**没有**期限,
 *                            采完才做一次, 而且那两片列数组是给屏幕任务读的。
 *
 * 中断和 tick 之间只传一个单调递增的记号 s_build_seq: 中断里加一, tick 里
 * 发现变了就去归约。中断绝不碰屏幕要读的东西, tick 也绝不碰采集状态机 ——
 * 两边各管一段, 不用互斥。
 * ========================================================================= */

#include "logic_trace.h"
#include "sharedconfig.h"
#include "oscillograph_trace.h"     /* 互斥: OSC_Trace_Stop */
#include "main.h"
#include "stm32h7xx_hal_gpio.h"
#include <stdint.h>

/* 每个 word 里两份 LOG_PIN_MASK: 低半字对一个样本, 高半字对另一个。 */
#define LOG_MASK_REP   (LOG_PIN_MASK | (LOG_PIN_MASK << 16))

#define LOG_REC_BUF    ((log_rec_t *)(uintptr_t)LOG_REC_BUF_ADDR)
#define LOG_SAMPLE_BUF ((const uint32_t *)(uintptr_t)LOG_SAMPLE_BUF_ADDR)

/* 逻辑通道 1..8 → GPIOG 的位号 (LOG_CH_BIT)。这儿用不着 —— 这一层从头到尾都在
 * 原始掩码上干活, "第几个通道"是显示层 (LogicView.cpp) 的事。 */

/* ---- 采集状态 (中断和 tick 都碰) ---- */
static uint32_t s_running;       /* 中断里读, tick 里读: 只增删这一个闸 */
static uint32_t s_consumed;      /* 已经处理到第几个半帧 */
static uint32_t s_rec_count;     /* 已写下的记录条数 */
static uint32_t s_last_ts;       /* 最后一条记录的 ts */
static uint32_t s_gap_pending;   /* 下一次写记录时要打 GAP */
static uint32_t s_full;          /* 记录区满了 */

/* 中断 → tick 的唯一一处通讯: "又有一次采集收工了, 该归约了"。
 *
 * 为什么要这么一个记号, 而不是让 tick 自己看 s_result_pending: 归约要读
 * s_rec_count / s_last_ts / 整个记录区, 那是几百微秒的活, 而中断随时会进来 ——
 * 把"该不该做"和"做的时候数据是不是还在变"混在一起, 就得给归约加锁。用一个
 * 单调记号把两件事分开: 中断只加一 (一条指令), tick 看到变了才去读。
 *
 * volatile: 它被中断改、被主循环读, 编译器不知道中断这回事, 不加的话可能把
 * tick 里那次比较提到整个循环外面。 */
static volatile uint32_t s_build_seq;
static uint32_t s_built_seq;     /* tick 本地: 已经归约到哪一号 */

/* 启动门超时用的时间戳 (HAL_GetTick)。tick 本地。 */
static uint32_t s_start_tick;

/* LED 的实际状态, tick 本地 —— 中断里不碰 LED (见 LOG_Trace_Poll)。 */
static uint32_t s_led_on;

/* 扫描用的"上一个样本":
 *   s_prev16 —— 上一次扫描到的那个样本的电平 (s1[i-1]), 精确判断用
 *   s_prev32 —— 同一个值复制到两个半字, 只给快路径的预筛用 (见 log_scan_half)
 * 采集开始时都填成记录 0 的电平。 */
static uint32_t s_prev16;
static uint32_t s_prev32;

/* ---- 出图用的列数据 ---- */
static uint16_t s_level[LOG_TRACE_COLS];
static uint16_t s_change[LOG_TRACE_COLS];
static uint32_t s_ncols;
static uint32_t s_result_pending;   /* 有算好还没交出去的一屏 */

/* ---- 诊断 ---- */
volatile uint32_t log_diag_gap_recs;
volatile uint32_t log_diag_recs;
volatile uint32_t log_diag_ts_span;

/* ===========================================================================
 * 记录写入 (docs/log_plan.md §8.4)
 * ========================================================================= */
static void log_rec_write(uint32_t ts, uint32_t smp)
{
    if (s_rec_count >= LOG_REC_MAX) {        /* 满 → 停止 */
        LOG_Trace_Stop();
        s_full = 1u;
        return;
    }

    log_rec_t *r = &LOG_REC_BUF[s_rec_count];
    r->ts    = ts;
    r->smp   = (uint16_t)smp;
    r->flags = (uint8_t)((s_gap_pending && s_rec_count) ? LOG_REC_GAP : 0u);
    r->rsvd  = 0u;

    s_rec_count++;
    s_last_ts = ts;
    s_gap_pending = 0u;
}

/* ===========================================================================
 * 扫描一个半帧 (docs/log_plan.md §8.3)
 * ========================================================================= */

/* 慢路径: 这一对样本里确实有跳变时才进来。用精确的"上一个样本"逐个判, 是就
 * 写一条记录 —— 快路径的 any 只是预筛, 它有假阳性 (不会漏, 见下面的推导)。 */
static void log_slow(uint32_t v, uint32_t idx, uint32_t any)
{
    const uint32_t s0 = v & 0xFFFFu;
    const uint32_t s1 = v >> 16;

    if ((any & LOG_PIN_MASK) != 0u &&
        ((s0 ^ s_prev16) & LOG_PIN_MASK) != 0u) {
        log_rec_write(idx, s0 & LOG_PIN_MASK);
    }
    if ((any & (LOG_PIN_MASK << 16)) != 0u &&
        ((s1 ^ s0) & LOG_PIN_MASK) != 0u) {
        log_rec_write(idx + 1u, s1 & LOG_PIN_MASK);
    }
    s_prev16 = s1;
}

/**
*   @brief 扫一个半帧, 把跳变点写成记录
*   @param w         半帧的起点 (uint32 视角, 一个 word 两个样本)
*   @param nwords    这个半帧有多少个 word
*   @param base_index 这个半帧第一个样本的采样序号 (= 半帧序号 × 8192)
*
*   @note 一次 32 位读拿两个样本, 用 SWAR 把"这两个样本里有没有任何通道变过"
*         压成一次判断 —— 绝大多数迭代 (没有跳变) 走最快路径:
*
*             inner = (v ^ (v >> 16)) & 0xFFFF      s0 与 s1 之间的变化
*             inner |= inner << 16                  复制到两个半字 ——
*                                                   s0↔s1 的变化同时属于这两个
*                                                   样本, 只标低半字会漏掉 s1
*             any   = (inner | (v ^ prev)) & MASK_REP
*
*         prev 是**上一个样本**复制到两个半字, 不是上一个 word。这一点容易写
*         错, 写成"上一个 word"的话:
*             低半字算出来是 s0[i] ^ s0[i-1]  —— 中间隔着一个 s1[i-1], 那不是
*                                                  相邻两拍, 也就不是"边界变化"
*         于是 s1[i-1]=0, s0[i]=1, s0[i-1]=1, s1[i]=1 这种一拍的窄脉冲会被
*         整条漏掉 (边界上那次跳变连记录都不会有)。用"上一个样本"就不一样了:
*             低半字 = s0[i] ^ s1[i-1]  —— 正是要判的那个边界变化
*             高半字 = s1[i] ^ s1[i-1]  —— 有假阳性, 但不会漏
*         严格说高半字那项是多余的 (inner 已经覆盖了 s0[i]^s1[i]); 留着是
*         为了让快路径的推导简单, 代价只是偶尔多走一次慢路径, 慢路径自己会
*         用精确条件把它筛掉。
*
*         快路径 8 条指令 / 2 个样本。
*/
static void log_scan_half(const uint32_t *w, uint32_t nwords, uint32_t base_index)
{
    uint32_t prev = s_prev32;
    uint32_t idx  = base_index;

    for (uint32_t i = 0u; i < nwords; i++) {
        /* 记录区满了就没必要再扫 —— log_rec_write 会立刻返回, 但它每次都要
         * 判一次满、而且第一下就调了 LOG_Trace_Stop()。密集信号下一个半帧能
         * 有几十万次调用, 不在这里拦一道的话, 那半帧的时间全花在空转上。 */
        if (s_full != 0u) {
            break;
        }

        const uint32_t v     = w[i];                        /* s0 = 低半字, s1 = 高半字 */
        uint32_t       inner = (v ^ (v >> 16)) & 0xFFFFu;   /* s0 vs s1 */
        inner |= inner << 16;                               /* 复制到两个半字 */

        const uint32_t any = (inner | (v ^ prev)) & LOG_MASK_REP;

        if (any != 0u) {
            log_slow(v, idx, any);
        }

        prev = v >> 16;                 /* 上一个样本 = 这个 word 的第二个样本 */
        prev |= prev << 16;             /* 复制到两个半字 */
        idx += 2u;
    }

    s_prev32 = prev;
}

/* ===========================================================================
 * 记录 → 列 (docs/log_plan.md §8.7)
 * ========================================================================= */
static void log_build_cols(void)
{
    const uint32_t n = s_rec_count;

    log_diag_recs    = n;
    log_diag_ts_span = s_last_ts;

    s_ncols = 0u;
    if (n == 0u) {
        return;     /* 一条都没写上 (刚开就停), 什么都不画 */
    }

    /* 记录 0 的电平就是整屏的起点 */
    uint16_t lvl  = LOG_REC_BUF[0].smp;
    uint16_t prev = lvl;
    uint16_t chg  = 0u;

    /* 记录 0 的 ts 恒为 0, 所以 s_last_ts 就是这次采集覆盖的采样区间长度 */
    const uint32_t ts1 = s_last_ts;
    uint32_t ri = 1u;                   /* 从记录 1 开始 */

    for (uint32_t c = 0u; c < LOG_TRACE_COLS; c++) {
        /* ts_c 用 64 位中间量: ts1 可以到 20e6 量级, 乘 520 在 32 位上会溢出 */
        const uint32_t ts_c = (ts1 == 0u)
                            ? 0u
                            : (uint32_t)(((uint64_t)ts1 * (uint64_t)c)
                                         / (uint64_t)(LOG_TRACE_COLS - 1u));

        /* 把落进这一列的所有记录都吃掉: 这一列末尾的电平 = 最后一条的电平,
         * 这一列里跳过的通道 = 所有变化的并集。这样窄脉冲 (一列里翻好几个
         * 来回) 不会丢 —— 它至少在 change 里留下一条竖条。 */
        while (ri < n && LOG_REC_BUF[ri].ts <= ts_c) {
            const uint16_t smp = LOG_REC_BUF[ri].smp;
            chg |= (uint16_t)(smp ^ prev);
            prev = smp;
            lvl  = smp;
            ri++;
        }

        s_level[c]  = lvl;
        s_change[c] = chg;
        chg = 0u;
    }

    s_ncols = LOG_TRACE_COLS;
    s_result_pending = 1u;
}

/* ===========================================================================
 * LED (M7_LED = PA3)
 *
 * 用 M7 的 LED, 不用 M4_LED: "有没有数据输入"只有 M7 知道 (它做检测), 跨核
 * 传给 M4 只会多一层延迟 (docs/log_plan.md §8.5)。
 *
 * 低有效 —— CubeMX 在 MX_GPIO_Init 里把它置成 SET (灭)。要改极性只动这两个
 * 小函数。
 * ========================================================================= */
static void led_on(void)
{
    HAL_GPIO_WritePin(M7_LED_GPIO_Port, M7_LED_Pin, GPIO_PIN_RESET);
}

static void led_off(void)
{
    HAL_GPIO_WritePin(M7_LED_GPIO_Port, M7_LED_Pin, GPIO_PIN_SET);
}

/* ===========================================================================
 * 启停
 * ========================================================================= */

void LOG_Trace_Init(void)
{
    s_running        = 0u;
    s_consumed       = 0u;
    s_rec_count      = 0u;
    s_last_ts        = 0u;
    s_gap_pending    = 0u;
    s_full           = 0u;
    s_prev16         = 0u;
    s_prev32         = 0u;
    s_ncols          = 0u;
    s_result_pending = 0u;
    s_build_seq      = 0u;
    s_built_seq      = 0u;
    s_start_tick     = 0u;
    s_led_on         = 0u;

    /* 只碰 CM7 写的三个字段 (见 sharedconfig.h 的写者约定), CM4 那几个留给
     * CM4 的 LOG_Init()。 */
    LOG_CTRL->cfg_sample_arr = LOG_SAMPLE_ARR_DEFAULT;
    LOG_CTRL->cfg_run        = 0u;
    __DMB();

    /* ---- 收下 2 号信号量 ----
     *
     * 和示波器那一路 (M7_OSC_Init 里的 OSC_HSEM_MASK) 是同一台机器: 打开这一位
     * 之后, CM4 那边 take+release 一次就会把 CM7 的 HSEM1_IRQn 拉起来, 中断里
     * 的 HAL_HSEM_FreeCallback 再按掩码分派到这儿的 OnHalfFrame。
     *
     * NVIC 那边不用动 —— HSEM 是**每核一条**中断线, 不是每个信号量一条, 1 号
     * 和 2 号都在 HSEM1_IRQn 上, 已经由 M7_OSC_Init 使能过了。 */
    HAL_HSEM_ActivateNotification(LOG_HSEM_MASK);

    led_off();
}

void LOG_Trace_Start(void)
{
    if (s_running != 0u) {
        return;                 /* 已经在跑, 别把记录冲掉 */
    }

    /* ---- 互斥: 示波器让路 (docs/log_plan.md §7.6) ----
     * 两个屏幕各自有个开关, 切屏不改变对方的状态, 所以不能指望"用户不会同时
     * 开"。CM7 这一层停掉示波器的两路采集, CM4 那一层再把 ADC 硬件停掉。 */
    OSC_Trace_Stop(OSC_TRACE_CH1);
    OSC_Trace_Stop(OSC_TRACE_CH2);

    s_consumed       = 0u;      /* CM4 的 log_start 会把 frame_seq 清零 */
    s_rec_count      = 0u;
    s_last_ts        = 0u;
    s_gap_pending    = 0u;
    s_full           = 0u;
    s_ncols          = 0u;
    s_result_pending = 0u;

    /* 上一屏的记号不用清: s_build_seq 只增不减, tick 那边比的是"变没变"，
     * 清成 0 反而可能和 s_built_seq 撞上而漏掉一次归约。 */
    s_start_tick     = HAL_GetTick();   /* 启动门的计时起点, 见 LOG_Trace_Poll */

    /* ---- 记录 0: 合上开关那一刻的电平, ts = 0 ----
     *
     * 这一条不能省。CM7 不知道"开关合上之前"的信号长什么样, 而后面每一条
     * 记录都只记"变化之后的电平"—— 没有起点的话, 第一个跳变点之前的整段
     * 波形就没有电平可画 (docs/log_plan.md §2.5)。
     *
     * 直接读 GPIOG->IDR: GPIO 是 AHB4 上的外设, 两个核都能读, 时钟是 CM4
     * 在 MX_GPIO_Init 里开的。读到的就是"此刻引脚上的电平"。
     */
    const uint32_t lvl0 = GPIOG->IDR & LOG_PIN_MASK;

    s_prev16 = lvl0;
    s_prev32 = lvl0 | (lvl0 << 16);

    log_rec_t *r0 = &LOG_REC_BUF[0];
    r0->ts    = 0u;
    r0->smp   = (uint16_t)lvl0;
    r0->flags = 0u;                     /* 记录 0 永远不带 GAP */
    r0->rsvd  = 0u;
    s_rec_count = 0u;                   /* log_rec_write 里会 ++, 这里手工写 */
    s_rec_count = 1u;
    s_last_ts   = 0u;

    log_diag_gap_recs = 0u;

    /* ---- 打开 CM4 的采集 ----
     *
     * 顺序: cfg_* 全部写完 → __DMB() → cmd_seq++ → __DMB() → cfg_run = 1。
     * cmd_seq 是给"配置变了好几项"用的 (CM4 不能逐字段轮询, 会读到半更新的
     * 状态); cfg_run 单独放在最后, 它一拍就把整条链路带起来了。 */
    __DMB();
    LOG_CTRL->cfg_sample_arr = LOG_SAMPLE_ARR_DEFAULT;
    __DMB();
    LOG_CTRL->cmd_seq++;
    __DMB();
    LOG_CTRL->cfg_run = 1u;
    __DMB();

    s_running = 1u;
    s_led_on  = 0u;
    led_off();                          /* 还没见过跳变, 先灭着 */
}

/**
*   @brief 停采。可以从**中断里**调 (记录满是在 log_rec_write 里就地停的)
*
*   @note  这里不碰 LED。LED 的驱动权整个归 tick (LOG_Trace_Poll), 中断里再插
*          一手的话两边就是对同一个引脚各写各的 —— 中断在 tick 的"判断"和
*          "写引脚"之间插进来的话, 那一次翻转会被 tick 随后的写入吃掉, 表现是
*          灯偶尔不灭。
*/
void LOG_Trace_Stop(void)
{
    if (s_running == 0u) {
        return;
    }

    /* cfg_run 是 CM4 唯一认的那一个闸 (docs/log_plan.md §7.5): 停采、LOG_EN
     * 拉低、DMA 停, 都在它那边。 */
    LOG_CTRL->cfg_run = 0u;
    __DMB();

    s_running = 0u;
}

void LOG_Trace_Clear(void)
{
    /* 先停。清一个正在被中断写的结构是没有意义的 —— 停了之后 OnHalfFrame
     * 会在开头的 s_running 判断上直接返回。
     *
     * 严格说还有一个几微秒的窗口: 已经越过那个判断、正在 log_scan_half 里的
     * 那一次通知会继续跑完, 可能在我们清完之后又写进一条记录。代价是 s_rec_count
     * 变成 1、s_consumed 被推进 —— 下一个 LOG_Trace_Start 会把这一批全部重清,
     * 所以不用为它加锁。 */
    LOG_Trace_Stop();

    /* 和 LOG_Trace_Start 开头那一批一一对应, 两边必须一起改。 */
    s_consumed    = 0u;
    s_rec_count   = 0u;
    s_last_ts     = 0u;
    s_gap_pending = 0u;
    s_full        = 0u;
    s_prev16      = 0u;
    s_prev32      = 0u;

    /* 已经算好的那一屏也得丢掉。留着的话 tick 会把 s_build_seq 当成"有新结果"
     * 再归约一次, 屏幕刚被 Cancel 清空就又画上旧波形。
     *
     * s_ncols = 0 同时也让 GetCols 交出的列数变成 0 —— View 那边是照这个
     * 决定"要不要擦控件"的 (见 LogicView::setupScreen)。 */
    s_ncols          = 0u;
    s_result_pending = 0u;
}

int LOG_Trace_IsRunning(void)
{
    return s_running != 0u;
}

/* ===========================================================================
 * 中断入口 —— 一个半帧一次
 *
 * 由 CM7 的 HAL_HSEM_FreeCallback 调 (Mylibrary/Src/oscillograph.c), 触发源是
 * CM4 在那个半帧的 HT/TC 里 take+release 的 2 号信号量。
 *
 * **期限**: 这个函数必须在下一个半帧完成之前跑完。半帧 409.6 µs, 整块缓冲绕
 * 一圈 819.2 µs, 慢过它 DMA 就把这 16 KB 盖掉了 —— 那就是真的丢数据 (会记进
 * log_diag_gap_recs)。所以这里只做有期限的那一半: 读 → 找跳变 → 写记录。
 *
 * 归约 (记录 → 520 列) 不在这儿, 见 LOG_Trace_Poll。
 * ========================================================================= */
void LOG_Trace_OnHalfFrame(void)
{
    /* 采集已经停了 (手动停 / 上次自动停), 而通知还在路上 —— 常见, 不是错。
     * 不去清信号量: HAL 在进回调之前已经把 MISR 的位清掉、把 IER 的位关掉了,
     * 重新打开是回调那边的事 (见 oscillograph.c)。 */
    if (s_running == 0u) {
        return;
    }

    /* ---- 等 CM4 真正开起来 ----
     *
     * LOG_Trace_Start 只是把 cfg_run 置 1, CM4 要等它下一圈主循环才看到、才会
     * 去清 frame_seq。在那之前 frame_seq 里是**上一次采集留下来的值**, 拿去当
     * "新数据"会去扫一块上一次的残留缓冲, 写出几条时间戳离谱的假记录 —— 而且
     * 它们的时间戳比后面所有真记录都大, 出图时会挤在最后几列上。
     *
     * state 是 CM4 写的 (它在 log_start 的最后一步才置 RUNNING, 那之前
     * frame_seq 已经清干净了), 拿它当"这边可以开始消费了"的信号正好。
     *
     * 顺带: 万一 CM4 那边 HAL_DMA_Start_IT 失败, state 会停在 IDLE —— 通知一次
     * 都不会来, 这里就一直等着。兜底在 LOG_Trace_Poll 的超时那一段。 */
    if (LOG_CTRL->state != LOG_ST_RUNNING) {
        return;
    }

    const uint32_t fs = LOG_CTRL->frame_seq;    /* CM4 已完成的半帧数 */
    if (fs == s_consumed) {
        return;                                  /* 这一格已经处理过 (重入/重复通知) */
    }

    /* 一次跨了不止一格: 中间那几个半帧已经被 DMA 绕回来覆盖了, 根本没扫过,
     * 里面可能藏着跳变。没有这个标志的话, "漏了一帧"和"这段时间没变化"在数据
     * 上完全一样, 波形会画出一段根本不存在的平线 (docs/log_plan.md §6.2)。 */
    if (fs - s_consumed > 1u) {
        s_gap_pending = 1u;
        log_diag_gap_recs++;
    }
    s_consumed = fs;        /* 立刻推进游标: 只处理最新那一格, 中间的不补扫 */

    /* 刚完成的是哪一格: (fs-1)&1。和 CM4 那边 DMA 的半帧编号同奇偶 —— 那是
     * "读到的一定是刚写完、还没被绕回来覆盖的那一格"的唯一保证。 */
    const uint32_t half = (fs - 1u) & 1u;
    const uint32_t *w   = LOG_SAMPLE_BUF + (half * (LOG_SAMPLE_HALF_BYTES / 4u));

    log_scan_half(w, LOG_SAMPLE_HALF_WORDS, (fs - 1u) * LOG_SAMPLE_HALF_SAMPS);

    /* ---- 记录满 / 空闲超时 → 收工, 并喊 tick 去出图 (docs/log_plan.md §8.6) ----
     *
     * 用减法比较, 跨 32 位回绕自动正确。s_last_ts 是最后一个跳变发生的采样
     * 序号, fs * 8192 是当前已经采到的采样序号。
     *
     * 归约留给 tick 做 (这儿只记一个号): 它是几百微秒的活, 放在中断里做就等于
     * 把中断关住那么久, 而 CM4 那边每 409.6 µs 就来一次。 */
    const uint32_t now_ts = fs * LOG_SAMPLE_HALF_SAMPS;

    if (s_full != 0u || (now_ts - s_last_ts) > LOG_IDLE_TIMEOUT_SAMPS) {
        LOG_Trace_Stop();
        s_build_seq++;
    }
}

/* ===========================================================================
 * tick 入口 —— 每个 tick 都调, 不管采集在不在跑
 *
 * 三件事, 按这个顺序:
 *   1) 中断那边收了工 → 把记录归约成一屏列数据
 *   2) 摆 LED
 *   3) 启动门: 等 CM4 把采集拉起来, 等不到就超时收摊
 *
 * 每一步都很快 (没有归约要做的时候就是几个读和比较), 挂在 60 Hz 的 tick 上
 * 没有负担 —— 真正有期限的那一半已经在中断里做完了。
 * ========================================================================= */
void LOG_Trace_Poll(void)
{
    /* ---- 1) 出图 ----
     *
     * 为什么由这个记号触发而不是"采集中就不管": 采集是在中断里停的 (记录满 /
     * 空闲超时), 停的那一刻 tick 还不知道 —— 得靠这里把最后一屏算出来, 不然
     * 界面就永远停在采集开始那一刻的空屏上。 */
    if (s_build_seq != s_built_seq) {
        s_built_seq = s_build_seq;
        log_build_cols();
    }

    /* ---- 2) LED (docs/log_plan.md §8.5) ----
     * 采集中且至少见过一个跳变 (记录 0 之外还有别的) → 亮; 其余一律灭。
     *
     * 只有状态真的变了才写引脚, 而且只在 tick 里写 —— 中断不碰它。两边都写的话
     * 会出现"中断在 tick 的判断和写引脚之间插进来", 那一次翻转被随后的写入吃掉,
     * 表现是灯偶尔不灭。 */
    const uint32_t led_want = (s_running != 0u && s_rec_count > 1u) ? 1u : 0u;
    if (led_want != s_led_on) {
        s_led_on = led_want;
        if (led_want != 0u) {
            led_on();
        } else {
            led_off();
        }
    }

    /* ---- 3) 启动门 ----
     *
     * 采集停了就到此为止 —— 上面两件事该做还得做 (尤其是出图), 所以这个 return
     * 只能放在最后。 */
    if (s_running == 0u) {
        return;
    }

    if (LOG_CTRL->state != LOG_ST_RUNNING) {
        /* CM4 还没把采集拉起来。正常情况下是几十微秒的事 (它下一圈主循环),
         * 但 HAL_DMA_Start_IT 失败时它会一直停在 IDLE —— 那样开关看着是 ON 的
         * 却没有波形, 界面也没有任何提示。超时之后自己收摊, 开关就会弹回 OFF,
         * 用户一眼看出这次没起来。
         *
         * 用减法比较, 跨 32 位回绕自动正确。 */
        if ((HAL_GetTick() - s_start_tick) > LOG_START_TIMEOUT_MS) {
            LOG_Trace_Stop();
        }
        return;
    }
}

uint8_t LOG_Trace_GetCols(const uint16_t **level, const uint16_t **change,
                          uint32_t *ncols)
{
    if (s_result_pending == 0u) {
        return 0u;
    }

    s_result_pending = 0u;      /* 取走式: 同一屏只交出去一次 */

    if (level  != 0) *level  = s_level;
    if (change != 0) *change = s_change;
    if (ncols  != 0) *ncols  = s_ncols;
    return 1u;
}
