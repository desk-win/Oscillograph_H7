#ifndef OSC_SETTINGVIEW_HPP
#define OSC_SETTINGVIEW_HPP

#include <gui_generated/osc_setting_screen/OSC_SettingViewBase.hpp>
#include <gui/osc_setting_screen/OSC_SettingPresenter.hpp>

/**
 * 示波器设置屏 —— 到目前只有一项: 两路各自的交直流耦合。
 *
 * 语义借的是逻辑分析仪设置屏那一套 (见 Model.hpp 里"协议配置"那一段):
 * **改完按下 Save 才生效**。拨开关动的是 Model 里的草稿, 硬件一根引脚都不动;
 * Save 才把草稿推给 CM4。Cancel 什么都不用回滚 —— 因为压根没往硬件写过东西。
 *
 * 开关的图**不当作状态用**: 两个 handler 都先问 Model 现在是什么、再决定拨成
 * 什么, 最后由 updateCouple() 按 Model 摆图。所以位图就算被换掉, 逻辑也不会跟着
 * 坏 (和示波器屏那两个采集开关同一个理由)。
 */
class OSC_SettingView : public OSC_SettingViewBase
{
public:
    OSC_SettingView();
    virtual ~OSC_SettingView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

protected:
    /* 两个耦合开关共用一个回调, 靠 &src 认出是哪一路 (和示波器屏那两个采集开关
     * 一样的做法)。用 View 自己的一份 Callback, 而不是往 ViewBase 那个生成的
     * buttonCallback 里塞分支 —— 那样 Designer 每次重新生成代码都会把它冲掉。 */
    touchgfx::Callback<OSC_SettingView, const touchgfx::AbstractButton&> coupleCallback;
    void coupleCallbackHandler(const touchgfx::AbstractButton& src);

    /**
     * Save / Cancel。
     *
     * ViewBase 里这两个按键是配了 interaction 的 (两句一模一样的跳转), 所以这里
     * 不只是"接管", 是"换掉" —— 跳转那句得在 handler 里自己补回来。(和
     * Logic_SettingView 里 Save/Cancel 的做法一样。)
     */
    touchgfx::Callback<OSC_SettingView, const touchgfx::AbstractButton&> saveCallback;
    touchgfx::Callback<OSC_SettingView, const touchgfx::AbstractButton&> cancelCallback;
    void saveCallbackHandler(const touchgfx::AbstractButton& src);
    void cancelCallbackHandler(const touchgfx::AbstractButton& src);

    /**
     * 按 Model 的草稿把两个开关的图摆一遍。进屏时和每次拨动之后都调。
     *
     * 单独留一个入口, 是因为"拨一下"和"摆图"必须分开: 拨动只改 Model, 摆图只读
     * Model —— 中间没有"控件自己记着状态"这一层。
     */
    void updateCouple();
};

#endif // OSC_SETTINGVIEW_HPP
