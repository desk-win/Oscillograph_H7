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
    updateOtaStatus();
}

void OTA_ScreenPresenter::deactivate()
{
    OTA_RequestStop();
}

void OTA_ScreenPresenter::otaStatusUpdated(const OTA_Status& status)
{
    view.updateStatus(status);
}

void OTA_ScreenPresenter::updateOtaStatus()
{
    OTA_Status status;
    model->getOtaStatus(&status);
    view.updateStatus(status);
}
