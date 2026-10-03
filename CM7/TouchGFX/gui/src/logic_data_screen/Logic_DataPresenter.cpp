#include <gui/logic_data_screen/Logic_DataView.hpp>
#include <gui/logic_data_screen/Logic_DataPresenter.hpp>

Logic_DataPresenter::Logic_DataPresenter(Logic_DataView& v)
    : view(v)
{

}

void Logic_DataPresenter::activate()
{
    /* 只置请求, 不解 —— 解码在 Model::tick 里做 (理由见 .hpp)。
     *
     * 放在这里而不是 setupScreen 里: 切屏时这一对都会被调到, 但 activate 才是
     * "这一屏变成当前屏"的语义。逻辑屏那边 (LogicPresenter) 也是拿 activate
     * 干同类的事 —— 以 Model 的状态为准摆界面。 */
    model->requestLogicDecode();
}

void Logic_DataPresenter::deactivate()
{

}

void Logic_DataPresenter::cancelLogicData()
{
    model->clearLogicData();
}

const log_ana_item_t* Logic_DataPresenter::getLogicItems(uint32_t* count)
{
    return model->getLogicItems(count);
}

void Logic_DataPresenter::logicDataUpdated(const log_ana_item_t* items, uint32_t count)
{
    /* 只是转交。写字是 View 的事 (它才有那块文字区), 状态在 Model / ModelListener
     * 里 —— 这一层把两件事串起来, 免得两个调用点各写一次、漏一个。 */
    view.showLogicData(items, count);
}
