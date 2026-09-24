/* ===========================================================================
 * 调优探针的实现 —— 用法和看数的方法都在 osc_probe.h 的文件头
 *
 * 这里故意不用 vsnprintf: defaultTask 的栈只有 512 B, newlib 的格式化函数
 * 一个人就能吃掉两三百字节, 而且会把 printf 那一大坨代码链进来。只为了打
 * 几个无符号整数, 手写一个够用了。
 * ========================================================================= */

#include "osc_probe.h"
#include "oscillograph.h"
#include "sharedconfig.h"   /* OSC_CTRL —— CM4 实测的采样率就在里面 */
#include "main.h"
#include "usart.h"

volatile uint32_t g_dbg_gap_cyc;
volatile uint32_t g_dbg_gap_max_cyc;
volatile uint32_t g_dbg_gap_min_cyc;      /* 0 = 还没有样本, 见 osc_probe.h */
volatile uint32_t g_dbg_build_cyc;
volatile uint32_t g_dbg_graph_cyc;

/* ---- 渲染 pass 耗时 (beginFrame→endFrame), 见 osc_probe.h ---- */
volatile uint32_t g_dbg_render_cyc;
volatile uint32_t g_dbg_render_max_cyc;
volatile uint32_t g_dbg_begin_cyc;
volatile uint32_t g_dbg_stall_cnt;
uint32_t g_osc_probe_stall_cyc = 1u;      /* 见 osc_probe.h */

/* ---- 中断耗时 (并集) ---- */
volatile uint32_t g_dbg_isr_cyc;
volatile uint32_t g_dbg_isr_cnt;
volatile uint32_t g_dbg_isr_depth;
volatile uint32_t g_dbg_isr_enter_cyc;

volatile uint32_t g_dbg_uart_cyc;
volatile uint32_t g_dbg_tick_cnt;

void OSC_Probe_Init(void)
{
    /* DWT 是内核私有外设, ST 出于安全考虑默认锁着。不先写 LAR 解锁, 后面
     * 对 DEMCR / CYCCNT 的写全是空操作, 而且不会报任何错。 */
    *(volatile uint32_t *)0xE0001FB0u = 0xC5ACCE55u;   /* DWT->LAR */
    *(volatile uint32_t *)0xE000EDFCu |= (1u << 24);   /* DEMCR.TRCENA */

    /* SystemCoreClock 由 HAL_RCC_ClockConfig() 更新, 这里拿到的是真实主频。
     * OSC_Probe_Init() 在 Model 构造函数里调, 而 Model 是在 MX_TouchGFX_Init()
     * 里构造的 —— 那时时钟早就配好了。 */
    /* 50 ms ≈ 3 帧。正常帧 15.46 ms 够不着它, 真卡住 (几秒) 一定超。 */
    g_osc_probe_stall_cyc = SystemCoreClock / 20u;
    if (g_osc_probe_stall_cyc == 0u)
    {
        g_osc_probe_stall_cyc = 1u;
    }

    OSC_PROBE_CYC = 0u;
    *(volatile uint32_t *)0xE0001000u |= 1u;           /* DWT_CTRL.CYCCNTENA */
}

/* ---------------------------------------------------------------------------
 * 极简格式化: 只认无符号十进制
 *
 * 拼出来的最长一行 (所有计数器都走到 10 位满宽) 大约 430 字节, 缓冲开 512
 * 留余量。放在静态区而不是栈上 —— 栈上那点空间留给 HAL。
 * 只有 defaultTask 调这个函数, 不存在重入。
 * ------------------------------------------------------------------------- */
static char s_line[512];

/* 上一次打印的时刻, 用来算 dt */
static uint32_t s_prevReportCyc;

/* 上一次打印时的中断累计周期数。g_dbg_isr_cyc 是 32 位、按 480 MHz 回绕
 * (8.9 秒一圈), 所以必须取差, 不能直接打。 */
static uint32_t s_prevIsrCyc;

/* 上一次打印时的 HAL 毫秒计数 (TIM7 驱动的那个)。取差就知道 HAL 的时基
 * 这一窗口里走了多少毫秒 —— 和 dt 对不上的话, 说明 TIM7 被饿过, 所有靠
 * HAL_GetTick() 的超时 (HAL_Delay / UART / I2C) 都会跟着失准。 */
static uint32_t s_prevHalTick;

static char *put_str(char *p, const char *s)
{
    while (*s != '\0')
    {
        *p++ = *s++;
    }
    return p;
}

static char *put_u32(char *p, uint32_t v)
{
    char tmp[10];       /* 2^32-1 是 10 位数 */
    int n = 0;

    do
    {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);

    while (n > 0)
    {
        *p++ = tmp[--n];
    }
    return p;
}

/* 一行里的一个 "名=值" */
static char *put_kv(char *p, const char *key, uint32_t v)
{
    p = put_str(p, key);
    *p++ = '=';
    return put_u32(p, v);
}

/* 周期 → 微秒。原始周期是 7 位数, 读起来费劲, 而且跟主频绑死。 */
static uint32_t to_us(uint32_t cyc, uint32_t cyc_per_us)
{
    return cyc / cyc_per_us;
}

void OSC_Probe_Report(void)
{
    /* SystemCoreClock 由 HAL_RCC_ClockConfig() 更新, 所以这里拿到的是真实
     * 主频, 换了时钟配置也不用改探针。 */
    uint32_t cyc_per_us = SystemCoreClock / 1000000u;
    if (cyc_per_us == 0u)
    {
        cyc_per_us = 1u;
    }

    /* dt = 距上一次打印真正过了多久。
     *
     * 这一列是关键: 前面几列里 irq / pushed 都是累计值, 只有知道"这一行是
     * 隔了多久打出来的", 才能把累计值换算成每秒速率。而 defaultTask 优先级
     * 比 TouchGFX 低, 它到底隔多久才被轮到一次, 光看代码看不出来 —— 必须量。 */
    const uint32_t nowCyc = OSC_PROBE_CYC;
    const uint32_t dtCyc = (s_prevReportCyc != 0u) ? (nowCyc - s_prevReportCyc) : 0u;
    const uint32_t dt = to_us(dtCyc, cyc_per_us);
    s_prevReportCyc = nowCyc;

    /* 中断占用: 这一窗口里 CPU 待在中断里的周期数, 以及它占 dt 的百分比。
     *
     * isrpct 是这次要看的那个数: 接近 100 就说明任务不是"慢", 是根本轮不上
     * 被调度 —— CPU 全在中断里。很小 (比如个位数) 就说明任务是真的阻塞在
     * 别处, 矛头要转向 OSWrappers 那条 vsync 消息队列。
     *
     * 百分比用"先除后除"算, 是为了避开 64 位除法 (会拖进 __aeabi_uldivmod,
     * 而 isrCyc * 100 在 32 位下会溢出)。 */
    const uint32_t isrCyc = g_dbg_isr_cyc - s_prevIsrCyc;
    s_prevIsrCyc = g_dbg_isr_cyc;
    const uint32_t isrUs  = to_us(isrCyc, cyc_per_us);

    /* 用万分之几而不是百分之几 —— 实测只有 0.55%, 按整数百分比算会被
     * 截断成 0, 看着像"没测到"。10000 = 100%。 */
    const uint32_t isrBp = (dtCyc >= 10000u) ? (isrCyc / (dtCyc / 10000u)) : 0u;

    /* HAL 毫秒时基这一窗口走了多少毫秒。正常情况下它应该 ≈ dt/1000,
     * 差得远就说明 TIM7 被饿过 —— 那 HAL_Delay / UART / I2C 的超时全失准。 */
    const uint32_t halNow = HAL_GetTick();
    const uint32_t hdt = (s_prevHalTick != 0u) ? (halNow - s_prevHalTick) : 0u;
    s_prevHalTick = halNow;

    const uint32_t gap = to_us(g_dbg_gap_cyc, cyc_per_us);
    const uint32_t max = to_us(g_dbg_gap_max_cyc, cyc_per_us);
    const uint32_t min = to_us(g_dbg_gap_min_cyc, cyc_per_us);
    const uint32_t bui = to_us(g_dbg_build_cyc, cyc_per_us);
    const uint32_t grp = to_us(g_dbg_graph_cyc, cyc_per_us);
    const uint32_t rnd = to_us(g_dbg_render_cyc, cyc_per_us);
    const uint32_t rmx = to_us(g_dbg_render_max_cyc, cyc_per_us);

    char *p = s_line;

    p = put_kv(p, "[osc] MHz", SystemCoreClock / 1000000u);
    p = put_kv(p, " dt", dt);
    p = put_kv(p, " frame", gap);
    p = put_kv(p, " fps", (gap != 0u) ? (1000000u / gap) : 0u);
    p = put_kv(p, " max", max);
    p = put_kv(p, " min", min);
    p = put_str(p, " |");
    p = put_kv(p, " hdt", hdt);
    p = put_kv(p, " ntick", g_dbg_tick_cnt);
    p = put_kv(p, " uart", to_us(g_dbg_uart_cyc, cyc_per_us));
    p = put_str(p, " |");
    p = put_kv(p, " isr", isrUs);
    p = put_kv(p, " isrbp", isrBp);
    p = put_kv(p, " isrcnt", g_dbg_isr_cnt);
    p = put_str(p, " |");
    p = put_kv(p, " build", bui);
    p = put_kv(p, " graph", grp);
    p = put_kv(p, " rend", rnd);
    p = put_kv(p, " rmax", rmx);
    p = put_str(p, " |");
    p = put_kv(p, " stall", g_dbg_stall_cnt);
    p = put_kv(p, " irq", m7_diag_irq);

    /* CM4 用 DWT 实测的采样率 (Sa/s), 和 CM4 主循环来不及扫的半帧数。
     * 有了它才能判断 CM4 那边到底按什么速率在发布 —— 光看 CM7 这边的 irq
     * 累计值是分不出"速率异常"和"打印间隔异常"的。 */
    p = put_kv(p, " rate1", OSC_CTRL->diag_rate_ch1);
    p = put_kv(p, " rate2", OSC_CTRL->diag_rate_ch2);
    p = put_kv(p, " cmiss", OSC_CTRL->diag_miss);

    /* pushed / overrun / err / missed */
    p = put_str(p, " | ch1=");
    p = put_u32(p, m7_diag_ch1.pushed);  *p++ = '/';
    p = put_u32(p, m7_diag_ch1.overrun); *p++ = '/';
    p = put_u32(p, m7_diag_ch1.err);     *p++ = '/';
    p = put_u32(p, m7_diag_ch1.missed);

    p = put_str(p, " ch2=");
    p = put_u32(p, m7_diag_ch2.pushed);  *p++ = '/';
    p = put_u32(p, m7_diag_ch2.overrun); *p++ = '/';
    p = put_u32(p, m7_diag_ch2.err);     *p++ = '/';
    p = put_u32(p, m7_diag_ch2.missed);

    p = put_str(p, "\r\n");

    /* 顺序发, 超时给足 —— 一行两百多个字符在 115200 下要 20 ms 上下。
     *
     * 掐表是为了把 dt 劈开: 如果这一发真的花了 2.9 秒, 那 dt 就是被它撑大的,
     * 任务根本没被饿着; 如果只花了二十几毫秒, 那 2.9 秒就是任务真没被调度。
     *
     * 注意超时是 200 ms, 而 HAL 的毫秒时基走 TIM7 —— TIM7 被饿住的话这里会
     * 一直等下去, 所以 uart 这一列同时也是 TIM7 是否健康的一个旁证。 */
    const uint32_t tUart = OSC_PROBE_CYC;
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)s_line,
                            (uint16_t)(p - s_line), 200u);
    g_dbg_uart_cyc = OSC_PROBE_CYC - tUart;
}
