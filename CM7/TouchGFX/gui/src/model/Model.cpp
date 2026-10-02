#include <gui/model/Model.hpp>
#include <gui/model/ModelListener.hpp>
#include "osc_probe.h"
#include "encoder.h"

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

Model::Model() : modelListener(0), tickDiv(0), logicNcols(0)
{
    OSC_Probe_Init();

    for (int ch = 0; ch < 2; ch++)
    {
        for (uint32_t i = 0; i < OSC_TRACE_POINTS; i++)
        {
            traceY[ch][i] = 0;
        }
    }

    for (uint32_t i = 0; i < LOG_TRACE_COLS; i++)
    {
        logicLevel[i]  = 0;
        logicChange[i] = 0;
    }
}

void Model::startLogicCapture()
{
    /* 里面会先把示波器两路停掉 (互斥), 见 logic_trace.c。 */
    LOG_Trace_Start();
}

void Model::stopLogicCapture()
{
    LOG_Trace_Stop();

    /* 注意这里没有清屏, 也没有丢掉 logicLevel/logicChange —— 那是**结果**,
     * 用户要能一直看着它。View 那边拨开关时也不擦 (见 LogicView.cpp)。 */
}

bool Model::isLogicRunning() const
{
    return LOG_Trace_IsRunning() != 0;
}

void Model::turnLogicEncoder(int delta)
{
    /* 只是转交。往哪个方向调 (调时基还是滚窗口) 由 logic_trace.c 按当前视图
     * 模式决定; 档位表 / 窗口起点 / 上下界 / "采集中不响应" 也全在那边 ——
     * 那些判断要用采样序号, 是那一层的语言, 不该渗到 GUI 这一侧来。
     *
     * 注意这一句不会立刻改到屏幕: LOG_Trace_Turn 只置一个"该重算"的记号, 真正
     * 归约还是 LOG_Trace_Poll 里做的。tick() 里读编码器那一段特意排在
     * LOG_Trace_Poll() 前面, 所以同一帧就会算出新的窗口并交给 View —— 转一格
     * 当帧生效, 不用等下一帧。 */
    LOG_Trace_Turn(delta);
}

void Model::toggleLogicViewMode()
{
    LOG_Trace_ToggleViewMode();
}

uint32_t Model::getLogicViewMode() const
{
    return LOG_Trace_GetViewMode();
}

void Model::clearLogicData()
{
    /* 采集中的话这里面先停 (等于替用户拨了一次开关), 然后记录 / 结果全清。 */
    LOG_Trace_Clear();

    /* Model 自己这两片也要清 —— 它们才是 View 直接读的那一份。不清的话,
     * 从 Logic_Data 切回 Logic 时 setupScreen 拿到的 ncols 还是旧的, 屏幕上
     * 会原样画回来一遍。 */
    for (uint32_t i = 0u; i < LOG_TRACE_COLS; i++)
    {
        logicLevel[i]  = 0;
        logicChange[i] = 0;
    }
    logicNcols = 0u;

    /* 通知一声, ncols = 0 就是"没东西可画了"。
     *
     * 收到的是哪个 Presenter 取决于现在停在哪一屏 —— Cancel 是在 Logic_Data
     * 上按的, 所以 modelListener 是那一屏的 Presenter, 它的 logicUpdated 是
     * 默认实现 (什么都不做)。也就是说 **Logic 那个控件不会因为这一句被擦掉** ——
     * 它靠的是 LogicView::setupScreen() 里按 ncols == 0 自己清一次。 */
    if (modelListener != 0)
    {
        modelListener->logicUpdated(logicLevel, logicChange, 0u);
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

    /* ---- 编码器 ----
     *
     * 必须**每个 tick 都读**, 不管当前是哪一屏: Encoder_Increment() 是有状态的
     * (它写自己那份 last_count), 不读的话增量会一直攒着, 等切回逻辑屏的那一下
     * 一次性爆发 —— 表现出来是"在别的屏上转了转, 回来波形突然跳到底"。
     *
     * 派发到 modelListener 而不是在这里判"是不是逻辑屏": 切屏时 MVPApplication
     * 会重新 bind, 所以 modelListener 恒等于当前活动屏, 不覆盖 encoderTurned 的
     * 屏 (示波器那两屏) 拿到的是默认空实现, 等于把这把转动丢掉。
     *
     * 放在 LOG_Trace_Poll() **之前**: 缩放只是置一个"该重算"的记号, 真正归约是
     * Poll 干的 —— 这样同一帧里就能把新档位算出来并交出去, 转一格当天生效,
     * 而不是等到下一帧。
     *
     * 位置和下面那个"两路都停了就提前返回"的守卫没关系: 这一段在它前面。 */
    {
        const int16_t enc = Encoder_Increment();
        if (enc != 0 && modelListener != 0)
        {
            modelListener->encoderTurned(enc);
        }
    }

    /* ---- 编码器上那个开关 (长按 = 切"调时基 / 滚窗口"两种模式) ----
     *
     * 每个 tick 都调, 不用卡节拍: Encoder_Key 内部按 HAL_GetTick 的毫秒计时
     * (LONG_PRESS_TIME_MS), 不数调用次数 —— 这一帧是 16.7 ms 还是 20 ms 都不
     * 影响长按判多久。
     *
     * 单击 (KEY_EVENT_PRESS) 直接丢掉, 不派发: 目前没有哪一屏要用它。
     *
     * 和 encoderTurned 一套派发: 收到它的恒是当前活动屏, 别的屏拿到的是默认空
     * 实现, 等于把这下长按丢掉 —— 这正是想要的, 只有逻辑屏有两种模式可切。
     *
     * 放在 LOG_Trace_Poll() 前面: 切完模式不需要重算那一屏 (画面不变), 但跟在
     * 后面的 Poll 顺手就把这一帧该出的图出了, 顺序上不欠任何东西。 */
    if (Encoder_Key() == KEY_EVENT_LONGPRESS && modelListener != 0)
    {
        modelListener->encoderLongPressed();
    }

    /* 逻辑分析仪: 采半帧、找跳变、写记录全在 HSEM 中断里做完了 (和示波器同一种
     * 驱动方式, 见 logic_trace.c 的 LOG_Trace_OnHalfFrame); 这里只干没期限的那
     * 一半 —— 记录满 / 空闲超时之后把记录抽成一屏列数据交给控件, 外加摆 LED。
     *
     * 必须放在下面那个"两路都停了就提前返回"的守卫**之前**: 采集是在中断里停
     * 的, 停完那一屏的归约得靠这里做, 而那一刻恰恰常常是示波器两路都停着的情
     * 况 —— 放在守卫后面的话, 那一帧算好的列就永远发不出去, 屏幕上一直是空的。
     *
     * 代价是空闲时多一次读 + 一次比较, 可以忽略。 */
    LOG_Trace_Poll();

    {
        const uint16_t* lvl = 0;
        const uint16_t* chg = 0;
        uint32_t        n   = 0;

        /* GetCols 是取走式的: 只在真的算出新一屏的那一拍返回 1, 所以这里不是
         * 每个 tick 都重画 —— 光有这一处节流就够了, 不用再数 tick。 */
        if (LOG_Trace_GetCols(&lvl, &chg, &n) != 0u)
        {
            for (uint32_t c = 0u; c < n; c++)
            {
                logicLevel[c]  = lvl[c];
                logicChange[c] = chg[c];
            }
            logicNcols = n;

            /* 送给 View。没有 fresh 标志那一套 —— 这里的语义就是"算出新的了",
             * 不用区分哪几路。 */
            if (modelListener != 0)
            {
                modelListener->logicUpdated(logicLevel, logicChange, logicNcols);
            }
        }
    }

    /* 两个通道都停了才没什么可做。只要还有一路在跑, 下面就得照常走 ——
     * 停掉的那一路在 OSC_Trace_Poll / OSC_Trace_Build 里各自被跳过。
     *
     * 逻辑采集也算"有一路在跑": 它在跑的时候, 上面的 LOG_Trace_Poll 每个 tick
     * 都要被调到 —— 启动门 (等 CM4 起来、等不到就超时弹回开关) 和 LED 都在那儿,
     * 少调几次就少几次兜底。 */
    if (!OSC_Trace_IsRunning(OSC_TRACE_CH1) && !OSC_Trace_IsRunning(OSC_TRACE_CH2) &&
        !LOG_Trace_IsRunning())
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
