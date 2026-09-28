#ifndef LOGICVIEW_HPP
#define LOGICVIEW_HPP

#include <gui_generated/logic_screen/LogicViewBase.hpp>
#include <gui/logic_screen/LogicPresenter.hpp>
#include <touchgfx/widgets/Widget.hpp>
#include <logic_trace.h>        /* LOG_TRACE_COLS */

/**
 * 逻辑分析仪波形控件 —— 自己往显存里画方波, 不用 Designer 里那 8 条 Line。
 *
 * Designer 里给每个通道画了一条 Line, 但那条线是**位置固定**的 —— 它只能
 * 表达"这个通道在这里", 表达不了波形。要让它显示电平就得每条线设一次
 * setStart/setEnd, 一次重绘 8 条线各自一路走 PainterRGB565 的 setPixel, 而
 * 中间那些空白像素还要靠背景重画来盖 —— 但真正的波形每列都不一样, 每列都
 * 是一次 setStart/setEnd, 等于每列都在改控件属性, 框架那边要重新算轮廓、
 * 重算脏区。
 *
 * 这里的做法和 OscTraceWidget (示波器波形) 是一样的: 直接锁帧缓冲写 RGB565。
 * 每列 8 段水平 + 至多 8 条竖条, 520 列, 一共几千次写, 一次重绘毫无压力。
 * 那 8 条 Line 留着不动 —— 它们画在框底 (top + 50), 正好当基线。
 *
 * 数据排布见 logic_trace.h:
 *   level[c]  第 c 列末尾的电平, 位映射同 LOG_CH_BIT (所以是 uint16_t: 最高
 *             那位是 bit 13, 8 位装不下)
 *   change[c] 第 c 列内发生跳变的通道掩码
 */
class LogicTraceWidget : public touchgfx::Widget
{
public:
    LogicTraceWidget();

    /**
     * 换一屏数据并标脏。传进来的是 Model 那两片, 这里拷一份 (520 × 2 × 2 =
     * 2080 字节, 几微秒), 拷完就不依赖别人的内存了。
     *
     * ncols == 0 表示还没有任何结果 (开机到现在没采过) —— 那就什么都不画,
     * 背景照常画过来, 屏幕上留着 Designer 里那 8 条基线。
     */
    void setData(const uint16_t* level, const uint16_t* change, uint32_t ncols);

    /**
     * 回到"没有数据"的状态, 并且立刻擦掉屏幕上的波形。
     *
     * 只在需要主动清屏时调 (目前没人调 —— 采集结束后留着最后一屏波形是有用的,
     * 不用擦)。留着是因为它和 setData 是配套的: 没有它就只能靠 setData(0,0,0)
     * 来清, 而那条路径不会标脏。
     */
    void clear();

    /**
     * 本控件不是实心的 —— 只画波形那几根线, 线之间的空隙是透的, 露出的是
     * 底下的背景。返回空矩形就是告诉框架"这里没有任何一块能保证被盖住",
     * 于是底下的背景照常画。要是返回整块矩形, 框架会把底下的绘制优化掉,
     * 空隙里就会是上一帧残留的花屏。
     */
    virtual touchgfx::Rect getSolidRect() const
    {
        return touchgfx::Rect();
    }

    virtual void draw(const touchgfx::Rect& invalidatedArea) const;

private:
    /* 8 个通道的方波颜色, 照抄 LogicViewBase.cpp 里 line1..line8。存的是压好的
     * RGB565 (显存里的样子), 不是 TouchGFX 内部那个 4 字节的 colortype ——
     * 每写一个像素都要查一次表, 所以先压好。 */
    uint16_t color[8];

    /* 拷贝过来的列数据。Model 那片活得更久, 但控件不该依赖别人的内存——
     * 拷一份省得去想"画的时候那片还在不在"。 */
    uint16_t level[LOG_TRACE_COLS];
    uint16_t change[LOG_TRACE_COLS];

    uint32_t ncols;
};

class LogicView : public LogicViewBase
{
public:
    LogicView();
    virtual ~LogicView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    /**
     * Model 那边算出新一屏了就送到这里重画。level/change 见 LogicTraceWidget。
     */
    void updateLogic(const uint16_t* level, const uint16_t* change, uint32_t ncols);

protected:
    /* 波形就画在这里面, 盖在 8 个通道框的地盘上。
     *
     * 摆成 (96, 0, 520, 462): x 从 96 (和 Designer 里那 8 条 Line 的左端对齐)
     * 起 520 列到 616; y 从 0 起是因为下面的 log_box_top[] 直接就是屏幕 y ——
     * 让控件坐标和屏幕坐标相等, 画的时候少一层换算, 少一处能算错的地方。
     *
     * 加在构造函数里而不是 setupScreen() —— add() 是增量的, 每次进界面再加
     * 一次就加两遍了。加在最后 = 盖在那 8 条 Line 上面。 */
    LogicTraceWidget traceWidget;

    /**
     * 采集开关 (toggleButton1) 的回调。
     *
     * 故意用 View 自己的一份 Callback, 而不是往 ViewBase 那个 generated 的
     * buttonCallback 里塞分支 —— 那样 TouchGFX Designer 每次重新生成代码都会
     * 把它冲掉。toggleButton1 在 Designer 里没配 interaction, 两边不打架。
     */
    touchgfx::Callback<LogicView, const touchgfx::AbstractButton&> runCallback;
    void runCallbackHandler(const touchgfx::AbstractButton& src);
};

#endif // LOGICVIEW_HPP
