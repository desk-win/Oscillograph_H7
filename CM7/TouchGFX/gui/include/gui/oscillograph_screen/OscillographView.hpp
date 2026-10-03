#ifndef OSCILLOGRAPHVIEW_HPP
#define OSCILLOGRAPHVIEW_HPP

#include <gui_generated/oscillograph_screen/OscillographViewBase.hpp>
#include <gui/oscillograph_screen/OscillographPresenter.hpp>
#include <touchgfx/widgets/Widget.hpp>
#include <oscillograph_trace.h>

/**
 * 波形控件 —— 自己往显存里画竖条, 不走 TouchGFX 的折线图。
 *
 * 为什么不用 dynamicGraph1/2 那两个 GraphElementLine:
 *
 *   它是 CanvasWidget, 画的每一步都要过画布光栅化器 —— 先把折线做成一个
 *   抗锯齿的闭合轮廓, 把轮廓登记成一个个 cell, 再扫线填充。而画布缓冲区
 *   只有 CANVAS_BUFFER_SIZE = 12000 字节, sizeof(Cell) = 8, 也就是 1500 个
 *   cell。可这边的波形是 512 列的 min/max 对, 连起来就是 512 条几乎贯穿
 *   整屏的竖线, 轮廓要几万个 cell —— 差两个数量级。
 *
 *   装不下时框架的做法是 (CanvasWidget::draw): 把区域高度砍半、把 1024 个点
 *   从头重走一遍, 砍到只剩一两行才装得下; 砍完一个 band 再从下一行重来。
 *   实测一次重画 2.98 秒, 而同样的数据从 SDRAM 读出来只要 2.1 毫秒。
 *
 * 这个控件直接算像素: 一列一根竖条, 从最小值那行画到最大值那行, 512 根。
 * 没有轮廓、没有 cell、没有抗锯齿混合。代价是线是硬边的, 不是平滑的 ——
 * 对示波器来说反而更像真的。
 *
 * 数据排布沿用 oscillograph_trace.h 的定义, 每通道 OSC_TRACE_POINTS 个点:
 *   trace[ch][2c]   = 第 c 列最小值
 *   trace[ch][2c+1] = 第 c 列最大值
 */
class OscTraceWidget : public touchgfx::Widget
{
public:
    /* 线宽, 像素。和原来折线图的 setLineWidth(2) 对齐。 */
    static const int LINE_WIDTH = 2;

    OscTraceWidget();

    /**
     * 换一屏数据并标脏。
     *
     * 传进来的两片是 Model 的, 这里拷一份 (4 KB, 几微秒)。拷完这个控件就不
     * 依赖别人的内存了, 省得去想"画的时候那片还在不在"。
     *
     * fresh1 / fresh2 说明哪几路是真的新数据 —— 本控件就按它来定 active,
     * 也就是"这一帧画哪几路"。停着的、或者刚 Start 还没凑够一屏的那一路是
     * false, 缓冲里那份旧数据不画。
     */
    void setTrace(const uint16_t* ch1, const uint16_t* ch2,
                  bool fresh1, bool fresh2);

    /**
     * 停掉一路时用: 立刻把它从屏幕上擦掉, 不等 Model 出下一帧。
     *
     * 为什么不能只靠 Model: 两路都停掉之后 Model::tick() 会提前返回, 再没有
     * 任何一帧能把"清空"送过来, 屏幕上会留着两条不动的波形擦不掉。开关事件
     * 是直接送到 View 的, 只有这条路上一定拿得到。
     *
     * 打开那一路不走这里 —— 交给 setTrace 的 fresh 标志, 免得把缓冲里上一次
     * 关机前的旧波形先闪一下。
     */
    void setActive(osc_trace_ch_t ch, bool on);

    /**
     * 进界面时用: 一次把两路都和 Model 对齐 (离开这个界面不会自动停采集,
     * 从 start_screen 切回来时可能还在跑)。
     *
     * 这里可以乐观地按"在跑就画"来摆 —— 一个通道只要在跑, 它必然是从这个
     * 界面启动的, trace[] 里那份数据最多也就旧一个刷新周期, 画出来是对的。
     */
    void setActive(bool ch1on, bool ch2on);

    virtual void draw(const touchgfx::Rect& invalidatedArea) const;

    /**
     * 本控件不是实心的 —— 只画波形那几根竖条, 竖条之间的空隙是透的, 露出的
     * 是底下的背景。返回空矩形就是告诉框架"这里没有任何一块能保证被盖住",
     * 于是底下的背景照常画。要是返回整块矩形, 框架会把底下的绘制优化掉,
     * 竖条之间的空隙就会是上一帧残留的花屏。
     */
    virtual touchgfx::Rect getSolidRect() const
    {
        return touchgfx::Rect();
    }

private:
    /* 两根线的颜色, 照抄原来 dynamicGraph1Line1 / dynamicGraph2Line1。存的
     * 是压好的 RGB565 (显存里的样子), 不是 TouchGFX 内部那个 ARGB8888 的
     * colortype —— 每写一个像素都要查一次表, 所以先压好。 */
    uint16_t color[2];

    uint16_t trace[2][OSC_TRACE_POINTS];

    /* bit0 = 画 CH1, bit1 = 画 CH2。不画的那一路 draw() 直接跳过 —— 于是它
     * 那份 trace[] 不用清, 屏幕上也不会留东西 (底下背景会照常画过来盖掉,
     * 因为 getSolidRect() 返回空矩形)。
     *
     * 平时由 setTrace 按 fresh 标志整体重写; setActive 只在"停掉一路要立刻
     * 擦掉"和"进界面时对齐"这两个时刻单独动它。 */
    uint8_t active;
};

class OscillographView : public OscillographViewBase
{
public:
    OscillographView();
    virtual ~OscillographView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    /**
     * Model 算好一屏波形 → 交给波形控件重画。
     * 每通道 OSC_TRACE_POINTS 个点, 排布是 [min0, max0, min1, max1, ...]。
     * fresh1/fresh2 见 setTrace。
     */
    void updateTrace(const uint16_t* ch1, const uint16_t* ch2,
                     bool fresh1, bool fresh2);

    /**
     * 把右下角两个模式文字区按 Model 的设置重写一遍。
     *
     * 一行写"这一路现在调的是什么 + 值", 例如 "time 2.62ms" / "scope x16";
     * 选中那一路的**文字变成亮蓝 18C8DB**, 没选中的是黑的。行首那个 "CH1:" /
     * "CH2:" 标签按通道各有固定颜色 (生成代码里配的), 这里不动 —— 那是"这是
     * 哪一路"的标记, 和"编码器现在管哪一路"是两回事, 混在一起会看不懂。
     *
     * 进屏时和每次编码器动作之后都调, 代价可以忽略 (两个短字符串)。
     */
    void updateModeText(const osc_ui_state_t& st);

protected:
    /* 波形就画在这里面 (盖在原来两个折线图的位置上)。
     * 加在构造函数里而不是 setupScreen() —— add() 是增量的, 每次进界面再加
     * 一次就加两遍了。 */
    OscTraceWidget traceWidget;

    /**
     * 两个采集开关 (ch1 / ch2) 共用的回调 —— 拨一下翻一下 (开 ↔ 停), 靠 &src
     * 认出是哪一路。
     *
     * 故意用 View 自己的一份 Callback, 而不是往 ViewBase 那个 generated 的
     * buttonCallback 里塞分支 —— 那样 TouchGFX Designer 每次重新生成代码都会
     * 把它冲掉。ViewBase 里 button1 (左上角那个回 start_screen 的) 用的是
     * buttonCallback, ch1/ch2 没配 interaction, 两边不打架。
     *
     * 不以开关自己的状态为准 (即不读 ch1.getState()), 而是问 Model。原因:
     * ToggleButton 没有独立的"状态位", 它的状态就是"现在显示的是哪张图"
     * —— getState() 比的是 up 和 originalPressed 两个位图 id (ToggleButton.hpp)。
     * 现在 Designer 里这两张图配得不一样 (OFF_NORMAL / ON_ACTIVE), getState()
     * 是准的; 但离开这个界面不会自动停采集, 从 start_screen 切回来时开关的
     * 显示和实际状态本来就是两回事。以 Model 为准就和外观彻底解耦了 ——
     * 位图哪天被换成同一张, 逻辑也不会跟着坏。
     *
     * 开关自己的 up/down 交换由 ToggleButton::handleClickEvent 做, 这里不管;
     * 只在 setupScreen() 里按 Model 的实际状态摆一次。
     */
    touchgfx::Callback<OscillographView, const touchgfx::AbstractButton&> acquireCallback;
    void acquireCallbackHandler(const touchgfx::AbstractButton& src);

    /**
     * 写一个模式文字区 —— 两个（CH1 / CH2）形状完全一样, 只有控件和缓冲不同。
     *
     * ch 决定取哪一路的设置、决定"选中没有"。buffer/size 得一起传进来: 那两个
     * 通配缓冲区是 ViewBase 里的 protected 成员, 名字分别是 ch1mode_valueBuffer /
     * ch2mode_valueBuffer, 这里没法用下标取。
     *
     * 这里**没有**标脏 —— 调用方两个都写完之后统一标一次 (box3), 理由见 .cpp。
     */
    void setModeText(touchgfx::TextAreaWithOneWildcard& ta,
                     touchgfx::Unicode::UnicodeChar* buffer, uint16_t bufferSize,
                     const osc_ui_state_t& st, uint32_t ch);
};

#endif // OSCILLOGRAPHVIEW_HPP
