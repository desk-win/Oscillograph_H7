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

void LogicPresenter::encoderTurned(int16_t delta)
{
    /* 只转交给 Model。重画不用在这里操心 —— 换了档位之后那一屏是被重新归约
     * 出来的, 会照常走 logicUpdated → view.updateLogic 那条路回来。 */
    model->zoomLogicTimebase(delta);
}
