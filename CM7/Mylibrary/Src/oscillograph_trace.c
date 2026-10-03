/* ===========================================================================
 * 屏幕波形取数 —— 从 SDRAM 深存储环里抠一屏
 *
 * 环是 CM4 一直写、CM7 一直搬的, 没有"存满了"这个状态, 所以读的时候必须
 * 保证读到的每一个 word 都已经落定。做法不是加锁, 而是"读旧的":
 *
 *   1) 一屏最多 OSC_TRACE_COLUMNS × 256 = 131072 个 word, 而环有 4194304 个
 *      word, 相差 32 倍。只要一屏的尾巴离写指针还有足够距离, 读的整个过程
 *      里写指针都不可能追上它。 (最慢档之外差得更多, 最快的档差 4096 倍。)
 *   2) 下面所有窗口的取法都保证了 窗口尾巴 <= 写指针, 而且写入是按半帧
 *      为单位完成的, 所以读到的都在已经搬完的半帧里 —— 不会读到搬了一半的
 *      半帧。
 *
 * 参数和对外接口在 oscillograph_trace.h 里。
 * ========================================================================= */

#include "oscillograph_trace.h"
#include "logic_trace.h"        /* 互斥: LOG_Trace_Stop */
#include "oscillograph.h"
#include "sharedconfig.h"

/* 环里一共多少个 word: 2048 个半帧 × 2048 word = 4194304 = 2^22。
 * 是 2 的幂, 所以"绕回"就是按位与, 不用真的做取模。 */
#define OSC_RING_WORDS  (OSC_SDRAM_HALVES * OSC_HALF_WORDS)
#define OSC_RING_MASK   (OSC_RING_WORDS - 1u)

/* 这几条要是破了, 后果是"波形莫名其妙拼错一格", 而且只在环绕回的那一刻出现
 * 一次, 极难查 —— 宁可编译不过。
 *
 * 后三条只按**最大档位** (W = OSC_TB_MAX_WPC) 判就够了: 档位表里 W = 1<<i,
 * i ≤ 7, 而 2048 = 2^11 —— 所有档位的 W 都是最大档的约数, 也都是 2048 的
 * 约数。最大的那个合上了, 小的自动合上。 */
OSC_STATIC_ASSERT((OSC_RING_WORDS & OSC_RING_MASK) == 0u,
                  "环的 word 数必须是 2 的幂, 否则 & 掩码取模不成立");
OSC_STATIC_ASSERT(OSC_TRACE_COLUMNS * OSC_TB_MAX_WPC <= OSC_RING_WORDS,
                  "最慢那一档的一屏比环还长");
OSC_STATIC_ASSERT(OSC_TB_MAX_WPC <= OSC_HALF_WORDS,
                  "最慢那一档的一列跨过了半帧边界");
OSC_STATIC_ASSERT((OSC_HALF_WORDS % OSC_TB_MAX_WPC) == 0u,
                  "半帧的 word 数除不下最大列宽 —— 一列会跨半帧边界");

/* ---- 绝对 word 号 → SDRAM 里的值 (自动绕回) ----
 * 绝对 word 号是"从采集开始算起的第几个 word", 单调递增, 不绕回; 绕回只发生
 * 在换算成地址这一步。这样时间戳永远是线性的, 比较新旧只要减法。 */
#define CH1_WORD(w) (*(const volatile uint32_t *)(OSC_SDRAM_CH1_BASE + (((w) & OSC_RING_MASK) * 4u)))
#define CH2_WORD(w) (*(const volatile uint16_t *)(OSC_SDRAM_CH2_BASE + (((w) & OSC_RING_MASK) * 2u)))

/* ===========================================================================
 * 时基档位表
 *
 * 一屏 = OSC_TRACE_COLUMNS × wpc 个 word, 时间 = 那个数 × OSC_WORD_PERIOD_NS。
 * 档位号就是 1<<i, 所以 wpc 全部是 2 的幂, 也就全部整除半帧 (2048) —— 一列
 * 因此永远不跨半帧边界, "读的每一段都落在已经搬完的半帧里"这条保证不用重证。
 *
 * refresh_div 是重画节流。抽一屏要读 1024×wpc 个 word, 非 cache 的 SDRAM 标量
 * 读约 19 周期/word @480 MHz —— wpc=16 是 650 µs (现状), wpc=256 是 10.4 ms。
 * 后者要是还按 50 ms 重画, 平均就吃掉 21% 的帧时间, 而且那一帧本身会超掉
 * 16.7 ms 的节拍, 画面肉眼可见地顿。所以档位越大节流越狠, 代价是刷新变慢
 * (134 ms 一次) —— 看 42 ms 的波形时这个刷新率够用。
 *
 * span_ns 给界面显示用, 在这儿算好省得界面那边做乘法再拼单位。
 * ========================================================================= */
#define OSC_TB_SPAN_NS(w)   (OSC_TRACE_COLUMNS * (uint32_t)(w) * OSC_WORD_PERIOD_NS)

typedef struct {
    uint16_t wpc;           /* 每列吃几个 word */
    uint16_t refresh_div;   /* 隔几个 tick 重画一次 */
    uint32_t span_ns;       /* 一屏的时长 */
} osc_tb_t;

static const osc_tb_t s_tb[OSC_TB_STEPS] = {
    {   1u, OSC_TRACE_REFRESH_TICKS, OSC_TB_SPAN_NS(  1u) },  /* 163.84 µs */
    {   2u, OSC_TRACE_REFRESH_TICKS, OSC_TB_SPAN_NS(  2u) },  /* 327.68 µs */
    {   4u, OSC_TRACE_REFRESH_TICKS, OSC_TB_SPAN_NS(  4u) },  /* 655.36 µs */
    {   8u, OSC_TRACE_REFRESH_TICKS, OSC_TB_SPAN_NS(  8u) },  /*   1.31 ms */
    {  16u, OSC_TRACE_REFRESH_TICKS, OSC_TB_SPAN_NS( 16u) },  /*   2.62 ms ← 默认 */
    {  32u, OSC_TRACE_REFRESH_TICKS, OSC_TB_SPAN_NS( 32u) },  /*   5.24 ms */
    {  64u, 4u,                      OSC_TB_SPAN_NS( 64u) },  /*  10.49 ms */
    { 128u, 6u,                      OSC_TB_SPAN_NS(128u) },  /*  20.97 ms */
    { 256u, 8u,                      OSC_TB_SPAN_NS(256u) },  /*  41.94 ms */
};

/* 每通道一份档位。默认 OSC_TB_DEFAULT —— 和加这个功能之前的行为完全一致。
 * 只被 TouchGFX 任务碰 (界面里转编码器), 不用加锁。 */
static uint8_t s_tb_idx[2] = { OSC_TB_DEFAULT, OSC_TB_DEFAULT };

/* 当前档位导出的三个量。ch 是 0/1 (OSC_TRACE_CH1/CH2), 调用方保证不越界。 */
static uint32_t wpc_of(uint32_t ch)    { return s_tb[s_tb_idx[ch]].wpc; }
static uint32_t span_words(uint32_t ch){ return OSC_TRACE_COLUMNS * wpc_of(ch); }
static uint32_t pre_words(uint32_t ch) { return OSC_TRACE_TRIG_COL * wpc_of(ch); }
static uint32_t post_words(uint32_t ch){ return span_words(ch) - pre_words(ch); }

/* ---------------------------------------------------------------------------
 * 触发锁存, 每个通道一份
 *
 * 要解决的问题: 一屏 512 列, 触发点在第 128 列, 也就是触发点右边还要 384 列
 * 的"触发后"数据 (默认档 6144 个 word)。触发刚发生的那一刻, 那些数据根本还没
 * 采出来, 所以不能一看到触发就用它 —— 那样画出来的窗口有一大半落在未来。
 *
 * 但也不能"每次都重新看最新的触发": 每次看到的都是最新的那一帧, 它的右边
 * 永远差 3 个半帧, 于是永远不满足条件 —— 死锁, 屏幕上永远是自由运行的波形。
 *
 * 做法: 看到触发先记成"候选", 然后冻住它, 不让后面更新的触发把它顶掉; 等它
 * 右边的数据补齐 (约 2 ms) 再正式采用, 采用之后才空出位置接下一个候选。
 * 两个变量就够, 不需要队列。
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t cand;        /* 候选触发点的绝对 word 号 */
    uint32_t trig;        /* 已采用的触发点的绝对 word 号 */
    uint8_t  cand_held;   /* 候选正冻着等它变老 */
    uint8_t  locked;      /* trig 可用 */
} osc_trig_t;

static osc_trig_t s_trig[2];

/* 每通道一份: 采集是否在跑。整个模块只被 TouchGFX 任务碰, 不用加锁。
 * OSC_TRACE_CH1 / CH2 就是 0 / 1, 直接拿来当下标。 */
static uint8_t s_running[2];

/* 每通道一份: 本次启动时数据从哪个绝对 word 下标开始才算新的。
 *
 * 为什么需要: 停掉一路只是让 CM4 不再扫描 —— 它的 seq_chN 就冻在那儿, 重新
 * 打开是从原位置接着涨的。可环里那个位置上的数据**还是停机前那次采的**。
 * 光看 seq 是分辨不出来的, 于是重新打开的第一帧会把停机前的旧波形画出来
 * (最坏要等攒够一屏才换掉, 也就是一个刷新周期 50 ms)。
 *
 * 探头在停机期间挪过地方的话, 这一下就是"屏幕在说谎"。所以记一个水位线,
 * 低于它的 word 一律当不存在。 */
static uint32_t s_valid_from[2];

/* CM4 已经发布的半帧数。它从不清零, 只在采集时才涨。 */
static uint32_t seq_of(uint32_t ch)
{
    return (ch == OSC_TRACE_CH1) ? OSC_CTRL->seq_ch1 : OSC_CTRL->seq_ch2;
}

/* ===========================================================================
 * 启停
 * ========================================================================= */

void OSC_Trace_Start(osc_trace_ch_t ch)
{
    const uint32_t c = (uint32_t)ch;

    /* ---- 互斥: 逻辑分析仪让路 (docs/log_plan.md §7.6) ----
     *
     * 两个屏幕各自有个开关, 切屏不改变对方的状态, 所以"用户不会同时开"不是
     * 个能依赖的假设。两个一起跑的后果不是坏一点而已: 逻辑采集把 TIM2 + DMA2
     * 占住, 而示波器的迟滞扫描要吃掉 CM4 主循环的一大块, CM4 那边会开始成片
     * 地追丢半帧 —— 逻辑波形上表现为每一条记录都带 GAP。
     *
     * 放在最前面 (在动 s_trig / s_running 之前): LOG_Trace_Stop 只清一个共享
     * 标志, 不碰这一路的状态。 */
    LOG_Trace_Stop();

    s_trig[c].cand_held = 0u;
    s_trig[c].locked    = 0u;
    s_running[c]        = 1u;

    /* 本次启动之后第一个被判为"落定"的半帧是 (当前 seq), 它的第一个 word 就
     * 在下标 seq * OSC_HALF_WORDS 上 —— 前面的全是停机前的残留。
     *
     * 要在置 cfg_run_chN 之前取 seq, 否则可能已经多算了一个半帧。 */
    s_valid_from[c] = seq_of(c) * OSC_HALF_WORDS;

    /* 把这一路的搬运游标对齐到当前序号。
     *
     * 停着的时候 CM4 根本不发布 (osc_scan_half 只在 cfg_run_chN 置着时才调),
     * 所以 seq 是冻住的, 这一句通常等于没做。留着是兜另一种情况: CM7 漏掉了
     * HSEM 通知, 游标落后了 —— 那样重启后的第一次搬运算出的 step 会是一段
     * 积压, m7_diag_chN.missed 被灌一个巨大的值, 那个计数器之后就没法用来
     * 判断"到底有没有真的漏帧"了。
     *
     * 只对齐这一路: 另一路要是正在跑, 连带它一起对齐会白白吃掉它一个半帧,
     * 那段 655 µs 前的旧数据会被拼进新波形里, 屏幕上看得出接缝。 */
    M7_OSC_SyncCh(c);

    /* 触发是"相位固定"的唯一来源: 不打开这一位, CM4 就只采不找触发点,
     * trig_pos_* 恒为 OSC_TRIG_NONE、trig_flags 恒为 0, 于是 trig_poll 永远
     * 收不到候选、locked 永远不置起, window_start 只能一路走自由运行 ——
     * 窗口贴着写指针走, 每次重画相位随机, 屏幕上就是波形在跳。
     *
     * 这个字段归 CM7 写 (见 sharedconfig.h 的写者约定), 所以在这里置, 而不是
     * 去改 CM4 的 ctrl_defaults。
     *
     * 代价在 CM4 那边: 每个半帧 (655 µs) 要多跑 CH1 的 4096 次 + CH2 的 2048 次
     * 迟滞比较。扛不住的表现是 diag_rate_* 掉下来、或 diag_ovr / diag_miss 开始
     * 涨 (环上出现空洞, 波形成片断裂)。 */
    __DMB();
    OSC_CTRL->cfg_trig_enable = 1u;

    /* cfg_run_chN 这个字段只有一个写者 (CM7), CM4 只读 —— 这是两核之间不用
     * 加锁的前提。DMB 保证这条写先于后面任何一次搬运被 CM4 看到。 */
    if (ch == OSC_TRACE_CH1) { OSC_CTRL->cfg_run_ch1 = 1u; }
    else                     { OSC_CTRL->cfg_run_ch2 = 1u; }
    __DMB();
}

void OSC_Trace_Stop(osc_trace_ch_t ch)
{
    if (ch == OSC_TRACE_CH1) { OSC_CTRL->cfg_run_ch1 = 0u; }
    else                     { OSC_CTRL->cfg_run_ch2 = 0u; }
    __DMB();
    s_running[(uint32_t)ch] = 0u;
}

int OSC_Trace_IsRunning(osc_trace_ch_t ch)
{
    return s_running[(uint32_t)ch] != 0u;
}

/* 可以安全读取的 word 数 —— 也就是"环里前多少个 word 已经确定落定"。
 *
 * 不能直接用 seq * OSC_HALF_WORDS, 理由见 oscillograph_trace.h 里
 * OSC_TRACE_SAFE_HALVES 的说明: 最新那一格多半还在搬。 */
static uint32_t safe_words(uint32_t seq)
{
    return (seq > OSC_TRACE_SAFE_HALVES)
         ? ((seq - OSC_TRACE_SAFE_HALVES) * OSC_HALF_WORDS)
         : 0u;
}

static void trig_poll(uint32_t ch)
{
    /* CM4 的发布顺序是: 先写 trig_pos_* / trig_flags, 再 __DMB(), 最后
     * seq_*++。所以 seq 前后读到一样, 就说明中间没有插进新的发布。
     *
     * 严格说还有个漏网之鱼: CM4 写完了新一帧的位置但还没递增 seq 的那一小
     * 段窗口。落到这个窗口里, 我们会把新帧的位置配上旧帧的序号, 触发点算错
     * 一个半帧 (655 µs)。代价只是这一屏的触发点偏了 1/4 屏宽, 而且下一次
     * 重画就自愈了 —— 不值得为它引入更重的一致性协议。 */
    uint32_t s1 = seq_of(ch);
    __DMB();
    uint32_t flags = OSC_CTRL->trig_flags;
    uint32_t pos   = (ch == OSC_TRACE_CH1) ? OSC_CTRL->trig_pos_ch1
                                           : OSC_CTRL->trig_pos_ch2;
    __DMB();
    uint32_t s2 = seq_of(ch);

    if (s1 != s2) return;                      /* 读一半又被发布了, 这组不配套, 这次不看了 */
    if (s1 == 0u) return;                      /* 一个半帧都还没落定 */

    osc_trig_t *t = &s_trig[ch];
    uint32_t latest = safe_words(s1);          /* 已经确定落定的 word 数 */

    /* 1) 这一帧有触发、而且手上没有候选 → 把它冻成候选。
     *    已经是候选就不动, 这就是"冻住" —— 让它安安静静变老。
     *
     *    trig_pos_* 是"半帧内的下标", 属于半帧 (s1-1)。CH1 那边是 word 下标,
     *    CH2 那边是样本下标 —— 但 CH2 一个 word 装一个样本, 所以两边量纲其实
     *    一样, 都是环上的 word 偏移。 */
    if (t->cand_held == 0u &&
        (flags & (1u << ch)) != 0u &&
        pos < OSC_HALF_WORDS) {
        const uint32_t cand = (s1 - 1u) * OSC_HALF_WORDS + pos;

        /* 重新打开的第一拍: 共享结构里 trig_pos_* / trig_flags 还是停机前那次
         * 留下的, 算出来的候选落在旧数据里。这种不收 —— 收了它也过不了
         * window_start 那道水位线, 只是白占着候选位, 把这段时间里真正的触发
         * 挡在外面。 */
        if (cand >= s_valid_from[ch]) {
            t->cand      = cand;
            t->cand_held = 1u;
        }
    }

    /* 2) 候选够老了就采用, 同时空出位置接下一个候选。
     *
     *    这一步跟"这一帧有没有触发"无关, 每次都要判 —— 否则输入只触发一次
     *    的时候, 选出来的那个候选会永远冻着等不来第二个触发, 屏幕上就一直
     *    是自由运行。 */
    if (t->cand_held != 0u) {
        uint32_t age = latest - t->cand;       /* 用减法比较, 跨 32 位回绕也对 */

        /* 上界 OSC_RING_WORDS 不能少: latest 是"安全水位", 可能比刚发布的
         * 那个触发点还靠前, 这时 age 会借位成一个巨大的数, 光看
         * >= post_words 会误判成"够老了"。
         *
         * pre/post 现在是按这一路自己的档位算的 (见 s_tb): 档位越大, 触发点
         * 右边的数据要等越久才采得出来 (最慢档 98304 word = 31.5 ms)。 */
        if (t->cand >= pre_words(ch) &&
            age >= post_words(ch) &&
            age < OSC_RING_WORDS) {
            t->trig      = t->cand;
            t->locked    = 1u;
            t->cand_held = 0u;
        } else if (t->cand < pre_words(ch) ||
                   (t->cand < latest && age >= OSC_RING_WORDS)) {
            /* 这个候选永远等不到能用的那一天了, 必须把位置让出来 —— 一直
             * 占着的话 cand_held 永不落, 后面来的触发一个都进不来, 屏幕从
             * 此只能自由运行, 而且再也不会自己恢复。两种情形:
             *
             *   cand < pre_words    环里它左边根本没有一屏的 1/4 那么多 word
             *                       可显示 (只可能出现在开机后头一个半帧上),
             *                       再等也一样;
             *   cand < latest 且已老过一整圈
             *                       两次 trig_poll 之间隔了超过 1.34 s, 它
             *                       左边那段已经被写指针绕回去盖掉了。
             *
             * cand >= latest 时 age 是借位出来的巨大值 —— 刚冻上的那一刻就是
             * 这样, 属正常, 不能清。 */
            t->cand_held = 0u;
        }
    }
}

void OSC_Trace_Poll(void)
{
    /* 停掉的那一路不碰: 它的候选/触发锁存冻结在原处, 重新打开时是从头开始的
     * (OSC_Trace_Start 会清掉), 中间不需要在这里维持什么。 */
    if (s_running[OSC_TRACE_CH1]) { trig_poll(OSC_TRACE_CH1); }
    if (s_running[OSC_TRACE_CH2]) { trig_poll(OSC_TRACE_CH2); }
}

/* ===========================================================================
 * 定窗口
 * ========================================================================= */

/* 算出这一屏的第一个 word 的绝对下标, 顺便把这一路当前的列宽交出去
 * (*out_wpc)。返回 0 = 没有可显示的数据。
 *
 * 列宽从这儿出去而不是让 Build 自己再取一次, 是为了让"窗口多长"和"一列多宽"
 * 一定是同一份快照算出来的 —— 分成两次取的话, 正好夹在中间转了编码器就会
 * 拿新档位的列宽去画旧档位的窗口, 屏幕上表现为整屏的时间量纲对不上真实窗口
 * (画得出来, 只是每列代表的时长和窗口总长自相矛盾), 而且下一次重画就自愈,
 * 极难复现。 */
static int window_start(uint32_t ch, uint32_t *out, uint32_t *out_wpc)
{
    osc_trig_t *t = &s_trig[ch];
    uint32_t latest = safe_words(seq_of(ch));   /* 已经确定落定的 word 数 */
    uint32_t valid  = s_valid_from[ch];         /* 本次启动之前的一律不算 */

    /* 这一路的档位导出的三个量, 整个函数用同一份快照算 —— 中途有人转编码器
     * 换了档位的话, 拿到的是"换之前"那一套, 自洽; 下一次 Build 再用新的。
     * 换档本身会把 locked 清掉 (见 OSC_Trace_SetTimebase), 所以不会出现
     * "用新档位的 pre 配旧档位的 trig"。 */
    const uint32_t wpc  = wpc_of(ch);
    const uint32_t span = span_words(ch);
    const uint32_t pre  = pre_words(ch);

    *out_wpc = wpc;

    if (t->locked) {
        uint32_t start = t->trig - pre;
        /* 触发点太老的话, 它左边那一段已经被写指针绕回去盖掉了。
         * 用减法比较, 跨回绕也对; 下界 span 顺手保证读的都在
         * 安全水位之内。
         *
         * start >= valid 这一条是重新打开时用的: 触发点本身可能是停之前
         * 留下来的 (trig_pos_* 那几个字段还冻在共享结构里), 那样它左边
         * 那一整段都是旧波形。这种直接放弃, 等下一个触发。 */
        uint32_t back = latest - start;
        if (back < OSC_RING_WORDS && start >= valid) {
            *out = start;
            return 1;
        }
        t->locked = 0u;      /* 放弃, 下面去自由运行 */
    }

    /* 自由运行: 没有触发 (直流输入下 CM4 的 trig_flags 恒为 0), 或者触发
     * 太久没来。窗口贴着写指针走, 波形从右往左滚。 */
    if (latest < span) return 0;          /* 环里还没凑够一屏 */
    if (latest - valid < span) return 0;  /* 攒的全是上次留下的 */
    *out = latest - span;
    return 1;
}

/* ===========================================================================
 * 抽点
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * 过零检测的门限, 每通道一份
 *
 * 门限必须在扫之前就定下来, 而幅度中点要扫完才知道 —— 要么扫两遍 (等于把
 * SDRAM 读翻倍, 这是这个模块里最贵的东西), 要么用**上一屏**量出来的幅度。
 * 取后者: 波形稳定时两者没有区别, 幅度突变时慢一屏 (50 ms) 跟上, 而幅度突变
 * 本身在屏幕上也要一屏才看得出来。
 *
 * 开机第一屏还没有历史, 用 0V 那个偏置点 (2048) 加一个小小的迟滞兜底。幅度小
 * 到 64 个计数以内的信号第一屏量不出频率, 第二屏就好了 —— 这条路径只在通道刚
 * 打开那一瞬间存在。
 * ------------------------------------------------------------------------- */
static uint16_t s_thr_mid[2]  = { 2048u, 2048u };
static uint16_t s_thr_hyst[2] = {   64u,   64u };

/* 迟滞下限: 平直信号上 ±1 LSB 的抖动不该被数成边沿。 */
#define OSC_MEAS_MIN_HYST  2u

/* 周期至少要占这么多个采样点。
 *
 * 4 个点的时候插值还能看 (正弦一波里有 4 个点, 过零那两下前后各有样本可以插);
 * 再密的东西 (接近奈奎斯特) 插值失真和混叠一起来, 与其显示一个错的数, 不如
 * 显示 "----"。CH1 采样间隔 160 ns → 上限约 1.56 MHz; CH2 320 ns → 约 780 kHz。 */
#define OSC_MEAS_MIN_SAMPLES_PER_PERIOD  4u

/* 迟滞宽度: 幅度的 1/8, 但不小于 OSC_MEAS_MIN_HYST。
 *
 * 按幅度取而不是取固定值 —— 信号小的时候固定迟滞会把它整个吃掉 (永远不穿越),
 * 信号大的时候又挡不住噪声。 */
static uint16_t meas_hyst(uint16_t lo, uint16_t hi)
{
    const uint32_t h = ((uint32_t)hi - (uint32_t)lo) / 8u;
    return (uint16_t)((h < OSC_MEAS_MIN_HYST) ? OSC_MEAS_MIN_HYST : h);
}

/* 把一个样本喂给过零检测器。
 *
 * ── 为什么是宏而不是函数 ──
 * 它驱动的这套状态**每个样本都要走一遍**, 所以必须留在寄存器里。写成函数的话
 * 只要编译器没把结构体提升进寄存器, 每个样本就多一次入栈出栈的往返 —— 正好
 * 把"塞进同一个循环"省下来的那些又还回去。代价是它引用的局部量必须在调用它的
 * 函数里同名叫出来, 名字统一是 m_ 开头, build_ch1 / build_ch2 各声明一份。
 *
 * v  样本值 (0..4095)
 * tv 这个样本的时刻 (ns, 相对窗口起点)
 *
 * 上升沿的判定是迟滞比较: 高于 thr_hi 才算"上", 低于 thr_lo 才算"下", 中间是
 * 死区。只数上升沿 —— 周期就是相邻两个上升沿的间隔。 */
#define OSC_MEAS_FEED(v, tv)                                                 \
    do {                                                                     \
        const int32_t fv_ = (int32_t)(v);                                    \
        if (m_seen == 0u) {                                                  \
            m_seen = 1u;                /* 第一个样本只用来定基准 */          \
        } else if (m_above != 0u) {                                          \
            if (fv_ < m_thr_lo) { m_above = 0u; }                            \
        } else if (fv_ > m_thr_hi && m_nedge < OSC_MEAS_MAX_EDGES) {         \
            m_above = 1u;                                                    \
            /* 上升沿落在上一个样本和这一个之间, 按值线性插值出穿越门限的     \
             * 时刻。不插值的话分辨率就是采样间隔 (CH1 只有 160 ns), 一屏里  \
             * 周期数一多, 量化误差会直接变成几十 Hz 的抖动。                 \
             *                                                               \
             * 除法在这里是安全的: 走到这儿必有 fv_ > thr_hi >= 上一个样本   \
             * (状态机只在 fv_ > thr_hi 时才置 above), 所以分母至少是 1,     \
             * 分子是 0 以上的, 不会除零也不会借位成一个大数。 */            \
            const uint32_t te_ = (tv) - m_step +                             \
                (uint32_t)((uint32_t)(m_thr_hi - (int32_t)m_prev) * m_step    \
                           / (uint32_t)(fv_ - (int32_t)m_prev));              \
            if (m_nedge == 0u) { m_t_first = te_; }                          \
            m_t_last = te_;                                                  \
            m_nedge++;                                                       \
        }                                                                    \
        m_prev = (uint16_t)fv_;                                              \
    } while (0)

/* 一屏扫完之后的结算: 全局 min/max、门限留给下一屏、频率。
 *
 * 也是宏 (同一个 m_ 约定), 因为两路的局部量一模一样。 */
#define OSC_MEAS_FINISH(meas_ptr, ch_idx)                                    \
    do {                                                                     \
        /* 全局 min/max 从每列的 min/max 上取 —— 每一列的结果已经覆盖了那一列  \
         * 里**所有**样本, 所以这两层取下来就是整屏的最小/最大值。1024 次比较  \
         * 在片上 SRAM 上, 相比之下可以忽略。                                 \
         *                                                                   \
         * 这也是为什么峰峰值几乎不要钱: 内层那个读 SDRAM 的循环一行都不用改。*/ \
        uint16_t g_lo_ = 0xFFFFu, g_hi_ = 0u;                                \
        for (uint32_t i_ = 0u; i_ < OSC_TRACE_POINTS; i_++) {                \
            const uint16_t yv_ = y[i_];                                      \
            if (yv_ < g_lo_) g_lo_ = yv_;                                    \
            if (yv_ > g_hi_) g_hi_ = yv_;                                    \
        }                                                                    \
                                                                             \
        (meas_ptr)->lo    = g_lo_;                                           \
        (meas_ptr)->hi    = g_hi_;                                           \
        (meas_ptr)->valid = 1u;                                              \
                                                                             \
        s_thr_mid[ch_idx]  = (uint16_t)(((uint32_t)g_lo_ + g_hi_) / 2u);     \
        s_thr_hyst[ch_idx] = meas_hyst(g_lo_, g_hi_);                        \
                                                                             \
        (meas_ptr)->freq_mhz = 0u;                                           \
        (meas_ptr)->has_freq = 0u;                                           \
                                                                             \
        /* 至少要两个上升沿才谈得上"一个周期"。 */                            \
        if (m_nedge >= 2u) {                                                 \
            const uint32_t span_ns_ = m_t_last - m_t_first;                  \
            const uint32_t nper_    = m_nedge - 1u;                          \
                                                                             \
            /* 周期太短就别报了, 见 OSC_MEAS_MIN_SAMPLES_PER_PERIOD。 */     \
            if (span_ns_ >= OSC_MEAS_MIN_SAMPLES_PER_PERIOD * m_step * nper_) { \
                /* 周期数 × 1e12 会溢出 32 位 (128 个周期就是 1.28e14), 用      \
                 * uint64 算。一屏每路只做一次, 除法的开销 (__aeabi_uldivmod) \
                 * 在这种频率下可以忽略。 */                                 \
                const uint64_t f_ = (uint64_t)nper_ * 1000000000000ull / span_ns_; \
                (meas_ptr)->freq_mhz = (f_ > 0xFFFFFFFFull) ? 0xFFFFFFFEu       \
                                                            : (uint32_t)f_;    \
                (meas_ptr)->has_freq = 1u;                                   \
            }                                                                \
        }                                                                    \
    } while (0)

/* CH1: 一个 word 装两个样本 —— 低 16 位是 ADC1 的, 时间在前; 高 16 位是 ADC2
 * 的, 时间在后 (CM4 那边 ADC1/ADC2 多重模式交替采同一个输入)。所以 W 个 word
 * = 2W 个样本, 只留下 min 和 max。
 *
 * 采样间隔是两个 ADC 交替的间隔, 也就是 OSC_WORD_PERIOD_NS / 2 —— 这一路的时间
 * 轴是它, 不是 word 的周期。 */
static void build_ch1(uint32_t start, uint32_t wpc, uint16_t *y, osc_meas_t *meas)
{
    const uint32_t m_step   = OSC_WORD_PERIOD_NS / 2u;
    const int32_t  m_thr_hi = (int32_t)s_thr_mid[0] + (int32_t)s_thr_hyst[0];
    const int32_t  m_thr_lo = (int32_t)s_thr_mid[0] - (int32_t)s_thr_hyst[0];
    uint16_t m_prev = 0u;
    uint32_t m_t    = m_step;      /* 第一个样本的时刻 */
    uint32_t m_t_first = 0u;
    uint32_t m_t_last  = 0u;
    uint32_t m_nedge   = 0u;
    uint8_t  m_above   = 0u;
    uint8_t  m_seen    = 0u;

    for (uint32_t c = 0u; c < OSC_TRACE_COLUMNS; c++) {
        uint32_t w = start + c * wpc;
        uint16_t lo = 0xFFFFu;
        uint16_t hi = 0u;

        for (uint32_t k = 0u; k < wpc; k++) {
            uint32_t word = CH1_WORD(w + k);
            uint16_t a = (uint16_t)(word & 0xFFFFu);
            uint16_t b = (uint16_t)(word >> 16);
            if (a < lo) lo = a;
            if (a > hi) hi = a;
            if (b < lo) lo = b;
            if (b > hi) hi = b;

            OSC_MEAS_FEED(a, m_t); m_t += m_step;   /* ADC1, 时间在前 */
            OSC_MEAS_FEED(b, m_t); m_t += m_step;   /* ADC2, 时间在后 */
        }

        y[c * 2u]      = lo;
        y[c * 2u + 1u] = hi;
    }

    OSC_MEAS_FINISH(meas, 0);
}

/* CH2: 一个 word 就是一个样本 (uint16), W 个 word = W 个样本。 */
static void build_ch2(uint32_t start, uint32_t wpc, uint16_t *y, osc_meas_t *meas)
{
    const uint32_t m_step   = OSC_WORD_PERIOD_NS;
    const int32_t  m_thr_hi = (int32_t)s_thr_mid[1] + (int32_t)s_thr_hyst[1];
    const int32_t  m_thr_lo = (int32_t)s_thr_mid[1] - (int32_t)s_thr_hyst[1];
    uint16_t m_prev = 0u;
    uint32_t m_t    = m_step;
    uint32_t m_t_first = 0u;
    uint32_t m_t_last  = 0u;
    uint32_t m_nedge   = 0u;
    uint8_t  m_above   = 0u;
    uint8_t  m_seen    = 0u;

    for (uint32_t c = 0u; c < OSC_TRACE_COLUMNS; c++) {
        uint32_t w = start + c * wpc;
        uint16_t lo = 0xFFFFu;
        uint16_t hi = 0u;

        for (uint32_t k = 0u; k < wpc; k++) {
            uint16_t v = CH2_WORD(w + k);
            if (v < lo) lo = v;
            if (v > hi) hi = v;

            OSC_MEAS_FEED(v, m_t); m_t += m_step;
        }

        y[c * 2u]      = lo;
        y[c * 2u + 1u] = hi;
    }

    OSC_MEAS_FINISH(meas, 1);
}

int OSC_Trace_Build(osc_trace_ch_t ch, uint16_t *y_out, osc_meas_t *meas)
{
    /* 先把结果写成"什么都没量到"。后面任何一条提前返回的路径 (没在跑、环里还
     * 没凑够一屏) 就都落在这一份上 —— 调用方不需要自己记"上一次是不是好的",
     * 每次拿到的都是这一屏的真话。 */
    meas->valid    = 0u;
    meas->has_freq = 0u;
    meas->lo       = 0u;
    meas->hi       = 0u;
    meas->freq_mhz = 0u;

    if (!OSC_Trace_IsRunning(ch)) return 0;    /* 停掉的那一路不再重新抠数 */

    uint32_t start, wpc;
    if (!window_start((uint32_t)ch, &start, &wpc)) return 0;

    if (ch == OSC_TRACE_CH1) build_ch1(start, wpc, y_out, meas);
    else                     build_ch2(start, wpc, y_out, meas);
    return 1;
}

/* ===========================================================================
 * 时基档位 —— 界面在转编码器时碰的那几个
 *
 * 整个模块的状态只被 TouchGFX 任务碰 (Model::tick / 编码器回调都在那个任务
 * 里), 所以这里不加锁。CM4 完全不关心档位 —— 它只管按固定节奏采, 抽点是
 * 这边的事。
 * ========================================================================= */

void OSC_Trace_SetTimebase(osc_trace_ch_t ch, int idx)
{
    const uint32_t c = (uint32_t)ch;

    /* 夹到边界而不是回绕: 转到 42 ms 就该停住。回绕的话从最慢档再往右一格
     * 会一把跳回 164 µs, 用户看到的是"转过了头, 量程突然没了"。 */
    if (idx < 0) idx = 0;
    if (idx > (int)OSC_TB_STEPS - 1) idx = (int)OSC_TB_STEPS - 1;

    if (s_tb_idx[c] == (uint8_t)idx) return;   /* 没变: 尤其别去动触发锁存 */

    s_tb_idx[c] = (uint8_t)idx;

    /* 必须丢掉这一路的触发锁存。锁存里的 trig 是绝对 word 号, 而"触发点落在
     * 第几列" = pre_words(), 是跟着档位走的 —— 留着的话新档位下的 pre 和旧
     * 锁存的 trig 配起来, 触发点会停在错误的列上, 而且看起来"有触发" (屏幕
     * 不抖), 于是不会有人想到是档位的问题。
     *
     * cand_held 一起清: 候选也是按旧档位的 pre/post 判老嫩的, 留着它可能
     * 立刻被按新档位判成"够老了"而采用 —— 同样是错的列。
     *
     * 清了之后 window_start 退回自由运行, 等下一个触发重新锁上 (几毫秒)。
     * 这是换档必然要付的代价, 不是 bug。 */
    s_trig[c].locked    = 0u;
    s_trig[c].cand_held = 0u;

    /* s_valid_from / s_running 不动: 环里有 1.34 s 的历史, 换档不需要重新
     * 采集, 也不会空屏 —— 只是相位那一下子会重来。 */
}

int OSC_Trace_GetTimebase(osc_trace_ch_t ch)
{
    return (int)s_tb_idx[(uint32_t)ch];
}

uint32_t OSC_Trace_SpanNs(uint32_t idx)
{
    if (idx > OSC_TB_STEPS - 1u) idx = OSC_TB_STEPS - 1u;
    return s_tb[idx].span_ns;
}

uint8_t OSC_Trace_RefreshTicks(void)
{
    uint32_t div = OSC_TRACE_REFRESH_TICKS;    /* 基准档的值, 也是下限 */

    /* 两路各跑各的档位, 但共用同一个 tick 回调 —— 只能取大的那个: 取小的
     * 会让慢档那一路按快档的节奏抽点, 每次重画都掉帧。都不在跑就返回基准值,
     * 反正也没人看。 */
    for (uint32_t c = 0u; c < 2u; c++) {
        if (s_running[c]) {
            const uint32_t d = s_tb[s_tb_idx[c]].refresh_div;
            if (d > div) div = d;
        }
    }

    /* 表里不会有 0, 但 0 会让调用方那边 (tickDiv - 1) 下溢成 0xFFFFFFFF,
     * 表现是完全不刷新 —— 便宜的一行, 挡住这一类事故。 */
    return (uint8_t)((div == 0u) ? 1u : div);
}
