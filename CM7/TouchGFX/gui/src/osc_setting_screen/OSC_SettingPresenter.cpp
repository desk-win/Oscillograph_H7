#include <gui/osc_setting_screen/OSC_SettingView.hpp>
#include <gui/osc_setting_screen/OSC_SettingPresenter.hpp>

OSC_SettingPresenter::OSC_SettingPresenter(OSC_SettingView& v)
    : view(v)
{

}

void OSC_SettingPresenter::activate()
{

}

void OSC_SettingPresenter::deactivate()
{

}

void OSC_SettingPresenter::beginOscEdit()
{
    model->beginOscEdit();
}

uint32_t OSC_SettingPresenter::getOscCoupleDraft(osc_trace_ch_t ch)
{
    return model->getOscCoupleDraft(ch);
}

void OSC_SettingPresenter::setOscCoupleDraft(osc_trace_ch_t ch, uint32_t coup)
{
    model->setOscCoupleDraft(ch, coup);
}

void OSC_SettingPresenter::commitOscEdit()
{
    model->commitOscEdit();
}
