#ifndef LOGICVIEW_HPP
#define LOGICVIEW_HPP

#include <gui_generated/logic_screen/LogicViewBase.hpp>
#include <gui/logic_screen/LogicPresenter.hpp>

class LogicView : public LogicViewBase
{
public:
    LogicView();
    virtual ~LogicView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
protected:
};

#endif // LOGICVIEW_HPP
