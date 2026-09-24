#ifndef MODEL_HPP
#define MODEL_HPP

#include <stdint.h>
#include <oscillograph_trace.h>

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

protected:
    ModelListener* modelListener;

    /* 两片一屏波形。Model 活得和程序一样久 (它住在 FrontendHeap 的静态实例
     * 里), 所以放这儿不占栈 —— 两片一共 4 KB, 放栈上会很难受。 */
    uint16_t traceY[2][OSC_TRACE_POINTS];

    /* 重画节流: 数到 OSC_TRACE_REFRESH_TICKS 才抽一次点 */
    uint8_t tickDiv;
};

#endif // MODEL_HPP
