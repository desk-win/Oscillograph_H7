#include <gui/logic_screen/LogicView.hpp>
#include <touchgfx/Color.hpp>
#include <touchgfx/hal/HAL.hpp>
#include <platform/driver/lcd/LCD16bpp.hpp>
#include "sharedconfig.h"

/* 下面按 uint16_t 一个像素直接往显存里写, 所以显存必须是 RGB565。改了
 * USE_BPP 这里得跟着改, 卡一道免得悄悄画花。 */
#if (USE_BPP != 16)
#error "波形控件按 RGB565 直接写显存, 换位深要改 LogicTraceWidget::draw()"
#endif

/* 8 个通道框的顶边 (y), 和 LogicViewBase.cpp 里 line1..line8 的 setPosition
 * 逐个对应。框高 50, 所以波形画在 top+5 到 top+45 之间 —— 上下各留 5 像素的
 * 空, 底下那条 Designer 画的 Line (在 top+50) 正好当基线。 */
static const int32_t LOG_BOX_TOP[8] = {6, 62, 119, 176, 240, 295, 354, 412};

/* 通道的位映射 —— 和采集侧 (CM4 记录的 smp) 共用同一个宏, 不重新打包。 */
static const uint16_t LOG_CH_BIT_TAB[8] = LOG_CH_BIT;

LogicTraceWidget::LogicTraceWidget() : ncols(0u)
{
    /* 8 个通道的颜色照抄 LogicViewBase.cpp 里 line1..line8 的 painter 颜色,
     * 顺序也一样 (line1 = 通道 1)。
     *
     * 要过一道 LCD16bpp::getNativeColor: TouchGFX 内部的 colortype 是
     * ARGB8888, 而显存里存的是压过的 RGB565 —— 就是 PainterRGB565 在
     * setColor() 里做的同一件事。少了这一步写进显存的是 ARGB8888 的低 16 位,
     * 颜色全错。 */
    static const uint8_t RGB[8][3] = {
        {  20, 151, 197 },   /* L1 青 */
        {  35, 204,  22 },   /* L2 绿 */
        { 196,  22,  22 },   /* L3 红 */
        { 118,  22, 196 },   /* L4 紫 */
        { 196, 170,  22 },   /* L5 黄 */
        {   0, 158, 126 },   /* L6 青绿 */
        { 196, 109,  22 },   /* L7 橙 */
        { 196,  14, 181 },   /* L8 品红 */
    };

    for (uint32_t ch = 0u; ch < 8u; ch++)
    {
        color[ch] = touchgfx::LCD16bpp::getNativeColor(
            touchgfx::Color::getColorFromRGB(RGB[ch][0], RGB[ch][1], RGB[ch][2]));
    }
}

void LogicTraceWidget::setData(const uint16_t* level_, const uint16_t* change_,
                               uint32_t ncols_)
{
    if (ncols_ > LOG_TRACE_COLS)
    {
        ncols_ = LOG_TRACE_COLS;
    }

    for (uint32_t c = 0u; c < ncols_; c++)
    {
        level[c]  = level_[c];
        change[c] = change_[c];
    }
    ncols = ncols_;

    /* 整块标脏。一次换一屏数据, 没有"只脏了一小块"这种情况。 */
    invalidate();
}

void LogicTraceWidget::clear()
{
    if (ncols == 0u)
    {
        return;
    }
    ncols = 0u;
    invalidate();
}

void LogicTraceWidget::draw(const touchgfx::Rect& invalidatedArea) const
{
    /* invalidatedArea 给的是相对本控件的坐标 (TouchGFX 的画法, 见 Box::draw
     * 之类), 先和本控件求交, 只画相交的那块。 */
    const touchgfx::Rect local =
        invalidatedArea & touchgfx::Rect(0, 0, getWidth(), getHeight());
    if (local.isEmpty() || ncols == 0u)
    {
        return;     /* 还没有任何结果: 什么都不画, 背景会把这块盖过去 */
    }

    /* 算显存地址要用屏幕坐标。 */
    const touchgfx::Rect abs = getAbsoluteRect();

    uint16_t* const fb = touchgfx::HAL::getInstance()->lockFrameBuffer();
    const int32_t stride = (int32_t)touchgfx::HAL::FRAME_BUFFER_WIDTH;

    /* 上面对应的像素是 origin + (x - local.x) + (y - local.y) * stride。 */
    uint16_t* const origin = fb + (int32_t)abs.y * stride + abs.x
                                  + (int32_t)local.y * stride + local.x;

    const int32_t xEnd = local.x + local.width;    /* 不含 */
    const int32_t yEnd = local.y + local.height;   /* 不含 */

    for (uint32_t ch = 0u; ch < 8u; ch++)
    {
        const uint16_t bit  = LOG_CH_BIT_TAB[ch];
        const int32_t yHi = LOG_BOX_TOP[ch] + 5;   /* 高电平画在上面 */
        const int32_t yLo = LOG_BOX_TOP[ch] + 45;  /* 低电平画在下面 */
        const uint16_t pen = color[ch];

        /* 这个通道的框整个在脏区外面就跳掉。8 个框里通常只有一两个和脏区相交,
         * 这一句省掉的是下面 520 次循环。 */
        if (yHi >= yEnd || yLo < local.y)
        {
            continue;
        }

        for (uint32_t c = 0u; c < ncols; c++)
        {
            const int32_t x = (int32_t)c;
            if (x < local.x || x >= xEnd)
            {
                continue;
            }

            /* 水平段: 这一列末尾的电平决定它画在上下哪一行。 */
            const int32_t y = ((level[c] >> bit) & 1u) ? yHi : yLo;
            origin[(y - local.y) * stride + (x - local.x)] = pen;

            /* 这一列里这个通道翻过 → 补一条贯穿整个框高的竖条。
             * 跳变只需要"看得见", 不需要落在正确的采样点上 —— 一列就是一个像素
             * 宽, 精确到像素就到头了。 */
            if (((change[c] >> bit) & 1u) != 0u)
            {
                int32_t y0 = yHi;
                int32_t y1 = yLo;
                if (y0 < local.y) y0 = local.y;
                if (y1 >= yEnd)  y1 = yEnd - 1;

                uint16_t* dst = origin + (y0 - local.y) * stride + (x - local.x);
                for (int32_t yy = y0; yy <= y1; yy++)
                {
                    *dst = pen;
                    dst += stride;
                }
            }
        }
    }

    touchgfx::HAL::getInstance()->unlockFrameBuffer();
}

LogicView::LogicView()
    : runCallback(this, &LogicView::runCallbackHandler)
{
    /* 波形控件摆成 (96, 0, 520, 462) —— x 从 96 起 (和 Designer 里那 8 条 Line
     * 的左端对齐) 宽 520 到 616, y 从 0 起高 462 (最后一个框的底边是
     * 412+50)。y 从 0 起是为了让控件坐标和屏幕 y 相等, draw() 里直接就着
     * LOG_BOX_TOP 用, 少一层换算。
     *
     * 加在最后 —— 容器里的子控件按顺序画, 加在最后就是盖在最上面。 */
    traceWidget.setPosition(96, 0, LOG_TRACE_COLS, 462);
    add(traceWidget);
}

void LogicView::setupScreen()
{
    LogicViewBase::setupScreen();

    /* ViewBase 里的 toggleButton1 (右上角那个采集开关) 只有外观没有
     * interaction, 这里给它接上。setupScreen() 每次进这个界面都会被调,
     * setAction 是幂等的。 */
    toggleButton1.setAction(runCallback);

    /* 按 Model 的实际状态把开关摆一次, 不能无条件 forceState(false) —— 离开
     * 这个界面不会自动停采集, 从 start_screen 切回来的时候 Model 可能还在跑。
     *
     * 读 Model 而不是指望开关自己的显示: ToggleButton 没有独立的状态位, 它的
     * 状态就是"现在显示的是哪张图"。 */
    toggleButton1.forceState(presenter->isLogicRunning());

    /* 波形控件也跟着对齐 —— 上一次采集的结果还在 Model 里, 切回来应该照原样
     * 显示, 而不是等下一次采集。 */
    const uint16_t* lvl  = 0;
    const uint16_t* chg  = 0;
    const uint32_t  n    = presenter->getLogicCols(&lvl, &chg);
    if (n != 0u)
    {
        traceWidget.setData(lvl, chg, n);
    }
    else
    {
        /* 没有数据 —— 两条路走得到这儿:
         *
         *   1. 开机后第一次进这个界面 (还没采过);
         *   2. 刚从 Logic_Data 按了 Cancel 回来。
         *
         * 第二种情况**必须在这里擦**: Cancel 走的是 model->clearLogicData(),
         * 那一下发出的 logicUpdated 是给当前活动屏幕的 Presenter 的, 也就是
         * Logic_Data 那一屏 —— 它的 logicUpdated 是默认空实现。所以控件上那
         * 一屏旧波形没人替它擦, 只能靠回到这一屏时按 ncols == 0 自己清掉。
         *
         * clear() 里对 ncols 已经是 0 的情况直接返回, 所以路径 1 不会白白
         * invalidate 一次。 */
        traceWidget.clear();
    }
}

void LogicView::tearDownScreen()
{
    LogicViewBase::tearDownScreen();
}

void LogicView::runCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &toggleButton1)
    {
        return;
    }

    /* 拨一下翻一下 (采集 ↔ 停止), 只认 Model —— 不读开关当前是哪一面。
     *
     * 理由和示波器那两个开关一样: ToggleButton 没有独立的状态位, 它的状态就是
     * "现在显示的是哪张图", 而离开这个界面不会停采集, 切回来时显示和实际状态
     * 本来就是两回事 (setupScreen 里会重新对齐)。以 Model 为准就和外观彻底
     * 解耦了。
     *
     * ToggleButton::handleClickEvent 是先换图再调 action 的, 所以这里调完
     * Start/Stop 之后, 开关显示的那一面正好就是新状态 —— 不用再 forceState。 */
    if (presenter->isLogicRunning())
    {
        presenter->stopLogicCapture();

        /* 停下来的这一屏**不擦**。和示波器那边不一样: 采完的波形是这次测量的
         * 结果, 用户要能一直看着它 —— 尤其"记录满"和"空闲超时"这两种自动停止
         * 的情况, 屏幕上留着波形才是这个功能的意义。
         *
         * 再看下一屏的路径是: 停着的时候再拨一次开关 (重新采), 采完 Model 会
         * 发 logicUpdated 过来覆盖掉。 */
    }
    else
    {
        presenter->startLogicCapture();
    }
}

void LogicView::updateLogic(const uint16_t* level, const uint16_t* change,
                            uint32_t ncols)
{
    traceWidget.setData(level, change, ncols);

    /* 顺手把开关拨回 OFF。
     *
     * 这个回调只在"一屏采完了"的时候来 —— 而采完只可能是记录满或者空闲超时
     * 自动停的 (手动停不会出图)。这时候 Model 那边已经不跑了, 可开关还停在
     * 用户拨上去的那一面, 看着像还在采。这里对齐一次。
     *
     * 读 Model 而不是无条件 forceState(false): 万一哪天真加上了"采完自动接着
     * 采", 这里也不会把开关拨错。 */
    toggleButton1.forceState(presenter->isLogicRunning());
}
