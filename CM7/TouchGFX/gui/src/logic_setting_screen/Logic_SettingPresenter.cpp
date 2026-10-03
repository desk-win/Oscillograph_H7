#include <gui/logic_setting_screen/Logic_SettingView.hpp>
#include <gui/logic_setting_screen/Logic_SettingPresenter.hpp>

Logic_SettingPresenter::Logic_SettingPresenter(Logic_SettingView& v)
    : view(v)
{

}

void Logic_SettingPresenter::activate()
{

}

void Logic_SettingPresenter::deactivate()
{

}

uint32_t Logic_SettingPresenter::getLogicProtoMode()
{
    return model->getLogicProtoMode();
}

void Logic_SettingPresenter::nextLogicProtoMode()
{
    /* 取模轮询 —— UART→I2C→SPI→CAN→UART。顺序就是 Model.hpp 里 LOG_PROTO_*
     * 的声明顺序, 想改顺序去改那儿的枚举, 这里不用动。 */
    model->setLogicProtoMode((model->getLogicProtoMode() + 1u) % LOG_PROTO_COUNT);

    view.updateProtoMode(model->getLogicProtoMode());
}

uint8_t Logic_SettingPresenter::getLogicCfgValue(uint32_t row)
{
    /* 模式从 Model 现取 —— 不劳 View 传进来, 少一处"传的和 Model 里的不一致"
     * 的机会。读到的永远是当前这一屏正在显示的那个模式的配置。 */
    return model->getLogicCfgValue(model->getLogicProtoMode(), row);
}

void Logic_SettingPresenter::setLogicCfgValue(uint32_t row, uint8_t value)
{
    model->setLogicCfgValue(model->getLogicProtoMode(), row, value);
}

void Logic_SettingPresenter::beginLogicEdit()
{
    model->beginLogicEdit();
}

void Logic_SettingPresenter::commitLogicConfig(const log_ana_cfg_t& cfg)
{
    model->commitLogicConfig(cfg);
}
