#ifndef OSCILLOGRAPHPRESENTER_HPP
#define OSCILLOGRAPHPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class OscillographView;

class OscillographPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    OscillographPresenter(OscillographView& v);

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

    virtual ~OscillographPresenter() {}

    /**
     * 某一个通道的采集开关被拨到 ON。
     */
    void startAcquisition(osc_trace_ch_t ch);

    /**
     * 某一个通道的采集开关被拨到 OFF。停下来的只是"采集和刷新", Model 那边兜
     * 着最后一屏数据; 屏幕上那一路由 View 直接擦掉 (见 OscillographView.cpp)。
     */
    void stopAcquisition(osc_trace_ch_t ch);

    /**
     * 这一路采集是否在跑。View 用它决定进界面时把开关摆到哪一面。
     */
    bool isAcquisitionRunning(osc_trace_ch_t ch);

    /**
     * Model 算好一屏波形了 (来自 ModelListener)。两个通道的内容都在里面,
     * fresh1/fresh2 说明哪几路是真的新数据 —— 谁该画由 View 那边的控件按它
     * 决定。
     */
    virtual void traceUpdated(const uint16_t* ch1, const uint16_t* ch2,
                              bool fresh1, bool fresh2);

    /**
     * 编码器转过 delta 格 (来自 ModelListener)。转发给 Model, 由它按"当前选中
     * 哪一路、调时基还是调倍数"改值, 然后把新的界面文字交给 View。
     */
    virtual void encoderTurned(int16_t delta);

    /**
     * 编码器上那个开关**短按**: 换选中哪一路 (CH1 ↔ CH2)。
     */
    virtual void encoderPressed();

    /**
     * 编码器上那个开关**长按**: 换当前这一路调时基还是调放大倍数。
     */
    virtual void encoderLongPressed();

    /**
     * 进屏时把界面上的模式文字摆成 Model 里记的那份 —— 切屏回来时不会丢。
     */
    void updateModeText();

private:
    OscillographPresenter();

    OscillographView& view;
};

#endif // OSCILLOGRAPHPRESENTER_HPP
