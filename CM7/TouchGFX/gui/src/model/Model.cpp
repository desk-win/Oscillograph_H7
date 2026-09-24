#include <gui/model/Model.hpp>
#include <gui/model/ModelListener.hpp>
#include "osc_probe.h"

/* ===========================================================================
 * 分半开关 —— 只用来定位"卡在哪", 定位完请都改回 0
 *
 *   SKIP_BUILD = 1: 不读 SDRAM, 但照样把上一屏数据送去画。
 *                   画图的工作量不变, 少掉的只有那 16384 次 SDRAM 读。
 *   SKIP_GRAPH = 1: 照常读 SDRAM、照常抽点, 但不通知 View, 于是这一帧
 *                   没有脏区, 框架什么都不用渲染。
 *
 * 两次烧录、各看一眼串口上打出来的 frame, 就知道是"读数据"还是"画图"占了
 * 大头。
 * ========================================================================= */
#define OSC_DBG_SKIP_BUILD  0
#define OSC_DBG_SKIP_GRAPH  0

/* ===========================================================================
 * 调优探针 (osc_probe.h) —— 量一帧里各段各花了多少
 *
 * 处理点击的代码和 tick() 在同一个任务里, 所以"开关一开画面就卡、按关闭要
 * 等一会儿"这种症状, 根子必然在 tick 这一帧多花掉的时间上。具体怎么读那几
 * 个计数器, 看 osc_probe.h 的文件头。
 *
 * 探针本身不在这里实现, 这里只负责在正确的时刻取值。
 * ========================================================================= */

Model::Model() : modelListener(0), tickDiv(0)
{
    OSC_Probe_Init();

    for (int ch = 0; ch < 2; ch++)
    {
        for (uint32_t i = 0; i < OSC_TRACE_POINTS; i++)
        {
            traceY[ch][i] = 0;
        }
    }
}

void Model::startAcquisition(osc_trace_ch_t ch)
{
    OSC_Trace_Start(ch);
    tickDiv = 0;    /* 下一个 tick 就试着出一屏, 不用等节流 */
}

void Model::stopAcquisition(osc_trace_ch_t ch)
{
    OSC_Trace_Stop(ch);

    /* 注意这里没有清屏 —— 那是 View 的事 (拨开关时它直接把那一路擦掉, 因为
     * 两路都停了之后 tick 会提前返回, 没有哪一帧能把"清空"送出去)。
     *
     * Model 这边 traceY 里那一屏还留着, 但 OSC_Trace_Build 对停掉的通道返回
     * 0, 所以它不会再被更新。 */
}

bool Model::isRunning(osc_trace_ch_t ch) const
{
    return OSC_Trace_IsRunning(ch) != 0;
}

void Model::tick()
{
    /* 帧周期探针放在最前面 —— 没在采集的时候也要量, 那是"不卡"的基准值,
     * 有对照才知道慢了多少。 */
    static uint32_t lastEntry;
    const uint32_t now = OSC_PROBE_CYC;
    g_dbg_tick_cnt++;   /* 累计调用次数 —— 除以 dt 就是真实帧率 */
    if (lastEntry != 0u)
    {
        const uint32_t gap = now - lastEntry;
        g_dbg_gap_cyc = gap;
        if (gap > g_dbg_gap_max_cyc) g_dbg_gap_max_cyc = gap;
        if (g_dbg_gap_min_cyc == 0u || gap < g_dbg_gap_min_cyc)
        {
            g_dbg_gap_min_cyc = gap;
        }
        if (gap > g_osc_probe_stall_cyc)       /* > 50 ms = 卡了一回 */
        {
            g_dbg_stall_cnt++;
        }
    }
    lastEntry = now;

    /* 两个通道都停了才没什么可做。只要还有一路在跑, 下面就得照常走 ——
     * 停掉的那一路在 OSC_Trace_Poll / OSC_Trace_Build 里各自被跳过。 */
    if (!OSC_Trace_IsRunning(OSC_TRACE_CH1) && !OSC_Trace_IsRunning(OSC_TRACE_CH2))
    {
        return;
    }

    /* 注意这里没有 M7_OSC_Poll(): tick 是 60 Hz (16.7 ms), 而半帧只有
     * 655 µs, 挂在这里每次会跳掉 25 个半帧、只搬 1 个, 环里 96% 是空洞。
     * 搬运实际由 CM4 的 HSEM 通知驱动 (oscillograph.c 里的 HSEM1_IRQHandler),
     * 节奏天然跟着 CM4 走。详见 oscillograph.h 文件头的说明。
     *
     * 想知道 HSEM 有没有在响, 看 m7_diag_irq。 */

    /* ① 触发锁存: 候选触发点要冻着等到右边 6144 个 word 补齐才会被采用 (约
     *    2 ms)。所以这一步必须每个 tick 都跑, 而不是只在重画时跑 —— 只在重画
     *    时跑的话, 显示会白白多滞后一个重画周期。只读几个共享变量, 可以忽略
     *    不计。 */
    OSC_Trace_Poll();

    /* ② 抽点重画: 两个通道一共读 16384 个 word, 约 650 µs, 所以节流到 20 Hz。 */
    if (tickDiv > 0)
    {
        tickDiv--;
        return;
    }
    tickDiv = OSC_TRACE_REFRESH_TICKS - 1u;

    const uint32_t tBuild = OSC_PROBE_CYC;
#if OSC_DBG_SKIP_BUILD
    /* 分半用: 不读 SDRAM, 拿上一屏的旧数据接着往下走, 画图工作量不变 */
    (void)OSC_Trace_Build;
    const bool ok1 = true;
    const bool ok2 = true;
#else
    const bool ok1 = OSC_Trace_Build(OSC_TRACE_CH1, traceY[0]) != 0;
    const bool ok2 = OSC_Trace_Build(OSC_TRACE_CH2, traceY[1]) != 0;
#endif
    g_dbg_build_cyc = OSC_PROBE_CYC - tBuild;

    /* 两个都返回 0 就不通知 View —— 屏幕上留着上一次的波形, 不会有半截波形
     * 闪一下。(这一路停着、或者刚 Start 还没凑够一屏, 都会返回 0。)
     *
     * 只有一路返回 0 的时候照样要通知, 但把那两个标志一起传下去: 缓冲里那一
     * 路是旧数据, 控件按 fresh 决定不画它。不能靠"整帧不发"来挡 —— 另一路还
     * 在跑, 这一帧不发会把它的刷新也一起掐掉。 */
    if ((ok1 || ok2) && modelListener != 0)
    {
#if OSC_DBG_SKIP_GRAPH
        g_dbg_graph_cyc = 0u;       /* 分半用: 只量抽点, 不画 */
#else
        const uint32_t tGraph = OSC_PROBE_CYC;
        modelListener->traceUpdated(traceY[0], traceY[1], ok1, ok2);
        g_dbg_graph_cyc = OSC_PROBE_CYC - tGraph;
#endif
    }
}
