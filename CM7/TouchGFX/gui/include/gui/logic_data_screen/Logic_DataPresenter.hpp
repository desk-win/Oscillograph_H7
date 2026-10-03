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
     * 这一屏被切进来了 (切屏时框架自动调): 请 Model 解一次码。
     *
     * **只置一个记号**, 真正的解码在 Model::tick 里做 —— 这一屏的 setupScreen
     * 是同步跑的, 几十毫秒的解码塞在那儿会把切屏动画一起堵住。
     *
     * 每次进屏都请求一次没有副作用: Model 那边拿采集代数一比, 同一轮采集只会
     * 真解一遍。
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
     * 里面清的不只是波形记录, 解码结果也一起丢 (Model::clearLogicData 里说明
     * 了为什么必须一起丢)。
     *
     * 只做清数据这一件事, 界面跳转不归 Presenter 管 (这条路上 View 也要跳,
     * 是 View 自己调 application().goto...)。Save 以后走另一条路: 它得先读
     * 数据再清, 所以清的动作放在这里由调用方决定, 不做成按键的隐含副作用。
     */
    void cancelLogicData();

    /**
     * 取上一次解码的结果, 给 View::setupScreen 用。
     *
     * 用在"这一轮采集早就解过了"的情形: 那时候 tick 不会再发 logicDataUpdated
     * (记号没变), 屏幕上该显示什么就靠这一次取。进去之前没解过的话取到 0 条,
     * 屏幕先空着, 等 tick 解完回调过来再填。
     */
    const log_ana_item_t* getLogicItems(uint32_t* count);

    /**
     * 解码结果出来了 (来自 ModelListener)。**一次性回调** —— 只有真的解完一轮
     * 才会被调到, 不是每个 tick。
     *
     * count == 0 表示这次没东西可显示 (采集还在跑 / 还没采过 / 一条都没解出来),
     * View 收到就把文字区清空 —— 不是"保持上一次的"。
     */
    virtual void logicDataUpdated(const log_ana_item_t* items, uint32_t count);

private:
    Logic_DataPresenter();

    Logic_DataView& view;
};

#endif // LOGIC_DATAPRESENTER_HPP
