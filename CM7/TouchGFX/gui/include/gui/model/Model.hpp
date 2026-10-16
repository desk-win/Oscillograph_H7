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

    /* =======================================================================
     * 示波器屏的编码器 —— 见 Model.cpp 里那一段的说明
     *
     * 短按换通道 (CH1 ↔ CH2), 长按换这一路调什么 (时基 ↔ 放大倍数), 转动改值。
     * 编的是**当前选中那一路**的设置, 另一路不动。
     * ======================================================================= */

    /**
     * 编码器转过 delta 格 (正 = 顺时针): 按当前选中那一路的当前目标改值。
     *
     * 时基 → OSC_Trace_SetTimebase (上下界、清触发锁存在那边); 倍数 → 写共享
     * 控制块, 引脚由 CM4 摆 (见 oscillograph.h 的 M7_OSC_SetFrontend)。
     *
     * 两个方向都是"往上转 = 变大": 时基变慢、倍数变大。到底哪个方向符合直觉是
     * 硬件接线的锅, 要反待在 Encoder_Increment 那一层反, 不要在这儿加负号 ——
     * 不然逻辑屏和示波器屏的转动方向会不一致。
     */
    void turnOscEncoder(int16_t delta);

    /** 短按: 在两个通道之间跳, 换"编码器现在管哪一路"。 */
    void toggleOscSelection();

    /** 长按: 换当前这一路是调时基还是调放大倍数。 */
    void toggleOscMode();

    /**
     * 把界面要显示的那一份设置整个取出来 —— 两个文本区的内容全靠它。
     *
     * 时基档位是从 oscillograph_trace.c 现取的 (真相在那边), 不是 Model 记的。
     */
    void getOscUiState(osc_ui_state_t* out) const;

    /**
     * 这一路当前的放大档位 (0..OSC_GAIN_STEPS-1, 实际倍数 = 1 << n)。
     *
     * 界面把测量出来的**原始计数**换算成电压时要用它: 探头那边先分压了 10 倍,
     * 放大这一级又乘回去, 不知道乘了多少就算不出探头上的真实电压。
     *
     * 越界的 ch 落到 CH1 上 (只可能是 0/1, 这里只是不让它读到数组外)。
     */
    uint8_t getOscGain(osc_trace_ch_t ch) const
    {
        return (uint8_t)oscGain[(ch == OSC_TRACE_CH2) ? 1u : 0u];
    }

    /**
     * 把 Model 记得的设置重新推给硬件。**进示波器屏时调一次**。
     *
     * 兜的是"两核各自独立复位"那种情况: CM4 那边 osc_ctrl 里的 cfg_gain_* 可能
     * 还留着上一次上电的值, 而它的 frontend_poll 只在值变了的时候才动引脚。不
     * 推这一下的话屏幕写着 X1、实际倍数还是上次那个。详见 Model.cpp。
     */
    void syncOscUi();

    /* =======================================================================
     * 示波器设置屏正在编辑的那一份 —— 交直流耦合的草稿
     *
     * 和下面逻辑分析仪的协议配置是同一套语义 (那一段的说明更细): 改完按下 Save
     * 才生效, 所以编辑期间动的是草稿。
     *
     *   进屏     beginOscEdit()      已提交 → 草稿
     *   拨开关   改草稿 (setOscCoupleDraft)
     *   Save     commitOscEdit()     草稿 → 已提交, 并推给 CM4 (M7_OSC_SetCoupling)
     *   Cancel   什么都不做 —— 草稿就地作废, 下次进屏又被覆盖掉
     *
     * 所以"没按 Save 就退出"之后, 硬件上、以及下次进屏摆出来的, 都还是上一次
     * 提交的那份。这也是为什么草稿和已提交要分成两个数组: 合成一个的话, 拨一下
     * 开关就把已提交那份改了, Cancel 再想恢复也没有第二份可比对。
     *
     * 耦合值就是 OSC_COUPLE_DC / OSC_COUPLE_AC (sharedconfig.h)。默认直流, 必须
     * 和 CM4 的 ctrl_defaults() 和 main.c 里那两句一致。
     * ======================================================================= */

    /** 进设置屏时调一次 (View::setupScreen 的第一件事): 已提交 → 草稿。 */
    void beginOscEdit();

    /** 草稿: 这一路现在是直流还是交流。越界的 ch 落到 CH1。 */
    uint32_t getOscCoupleDraft(osc_trace_ch_t ch) const;

    /** 草稿: 写。非 OSC_COUPLE_AC 的值一律按直流存 (和 CM4 那边的判法一致)。 */
    void setOscCoupleDraft(osc_trace_ch_t ch, uint32_t coup);

    /**
     * 按下 Save: 草稿 → 已提交, 两路一起推给 CM4。
     *
     * 两路都写, 不管这一路改没改 —— CM4 那边是拿本地旧值比过才动引脚的
     * (frontend_poll), 所以多写一次不会让耦合脚多抖一下。
     */
    void commitOscEdit();

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

    /**
     * Save 按键: 请求把这一次采集的跳变表存到 SD 卡上。
     *
     * 只通知 (LOG_Save_Request) —— 借记录表、写 CSV 全在 logicSave 任务里做,
     * GUI 帧里一个文件操作都没有。界面按下之后直接跳回 Logic 屏。
     */
    void saveLogicToSd();

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
     * Logic_Data 屏进来了: 请解码任务解一次码。
     *
     * 只发一个请求 (带配置快照) —— 解码是几毫秒到几十毫秒的活, 不能堵在
     * setupScreen (同步的, 会卡住切屏) 里, 也不放在 tick 里 (那是 GUI 帧)。
     * 同一代记录解过一次就不再解 (按 LOG_Trace_GetResultSeq 去重)。
     */
    void requestLogicDecode();

    /**
     * 取上一次解码的结果 (只读, 不用释放)。*count 收条数。
     *
     * View::setupScreen 用它: 那一轮已经解过了的话, tick 不会再发回调 (记号没
     * 变), 屏幕上的内容就靠这一次取。
     * (这一次取用没在 PubLock 里 —— 只有"新请求正好在这一帧解完"才会撞上,
     *  概率极低; 要彻底关掉就把 View 那段格式化也包进去。)
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

    /* ---- 示波器屏的编码器状态 ----
     *
     * 只被 TouchGFX 任务碰 (tick 里的派发和 Presenter 都在那个任务里), 不加锁。
     *
     * oscSelected 是"编码器现在管哪一路" (0 = CH1, 1 = CH2)。它不是"哪一路在采",
     * 也和屏幕开关无关 —— 停着的那一路照样能被选中、能调, 只是调完看不出来
     * (波形不刷)。这样切来切去不会因为"选中了一路停着的"而变成按了没反应。
     *
     * 两个数组都是每路一份: oscMode 是调时基还是调倍数, oscGain 是倍数档
     * (0..OSC_GAIN_STEPS-1)。时的那个数组不在这儿 —— 真相在 oscillograph_trace.c,
     * 那份要配合触发锁存一起维护, 分开存反而会不一致。 */
    uint32_t oscSelected;
    uint32_t oscMode[2];
    uint32_t oscGain[2];

    /* ---- 交直流耦合: 已提交 / 草稿 ----
     *
     * 和上面协议配置那两份是一个道理: 已提交那份给"下次进设置屏把开关摆回去"和
     * syncOscUi() 用, 草稿那份给设置屏编辑用。两者的说明见上面那一段。 */
    uint32_t committedCouple[2];
    uint32_t draftCouple[2];

    /* ---- 协议解码 ---- */

    /* 已经消费到第几个解码结果 (LOG_Analyze_GetPubSeq 的那个数)。0 = 还没有
     * 结果。由 tick 写、View::setupScreen 读, 都在屏幕任务里, 不用 volatile。 */
    uint32_t logicPubSeq;

    /* 有人请求过解码 (进 Logic_Data 屏), 还没真的发出去。
     * 采集还在跑 (代数为 0, 也就是这一轮还没收工) 时挂着, tick 里等它收工再发。 */
    bool logicDecodeReq;
};

#endif // MODEL_HPP
