#include <gui/model/Model.hpp>
#include <gui/model/ModelListener.hpp>
#include "osc_probe.h"
#include "encoder.h"
#include "logic_save.h"    /* LOG_Save_Init / LOG_Save_Request */
#include "oscillograph.h"   /* M7_OSC_SetFrontend —— 换放大倍数 (引脚在 CM4 那边) */

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

/* 初始化列表的顺序必须和 Model.hpp 里**成员声明的顺序**一致, 否则 -Wreorder
 * 会警告 (而且真跑起来是按声明顺序初始化的, 列表写成什么样都没用)。 */
Model::Model() : modelListener(0), tickDiv(0), logicNcols(0),
                 committedProtoMode(LOG_PROTO_UART), draftProtoMode(LOG_PROTO_UART),
                 oscSelected(OSC_TRACE_CH1),
                 logicPubSeq(0u), logicDecodeReq(false)
{
    OSC_Probe_Init();

    /* 编码器的初始状态: 选中 CH1, 两路都是"调时基", 放大倍数都是 X1。
     *
     * 时基档位不在这儿初始化 —— 它存在 oscillograph_trace.c 的 s_tb_idx 里,
     * 那边有静态初值 (OSC_TB_DEFAULT), 这里再"初始化"一遍反而会变成两个真相。
     *
     * 放大倍数在这儿存一份是对的: 它是纯界面状态, CM4 那边只是执行机构。X1 要
     * 和 CM4 ctrl_defaults() 里的 0 (也是 X1) 以及 CM4 main.c 开机那两句
     * OSC_Chx_Multiple(1, OSC_COUPLE_DC) 对上 —— 三处都是 X1, 所以开机不用动任何引脚。
     *
     * 交直流耦合同理: 默认直流, 上面那三处也是直流。草稿那份跟着一起给初值,
     * 免得有人还没进过设置屏就先调了 getOscCoupleDraft() 拿到个 0 (交流)。 */
    for (int c = 0; c < 2; c++)
    {
        oscMode[c] = OSC_MODE_TIME;
        oscGain[c] = OSC_GAIN_DEFAULT;

        committedCouple[c] = OSC_COUPLE_DC;
        draftCouple[c]     = OSC_COUPLE_DC;
    }

    /* 解码层的默认配置 (UART + 设置屏候选值里第一个)。必须赶在任何人调
     * LOG_Analyze_Request() 之前跑过 —— 那要等到用户点进 Logic_Data 屏了, 所以
     * 这儿是最早也最合适的位置。它不碰任何硬件, 在 main() 之前跑是安全的。 */
    LOG_Analyze_Init();

    /* 存卡任务的请求信号量 (任务本体在 freertos.c 里创建) */
    LOG_Save_Init();

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

    /* 协议配置的取值全给 0 —— 也就是说每种协议开机都用候选值列表里第一个:
     * 9600 / 1 停止位 / 7 位数据 / 无校验 / CPOL=1 / CPHA=1 / CS=high / 125k。
     * 那份候选值列表在设置屏的 .cpp 里, 想看全貌去那儿。
     *
     * 已提交和草稿两份都清 —— 开机时本来就没人在编辑, 两份必须是一致的。
     * (这两份和 LOG_Analyze_Init() 那份默认值也是同一个东西, 三方对得上。) */
    for (uint32_t m = 0; m < LOG_PROTO_COUNT; m++)
    {
        for (uint32_t r = 0; r < LOG_PROTO_CFG_ROWS; r++)
        {
            committedCfgVal[m][r] = 0u;
            draftCfgVal[m][r]     = 0u;
        }
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

uint8_t Model::getLogicCfgValue(uint32_t mode, uint32_t row) const
{
    /* 越界返回 0 —— 调用方是设置屏, 正常情况下不会越界, 这里只是不让它读到数组外。 */
    if (mode >= LOG_PROTO_COUNT || row >= LOG_PROTO_CFG_ROWS)
    {
        return 0u;
    }
    return draftCfgVal[mode][row];
}

void Model::setLogicCfgValue(uint32_t mode, uint32_t row, uint8_t value)
{
    if (mode >= LOG_PROTO_COUNT || row >= LOG_PROTO_CFG_ROWS)
    {
        return;
    }
    draftCfgVal[mode][row] = value;
}

void Model::beginLogicEdit()
{
    /* 已提交 → 草稿, 整个覆盖。
     *
     * 每次进设置屏都要走一遍: 上一次在这屏上改了没 Save (按了 Cancel) 的话,
     * 草稿还是脏的 —— 不覆盖就会把上一次作废掉的改动又摆回屏幕上, 看起来像
     * "Cancel 没生效"。
     *
     * 拷的是**下标**那一份, 不碰 logic_analyze.c 的真值那份 —— 编辑期间解码
     * 层该用什么配置还是什么配置 (这正是"没 Save 就不生效"的落点)。 */
    draftProtoMode = committedProtoMode;

    for (uint32_t m = 0; m < LOG_PROTO_COUNT; m++)
    {
        for (uint32_t r = 0; r < LOG_PROTO_CFG_ROWS; r++)
        {
            draftCfgVal[m][r] = committedCfgVal[m][r];
        }
    }
}

void Model::commitLogicConfig(const log_ana_cfg_t& cfg)
{
    /* 草稿转正。整份拷 (所有模式、所有行), 不是只拷当前这一个模式 —— 用户可能
     * 在这个屏上把四种模式挨个配了一遍才按 Save。 */
    committedProtoMode = draftProtoMode;

    for (uint32_t m = 0; m < LOG_PROTO_COUNT; m++)
    {
        for (uint32_t r = 0; r < LOG_PROTO_CFG_ROWS; r++)
        {
            committedCfgVal[m][r] = draftCfgVal[m][r];
        }
    }

    /* 真值那份推给解码层 —— 它才是解码唯一认的。cfg 是 View 拿这个草稿翻出来
     * 的 (翻表在那边, 候选值的顺序本来就是它的约定), 和上面那份不会不一致。 */
    LOG_Analyze_SetConfig(&cfg);
}

void Model::requestLogicDecode()
{
    /* 只置记号, 真正发请求在 tick 里 (见下面那一段) —— 采集还开着的时候代数
     * 还是 0, 那时候发出去会被当成"还没采过"丢掉。 */
    logicDecodeReq = true;
}

void Model::saveLogicToSd()
{
    /* 只通知: 借表 + 写卡在 logicSave 任务里做 (GUI 帧里一个文件操作都没有)。
     * 存卡期间再按一次会被任务那边丢掉 —— 一次只写一份。 */
    LOG_Save_Request();
}

const log_ana_item_t* Model::getLogicItems(uint32_t* count) const
{
    return LOG_Analyze_GetItems(count);
}

void Model::clearLogicData()
{
    /* 采集中的话这里面先停 (等于替用户拨了一次开关), 然后记录 / 结果全清。 */
    LOG_Trace_Clear();

    /* 解码结果一起丢 —— 它和记录是同一轮采集的产物, 记录没了它就没有意义。
     * LOG_Analyze_Clear() 把发布条数清 0 (在途的那次解码也会被丢弃), 所以下次
     * 进 Logic_Data 屏 setupScreen 取到的是 0 条, 不会把上一轮的字节再摆一遍。 */
    LOG_Analyze_Clear();

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

/* ===========================================================================
 * 示波器屏的编码器 —— 短按换通道, 长按换调什么, 转动改值
 *
 * 状态怎么分家
 *   时基那一份存在 oscillograph_trace.c 里 (s_tb_idx): 换档要清触发锁存, 而
 *   锁存是那边的私有状态, 隔着 Model 去清很别扭。这里只是转发 + 记一个"当前
 *   在调哪一路、调什么"的界面状态。
 *
 *   放大倍数那一份就在这儿 (oscGain): 它是一个纯粹的"用户设置", 和抽点那套
 *   没有任何关系, 唯一的作用是往共享块里写一个字让 CM4 去摆 MUX 引脚。
 *
 * 两路各调各的 —— 短按切的是"编码器现在管哪一路", 切过去的瞬间另一路停在原
 * 值上不动。
 * ========================================================================= */

/* 初始状态 (选中 CH1, 调时基, 两路都是 X1) 在构造函数里给 —— 见那边。 */

void Model::turnOscEncoder(int16_t delta)
{
    if (delta == 0) return;                 /* 调用方本来就只在非 0 时才叫, 兜一下 */

    const uint32_t ch = oscSelected;

    if (oscMode[ch] == OSC_MODE_SCOPE)
    {
        /* 倍数档: 0..7 → X1..X128。夹到边界 (不回绕): 转到 X128 就该停住, 再
         * 往右一把跳回 X1 的话用户会以为"增益炸了"。
         *
         * 夹完还是原值就什么都不做 —— 尤其不要白写一遍共享块: 那会让 CM4 那边
         * 的继电器/模拟开关再动一次 (frontend_poll 是按"和上一轮不一样"才动的,
         * 但我们写下去的值没变它本来就不动; 这里是省掉一次跨核写的开销)。 */
        int32_t g = (int32_t)oscGain[ch] + (int32_t)delta;
        if (g < 0) g = 0;
        if (g > (int32_t)OSC_GAIN_STEPS - 1) g = (int32_t)OSC_GAIN_STEPS - 1;

        if ((uint32_t)g != oscGain[ch])
        {
            oscGain[ch] = (uint32_t)g;
            M7_OSC_SetFrontend(ch, (uint32_t)g);
        }
    }
    else
    {
        /* 时基档: 上下界和"值没变就不动"都在 OSC_Trace_SetTimebase 里 (见那边
         * 的说明 —— 它还要负责清触发锁存)。 */
        OSC_Trace_SetTimebase((osc_trace_ch_t)ch,
                              OSC_Trace_GetTimebase((osc_trace_ch_t)ch) + (int)delta);
    }
}

void Model::toggleOscSelection()
{
    /* 就在两路之间跳。没有第三路, 所以直接取反。 */
    oscSelected ^= 1u;
}

void Model::toggleOscMode()
{
    const uint32_t ch = oscSelected;
    oscMode[ch] = (oscMode[ch] == OSC_MODE_TIME) ? OSC_MODE_SCOPE : OSC_MODE_TIME;
}

void Model::getOscUiState(osc_ui_state_t* out) const
{
    out->selected = (uint8_t)oscSelected;

    for (uint32_t c = 0u; c < 2u; c++)
    {
        out->mode[c] = (uint8_t)oscMode[c];
        out->gain[c] = (uint8_t)oscGain[c];
        out->tb[c]   = (uint8_t)OSC_Trace_GetTimebase((osc_trace_ch_t)c);
    }
}

void Model::syncOscUi()
{
    /* 进屏时把 Model 记得的设置重新推一遍。正常情况下是白做 —— 但两核是各
     * 自独立复位的 (调试时经常只烧一个核), 那边 osc_ctrl 里的 cfg_gain_* 可能
     * 还是上一次上电留下的值, 而 CM4 只有"和上一轮不一样"才会动 MUX 引脚
     * (frontend_poll)。不推这一下的话表现是"屏幕写着 X1, 实际还是上次那个倍
     * 数"—— 而且怎么转编码器都只在错的基准上加减, 极难查。
     *
     * 交直流耦合是同一个理由、同一时刻推: 它是跟放大倍数相邻的那一格, 也是
     * CM4 只在变了才动引脚。推的是**已提交**那份 —— 设置屏上的草稿没按 Save
     * 之前不该到硬件上去 (也没机会到这里, 推的入口只有一个)。
     *
     * 时基那边不用推: 档位存在 CM7 自己的 osc_trace 里, 和 CM4 无关。 */
    for (uint32_t c = 0u; c < 2u; c++)
    {
        M7_OSC_SetFrontend(c, oscGain[c]);
        M7_OSC_SetCoupling(c, committedCouple[c]);
    }
}

void Model::beginOscEdit()
{
    /* 已提交 → 草稿, 和 beginLogicEdit() 一个道理: 上一次在这屏上拨了没 Save
     * (按了 Cancel) 的话草稿还是脏的, 不覆盖就会把上一次作废的改动又摆回屏幕上,
     * 看起来像"Cancel 没生效"。 */
    for (uint32_t c = 0u; c < 2u; c++)
    {
        draftCouple[c] = committedCouple[c];
    }
}

uint32_t Model::getOscCoupleDraft(osc_trace_ch_t ch) const
{
    return draftCouple[(ch == OSC_TRACE_CH2) ? 1u : 0u];
}

void Model::setOscCoupleDraft(osc_trace_ch_t ch, uint32_t coup)
{
    /* 只有正好是 OSC_COUPLE_AC 才当交流, 其余一律直流 —— 和 CM4 的 frontend_dc()
     * 判法一致, 这样"存进去什么"和"引脚上是什么"永远是同一个映射。 */
    draftCouple[(ch == OSC_TRACE_CH2) ? 1u : 0u] =
        (coup == OSC_COUPLE_AC) ? OSC_COUPLE_AC : OSC_COUPLE_DC;
}

void Model::commitOscEdit()
{
    for (uint32_t c = 0u; c < 2u; c++)
    {
        committedCouple[c] = draftCouple[c];

        /* 两路都写, 不管改没改 —— CM4 那边拿旧值比过才动引脚, 多写一次不会让
         * 耦合脚多抖一下 (和 turnOscEncoder 里那句"值没变就不写"省下的其实只有
         * 一次跨核写, 这儿不值得为它多一个分支)。 */
        M7_OSC_SetCoupling(c, committedCouple[c]);
    }
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

    /* ---- 编码器上那个开关 ----
     *
     * 每个 tick 都调, 不用卡节拍: Encoder_Key 内部按 HAL_GetTick 的毫秒计时
     * (LONG_PRESS_TIME_MS), 不数调用次数 —— 这一帧是 16.7 ms 还是 20 ms 都不
     * 影响长按判多久。
     *
     * 短按和长按都要派发: 逻辑屏只用长按 (切"调时基/滚窗口"), 示波器屏两个都
     * 用 (短按换选中哪一路, 长按换调时基还是调倍数)。Encoder_Key() 是分开判的
     * —— 长按到点就直接返回 LONGPRESS, 不会先给一次 PRESS —— 所以同一次按下
     * 只会走到其中一个分支, 不会"短按和长按都触发一遍"。
     *
     * 一次只调一次 Encoder_Key(): 那是个状态机, 事件是**当场判出来的**, 不是存
     * 在队列里等人取。长按那一支把 longpress 置上再返回, 单击那一支把状态机推
     * 回 RELEASE 再返回 —— 所以同一 tick 里调第二次只会拿到 KEY_EVENT_RELEASE,
     * 事件本身已经过去了。
     *
     * 和 encoderTurned 一套派发: 收到它的恒是当前活动屏, 别的屏拿到的是默认空
     * 实现, 等于把这下按键丢掉 —— 这正是想要的。
     *
     * 放在 LOG_Trace_Poll() 前面: 切完模式不需要重算那一屏 (画面不变), 但跟在
     * 后面的 Poll 顺手就把这一帧该出的图出了, 顺序上不欠任何东西。 */
    if (modelListener != 0)
    {
        switch (Encoder_Key())
        {
        case KEY_EVENT_PRESS:
            modelListener->encoderPressed();
            break;

        case KEY_EVENT_LONGPRESS:
            modelListener->encoderLongPressed();
            break;

        default:
            /* KEY_EVENT_RELEASE: 绝大多数 tick 都走这儿 (没按, 或者按着的过程中) */
            break;
        }
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

    /* ---- 请求解码 (等这一轮收工) ----
     *
     * 进 Logic_Data 屏时采集很可能还开着, 那时候代数是 0 —— 先记着, 等这一轮
     * 收工 (记录满 / 空闲超时 / 手动停) 拿到代数再发给解码任务。 */
    if (logicDecodeReq)
    {
        const uint32_t gen = LOG_Trace_GetResultSeq();
        if (gen != 0u)
        {
            logicDecodeReq = false;
            LOG_Analyze_Request(gen);
        }
    }

    /* ---- 协议解码结果 ----
     *
     * 解码在独立任务里做 (freertos.c 里创建): 这里只比结果代数, 变了才取 ——
     * 和上面 LOG_Trace_GetCols 的取走式是同一个套路。取的时候用 PubLock 包住
     * 整段通知 (View 是同步格式化进控件的), 免得任务正好在换发布指针。
     *
     * **必须在下面那个"两路都停了就提前返回"的守卫之前**: 用户点进 Logic_Data
     * 屏时采集在不在跑都无所谓, 结果一到就得通知 View。 */
    {
        const uint32_t ps = LOG_Analyze_GetPubSeq();
        if (ps != logicPubSeq)
        {
            logicPubSeq = ps;

            uint32_t cnt = 0u;
            LOG_Analyze_PubLock();
            const log_ana_item_t* items = LOG_Analyze_GetItems(&cnt);
            if (modelListener != 0 && items != 0)
            {
                modelListener->logicDataUpdated(items, cnt);
            }
            LOG_Analyze_PubUnlock();
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

    /* ② 抽点重画: 基准档两个通道一共读 16384 个 word, 约 650 µs, 所以节流到
     *    20 Hz。档位越慢读得越多 (W=256 时 10.4 ms, 塞进一帧会明显顿一下), 所以
     *    真正的分频数按当前档位取, 见 OSC_Trace_RefreshTicks()。
     *
     *    取的是"两路里在跑的那个较大值", 转编码器换档之后下一帧就跟着变 ——
     *    tickDiv 是这一帧开始时减的, 换档那一帧最多多跑一次旧节奏, 无所谓。
     *
     *    -1 是因为 tickDiv 是"还要再等几帧": 减到 0 的那一帧就该画了, 写回时给
     *    的必须是"距下一次还有几帧", 也就是 N-1。恒 >= 1, 不会下溢。 */
    if (tickDiv > 0)
    {
        tickDiv--;
        return;
    }
    tickDiv = OSC_Trace_RefreshTicks() - 1u;

    const uint32_t tBuild = OSC_PROBE_CYC;
    /* 每一路这一屏的测量结果 (峰峰值 / 频率)。放在这里当局部量: 它只活到下面
     * 那一次 traceUpdated —— View 是同步把它格式化成文字写进控件的, 不存指针。 */
    osc_meas_t meas1;
    osc_meas_t meas2;
#if OSC_DBG_SKIP_BUILD
    /* 分半用: 不读 SDRAM, 拿上一屏的旧数据接着往下走, 画图工作量不变。
     * 测量那两份一起清掉, 让它表现得和"没量到"一样 —— 否则屏幕上会挂着上一帧
     * 的数字不动, 看不出这条路径根本没读数据。 */
    (void)OSC_Trace_Build;
    meas1.valid = 0u; meas1.has_freq = 0u; meas1.lo = 0u; meas1.hi = 0u; meas1.freq_mhz = 0u;
    meas2 = meas1;
    const bool ok1 = true;
    const bool ok2 = true;
#else
    const bool ok1 = OSC_Trace_Build(OSC_TRACE_CH1, traceY[0], &meas1) != 0;
    const bool ok2 = OSC_Trace_Build(OSC_TRACE_CH2, traceY[1], &meas2) != 0;
#endif
    g_dbg_build_cyc = OSC_PROBE_CYC - tBuild;

    /* 两个都返回 0 就不通知 View —— 屏幕上留着上一次的波形, 不会有半截波形
     * 闪一下。(这一路停着、或者刚 Start 还没凑够一屏, 都会返回 0。)
     *
     * 只有一路返回 0 的时候照样要通知, 但把那两个标志一起传下去: 缓冲里那一
     * 路是旧数据, 控件按 fresh 决定不画它。不能靠"整帧不发"来挡 —— 另一路还
     * 在跑, 这一帧不发会把它的刷新也一起掐掉。
     *
     * 测量值跟着一起走 (measN->valid 就是 freshN): View 那边同一个回调里既换
     * 波形又换数字, 屏幕上的波形和数字因此永远是同一屏的 —— 不会出现"数字是
     * 这一屏的、波形还在上一屏"。 */
    if ((ok1 || ok2) && modelListener != 0)
    {
#if OSC_DBG_SKIP_GRAPH
        g_dbg_graph_cyc = 0u;       /* 分半用: 只量抽点, 不画 */
#else
        const uint32_t tGraph = OSC_PROBE_CYC;
        modelListener->traceUpdated(traceY[0], traceY[1], &meas1, &meas2, ok1, ok2);
        g_dbg_graph_cyc = OSC_PROBE_CYC - tGraph;
#endif
    }
}
