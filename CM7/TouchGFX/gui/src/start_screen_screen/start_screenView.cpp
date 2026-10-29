#include <gui/start_screen_screen/start_screenView.hpp>
#include <touchgfx/Unicode.hpp>
#include "firmware_version.h"

start_screenView::start_screenView()
{

}

void start_screenView::setupScreen()
{
    start_screenViewBase::setupScreen();

    touchgfx::Unicode::strncpy(textArea3Buffer, FIRMWARE_VERSION_STRING,
                               TEXTAREA3_SIZE);
    textArea3.resizeToCurrentText();
    textArea3.invalidate();
}

void start_screenView::tearDownScreen()
{
    start_screenViewBase::tearDownScreen();
}
