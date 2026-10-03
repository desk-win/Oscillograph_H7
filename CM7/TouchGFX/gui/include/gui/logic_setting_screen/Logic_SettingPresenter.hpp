#ifndef LOGIC_SETTINGPRESENTER_HPP
#define LOGIC_SETTINGPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class Logic_SettingView;

class Logic_SettingPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    Logic_SettingPresenter(Logic_SettingView& v);

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

    virtual ~Logic_SettingPresenter() {}

    /**
     * 当前协议模式 (LOG_PROTO_*)。View 进界面时按它写一遍那几块文字区 ——
     * 状态在 Model 里, 不跟界面走, 所以不能指望屏上还留着上次写的字。
     */
    uint32_t getLogicProtoMode();

    /**
     * mode_switch 按下了: 轮到下一个协议模式 (UART→I2C→SPI→CAN→UART)。
     *
     * 状态和写字收在这一个入口里, 免得两个调用点各写一次、漏一个 —— 和
     * LogicPresenter::toggleLogicViewMode() 一个路子。顺序不能反: 先切状态,
     * 再按**新**状态写字, 反过来就是照旧模式写一遍, 白写。
     */
    void nextLogicProtoMode();

    /**
     * 第 row 行当前选的是候选值里的第几个 / 改成第几个。
     *
     * 两个都只是转给 Model —— 候选值有几个、叫什么名字是设置屏自己的事 (表在那儿,
     * 它本来就要拿文本 id 去写字), 这一层和 Model 都不掺和, 只当着不透明的格子
     * 记下来。所以这里没有"下一个是什么"的判断, 那是 View 算好了再进来写的。
     */
    uint8_t getLogicCfgValue(uint32_t row);

    void setLogicCfgValue(uint32_t row, uint8_t value);

    /**
     * 进这一屏: 已提交的配置整个拷进草稿, 从这里开始改的都是草稿 (Model.hpp 里
     * 那一大段说的)。View::setupScreen 的第一件事 —— 必须赶在摆界面之前, 不然
     * 摆出来的是上一次没 Save 的那份脏草稿。
     */
    void beginLogicEdit();

    /**
     * 按了 Save: 草稿 → 已提交, 并且整份推给解码层 (logic_analyze.c)。
     *
     * cfg 由 View 按草稿翻出来 —— 候选值下标到真值的翻表在它那儿 (顺序只有它
     * 知道)。这一层不掺和, 只是转交。
     */
    void commitLogicConfig(const log_ana_cfg_t& cfg);

private:
    Logic_SettingPresenter();

    Logic_SettingView& view;
};

#endif // LOGIC_SETTINGPRESENTER_HPP
