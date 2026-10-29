#ifndef OTA_SCREENPRESENTER_HPP
#define OTA_SCREENPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class OTA_ScreenView;

class OTA_ScreenPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    OTA_ScreenPresenter(OTA_ScreenView& v);

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

    virtual void otaStatusUpdated(const OTA_Status& status);

    virtual ~OTA_ScreenPresenter() {}

private:
    OTA_ScreenPresenter();

    void updateOtaStatus();

    OTA_ScreenView& view;
};

#endif // OTA_SCREENPRESENTER_HPP
