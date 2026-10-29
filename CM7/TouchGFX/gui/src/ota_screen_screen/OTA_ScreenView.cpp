#include <gui/ota_screen_screen/OTA_ScreenView.hpp>
#include <texts/TextKeysAndLanguages.hpp>
#include <touchgfx/Unicode.hpp>

OTA_ScreenView::OTA_ScreenView()
    : lastStage(0xFFU), lastPercent(0xFFU)
{

}

void OTA_ScreenView::setupScreen()
{
    OTA_ScreenViewBase::setupScreen();
}

void OTA_ScreenView::tearDownScreen()
{
    OTA_ScreenViewBase::tearDownScreen();
}

void OTA_ScreenView::updateStatus(const OTA_Status& status)
{
    const uint8_t percent = (status.percent > 100U) ? 100U : status.percent;
    const bool stageChanged = status.stage != lastStage;
    const bool percentChanged = percent != lastPercent;

    if (percentChanged)
    {
        slider1.setValue(percent);
        slider1.invalidate();
    }

    if (stageChanged || percentChanged)
    {
        switch (status.stage)
        {
        case OTA_STAGE_WAITING_M7:
        case OTA_STAGE_RECEIVING_M7:
            textArea1.setTypedText(touchgfx::TypedText(T___SINGLEUSE_W9UR));
            touchgfx::Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE,
                                        "M7 %u", (unsigned int)percent);
            break;

        case OTA_STAGE_WAITING_M4:
        case OTA_STAGE_RECEIVING_M4:
            textArea1.setTypedText(touchgfx::TypedText(T___SINGLEUSE_W9UR));
            touchgfx::Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE,
                                        "M4 %u", (unsigned int)percent);
            break;

        case OTA_STAGE_VERIFYING_M7:
            textArea1.setTypedText(touchgfx::TypedText(T___SINGLEUSE_QK9F));
            touchgfx::Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE,
                                        "CHECK M7");
            break;

        case OTA_STAGE_VERIFYING_M4:
            textArea1.setTypedText(touchgfx::TypedText(T___SINGLEUSE_QK9F));
            touchgfx::Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE,
                                        "CHECK M4");
            break;

        case OTA_STAGE_FINALIZING:
            textArea1.setTypedText(touchgfx::TypedText(T___SINGLEUSE_QK9F));
            touchgfx::Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE,
                                        "WRITING");
            break;

        case OTA_STAGE_DONE:
            textArea1.setTypedText(touchgfx::TypedText(T___SINGLEUSE_QK9F));
            touchgfx::Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE,
                                        "SUCCESS");
            break;

        case OTA_STAGE_FAILED:
            textArea1.setTypedText(touchgfx::TypedText(T___SINGLEUSE_QK9F));
            touchgfx::Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE,
                                        "FAILED");
            break;

        case OTA_STAGE_IDLE:
        default:
            textArea1.setTypedText(touchgfx::TypedText(T___SINGLEUSE_QK9F));
            touchgfx::Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE,
                                        "READY");
            break;
        }

        textArea1.resizeToCurrentText();
        textArea1.invalidate();
    }

    lastStage = status.stage;
    lastPercent = percent;
}
