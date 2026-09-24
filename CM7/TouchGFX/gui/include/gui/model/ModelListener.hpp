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

protected:
    Model* model;
};

#endif // MODELLISTENER_HPP
