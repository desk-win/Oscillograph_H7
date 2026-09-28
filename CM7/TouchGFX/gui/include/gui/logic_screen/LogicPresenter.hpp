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

    /**
     * 右上角那个采集开关拨到 ON。里面会把示波器两路停掉 (两个屏幕的开关互相
     * 独立, 切屏不改变对方的状态, 所以互斥是在这一层强制的)。
     */
    void startLogicCapture();

    /**
     * 采集开关拨到 OFF, 或者采完之后用户再拨一次把它关掉。
     */
    void stopLogicCapture();

    /**
     * 采集是否在跑。View 用它决定进界面时把开关摆到哪一面。
     */
    bool isLogicRunning();

    /**
     * 取上一屏逻辑波形 (Model 里存着的那一份)。返回列数, 0 表示还没采过。
     */
    uint32_t getLogicCols(const uint16_t** level, const uint16_t** change);

    /**
     * Model 算好一屏逻辑波形了 (来自 ModelListener)。一次性回调 —— 只有真的
     * 算出新一屏的那一拍才会被调到, 不是每个 tick。
     */
    virtual void logicUpdated(const uint16_t* level, const uint16_t* change,
                              uint32_t ncols);

private:
    LogicPresenter();

    LogicView& view;
};

#endif // LOGICPRESENTER_HPP
