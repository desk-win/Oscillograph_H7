#ifndef LOGIC_DATAPRESENTER_HPP
#define LOGIC_DATAPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class Logic_DataView;

class Logic_DataPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    Logic_DataPresenter(Logic_DataView& v);

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

    virtual ~Logic_DataPresenter() {}

    /**
     * Cancel: 丢掉这次采集的数据, 回到"还没采过"的状态 —— 下一次采集从零开始。
     *
     * 只做清数据这一件事, 界面跳转不归 Presenter 管 (这条路上 View 也要跳,
     * 是 View 自己调 application().goto...)。Save 以后走另一条路: 它得先读
     * 数据再清, 所以清的动作放在这里由调用方决定, 不做成按键的隐含副作用。
     */
    void cancelLogicData();

private:
    Logic_DataPresenter();

    Logic_DataView& view;
};

#endif // LOGIC_DATAPRESENTER_HPP
