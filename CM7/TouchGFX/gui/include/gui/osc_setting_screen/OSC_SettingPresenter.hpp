#ifndef OSC_SETTINGPRESENTER_HPP
#define OSC_SETTINGPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

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

private:
    OSC_SettingPresenter();

    OSC_SettingView& view;
};

#endif // OSC_SETTINGPRESENTER_HPP
