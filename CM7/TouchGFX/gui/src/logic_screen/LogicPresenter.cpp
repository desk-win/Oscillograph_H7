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
    /* 只转交给 Model —— 时基还是滚窗口由它按视图模式分派。重画不用在这里操心:
     * 窗口变过之后那一屏是被重新归约出来的, 会照常走
     * logicUpdated → view.updateLogic 那条路回来。 */
    model->turnLogicEncoder(delta);
}

void LogicPresenter::encoderLongPressed()
{
    toggleLogicViewMode();
}

void LogicPresenter::toggleLogicViewMode()
{
    /* 先切状态, 再按新状态摆图 —— 顺序反过来就是照旧状态摆, 白摆一次。 */
    model->toggleLogicViewMode();
    view.updateLogicViewMode(model->getLogicViewMode());
}

uint32_t LogicPresenter::getLogicViewMode()
{
    return model->getLogicViewMode();
}
