#include <gui/oscillograph_screen/OscillographView.hpp>
#include <touchgfx/Color.hpp>
#include <touchgfx/hal/HAL.hpp>
#include <platform/driver/lcd/LCD16bpp.hpp>

/* 下面按 uint16_t 一个像素直接往显存里写, 所以显存必须是 RGB565。改了
 * USE_BPP 这里得跟着改, 卡一道免得悄悄画花。 */
#if (USE_BPP != 16)
#error "波形控件按 RGB565 直接写显存, 换位深要改 OscTraceWidget::draw()"
#endif

OscTraceWidget::OscTraceWidget() : active(0u)
{
    /* 颜色照抄原来 dynamicGraph1Line1 (青) 和 dynamicGraph2Line1 (黄),
     * 见 OscillographViewBase.cpp。
     *
     * 要过一道 LCD16bpp::getNativeColor: TouchGFX 内部的 colortype 是
     * ARGB8888 (4 字节, 顶字节是 0xFF), 而显存里存的是压过的 RGB565 ——
     * 就是 PainterRGB565 在 setColor() 里做的同一件事。少了这一步写进显存
     * 的是 ARGB8888 的低 16 位, 颜色全错。 */
    color[0] = touchgfx::LCD16bpp::getNativeColor(
        touchgfx::Color::getColorFromRGB(20, 151, 197));
    color[1] = touchgfx::LCD16bpp::getNativeColor(
        touchgfx::Color::getColorFromRGB(214, 208, 30));
}

void OscTraceWidget::setTrace(const uint16_t* ch1, const uint16_t* ch2,
                              bool fresh1, bool fresh2)
{
    for (uint32_t i = 0u; i < OSC_TRACE_POINTS; i++)
    {
        trace[0][i] = ch1[i];
        trace[1][i] = ch2[i];
    }

    /* 画哪几路完全跟着 fresh 走 —— 不 fresh 的那一路缓冲里是上一轮的旧数据,
     * 画出去就是"关掉再打开先闪一下关机前的波形"。整帧重写而不是按位改:
     * 万一哪天 setActive 那一侧漏了一次, 这里下一帧自己会纠正回来。 */
    active = (uint8_t)((fresh1 ? 1u : 0u) | (fresh2 ? 2u : 0u));

    /* 整块标脏。一次换一屏数据, 没有"只脏了一小块"这种情况。 */
    invalidate();
}

void OscTraceWidget::setActive(osc_trace_ch_t ch, bool on)
{
    const uint8_t bit  = (uint8_t)(1u << (uint32_t)ch);
    const uint8_t next = on ? (uint8_t)(active | bit) : (uint8_t)(active & (uint8_t)~bit);

    if (next == active)
    {
        return;     /* 状态没变就别标脏 —— 进界面时两路都会走一遍这条路 */
    }
    active = next;
    invalidate();
}

void OscTraceWidget::setActive(bool ch1on, bool ch2on)
{
    setActive(OSC_TRACE_CH1, ch1on);
    setActive(OSC_TRACE_CH2, ch2on);
}

void OscTraceWidget::draw(const touchgfx::Rect& invalidatedArea) const
{
    /* invalidatedArea 给的是相对本控件的坐标 (TouchGFX 的画法, 见 Box::draw
     * 之类), 先和本控件求交, 只画相交的那块。 */
    const touchgfx::Rect local =
        invalidatedArea & touchgfx::Rect(0, 0, getWidth(), getHeight());
    if (local.isEmpty())
    {
        return;
    }

    /* 算显存地址要用屏幕坐标。 */
    const touchgfx::Rect abs = getAbsoluteRect();

    uint16_t* const fb = touchgfx::HAL::getInstance()->lockFrameBuffer();
    const int32_t stride = (int32_t)touchgfx::HAL::FRAME_BUFFER_WIDTH;

    /* 上面对应的像素是 origin + (x - local.x) + (y - local.y) * stride。 */
    uint16_t* const origin = fb + (int32_t)abs.y * stride + abs.x
                                  + (int32_t)local.y * stride + local.x;

    const int32_t w = getWidth();
    const int32_t h = getHeight();
    const int32_t xEnd = local.x + local.width;    /* 不含 */
    const int32_t yEnd = local.y + local.height;   /* 不含 */

    for (int ch = 0; ch < 2; ch++)
    {
        /* 停掉的那一路整个跳过。不用清它的 trace[], 也不用自己擦像素 ——
         * getSolidRect() 返回空矩形, 所以框架下一帧会把底下的背景重新画过来,
         * 竖条的位置自然就恢复了。 */
        if ((active & (1u << ch)) == 0u)
        {
            continue;
        }

        const uint16_t* const y = trace[ch];
        const uint16_t pen = color[ch];

        for (uint32_t c = 0u; c < OSC_TRACE_COLUMNS; c++)
        {
            /* 第 c 列占两个下标: 2c 是最小值, 2c+1 是最大值。1024 个下标
             * 摊在宽 w 上 —— 和原来折线图 indexToScreenXQ5 的摊法相同, 所以
             * 画出来的横向位置和以前一模一样 (一列占 1~2 个像素列)。 */
            int32_t x0 = (int32_t)(2u * c) * (w - 1) / (int32_t)(OSC_TRACE_POINTS - 1u);
            int32_t x1 = (int32_t)(2u * c + 1u) * (w - 1) / (int32_t)(OSC_TRACE_POINTS - 1u);

            /* 竖条本来就有 1~2 像素宽, 再补 1 像素才和原来的 2 像素线一样粗。 */
            x1 += LINE_WIDTH - 1;

            /* 值 → 行。照抄 AbstractDataGraph::valueToScreenYQ5 在本图
             * (graphRangeY = [0, 4096], 无边距) 下的算法:
             *     row = (h - 1) - value * (h - 1) / 4096
             * 值越大行号越小, 也就是越高。 */
            int32_t yTop = (h - 1) - (int32_t)y[2u * c + 1u] * (h - 1) / 4096;
            int32_t yBot = (h - 1) - (int32_t)y[2u * c]     * (h - 1) / 4096;

            /* 竖条至少 LINE_WIDTH 行高 —— 平直的部分 min 和 max 挨着, 不撑一下
             * 会细成一根头发丝。 */
            if (yBot - yTop < LINE_WIDTH - 1)
            {
                yBot = yTop + LINE_WIDTH - 1;
            }

            /* 和脏区求交。整块重画时这一步什么都不裁, 但别假设脏区一定是整块。 */
            if (x0 < local.x)   x0 = local.x;
            if (x1 >= xEnd)     x1 = xEnd - 1;
            if (yTop < local.y) yTop = local.y;
            if (yBot >= yEnd)   yBot = yEnd - 1;
            if (x0 > x1 || yTop > yBot)
            {
                continue;
            }

            /* 干活。列宽只有 1~3 个像素, 所以内层是个很短的小循环。
             * 直接写显存, 不走 painter —— 没有抗锯齿混合, 一个像素一次写。 */
            uint16_t* dst = origin + (yTop - local.y) * stride + (x0 - local.x);
            const int32_t run = x1 - x0 + 1;
            for (int32_t row = yTop; row <= yBot; row++)
            {
                for (int32_t i = 0; i < run; i++)
                {
                    dst[i] = pen;
                }
                dst += stride;
            }
        }
    }

    touchgfx::HAL::getInstance()->unlockFrameBuffer();
}

OscillographView::OscillographView()
    : acquireCallback(this, &OscillographView::acquireCallbackHandler)
{
    /* 波形控件摆成原来两个折线图的位置 (见 OscillographViewBase.cpp), 加在
     * 最后 —— 容器里的子控件按顺序画, 加在最后就是盖在最上面。 */
    traceWidget.setPosition(0, 69, 674, 361);
    add(traceWidget);
}

void OscillographView::setupScreen()
{
    OscillographViewBase::setupScreen();

    /* 原来自带的那两个 TouchGFX 折线图已经在 Designer 里删掉了 —— 波形现在
     * 由 traceWidget 画, 理由见 OscillographView.hpp 里那个类的说明。删掉
     * 之后 12 KB 的画布缓冲区和两个图各 1024 个点的 yValues 也跟着省下来了,
     * 所以这里不再需要把它们设成不可见。 */

    /* ViewBase 里 ch1 / ch2 (两个通道各自的 ON/OFF 开关) 只有外观没有
     * interaction, 这里给它们接上。setupScreen() 每次进这个界面都会被调,
     * setAction 是幂等的。 */
    ch1.setAction(acquireCallback);
    ch2.setAction(acquireCallback);

    /* 按 Model 的实际状态把开关摆一次, 不能无条件 forceState(false) —— 离开
     * 这个界面不会自动停采集, 从 start_screen 切回来的时候 Model 可能还在跑。 */
    const bool run1 = presenter->isAcquisitionRunning(OSC_TRACE_CH1);
    const bool run2 = presenter->isAcquisitionRunning(OSC_TRACE_CH2);
    ch1.forceState(run1);
    ch2.forceState(run2);

    /* 波形控件也要跟着对齐。它自己记着"哪一路在画", 不跟着复位的话, 上一次
     * 离开时停掉的那一路会一直不画 —— 而开关看起来是 ON 的。 */
    traceWidget.setActive(run1, run2);
}

void OscillographView::tearDownScreen()
{
    OscillographViewBase::tearDownScreen();
}

void OscillographView::acquireCallbackHandler(const touchgfx::AbstractButton& src)
{
    osc_trace_ch_t ch;
    if      (&src == &ch1) { ch = OSC_TRACE_CH1; }
    else if (&src == &ch2) { ch = OSC_TRACE_CH2; }
    else                   { return; }

    /* 拨一下就翻一下, 只认 Model —— 不读开关当前是哪一面, 理由见 .hpp。 */
    if (presenter->isAcquisitionRunning(ch))
    {
        presenter->stopAcquisition(ch);

        /* 停下来的那一路立刻从屏幕上擦掉。
         *
         * 必须在这里做, 不能指望 Model 事后发一帧过来: 两路都停了的话
         * Model::tick() 会提前 return, 一帧都不会发, 屏幕上那两条波形就永远
         * 擦不掉了。开关事件是直接送到这里来的, 只有这个位置一定拿得到。
         *
         * 打开那一路反过来不在这里做 —— 交给 setTrace 的 fresh 标志, 免得
         * 先把缓冲里关机前那条旧波形闪一下。 */
        traceWidget.setActive(ch, false);
    }
    else
    {
        presenter->startAcquisition(ch);
    }
}

void OscillographView::updateTrace(const uint16_t* ch1, const uint16_t* ch2,
                                   bool fresh1, bool fresh2)
{
    /* 通道 1 → 青色, 通道 2 → 黄色, 两条都画在同一个图区里叠着 (和以前一样) */
    traceWidget.setTrace(ch1, ch2, fresh1, fresh2);
}
