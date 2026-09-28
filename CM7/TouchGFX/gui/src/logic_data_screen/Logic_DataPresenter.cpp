#include <gui/logic_data_screen/Logic_DataView.hpp>
#include <gui/logic_data_screen/Logic_DataPresenter.hpp>

Logic_DataPresenter::Logic_DataPresenter(Logic_DataView& v)
    : view(v)
{

}

void Logic_DataPresenter::activate()
{

}

void Logic_DataPresenter::deactivate()
{

}

void Logic_DataPresenter::cancelLogicData()
{
    model->clearLogicData();
}
