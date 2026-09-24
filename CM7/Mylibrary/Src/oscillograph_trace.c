/* ===========================================================================
 * 屏幕波形取数 —— 从 SDRAM 深存储环里抠一屏
 *
 * 环是 CM4 一直写、CM7 一直搬的, 没有"存满了"这个状态, 所以读的时候必须
 * 保证读到的每一个 word 都已经落定。做法不是加锁, 而是"读旧的":
 *
 *   1) 一屏只有 OSC_TRACE_WORDS (8192) 个 word, 而环有 4194304 个 word,
 *      相差 512 倍。只要一屏的尾巴离写指针还有足够距离, 读的整个过程里写
 *      指针都不可能追上它。
 *   2) 下面所有窗口的取法都保证了 窗口尾巴 <= 写指针, 而且写入是按半帧
 *      为单位完成的, 所以读到的都在已经搬完的半帧里 —— 不会读到搬了一半的
 *      半帧。
 *
 * 参数和对外接口在 oscillograph_trace.h 里。
 * ========================================================================= */

#include "oscillograph_trace.h"
#include "oscillograph.h"
#include "sharedconfig.h"

/* 环里一共多少个 word: 2048 个半帧 × 2048 word = 4194304 = 2^22。
 * 是 2 的幂, 所以"绕回"就是按位与, 不用真的做取模。 */
#define OSC_RING_WORDS  (OSC_SDRAM_HALVES * OSC_HALF_WORDS)
#define OSC_RING_MASK   (OSC_RING_WORDS - 1u)

/* 这两条要是破了, 后果是"波形莫名其妙拼错一格", 而且只在环绕回的那一刻出现
 * 一次, 极难查 —— 宁可编译不过。 */
OSC_STATIC_ASSERT((OSC_RING_WORDS & OSC_RING_MASK) == 0u,
                  "环的 word 数必须是 2 的幂, 否则 & 掩码取模不成立");
OSC_STATIC_ASSERT(OSC_TRACE_WORDS <= OSC_RING_WORDS,
                  "一屏比环还长");
OSC_STATIC_ASSERT(OSC_TRACE_WORDS_PER_COL <= OSC_HALF_WORDS,
                  "一列跨过了半帧边界");

/* ---- 绝对 word 号 → SDRAM 里的值 (自动绕回) ----
 * 绝对 word 号是"从采集开始算起的第几个 word", 单调递增, 不绕回; 绕回只发生
 * 在换算成地址这一步。这样时间戳永远是线性的, 比较新旧只要减法。 */
#define CH1_WORD(w) (*(const volatile uint32_t *)(OSC_SDRAM_CH1_BASE + (((w) & OSC_RING_MASK) * 4u)))
#define CH2_WORD(w) (*(const volatile uint16_t *)(OSC_SDRAM_CH2_BASE + (((w) & OSC_RING_MASK) * 2u)))

/* ---------------------------------------------------------------------------
 * 触发锁存, 每个通道一份
 *
 * 要解决的问题: 一屏 8192 个 word, 触发点在第 128 列, 也就是触发点右边还要
 * 6144 个 word 的"触发后"数据。触发刚发生的那一刻, 那些数据根本还没采出来,
 * 所以不能一看到触发就用它 —— 那样画出来的窗口有一大半落在未来。
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

    /* cfg_run_chN 这个字段只有一个写者 (CM7), CM4 只读 —— 这是两核之间不用
     * 加锁的前提。DMB 保证这条写先于后面任何一次搬运被 CM4 看到。 */
    __DMB();
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
         * >= POST_WORDS 会误判成"够老了"。 */
        if (t->cand >= OSC_TRACE_PRE_WORDS &&
            age >= OSC_TRACE_POST_WORDS &&
            age < OSC_RING_WORDS) {
            t->trig      = t->cand;
            t->locked    = 1u;
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

/* 算出这一屏的第一个 word 的绝对下标。返回 0 = 没有可显示的数据。 */
static int window_start(uint32_t ch, uint32_t *out)
{
    osc_trig_t *t = &s_trig[ch];
    uint32_t latest = safe_words(seq_of(ch));   /* 已经确定落定的 word 数 */
    uint32_t valid  = s_valid_from[ch];         /* 本次启动之前的一律不算 */

    if (t->locked) {
        uint32_t start = t->trig - OSC_TRACE_PRE_WORDS;
        /* 触发点太老的话, 它左边那一段已经被写指针绕回去盖掉了。
         * 用减法比较, 跨回绕也对; 下界 OSC_TRACE_WORDS 顺手保证读的都在
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
    if (latest < OSC_TRACE_WORDS) return 0;          /* 环里还没凑够一屏 */
    if (latest - valid < OSC_TRACE_WORDS) return 0;  /* 攒的全是上次留下的 */
    *out = latest - OSC_TRACE_WORDS;
    return 1;
}

/* ===========================================================================
 * 抽点
 * ========================================================================= */

/* CH1: 一个 word 装两个样本 —— 低 16 位是 ADC1 的, 时间在前; 高 16 位是 ADC2
 * 的, 时间在后 (CM4 那边 ADC1/ADC2 多重模式交替采同一个输入)。
 * 所以 16 个 word = 32 个样本, 只留下 min 和 max。 */
static void build_ch1(uint32_t start, uint16_t *y)
{
    for (uint32_t c = 0u; c < OSC_TRACE_COLUMNS; c++) {
        uint32_t w = start + c * OSC_TRACE_WORDS_PER_COL;
        uint16_t lo = 0xFFFFu;
        uint16_t hi = 0u;

        for (uint32_t k = 0u; k < OSC_TRACE_WORDS_PER_COL; k++) {
            uint32_t word = CH1_WORD(w + k);
            uint16_t a = (uint16_t)(word & 0xFFFFu);
            uint16_t b = (uint16_t)(word >> 16);
            if (a < lo) lo = a;
            if (a > hi) hi = a;
            if (b < lo) lo = b;
            if (b > hi) hi = b;
        }

        y[c * 2u]      = lo;
        y[c * 2u + 1u] = hi;
    }
}

/* CH2: 一个 word 就是一个样本 (uint16), 16 个 word = 16 个样本。 */
static void build_ch2(uint32_t start, uint16_t *y)
{
    for (uint32_t c = 0u; c < OSC_TRACE_COLUMNS; c++) {
        uint32_t w = start + c * OSC_TRACE_WORDS_PER_COL;
        uint16_t lo = 0xFFFFu;
        uint16_t hi = 0u;

        for (uint32_t k = 0u; k < OSC_TRACE_WORDS_PER_COL; k++) {
            uint16_t v = CH2_WORD(w + k);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }

        y[c * 2u]      = lo;
        y[c * 2u + 1u] = hi;
    }
}

int OSC_Trace_Build(osc_trace_ch_t ch, uint16_t *y_out)
{
    if (!OSC_Trace_IsRunning(ch)) return 0;    /* 停掉的那一路不再重新抠数 */

    uint32_t start;
    if (!window_start((uint32_t)ch, &start)) return 0;

    if (ch == OSC_TRACE_CH1) build_ch1(start, y_out);
    else                     build_ch2(start, y_out);
    return 1;
}
