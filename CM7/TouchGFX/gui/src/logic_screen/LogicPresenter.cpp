#include <gui/logic_screen/LogicView.hpp>
#include <gui/logic_screen/LogicPresenter.hpp>

LogicPresenter::LogicPresenter(LogicView& v)
    : view(v)
{

}

void LogicPresenter::activate()
{

}

void LogicPresenter::deactivate()
{

}

void LogicPresenter::startLogicCapture()
{
    model->startLogicCapture();
}

void LogicPresenter::stopLogicCapture()
{
    model->stopLogicCapture();
}

bool LogicPresenter::isLogicRunning()
{
    return model->isLogicRunning();
}

uint32_t LogicPresenter::getLogicCols(const uint16_t** level, const uint16_t** change)
{
    return model->getLogicCols(level, change);
}

void LogicPresenter::logicUpdated(const uint16_t* level, const uint16_t* change,
                                  uint32_t ncols)
{
    view.updateLogic(level, change, ncols);
}
