#ifndef LOGICPRESENTER_HPP
#define LOGICPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class LogicView;

class LogicPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    LogicPresenter(LogicView& v);

    /**
     * The activate function is called automatically when this screen is "switched in"
     * (ie. made active). Initialization logic can be placed here.
     */
    virtual void activate();

    /**
     * The deactivate function is called automatically when this screen is "switched out"
     * (ie. made inactive). Teardown functionality can be placed here.
     */
    virtual void deactivate();

    virtual ~LogicPresenter() {}

private:
    LogicPresenter();

    LogicView& view;
};

#endif // LOGICPRESENTER_HPP
