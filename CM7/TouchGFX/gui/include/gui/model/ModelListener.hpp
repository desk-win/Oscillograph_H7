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
     * 协议解码的结果出来了, 该把 Logic_Data 屏上那块文字区重写一遍。
     *
     * items 指到 logic_analyze.c 内部的静态区, **下一次解码或者 Clear 就会把
     * 它改掉** —— 所以收到就当场把要显示的字格式化到自己的缓冲里, 不要把指针
     * 存起来 (和 logicUpdated 传列缓冲是同一个约定: 处理是同步做完的)。
     *
     * count 是条数, **0 表示这次没东西可显示** (采集还在跑 / 还没采过 / 这一轮
     * 一条都没解出来), View 收到就把文字区清空 —— 不是"保持上一次的"。
     *
     * 一次性回调: 只有真的解完一轮才发, 不是每个 tick。同一轮采集里反复进出
     * Logic_Data 屏不会再发 (那一轮早就解过了), 那一次屏幕上该显示什么由
     * View::setupScreen 自己取 (Model::getLogicItems)。
     */
    virtual void logicDataUpdated(const log_ana_item_t* items, uint32_t count)
    {
        (void)items;
        (void)count;
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

    /**
     * 编码器上那个开关被**短按**了。
     *
     * 和 encoderTurned 同一套派发: 默认空实现, 只有想用它的屏才覆盖 —— 收到它的
     * 永远是当前活动屏, 别的屏等于把这下短按丢掉。目前只有示波器覆盖它: 短按在
     * 两个通道之间切"正在调哪一路"。
     *
     * 和转动的区别: 这个是**离散事件**, 按一下来一次, 没有什么 delta 可言。所以
     * Model::tick 那边必须先自己判一下这次 tick 里开关释放成了什么状态, 再派发
     * 一次 (见 Model::tick 里的 switch) —— 不能像 encoderTurned 那样每次都发。
     */
    virtual void encoderPressed()
    {
    }

    /**
     * 编码器上那个开关被**长按**了 (约 2 s)。
     *
     * 和 encoderTurned 同一套派发: 默认空实现, 只有想用它的屏才覆盖 —— 收到它的
     * 永远是当前活动屏, 别的屏等于把这下长按丢掉。逻辑分析仪覆盖它: 长按在
     * "调时基"和"滚窗口"两种视图模式之间切。示波器也覆盖它: 长按切这一路的
     * 编码器是调时基还是调放大倍数。
     *
     * 短按/长按是 Encoder_Key() 那台状态机分开判的 (长按 2 s 到点就直接返回
     * LONGPRESS, 不会先给一次 PRESS), 所以同一次按下只会走到这里两者之一。
     */
    virtual void encoderLongPressed()
    {
    }

protected:
    Model* model;
};

#endif // MODELLISTENER_HPP
