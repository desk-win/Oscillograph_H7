#ifndef MODEL_HPP
#define MODEL_HPP

#include <stdint.h>
#include <oscillograph_trace.h>
#include <logic_trace.h>

class ModelListener;

/**
 * 逻辑分析仪采的是哪种协议的数据线 —— Logic_Setting 屏上那个 Mode。
 *
 * 顺序就是 mode_switch 按键的轮询顺序 (UART → I2C → SPI → CAN → 回到 UART,
 * 见 Logic_SettingPresenter::nextLogicProtoMode), 所以这里改顺序 = 改按键顺序。
 * 默认 UART, 见 Model::Model()。
 *
 * 为什么放在 GUI 这一侧、而不是像视图模式那样放进 logic_trace.c: 现在它只决定
 * 设置屏上写哪几个字, 采集和归约都还用不到它。等解码器真的要按它分派时, 该和
 * 视图模式一样挪到 logic_trace.c —— 那边才是 CM7 侧逻辑分析仪状态的落脚点,
 * 也是唯一能把它发给 CM4 的地方。
 */
enum LogicProtoMode
{
    LOG_PROTO_UART = 0,
    LOG_PROTO_I2C,
    LOG_PROTO_SPI,
    LOG_PROTO_CAN,
    LOG_PROTO_COUNT
};

/* 每个协议最多显示几行可配置项 —— 就是设置屏 box2 里那几个 setting 文字区。
 *
 * 这个数改不动屏上的控件 (那几个是 Designer 放的), 所以设置屏里有一条 static_assert
 * 把它和控件个数钉在一起: Designer 里加了第五个 setting, 那边会编译不过, 而不是
 * 悄悄少显示一行。 */
#define LOG_PROTO_CFG_ROWS  4

class Model
{
public:
    Model();

    void bind(ModelListener* listener)
    {
        modelListener = listener;
    }

    /**
     * 每个 TouchGFX tick 调一次 (60 Hz)。里面做三件事, 见 .cpp。
     */
    void tick();

    /**
     * 屏幕上的开关拨到 ON: 让 CM4 开始采这一路, 屏幕这边也开始刷新。
     */
    void startAcquisition(osc_trace_ch_t ch);

    /**
     * 屏幕上的开关拨到 OFF: 让 CM4 停掉这一路的采集。
     *
     * 屏幕上那一路怎么处理不归 Model 管 —— View 拨开关的时候会立刻把它擦掉
     * (见 OscillographView.cpp), 因为 Model 这边已经没有 tick 会把"清空"这个
     * 动作送出去了。
     */
    void stopAcquisition(osc_trace_ch_t ch);

    /**
     * 这一路采集是否在跑。
     */
    bool isRunning(osc_trace_ch_t ch) const;

    /**
     * 取一屏波形。ch 0 = CH1 (折线图 1), ch 1 = CH2 (折线图 2)。
     * 返回的缓冲每通道 OSC_TRACE_POINTS 个点。
     */
    const uint16_t* getTrace(int ch) const
    {
        return traceY[ch & 1];
    }

    /* ---- 逻辑分析仪 ---- */

    /**
     * 逻辑屏上的开关拨到 ON。里面会把示波器两路停掉 (互斥, 见 logic_trace.c)。
     */
    void startLogicCapture();

    /** 逻辑屏上的开关拨到 OFF。已经采到的记录不动。 */
    void stopLogicCapture();

    /**
     * 丢掉这一次采集的结果, 回到"还没采过"的状态。
     *
     * Logic_Data 界面上的 Cancel 走这条路。清完之后屏幕该是空的, 下一次
     * startLogicCapture() 是从零开始的一轮 —— 和开机后第一次一样。
     *
     * Save (存 TF 卡) 以后要在这个界面里做, 那时候它读的是清理**之前**的数据,
     * 所以不能让 Model 自己在这条路径上清 —— 谁来调由界面决定。
     */
    void clearLogicData();

    /** 逻辑采集是否在跑。View 用它决定进界面时把开关摆到哪一面。 */
    bool isLogicRunning() const;

    /**
     * 取上一屏逻辑波形。两个出参都指到 Model 内部的缓冲上, 不用释放。
     * 返回列数 —— 0 表示还没采过 (或者采集完但一条记录都没有), 那就没什么
     * 可画的。
     */
    uint32_t getLogicCols(const uint16_t** level, const uint16_t** change) const
    {
        *level  = logicLevel;
        *change = logicChange;
        return logicNcols;
    }

    /**
     * 逻辑屏的编码器: 按**当前视图模式**调时基或者滚窗口。delta 是净格数,
     * 正 = 顺时针。
     *
     * 这一层只是把调用转给 LOG_Trace_Turn —— 往哪个方向走由 logic_trace.c 按
     * 模式自己决定。档位表、窗口起点、上下界、"采集中不响应" 全在那边, GUI
     * 这一侧不需要知道采样序号这回事。
     */
    void turnLogicEncoder(int delta);

    /**
     * 编码器上的开关长按: 在"调时基"和"滚窗口"两种视图模式之间切。
     * 点右下角那个开关走的也是这条路。
     *
     * 模式存在 logic_trace.c 里 (它要拿它决定 Turn 干什么), 这儿是转发。界面
     * 上那个开关的**图**不在这儿管 —— View 按 getLogicViewMode() 自己摆, 和
     * toggleButton1 一样的做法。
     */
    void toggleLogicViewMode();

    /** 当前视图模式 (LOG_VIEW_TIMEBASE / LOG_VIEW_SCROLL)。 */
    uint32_t getLogicViewMode() const;

    /* ---- 逻辑分析仪的协议模式 (Logic_Setting 屏上的 Mode) ---- */

    /** 当前协议模式 (LOG_PROTO_*)。 */
    uint32_t getLogicProtoMode() const
    {
        return logicProtoMode;
    }

    /**
     * 换一个协议模式。越界由调用方保证 —— 设置屏那条路是按 LOG_PROTO_COUNT
     * 取模轮询过来的。
     */
    void setLogicProtoMode(uint32_t mode)
    {
        logicProtoMode = mode;
    }

    /* ---- 协议配置的取值 (设置屏 box2 里那几行) ---- */

    /**
     * 第 mode 个模式的第 row 行, 选的是候选值里的第几个 (0 = 列表里第一个)。
     *
     * 存的是**下标**, 不是 9600 那种真值: 候选值表在设置屏那边 (它本来就要拿文本 id
     * 去写字), 这里只当一个不透明的格子记着。以后解码器要真值时, 在那边表里加一列
     * raw, 再写一个 Model → logic_trace.c 的桥把它翻过去 —— 现在没人要, 不提前做。
     *
     * 越界的 mode/row 都返回 0, 不会读到数组外。
     */
    uint8_t getLogicCfgValue(uint32_t mode, uint32_t row) const;

    /** 同上, 写。越界直接丢掉。 */
    void setLogicCfgValue(uint32_t mode, uint32_t row, uint8_t value);

protected:
    ModelListener* modelListener;

    /* 两片一屏波形。Model 活得和程序一样久 (它住在 FrontendHeap 的静态实例
     * 里), 所以放这儿不占栈 —— 两片一共 4 KB, 放栈上会很难受。 */
    uint16_t traceY[2][OSC_TRACE_POINTS];

    /* 重画节流: 数到 OSC_TRACE_REFRESH_TICKS 才抽一次点 */
    uint8_t tickDiv;

    /* 上一屏逻辑波形。和 traceY 一样放这儿不占栈 —— 两片各 520 × 2 字节 =
     * 2080 字节。Model 活得和程序一样久 (它住在 FrontendHeap 的静态实例里),
     * 所以留着它, 从别的界面切回逻辑屏时能照原样把上一屏画出来, 而不是等
     * 下一次采集。 */
    uint16_t logicLevel[LOG_TRACE_COLS];
    uint16_t logicChange[LOG_TRACE_COLS];
    uint32_t logicNcols;

    /* 设置屏上选的协议模式 (LOG_PROTO_*)。放这儿而不是逻辑屏的 View 里, 是因为
     * 它得活过切屏 —— Save/Cancel 都会离开这一屏, 再进来时要还是上次选的
     * 那个。开机默认 UART, 见构造函数。 */
    uint32_t logicProtoMode;

    /* [模式][第几行] = 选了第几个候选值。全 0 是开机默认值, 也就是每种协议都用
     * 候选值列表里第一个 (9600 / 1 停止位 / 7 位数据 / 无校验 / CPOL=1 ...)。 */
    uint8_t logicCfgVal[LOG_PROTO_COUNT][LOG_PROTO_CFG_ROWS];
};

#endif // MODEL_HPP
