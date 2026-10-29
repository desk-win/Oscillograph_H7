#ifndef OTA_SCREENVIEW_HPP
#define OTA_SCREENVIEW_HPP

#include <gui_generated/ota_screen_screen/OTA_ScreenViewBase.hpp>
#include <gui/ota_screen_screen/OTA_ScreenPresenter.hpp>
#include "ota.h"

class OTA_ScreenView : public OTA_ScreenViewBase
{
public:
    OTA_ScreenView();
    virtual ~OTA_ScreenView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    /** 更新进度条、百分比和当前 OTA 阶段。 */
    void updateStatus(const OTA_Status& status);
protected:
    uint8_t lastStage;
    uint8_t lastPercent;
};

#endif // OTA_SCREENVIEW_HPP
