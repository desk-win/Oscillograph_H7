#ifndef MODEL_HPP
#define MODEL_HPP

#include <stdint.h>
#include <oscillograph_trace.h>
#include <logic_trace.h>

class ModelListener;

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
};

#endif // MODEL_HPP
