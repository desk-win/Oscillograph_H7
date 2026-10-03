#ifndef LOGIC_DATAVIEW_HPP
#define LOGIC_DATAVIEW_HPP

#include <gui_generated/logic_data_screen/Logic_DataViewBase.hpp>
#include <gui/logic_data_screen/Logic_DataPresenter.hpp>

class Logic_DataView : public Logic_DataViewBase
{
public:
    Logic_DataView();
    virtual ~Logic_DataView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    /**
     * 把解码结果摆到白框里那块文字区上 (Presenter 转发 ModelListener 的回调,
     * setupScreen 也直接调它一次)。
     *
     * count == 0 就是清空 —— 不是"保持上一次的"。
     *
     * 现在只认 UART (每条一个字节); 加协议时改的是这一个函数 (按 items[k].proto
     * 分派不同的排法)。
     */
    void showLogicData(const log_ana_item_t* items, uint32_t count);

protected:
    /**
     * Cancel 按键的回调。
     *
     * 故意用 View 自己的一份 Callback —— 和 LogicView 那个 runCallback 同一个
     * 理由: 往 ViewBase 里 generated 的 buttonCallbackHandler 塞分支的话,
     * TouchGFX Designer 每次重新生成代码都会把它冲掉。
     *
     * 代价是 base 里 cancel 那条 "goto Logic" 的 interaction 被顶掉了, 所以
     * 跳转要在这里自己补 (见 .cpp)。
     */
    touchgfx::Callback<Logic_DataView, const touchgfx::AbstractButton&> cancelCallback;
    void cancelCallbackHandler(const touchgfx::AbstractButton& src);
};

#endif // LOGIC_DATAVIEW_HPP
