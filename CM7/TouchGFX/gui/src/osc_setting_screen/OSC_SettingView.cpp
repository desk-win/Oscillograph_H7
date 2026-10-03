#include <gui/osc_setting_screen/OSC_SettingView.hpp>
#include "sharedconfig.h"   /* OSC_COUPLE_DC / OSC_COUPLE_AC */

/* ===========================================================================
 * 交直流耦合 —— 这一屏到目前唯一的一项设置
 *
 * 硬件上就是模拟前端那只耦合脚, 1 = 直流, 0 = 交流 (见 CM4/osc_operate.c 的
 * OSC_Chx_Multiple 第二个参数)。上电默认直流。
 *
 * 数据流 (改完按下 Save 才生效):
 *
 *   setupScreen    beginOscEdit()      已提交 → 草稿, 然后按草稿摆两个开关
 *   拨开关         setOscCoupleDraft() 只改草稿, 一根引脚都不动
 *   Save           commitOscEdit()     草稿 → 已提交 + 推给 CM4
 *   Cancel         什么都不做 (草稿就地作废, 下次进屏又被覆盖)
 *
 * 所以这一屏**没有**"回滚"这个动作: 编辑期间硬件那份、已提交那份都没被碰过。
 * ========================================================================= */

OSC_SettingView::OSC_SettingView()
    : coupleCallback(this, &OSC_SettingView::coupleCallbackHandler),
      saveCallback(this, &OSC_SettingView::saveCallbackHandler),
      cancelCallback(this, &OSC_SettingView::cancelCallbackHandler)
{

}

void OSC_SettingView::setupScreen()
{
    OSC_SettingViewBase::setupScreen();

    /* 第一件事: 已提交的耦合整个拷进草稿。
     *
     * 必须排在下面摆开关之前 —— updateCouple() 读的就是草稿, 上一次进这一屏拨了
     * 没按 Save 就退出的话草稿还是脏的, 不覆盖就会把上次作废的改动又摆到屏上。 */
    presenter->beginOscEdit();

    /* ViewBase 里两个开关只有外观、没有 interaction, 这里给它们接上。
     * setupScreen() 每次进这一屏都会被调, setAction 是幂等的。 */
    ch1_switch.setAction(coupleCallback);
    ch2_switch.setAction(coupleCallback);

    /* Save / Cancel 是"换掉" ViewBase 里那两句跳转, 见头文件。 */
    save.setAction(saveCallback);
    cancel.setAction(cancelCallback);

    updateCouple();
}

void OSC_SettingView::tearDownScreen()
{
    OSC_SettingViewBase::tearDownScreen();
}

void OSC_SettingView::coupleCallbackHandler(const touchgfx::AbstractButton& src)
{
    osc_trace_ch_t ch;
    if (&src == &ch1_switch)
    {
        ch = OSC_TRACE_CH1;
    }
    else if (&src == &ch2_switch)
    {
        ch = OSC_TRACE_CH2;
    }
    else
    {
        return;
    }

    /* 翻一下草稿。
     *
     * **不读开关自己的状态** (即不用 ch1_switch.getState()): ToggleButton 没有
     * 独立的状态位, 它的 getState() 比的是 up 和 originalPressed 两张位图的 id
     * (ToggleButton.hpp) —— 位图哪天被换成同一张, "现在是不是交流"就永远是 true
     * 了。以 Model 为准, 开关的图只是照它画出来的结果。(和示波器屏那两个采集
     * 开关一个道理。) */
    const uint32_t now = presenter->getOscCoupleDraft(ch);
    presenter->setOscCoupleDraft(ch, (now == OSC_COUPLE_AC) ? OSC_COUPLE_DC : OSC_COUPLE_AC);

    /* 拨完立刻按草稿重摆 —— ToggleButton::handleClickEvent 自己也换过图了, 但
     * 那是它按"按下/松开"换的, 和 Model 里那份是不是一致它并不知道。这里以
     * Model 为准再对齐一次, 两边就永远同步。 */
    updateCouple();
}

void OSC_SettingView::saveCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &save)
    {
        return;
    }

    /* 草稿转正并推给 CM4。真正的效果是异步的 —— CM4 下一次主循环迭代
     * (≤655 µs) 才去摆耦合脚, 所以这里不用等, 改完就走。 */
    presenter->commitOscEdit();

    application().gotoOscillographScreenSlideTransitionEast();
}

void OSC_SettingView::cancelCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &cancel)
    {
        return;
    }

    /* 这里**什么都不用回滚**: 这些改动从头到尾只写在草稿上, 已提交那份和硬件
     * 都没碰过。草稿就让它脏着 —— 下次进这一屏 beginOscEdit() 会整个覆盖掉。
     * 所以只剩跳转一件事。 */
    application().gotoOscillographScreenSlideTransitionEast();
}

void OSC_SettingView::updateCouple()
{
    /* 开关的"开"那一面 (第二张图) = 交流。
     *
     * 屏上 "dc" 两个字在开关左边、"ac" 在右边, 而 ToggleButton 那两张图里旋钮
     * 的位置正好是左/右 —— 也就是说默认的直流就是上电那张图, 进这一屏开关不用
     * 动, 看不出来"这里还能拨"。
     *
     * forceState() 只是把 up/down 两张位图换一下, 自己**不标脏**, 所以这里手工
     * 补一次。一个开关才 70×30 像素, 两个一起也不到 4 KB。 */
    ch1_switch.forceState(presenter->getOscCoupleDraft(OSC_TRACE_CH1) == OSC_COUPLE_AC);
    ch2_switch.forceState(presenter->getOscCoupleDraft(OSC_TRACE_CH2) == OSC_COUPLE_AC);

    ch1_switch.invalidate();
    ch2_switch.invalidate();
}
