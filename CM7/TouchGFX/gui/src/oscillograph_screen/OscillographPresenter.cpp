#include <gui/oscillograph_screen/OscillographView.hpp>
#include <gui/oscillograph_screen/OscillographPresenter.hpp>

OscillographPresenter::OscillographPresenter(OscillographView& v)
    : view(v)
{

}

void OscillographPresenter::activate()
{
    /* 进屏时做两件事, 顺序不能反:
     *
     *   1) 把 Model 记的设置推给硬件一遍 (syncOscUi)。兜的是"两核各自独立复位"
     *      —— 详见 Model::syncOscUi 的说明。它只写共享块, 不碰屏幕。
     *
     *   2) 把模式文字摆成 Model 里那份 (updateModeText)。切屏回来时不能丢,
     *      而且 ViewBase 生成的初始文本是空的, 不摆一次屏幕右下角就是空的。
     *
     * activate() 在 setupScreen() 之后被调 (MVPApplication::finalizeTransition),
     * 所以这时控件都已经摆好位置了。 */
    model->syncOscUi();
    updateModeText();
}

void OscillographPresenter::deactivate()
{

}

void OscillographPresenter::startAcquisition(osc_trace_ch_t ch)
{
    model->startAcquisition(ch);
}

void OscillographPresenter::stopAcquisition(osc_trace_ch_t ch)
{
    model->stopAcquisition(ch);
}

bool OscillographPresenter::isAcquisitionRunning(osc_trace_ch_t ch)
{
    return model->isRunning(ch);
}

uint8_t OscillographPresenter::getOscGain(osc_trace_ch_t ch)
{
    return model->getOscGain(ch);
}

void OscillographPresenter::traceUpdated(const uint16_t* ch1, const uint16_t* ch2,
                                         const osc_meas_t* meas1, const osc_meas_t* meas2,
                                         bool fresh1, bool fresh2)
{
    view.updateTrace(ch1, ch2, meas1, meas2, fresh1, fresh2);
}

/* ===========================================================================
 * 编码器
 *
 * 三条路都短: 改 Model 里的状态, 再把结果摆到屏幕上。改值本身不产生任何延时
 * (时基只是改一个下标, 倍数只是往共享块里写一个字), 真正的效果是异步的 ——
 * 时基要等下一次 OSC_Trace_Build 才看得到, 倍数要等 CM4 下一轮主循环摆引脚。
 * 所以这里不用等, 改完就把文字更新了: 文字是"设置", 不是"结果"。
 * ========================================================================= */

void OscillographPresenter::encoderTurned(int16_t delta)
{
    model->turnOscEncoder(delta);
    updateModeText();
}

void OscillographPresenter::encoderPressed()
{
    model->toggleOscSelection();
    updateModeText();
}

void OscillographPresenter::encoderLongPressed()
{
    model->toggleOscMode();
    updateModeText();
}

void OscillographPresenter::updateModeText()
{
    osc_ui_state_t st;
    model->getOscUiState(&st);
    view.updateModeText(st);
}
