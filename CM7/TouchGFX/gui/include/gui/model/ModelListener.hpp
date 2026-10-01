#ifndef MODELLISTENER_HPP
#define MODELLISTENER_HPP

#include <stdint.h>
#include <gui/model/Model.hpp>

class ModelListener
{
public:
    ModelListener() : model(0) {}

    virtual ~ModelListener() {}

    void bind(Model* m)
    {
        model = m;
    }

    /**
     * 一屏新波形算好了, 该重画了。
     *
     * 两个缓冲就是 Model 里那两片 Y 缓冲本身, 不用再拷一份 —— 重画是同步做完
     * 的, 期间 Model 不会去改它们。
     *
     * 每通道 OSC_TRACE_POINTS 个点, 排布是 [min0, max0, min1, max1, ...]。
     *
     * fresh1 / fresh2: 对应那一路这一帧是不是**真的新算出来的**。两种情况会
     * 是 false —— 这一路停着, 或者刚 Start、环里还没凑够一屏。这两种时候缓冲
     * 里装的是上一轮的旧数据, 不能往屏幕上画 (画了就是"关掉再打开会先闪一下
     * 关机前那条波形")。控件拿它决定这一帧画哪几路, 见 OscTraceWidget。
     */
    virtual void traceUpdated(const uint16_t* ch1, const uint16_t* ch2,
                              bool fresh1, bool fresh2)
    {
        (void)ch1;
        (void)ch2;
        (void)fresh1;
        (void)fresh2;
    }

    /**
     * 逻辑分析仪的一屏波形算好了, 该重画了。
     *
     * 两个缓冲就是 Model 里那两片列数组本身, 不用再拷一份 —— 重画是同步做完
     * 的, 期间 Model 不会去改它们。
     *
     * level[c]  第 c 列末尾的电平, change[c] 第 c 列内发生跳变的通道掩码,
     * 位映射同 LOG_CH_BIT。两者都是 uint16_t —— 最高那位是 bit 13, 8 位装不下。
     *
     * 这个回调是**一次性**的: Model 只在真的算出新一屏的那一帧发它 (GetCols
     * 是取走式的), 不是每个 tick 都发。所以 View 收到就整块重画, 不用去想节流。
     */
    virtual void logicUpdated(const uint16_t* level, const uint16_t* change,
                              uint32_t ncols)
    {
        (void)level;
        (void)change;
        (void)ncols;
    }

    /**
     * 编码器转过了。delta 是自上次调用以来的**净格数**, 正 = 顺时针。
     *
     * 默认空实现 —— 只有想用编码器的屏才覆盖它 (目前是逻辑分析仪, 调时基)。
     * 不覆盖就等于把这一把转动丢掉, 这正是想要的: 编码器在别的屏上不该有作用。
     *
     * 用 ModelListener 派发而不是让 Model 去判"现在是不是逻辑屏": MVPApplication
     * 每次切屏都会 model->bind(newPresenter) (mvp/MVPApplication.hpp), 所以
     * 收到这个回调的永远是当前活动屏, 不用另外记一个"当前屏"的状态。
     *
     * 和 logicUpdated 一样, 它只在真的转过了的时候才会被调到 (Model::tick 里
     * 先判 delta != 0), 不是每个 tick 都来一次。
     */
    virtual void encoderTurned(int16_t delta)
    {
        (void)delta;
    }

protected:
    Model* model;
};

#endif // MODELLISTENER_HPP
