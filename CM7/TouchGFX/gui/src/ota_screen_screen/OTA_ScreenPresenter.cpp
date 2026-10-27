#include <gui/ota_screen_screen/OTA_ScreenView.hpp>
#include <gui/ota_screen_screen/OTA_ScreenPresenter.hpp>
#include "ota.h"

OTA_ScreenPresenter::OTA_ScreenPresenter(OTA_ScreenView& v)
    : view(v)
{

}

void OTA_ScreenPresenter::activate()
{
    (void)OTA_RequestStart();
}

void OTA_ScreenPresenter::deactivate()
{
    OTA_RequestStop();
}
