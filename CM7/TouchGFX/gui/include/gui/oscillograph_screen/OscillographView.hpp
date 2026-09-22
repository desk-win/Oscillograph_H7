#ifndef OSCILLOGRAPHVIEW_HPP
#define OSCILLOGRAPHVIEW_HPP

#include <gui_generated/oscillograph_screen/OscillographViewBase.hpp>
#include <gui/oscillograph_screen/OscillographPresenter.hpp>

class OscillographView : public OscillographViewBase
{
public:
    OscillographView();
    virtual ~OscillographView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
protected:
};

#endif // OSCILLOGRAPHVIEW_HPP
