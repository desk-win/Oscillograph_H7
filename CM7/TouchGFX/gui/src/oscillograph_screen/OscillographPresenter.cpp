#include <gui/oscillograph_screen/OscillographView.hpp>
#include <gui/oscillograph_screen/OscillographPresenter.hpp>

OscillographPresenter::OscillographPresenter(OscillographView& v)
    : view(v)
{

}

void OscillographPresenter::activate()
{

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

void OscillographPresenter::traceUpdated(const uint16_t* ch1, const uint16_t* ch2,
                                         bool fresh1, bool fresh2)
{
    view.updateTrace(ch1, ch2, fresh1, fresh2);
}
