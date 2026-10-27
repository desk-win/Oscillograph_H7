#ifndef OTA_SCREENVIEW_HPP
#define OTA_SCREENVIEW_HPP

#include <gui_generated/ota_screen_screen/OTA_ScreenViewBase.hpp>
#include <gui/ota_screen_screen/OTA_ScreenPresenter.hpp>

class OTA_ScreenView : public OTA_ScreenViewBase
{
public:
    OTA_ScreenView();
    virtual ~OTA_ScreenView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
protected:
};

#endif // OTA_SCREENVIEW_HPP
