#ifndef OSC_SETTINGPRESENTER_HPP
#define OSC_SETTINGPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>
#include <oscillograph_trace.h>     /* osc_trace_ch_t */

using namespace touchgfx;

class OSC_SettingView;

class OSC_SettingPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    OSC_SettingPresenter(OSC_SettingView& v);

    /**
     * The activate function is called automatically when this screen is "switched in"
     * (ie. made active). Initialization logic can be placed here.
     */
    virtual void activate();

    /**
     * The deactivate function is called automatically when this screen is "switched out"
     * (ie. made inactive). Teardown functionality can be placed here.
     */
    virtual void deactivate();

    virtual ~OSC_SettingPresenter() {}

    /* =======================================================================
     * 交直流耦合 —— 全是转发, 状态在 Model 里 (见 Model.hpp 那一段)
     *
     * 和 Logic_SettingPresenter 上那一组一样: 这一层不存东西, 也不判断什么,
     * 只是把 View 的动作和 Model 的状态接起来。
     * ======================================================================= */

    /**
     * 进这一屏: 已提交的耦合整个拷进草稿。View::setupScreen 的第一件事 ——
     * 必须赶在摆开关之前, 不然摆出来的是上一次没 Save 的那份脏草稿。
     */
    void beginOscEdit();

    /** 草稿: 这一路现在是直流还是交流 (OSC_COUPLE_DC / OSC_COUPLE_AC)。 */
    uint32_t getOscCoupleDraft(osc_trace_ch_t ch);

    /** 草稿: 写。只改草稿, 硬件和已提交那份都不动 —— 这就是"没 Save 不生效"。 */
    void setOscCoupleDraft(osc_trace_ch_t ch, uint32_t coup);

    /** 按了 Save: 草稿 → 已提交, 并推给 CM4 (引脚在那边)。 */
    void commitOscEdit();

private:
    OSC_SettingPresenter();

    OSC_SettingView& view;
};

#endif // OSC_SETTINGPRESENTER_HPP
