#ifndef OSC_SETTINGVIEW_HPP
#define OSC_SETTINGVIEW_HPP

#include <gui_generated/osc_setting_screen/OSC_SettingViewBase.hpp>
#include <gui/osc_setting_screen/OSC_SettingPresenter.hpp>

class OSC_SettingView : public OSC_SettingViewBase
{
public:
    OSC_SettingView();
    virtual ~OSC_SettingView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
protected:
};

#endif // OSC_SETTINGVIEW_HPP
