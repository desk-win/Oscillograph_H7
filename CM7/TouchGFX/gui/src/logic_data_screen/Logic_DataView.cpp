#include <gui/logic_data_screen/Logic_DataView.hpp>

Logic_DataView::Logic_DataView()
    : cancelCallback(this, &Logic_DataView::cancelCallbackHandler)
{

}

void Logic_DataView::setupScreen()
{
    Logic_DataViewBase::setupScreen();

    /* 顶掉 base 里 cancel 那条 "goto Logic" 的 interaction。
     *
     * 光靠 setAction 不够 —— ToggleButton 那类控件的 action 是自己设的, 而
     * ButtonWithLabel 在 Designer 里配了 interaction 之后, base 的
     * buttonCallbackHandler 里会直接写死一句 application().goto...()。所以这里
     * 不只是"接管", 是"换掉" —— 跳转那一句得在下面的 handler 里自己补回来。
     *
     * setupScreen() 每次进这个界面都会被调, setAction 是幂等的。
     * save 不动, 它现在还是 base 里那条空跳转。 */
    cancel.setAction(cancelCallback);
}

void Logic_DataView::tearDownScreen()
{
    Logic_DataViewBase::tearDownScreen();
}

void Logic_DataView::cancelCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &cancel)
    {
        return;
    }

    /* 先清再跳 —— 顺序不能倒过来。
     *
     * 清的那一下会让 Model 发一次 logicUpdated(ncols = 0), 收到的是**当前活动
     * 屏幕**的 Presenter, 也就是这一屏的 Logic_DataPresenter, 它的
     * logicUpdated 是默认实现 (什么都不做)。所以清完屏幕上不会自己变 ——
     * Logic 控件是靠回去时 LogicView::setupScreen() 看见 ncols == 0 自己擦的。 */
    presenter->cancelLogicData();

    application().gotoLogicScreenSlideTransitionEast();
}
