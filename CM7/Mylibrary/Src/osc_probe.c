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

/* 上一次打印时的中断累计周期数 / HAL 毫秒计数。原来用来算 isr/us 和 hdt
 * 两列 —— 那两列是查渲染卡顿时加的, 跟现在的问题无关, 已经砍掉, 连带这两
 * 个"上一次的值"一起删。要恢复的话, g_dbg_isr_cyc / HAL_GetTick() 都还在,
 * 重新取一次差就行。 */

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

/* ---------------------------------------------------------------------------
 * 每秒一行, 只剩四个数
 *
 * 砍掉的那些列 (MHz / frame / fps / max / min / hdt / ntick / uart / isr /
 * isrbp / isrcnt / build / graph / rend / rmax / stall / rate1 / rate2 / err)
 * 是上一轮查"屏幕卡顿"时加的, 跟现在这个"波形只有两三种相位"无关。它们的
 * 原始计数 (g_dbg_*) 还在各个文件里被更新, 要恢复某一列, 把对应的 put_kv
 * 加回来即可。
 *
 * 一行变短不只是好看: 原来两百多个字符在 115200 下要 20 ms, 而 TouchGFX 的
 * 一帧才 15.46 ms —— 打印本身就在拖慢被测对象。换句话说, 这个"仪器"的读数
 * 里掺着仪器自己的重量。越小越好。
 *
 * 留下的四个数都是在回答同一个问题: "环上到底有没有洞, 洞是谁造成的"。
 *   dt     这行是隔了多久打出来的 (累计值除以它就是每秒速率)
 *   irq    CM7 收到 HSEM 中断的累计次数。两路都开时理论值 2 × 3052/s;
 *          明显偏小就说明通知被合并或丢掉了 —— 那会让 CH1/CH2 的搬运
 *          游标落后, 进而 missed 涨、环上出现空洞
 *   cmiss  CM4 主循环来不及扫而丢掉的半帧数 (就是这个数在环上凿洞)
 *   chN=   pushed / overrun / missed
 *            pushed  成功搬进 SDRAM 的半帧数
 *            overrun MDMA 还没搬完又来通知, 这一帧直接扔掉
 *            missed  游标落后了多少帧, 也就是被跳过的半帧数
 * ------------------------------------------------------------------------- */
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
     * 这一列是关键: irq / pushed / missed 都是累计值, 只有知道"这一行是
     * 隔了多久打出来的", 才能把累计值换算成每秒速率。而 defaultTask 优先级
     * 比 TouchGFX 低, 它到底隔多久才被轮到一次, 光看代码看不出来 —— 必须量。 */
    const uint32_t nowCyc = OSC_PROBE_CYC;
    const uint32_t dtCyc = (s_prevReportCyc != 0u) ? (nowCyc - s_prevReportCyc) : 0u;
    const uint32_t dt = to_us(dtCyc, cyc_per_us);
    s_prevReportCyc = nowCyc;

    char *p = s_line;

    p = put_kv(p, "[osc] dt", dt);
    p = put_kv(p, " irq", m7_diag_irq);
    p = put_kv(p, " cmiss", OSC_CTRL->diag_miss);

    p = put_str(p, " | ch1=");
    p = put_u32(p, m7_diag_ch1.pushed);  *p++ = '/';
    p = put_u32(p, m7_diag_ch1.overrun); *p++ = '/';
    p = put_u32(p, m7_diag_ch1.missed);

    p = put_str(p, " ch2=");
    p = put_u32(p, m7_diag_ch2.pushed);  *p++ = '/';
    p = put_u32(p, m7_diag_ch2.overrun); *p++ = '/';
    p = put_u32(p, m7_diag_ch2.missed);

    p = put_str(p, "\r\n");

    /* 顺序发, 超时给足。原来这里还掐表记 g_dbg_uart_cyc (发一行耗了多少),
     * 那列已经砍掉 —— 但超时不能省, 而且 HAL 的毫秒时基走 TIM7, TIM7 被饿住
     * 的话这里会一直等下去。 */
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)s_line,
                            (uint16_t)(p - s_line), 200u);
}
