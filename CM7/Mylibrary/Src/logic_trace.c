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

/* ---- 显示窗口 (时基) ----
 *
 * 一屏 520 列铺在 [0, span] 这段采样区间上 (闭区间, 所以是 span+1 个采样点):
 * span 越大 = 时基越慢 = 波形越"挤", span 越小 = 越放大。窗口起点恒为 0。
 *
 * 这张表是"一屏覆盖多少个采样点", 从粗到细排, 1-2-5 序列。档位 0 是"整屏",
 * **不查表** —— 它等于这次采集实际覆盖的长度 s_last_ts, 是个变数: 边沿密的
 * 采集可能只有几万个采样点 (记录区先满), 空闲超时停的能到 2000 万。
 *
 * 时间换算 @20 MSa/s: 519 ≈ 26 µs, 1000 = 50 µs, 20000 = 1 ms, 2000000 = 100 ms。
 * 以后要在界面上显示成 µs/格, 拿这里的 span 除以 LOG_CTRL->fs_hz (CM4 实测
 * 发布的那个, 不是标称的 20 MHz)。
 *
 * 下面这三个量只被屏幕任务 (Model::tick) 读写, 半帧中断完全不碰 —— 所以不用
 * volatile, 也不用和 s_build_seq 那一套打交道。 */
static const uint32_t s_span_tab[] = {
    20000000u,          /*    1 s   */
    10000000u,          /*  500 ms  */
     5000000u,          /*  250 ms  */
     2000000u,          /*  100 ms  */
     1000000u,          /*   50 ms  */
      500000u,          /*   25 ms  */
      200000u,          /*   10 ms  */
      100000u,          /*    5 ms  */
       50000u,          /*  2.5 ms  */
       20000u,          /*    1 ms  */
       10000u,          /*  500 µs  */
        5000u,          /*  250 µs  */
        2000u,          /*  100 µs  */
        1000u,          /*   50 µs  */
   LOG_TRACE_SPAN_MIN,  /*   26 µs  <- 最细, 一列正好一个采样点 */
};

#define LOG_SPAN_COUNT  ((uint32_t)(sizeof(s_span_tab) / sizeof(s_span_tab[0])))
#define LOG_SPAN_FULL   0u      /* 档位 0: 整屏 (= s_last_ts) */

/* 滚动模式下转一格挪多少: 一屏的 1/LOG_PAN_DIV。
 *
 * 别设成 1 (一格一整屏): 那样慢转也没法定到屏幕中间的位置。而"转得快就挪得远"
 * 本来就有了 —— Encoder_Increment 给的是净格数, 步长乘它, 快转一帧能翻好几屏。
 * 所以调粗这个数只会把最快那一档也一起拖慢, 换不到任何好处。4 是"慢转能定到
 * 1/4 屏、快转一帧翻好几屏"的折中。 */
#define LOG_PAN_DIV  4u

static uint32_t s_span_idx;     /* 当前档位: 0 = 整屏, k >= 1 = s_span_tab[k-1] */
static uint32_t s_view_dirty;   /* 档位 / 窗口起点变过了, 等 Poll 重算一屏 */

/* 窗口起点 (采样序号): 窗口是 [s_view_start, s_view_start + view_span()]。
 * 0 = 采集开头 —— 也就是加滚动之前那个"左对齐"的行为。缩放不动它, 只有滚动
 * 模式动它, 所以不开滚动时它恒为 0。
 *
 * 不变式: s_view_start <= view_start_max(), 即窗口右沿不越过 s_last_ts。三个
 * 地方维持它 —— Start / Clear 清成 0, view_pan 自己夹一次, view_zoom 缩小时再
 * 夹一次 (span 变大, 原来合法的起点可能就越界了)。 */
static uint32_t s_view_start;

/* 视图模式 (LOG_VIEW_*)。和上面那几个一样, 只被屏幕任务读写, 中断完全不碰。 */
static uint32_t s_view_mode;

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
 * 显示窗口 (时基) —— 见文件上方 s_span_tab 那一段
 * ========================================================================= */

/**
*   @brief 当前一屏覆盖多少个采样点
*
*   @note  整屏档直接返回 s_last_ts (可能是 0 —— 只有记录 0 的那种采集, 交给
*          log_build_cols 里那个三元判断处理)。其余档返回查表值和 s_last_ts
*          中**较小**的那个: 表里比这次采集还长的档会退化成整屏, 而不是去画
*          一段根本不存在的采样区间。正常路径下 view_zoom 已经把档位夹在有
*          意义的范围里了, 这里的 min 是兜底 —— s_last_ts 是中断写的, 多一道
*          保险比省一个比较划算。
*/
static uint32_t view_span(void)
{
    if (s_span_idx == LOG_SPAN_FULL) {
        return s_last_ts;
    }

    const uint32_t s = s_span_tab[s_span_idx - 1u];
    return (s < s_last_ts) ? s : s_last_ts;
}

/**
*   @brief 窗口起点能取到的最大值
*
*   @note  窗口右沿 (起点 + span) 不越过数据末尾 s_last_ts —— 越过去右边就是
*          一段根本没采过的空白, 画出来是骗人的。
*
*          整屏档下 span == s_last_ts, 这个值恒为 0, 也就是**整屏档滚不动**:
*          全都在屏幕上了, 本来也没得滚。采集太短 (s_last_ts <= span) 同理。
*
*          s_last_ts 是中断写的, 这里每次都重新读、用 min 兜底, 和 view_span()
*          一样的路子 —— 多一个比较比省它划算。
*/
static uint32_t view_start_max(void)
{
    const uint32_t span = view_span();
    return (s_last_ts > span) ? (s_last_ts - span) : 0u;
}

/**
*   @brief 把窗口起点夹回合法范围
*
*   @note  给缩放用的: 缩小时 span 变大, 原来合法的起点可能越过新的右界 ——
*          夹回来就是"窗口整体滑到采集末尾贴住"。平移 (view_pan) 自己会夹,
*          不用再走这儿。
*/
static void view_start_clamp(void)
{
    const uint32_t hi = view_start_max();
    if (s_view_start > hi) {
        s_view_start = hi;
    }
}

/**
*   @brief 最粗的那个"真档"的档位号
*
*   @note  表里排在它前面的档都比这次采集还长, 选中了 view_span() 会夹回整屏,
*          表现为"转好几格画面不动"。所以从整屏转进第一档时直接跳到这儿, 保证
*          每一格都看得见变化。
*
*          表是从粗到细排的, 所以"第一条比 s_last_ts 短的"就是最粗的有用档。
*          循环走完说明连表里最细的那条 (LOG_TRACE_SPAN_MIN) 都不比 s_last_ts
*          短 —— 也就是这次采集连一屏都铺不满, 压根没得放大。
*
*   @return 档位号 (>= 1); LOG_SPAN_FULL 表示这次采集太短, 没有更细的档。
*/
static uint32_t span_idx_lo(void)
{
    for (uint32_t i = 0u; i < LOG_SPAN_COUNT; i++) {
        if (s_span_tab[i] < s_last_ts) {
            return i + 1u;
        }
    }
    return LOG_SPAN_FULL;
}

/**
*   @brief 时基模式: 转一格走一个档位
*
*   @note  两头的边界不一样, 是故意的: 最细的一头停在表尾 (LOG_SPAN_COUNT),
*          最粗的一头停在"整屏" (LOG_SPAN_FULL) —— 整屏不是一个表项, 它是变数
*          s_last_ts, 永远比表里最粗那条更长。所以从 lo 再往外转一格是"退回
*          整屏", 而不是停在 lo 不动。
*
*          起点 t0 不动 (缩放的锚点是左沿): 放大时 span 变小, 右沿自己往里收;
*          缩小时 span 变大, t0+span 可能越过 s_last_ts, 夹一次。
*/
static void view_zoom(int delta)
{
    const uint32_t lo = span_idx_lo();
    if (lo == LOG_SPAN_FULL) {
        return;         /* 这次采集连一屏都铺不满, 没有更细的档可进 */
    }

    /* ---- 一格一格走 ---- */
    uint32_t idx = s_span_idx;
    while (delta > 0 && idx != LOG_SPAN_COUNT) {
        idx = (idx == LOG_SPAN_FULL) ? lo : idx + 1u;
        delta--;
    }
    while (delta < 0 && idx != LOG_SPAN_FULL) {
        idx = (idx > lo) ? idx - 1u : LOG_SPAN_FULL;
        delta++;
    }

    if (idx == s_span_idx) {
        return;         /* 已经顶着上界或下界, 别白白重算一屏 */
    }

    s_span_idx = idx;
    view_start_clamp();     /* 缩小之后原来的起点可能越界, 夹回来 */
    s_view_dirty = 1u;
}

/**
*   @brief 滚动模式: 把窗口前后挪
*
*   @note  一格挪 1/LOG_PAN_DIV 屏, **乘上净格数** —— 转得快就挪得远, 加速是
*          白来的, 不用另做一套。
*
*          delta 正 = 顺时针 = 往**更晚**的数据走 (画面往左滚), 和"顺时针 =
*          放大/前进"的直觉一致。
*
*          两头停住, 不回绕: 夹在 [0, view_start_max()] 里。顶着界的时候直接
*          返回, 免得白重算一屏 —— 顺带也让"整屏档滚不动"表现为干干净净的
*          无操作 (那里 view_start_max() 恒为 0)。
*/
static void view_pan(int delta)
{
    const uint32_t span = view_span();

    uint32_t step = span / LOG_PAN_DIV;
    if (step == 0u) {
        /* span 比 LOG_PAN_DIV 还小。有 LOG_TRACE_SPAN_MIN (= 519) 兜着, 实际
         * 走不到这儿; 留一道免得哪天表尾改了变成"转半天不动一格"。 */
        step = 1u;
    }

    /* 64 位中间量: step 能到 5e6 (span 20e6 / 4), 净格数理论上能到 9999
     * (ENCODER_COUNT_MAX), 乘出来 5e10, 32 位装不下。 */
    const int64_t hi = (int64_t)view_start_max();
    int64_t       t  = (int64_t)s_view_start + (int64_t)delta * (int64_t)step;

    if (t < 0)  t = 0;
    if (t > hi) t = hi;

    if ((uint32_t)t == s_view_start) {
        return;         /* 顶着界了, 别白白重算一屏 */
    }

    s_view_start = (uint32_t)t;
    s_view_dirty = 1u;
}

/* ===========================================================================
 * 记录 → 列 (docs/log_plan.md §8.7)
 * ========================================================================= */

/**
*   @brief 二分找"窗口左沿之前最后一条记录"的下标 (严格的 ts < t)
*
*   @param t 窗口起点 (采样序号)
*   @param n 记录条数 (>= 1, 调用点保证)
*
*   @note  窗口能滑动之后, "左沿那一刻的电平"不再等于记录 0 的电平了 —— 得从
*          左沿**之前**的最后一条记录上取: 记录只记"变化之后的电平", 那条记录
*          的电平从它之后一直保持到下一个跳变, 正好就是左沿该画的那个值。
*
*          **严格小于, 不是 "<= "**, 这一位差是故意的: ts 正好等于 t 的记录留在
*          窗口里, 由下面那个列循环当成"第 0 列里发生的变化"吃掉。写成 <= 的话
*          这些记录会被当成左沿的起点电平, 那一列的竖条就没了 —— t = 0 时它正好
*          就是采集的第一个样本, 那是**回归基准**上本来有的一根竖条。
*
*          ts 单调不减 (半帧序号 × 8192 + 半帧内偏移, 只在跳变时写), 所以能二分。
*          同一时刻会有多条记录 (记录 0 的 ts 是 0, 第一个半帧里第一个样本的
*          电平如果和它不同, 那条的 ts 也是 0), 所以找的是"最后一个满足的"。
*
*          n 能到 65536, 16 步, 比整片扫描便宜得多。
*
*   @return 下标 (0 <= 返回值 < n)。记录 0 的 ts 恒为 0, 所以 t = 0 (没得往前
*           退) 时返回 0 —— 那正是"窗口左沿 = 记录 0"的老行为。
*/
static uint32_t rec_before(uint32_t t, uint32_t n)
{
    /* 记录 0 的 ts 是 0, 所以 t = 0 时它就已经不在窗口左边了 —— 拿它自己当起点,
     * 后面那些 ts = 0 的记录由列循环处理。这也是不走二分的原因: 二分要求
     * 至少有一个 ts < t 的元素兜底。 */
    if (LOG_REC_BUF[0].ts >= t) {
        return 0u;
    }

    uint32_t lo = 0u;               /* 不变式: LOG_REC_BUF[lo].ts < t */
    uint32_t hi = n - 1u;

    while (lo < hi) {
        /* mid 上取整: 要有 lo = mid 也能往前推进的分支, 否则 lo == hi-1 时
         * (lo+hi)/2 == lo, 死循环。 */
        const uint32_t mid = lo + (hi - lo + 1u) / 2u;

        if (LOG_REC_BUF[mid].ts < t) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }

    return lo;
}

static void log_build_cols(void)
{
    const uint32_t n = s_rec_count;

    log_diag_recs    = n;
    log_diag_ts_span = s_last_ts;

    s_ncols = 0u;
    if (n == 0u) {
        return;     /* 一条都没写上 (刚开就停), 什么都不画 */
    }

    /* 窗口长度和起点 —— 两个自由度, 见文件上方 s_view_start 那一段。
     *
     * 整屏档 + 起点 0 时 ts1 == s_last_ts —— 和加时基之前逐像素一样 (回归基准)。
     * 放大之后 ts1 < s_last_ts, 窗口外那些记录因为 ts 落在 [t0, t0+ts1] 之外,
     * 下面那个 while 循环不会吃它们, 自然就不会被画出来。 */
    const uint32_t ts1 = view_span();
    const uint32_t t0  = s_view_start;

    /* 窗口左沿的电平 = 左沿**之前**最后一条记录的电平 (见 rec_before)。
     * ri 从它的下一条开始 —— 前面那些全在窗口左边, 不吃。
     *
     * 起点为 0 时这就是老代码那句 "记录 0 的电平就是整屏的起点": rec_before(0)
     * 返回 0, ri 从 1 开始, 一模一样。 */
    uint32_t ri = rec_before(t0, n);

    uint16_t lvl  = LOG_REC_BUF[ri].smp;
    uint16_t prev = lvl;
    uint16_t chg  = 0u;
    ri++;

    for (uint32_t c = 0u; c < LOG_TRACE_COLS; c++) {
        /* ts_c 用 64 位中间量: ts1 可以到 20e6 量级, 乘 520 在 32 位上会溢出。
         * 加 t0 不会溢出 —— view_pan / view_start_clamp 保证了
         * t0 + ts1 <= s_last_ts, 而 s_last_ts 是 32 位的采样序号。 */
        const uint32_t ts_c = (ts1 == 0u)
                            ? t0
                            : t0 + (uint32_t)(((uint64_t)ts1 * (uint64_t)c)
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
    s_span_idx       = LOG_SPAN_FULL;
    s_view_dirty     = 0u;
    s_view_start     = 0u;
    s_view_mode      = LOG_VIEW_TIMEBASE;

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

    /* 新的一轮从整屏开始 —— 上一轮放大到的档位对这一次的 s_last_ts 未必有意
     * 义 (可能比这次采集还长), 留着的话开局就是一张莫名其妙的画面。
     * s_view_dirty 一起清: 上一轮欠着的那次重算不该落到新采集头上。
     * 窗口起点同理 —— 上一轮滚到哪儿对这一次毫无意义 (s_last_ts 换了个数,
     * 不定式 s_view_start <= view_start_max() 都不一定还成立)。
     *
     * **视图模式不清**: 那是用户的偏好, 不是这一轮采集的状态。上一轮在滚动
     * 模式, 这一轮接着在滚动模式。 */
    s_span_idx       = LOG_SPAN_FULL;
    s_view_dirty     = 0u;
    s_view_start     = 0u;

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

    /* 时基和窗口起点也回到整屏/开头 —— 和 LOG_Trace_Start 那一批保持一致。
     * Cancel 之后屏幕上是空的, 再下次采集是从零开始的一轮, 档位和滚动位置都
     * 没有理由留着上一次的。
     *
     * 视图模式同样不清 (理由见 LOG_Trace_Start)。 */
    s_span_idx       = LOG_SPAN_FULL;
    s_view_dirty     = 0u;
    s_view_start     = 0u;
}

int LOG_Trace_IsRunning(void)
{
    return s_running != 0u;
}

uint32_t LOG_Trace_GetViewMode(void)
{
    return s_view_mode;
}

void LOG_Trace_ToggleViewMode(void)
{
    /* 只有两个值, 不用夹: 进来的不是 SCROLL 就是 TIMEBASE。 */
    s_view_mode = (s_view_mode == LOG_VIEW_SCROLL) ? LOG_VIEW_TIMEBASE
                                                   : LOG_VIEW_SCROLL;
}

void LOG_Trace_Turn(int delta)
{
    /* 采集中不响应。两条理由, 头文件里写了 —— 记录区正被中断追加, 而且屏幕上
     * 那一屏本来就是上一次采完的旧图。
     *
     * 注意这个 return 不代表"这一格白转了": Encoder_Increment 已经把增量取走
     * 了 (last_count 更新过), 所以不会攒着等采集结束再一次性爆发。 */
    if (delta == 0 || s_running != 0u) {
        return;
    }

    if (s_view_mode == LOG_VIEW_SCROLL) {
        view_pan(delta);
    } else {
        view_zoom(delta);
    }
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
     * 界面就永远停在采集开始那一刻的空屏上。
     *
     * 第二个触发源是窗口变了 (LOG_Trace_Turn → view_zoom / view_pan, 不管是
     * 调时基还是滚动)。归约是"读 s_rec_count / s_last_ts / 整片记录区"的活,
     * 而中断随时会进来写它们 —— 之所以一直只有这一个调用点, 就是为了让"什么
     * 时候可以安全地读"只有一个答案。缩放/滚动想重算也不能自己伸手去调
     * log_build_cols, 只能在这里挂个记号, 走同一条路。
     *
     * 两个条件合并成一次归约 (不是两次): 一帧里既采完了又转了编码器的话, 只算
     * 一遍, 而且用的是最新的档位 —— 正是想要的。 */
    if (s_build_seq != s_built_seq || s_view_dirty != 0u) {
        s_built_seq = s_build_seq;
        s_view_dirty = 0u;
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
