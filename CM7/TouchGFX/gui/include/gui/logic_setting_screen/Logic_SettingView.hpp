#ifndef LOGIC_SETTINGVIEW_HPP
#define LOGIC_SETTINGVIEW_HPP

#include <gui_generated/logic_setting_screen/Logic_SettingViewBase.hpp>
#include <gui/logic_setting_screen/Logic_SettingPresenter.hpp>

class Logic_SettingView : public Logic_SettingViewBase
{
public:
    Logic_SettingView();
    virtual ~Logic_SettingView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    /**
     * 按协议模式把 Mode 和各通道那几块文字区写一遍, 没用到的那几路藏起来。
     *
     * 会被调到的时机: 进界面时摆一次 (setupScreen) 和按下 mode_switch 之后
     * (Presenter::nextLogicProtoMode)。两条路都走这里, 所以没有第二处写字 ——
     * 和 LogicView::updateLogicViewMode() 一样, 以 Model 的状态为准, 不去反推
     * 屏上现在写的是哪几个字。
     */
    void updateProtoMode(uint32_t mode);

protected:
    /**
     * 按当前模式把 box2 里那几行设置写一遍 —— 选项名 + 当前值 + 选中颜色。
     *
     * 只有 View 自己会调 (进界面、切模式、两个按键), 所以不放进 public。
     * 模式从 Presenter 现取, 不传参: 调用点里有两处手上根本没有模式变量。
     */
    void renderSettings();

    /**
     * mode_switch (切换协议模式) 的回调。
     *
     * 故意用 View 自己的一份 Callback, 而不是往 ViewBase 那个 generated 的
     * buttonCallback 里塞分支 —— 那样 TouchGFX Designer 每次重新生成代码都会把
     * 它冲掉。mode_switch 在 Designer 里没配 interaction, 两边不打架。
     * (和 LogicView 里 toggleButton1 的做法完全一样。)
     */
    touchgfx::Callback<Logic_SettingView, const touchgfx::AbstractButton&> modeSwitchCallback;
    void modeSwitchCallbackHandler(const touchgfx::AbstractButton& src);

    /** upordown (上下选行) 的回调。只动 selRow, 不碰 Model —— 纯 UI 状态。 */
    touchgfx::Callback<Logic_SettingView, const touchgfx::AbstractButton&> rowSwitchCallback;
    void rowSwitchCallbackHandler(const touchgfx::AbstractButton& src);

    /** leftorright (改这一行的值) 的回调。算出下一个候选值下标, 交给 Presenter 存。 */
    touchgfx::Callback<Logic_SettingView, const touchgfx::AbstractButton&> valueSwitchCallback;
    void valueSwitchCallbackHandler(const touchgfx::AbstractButton& src);

    /**
     * 当前选中 box2 里的第几行 (从 0 数)。
     *
     * 纯 UI 状态, 不进 Model —— 换个界面再回来, 或者切了模式, 都从第 0 行重新开始
     * 就够了, 没人会期望"上次光标停在第三行"这种事被记住。
     *
     * 它必须始终 < 当前模式的行数: 切模式时行数会变 (UART 4 行、CAN 1 行、I2C
     * 0 行), 所以 updateProtoMode() 里会把它归 0。
     */
    uint8_t selRow;
};

#endif // LOGIC_SETTINGVIEW_HPP
