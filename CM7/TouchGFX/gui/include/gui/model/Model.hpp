#ifndef MODEL_HPP
#define MODEL_HPP

#include <stdint.h>
#include <oscillograph_trace.h>
#include <logic_trace.h>
#include <logic_analyze.h>

class ModelListener;

/**
 * 逻辑分析仪采的是哪种协议的数据线 —— Logic_Setting 屏上那个 Mode。
 *
 * 顺序就是 mode_switch 按键的轮询顺序 (UART → I2C → SPI → CAN → 回到 UART,
 * 见 Logic_SettingPresenter::nextLogicProtoMode), 所以这里改顺序 = 改按键顺序。
 * 默认 UART, 见 Model::Model()。
 *
 * 这个枚举留在 GUI 这一侧 (它主要还是决定设置屏上写哪几个字), 但**它的值和
 * logic_analyze.h 的 LOG_ANA_* 必须一一对应** —— 解码器就是拿那个值分派的。
 * 下面几条 static_assert 把这件事钉住: 哪天有人在这儿插一个协议或者调一次顺序,
 * 编译就过不去, 而不是变成"界面选 UART、解码按 SPI 解"那种没人查得出来的错。
 */
enum LogicProtoMode
{
    LOG_PROTO_UART = 0,
    LOG_PROTO_I2C,
    LOG_PROTO_SPI,
    LOG_PROTO_CAN,
    LOG_PROTO_COUNT
};

static_assert((int)LOG_PROTO_UART  == (int)LOG_ANA_UART,        "LOG_PROTO_UART 和 LOG_ANA_UART 对不上");
static_assert((int)LOG_PROTO_I2C   == (int)LOG_ANA_I2C,         "LOG_PROTO_I2C 和 LOG_ANA_I2C 对不上");
static_assert((int)LOG_PROTO_SPI   == (int)LOG_ANA_SPI,         "LOG_PROTO_SPI 和 LOG_ANA_SPI 对不上");
static_assert((int)LOG_PROTO_CAN   == (int)LOG_ANA_CAN,         "LOG_PROTO_CAN 和 LOG_ANA_CAN 对不上");
static_assert((int)LOG_PROTO_COUNT == (int)LOG_ANA_PROTO_COUNT, "协议个数对不上");

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

    /* =======================================================================
     * 协议配置 —— 设置屏正在编辑的那一份 (草稿)
     *
     * 这一屏的语义是"改完按下 Save 才生效", 所以编辑期间动的是**草稿**:
     *
     *   进屏     beginLogicEdit()   已提交 → 草稿 (整个覆盖一遍)
     *   按键     改草稿
     *   Save     commitLogicConfig() 草稿 → 已提交, 并且整个推给 logic_analyze.c
     *   Cancel   什么都不做 —— 草稿就地作废, 下次进来又被覆盖掉
     *
     * 所以"已提交"那一份有两处, 各管一头, 不冲突:
     *   - Model 里存**候选值下标**那一份 (committedProtoMode / committedCfgVal):
     *     下次进屏摆界面用的。存下标而不是真值, 是因为候选值表和它的顺序本来就是
     *     设置屏的东西 (它要拿文本 id 去写字), Model 不该也抄一份顺序。
     *   - logic_analyze.c 里存**真值**那一份: 解码唯一认的就是它。
     *     下标 → 真值的翻表在设置屏那边 (Logic_SettingView.cpp), 因为它才是
     *     顺序的拥有者; 翻好了当参数传进来。
     * ======================================================================= */

    /** 草稿: 当前选的协议模式 (LOG_PROTO_*)。 */
    uint32_t getLogicProtoMode() const
    {
        return draftProtoMode;
    }

    /**
     * 换一个草稿模式。越界由调用方保证 —— 设置屏那条路是按 LOG_PROTO_COUNT
     * 取模轮询过来的。
     */
    void setLogicProtoMode(uint32_t mode)
    {
        draftProtoMode = mode;
    }

    /**
     * 草稿: 第 mode 个模式的第 row 行, 选的是候选值里的第几个 (0 = 列表里第一个)。
     *
     * 越界的 mode/row 都返回 0, 不会读到数组外。
     */
    uint8_t getLogicCfgValue(uint32_t mode, uint32_t row) const;

    /** 草稿: 同上, 写。越界直接丢掉。 */
    void setLogicCfgValue(uint32_t mode, uint32_t row, uint8_t value);

    /**
     * 进设置屏时调一次 (View::setupScreen 的第一件事): 已提交 → 草稿。
     *
     * 必须每次进屏都调 —— 上一次在这屏上改了没 Save (按了 Cancel) 的话, 草稿
     * 还是脏的, 不覆盖就会把上次作废的改动又摆到屏幕上。
     */
    void beginLogicEdit();

    /**
     * 按下 Save: 草稿 → 已提交 (下标那一份), 并把真值那份整个推给解码层。
     *
     * cfg 是 View 按草稿翻出来的真值 —— 翻表在那边 (见上面那段的说明)。
     * 两条信息流 (下标那份 / 真值那份) 是同一个草稿翻出来的, 不会不一致。
     *
     * 采集**不受影响**: 解码是采集停了之后才做的, 改了配置再解一遍就行, 不需要
     * 停采或者重采。
     */
    void commitLogicConfig(const log_ana_cfg_t& cfg);

    /* =======================================================================
     * 协议解码 (logic_analyze.c)
     * ======================================================================= */

    /**
     * Logic_Data 屏进来了: 请 Model 解一次码。
     *
     * 只置一个记号, 真正解在下一个 tick 里做 —— 解码是几毫秒到几十毫秒的活,
     * 不能让它堵在 setupScreen 里 (那是同步的, 会卡住切屏动画)。和"缩放只置
     * 记号、归约留给 LOG_Trace_Poll"是同一个理由。
     *
     * 同一轮采集只会真解一次 (logicDecodedSeq 比 LOG_Trace_GetResultSeq),
     * 反复进出这一屏不会反复重解。
     *
     * **请求会一直挂着, 直到采集停稳**: 点进来的时候开关还开着是常事, 那时候
     * 记录表还在变, 解出来是半截数据 (LOG_Analyze_Ready 会这么说)。这种时候
     * tick 不解也不丢请求, 每个 tick 再看一次, 等它停。
     */
    void requestLogicDecode();

    /**
     * 取上一次解码的结果 (只读, 不用释放)。*count 收条数。
     *
     * View::setupScreen 用它: 那一轮已经解过了的话, tick 不会再发回调 (记号没
     * 变), 屏幕上的内容就靠这一次取。
     */
    const log_ana_item_t* getLogicItems(uint32_t* count) const;

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

    /* ---- 协议配置: 已提交 (候选值下标形式) ----
     *
     * 只给"下次进设置屏时把界面摆回去"用 —— 解码认的那份真值在 logic_analyze.c
     * 里。两者在 commitLogicConfig() 里一起更新, 不会各自跑偏。
     *
     * 放这儿而不是设置屏的 View 里, 是因为它得活过切屏: Save/Cancel 都会离开
     * 这一屏, 再进来时要还是上次提交的那个。
     *
     * [模式][第几行] = 选了第几个候选值。全 0 是开机默认值, 也就是每种协议都用
     * 候选值列表里第一个 —— **必须和 LOG_Analyze_Init() 里的默认值一模一样**,
     * 不然会出现"界面写着 7 位数据、解码按 8 位解"这种没人查得出来的错。 */
    uint32_t committedProtoMode;
    uint8_t  committedCfgVal[LOG_PROTO_COUNT][LOG_PROTO_CFG_ROWS];

    /* ---- 协议配置: 草稿 (设置屏正在编辑的那一份) ----
     *
     * 进屏时 beginLogicEdit() 从已提交那份整个拷过来, 按键改的都只是它。 */
    uint32_t draftProtoMode;
    uint8_t  draftCfgVal[LOG_PROTO_COUNT][LOG_PROTO_CFG_ROWS];

    /* ---- 协议解码 ---- */

    /* 有人请求过解码 (进 Logic_Data 屏), 等 tick 去消费。
     *
     * 它不是"下一拍就清掉"的那种记号: 采集还在跑的时候 tick 会把它留着, 每拍
     * 再问一次, 直到真的解出一次为止 (见 Model.cpp 里那一段)。置它的是屏幕任务,
     * 消费它的 (Model::tick) 也是同一个任务 —— 不用 volatile, 和编码器那条路
     * 是同一个道理。 */
    bool logicDecodeReq;

    /* 已经解到第几代采集 (LOG_Trace_GetResultSeq 的那个数)。同一轮只解一次:
     * 反复进出 Logic_Data 屏不该把同一批记录重解好几遍。0 = 还没解过。 */
    uint32_t logicDecodedSeq;
};

#endif // MODEL_HPP
