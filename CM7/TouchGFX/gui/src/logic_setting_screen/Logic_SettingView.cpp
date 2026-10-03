#include <gui/logic_setting_screen/Logic_SettingView.hpp>
#include <touchgfx/Color.hpp>
#include <texts/TextKeysAndLanguages.hpp>   /* T_PROTOUART / T_SIGRX / T_VAL9600 ... */

/* ===========================================================================
 * 这个屏上有两张表
 *
 *   ① 通道表 —— 每种协议模式下, 8 个通道各收的是哪根线 (屏幕左边那一片)。
 *   ② 配置表 —— 每种协议模式下, box2 里要显示哪几行可配项、每行有哪些候选值
 *      (屏幕中间那一片)。
 *
 * 两张表都是"每种模式一份", 下标都是 Model.hpp 里 LOG_PROTO_* 的值。之所以拆成
 * 两个结构体而不是塞进同一个, 是因为 ② 里每一行还要再挂一张候选值表, 混在一起
 * 读起来就乱了。
 *
 * 表里的字**全是 texts.xml 里的实文本**, 不是通配模板 —— 这是有意选的。字形是
 * 生成器按 texts.xml 收进字体里的, 它只把 <...> 当占位符剥掉, 不认识代码里写的
 * 字面量: 在代码里自己写 "115200" 是不算数的, 缺的字母上屏时会按 Typography 的
 * FallbackCharacter 画成 '?' —— 不报错、不警告, 只能靠肉眼看出来。
 *
 * 所以加一个新的选项或候选值, 顺序是: 先在 texts.xml 里加一条实文本 → 重新生成
 * 代码 → 才能在这两张表里引用它。(同 LogicView 里写视图模式那一段的注释。)
 * ========================================================================= */
namespace
{
/* ---- ① 每个通道收的是哪根线 ----------------------------------------------
 *
 * 通道映射是接线约定, 不是这里能改的东西:
 *   UART: CH1 = RX
 *   I2C : CH1 = SCL, CH2 = SDA
 *   SPI : CH1 = CS, CH2 = SCK, CH3 = MISO, CH4 = MOSI
 *   CAN : CH1 = RX
 */

/* ---- ② 可配项的候选值 -----------------------------------------------------
 *
 * 一条表 = 一个选项的所有取值, 顺序就是按下 leftorright 的轮询顺序。开机默认
 * 选第 0 个 (见 Model::Model() 里把 logicCfgVal 全清 0), 所以把想当默认值的
 * 放在最前面。 */
const TEXTS VAL_UART_BAUD[3]   = { T_VAL9600, T_VAL38400, T_VAL115200 };
const TEXTS VAL_UART_STOP[3]   = { T_VAL1, T_VAL1_5, T_VAL2 };
const TEXTS VAL_UART_DATA[2]   = { T_VAL7, T_VAL8 };
const TEXTS VAL_UART_PARITY[3] = { T_VALNONE, T_VALODD, T_VALEVEN };

/* CPOL 和 CPHA 的取值都是 1、0, 共用这一条。 */
const TEXTS VAL_SPI_LEVEL[2]   = { T_VAL1, T_VAL0 };

const TEXTS VAL_SPI_CS[2]      = { T_VALHIGH, T_VALLOW };
const TEXTS VAL_CAN_BAUD[3]    = { T_VAL125K, T_VAL250K, T_VAL500K };

/* ---- ③ 候选值下标 → 真值 ---------------------------------------------------
 *
 * ②里那些 VAL_* 管的是"屏幕上写哪几个字", 这里这几张管的是"解码层要什么数"。
 * 两张表按同一个下标一一对齐, 所以每对下面压了一句 static_assert —— 以后往
 * VAL_UART_BAUD 里插一档波特率而忘了改这边, 是编译不过, 而不是拿错的波特率去
 * 解、解出一屏乱码来。
 *
 * 翻表放在这一层, 而不是 Model 或者 logic_analyze: 候选值的顺序本来就是设置屏
 * 的东西 (它要拿文本 id 去写字), 别处不该再存一份。(Model.hpp 里那段说的。)
 */
const uint32_t UART_BAUD_VAL[3]   = { 9600u, 38400u, 115200u };
const uint8_t  UART_STOP_VAL[3]   = { 10u, 15u, 20u };   /* 停止位 ×10 = 1 / 1.5 / 2 位 */
const uint8_t  UART_DATA_VAL[2]   = { 7u, 8u };          /* 数据位个数 */
const uint8_t  UART_PARITY_VAL[3] = { LOG_UART_PARITY_NONE,
                                      LOG_UART_PARITY_ODD,
                                      LOG_UART_PARITY_EVEN };

/* 几个元素 —— 给下面那堆 static_assert 用的。 */
template <typename T, uint32_t N>
constexpr uint32_t nElem(const T (&)[N])
{
    return N;
}

static_assert(nElem(UART_BAUD_VAL)   == nElem(VAL_UART_BAUD),   "波特率: 文本表和真值表条数对不上");
static_assert(nElem(UART_STOP_VAL)   == nElem(VAL_UART_STOP),   "停止位: 文本表和真值表条数对不上");
static_assert(nElem(UART_DATA_VAL)   == nElem(VAL_UART_DATA),   "数据位: 文本表和真值表条数对不上");
static_assert(nElem(UART_PARITY_VAL) == nElem(VAL_UART_PARITY), "校验位: 文本表和真值表条数对不上");

struct ProtoOptRow
{
    TEXTS         label;    /* 选项名 ("baud" / "CPOL" ...) */
    const TEXTS*  values;   /* 候选值文本表 */
    uint8_t       nvalues;  /* 候选值有几个 */
};

/* ROWS_UART 的行号。
 *
 * 按草稿翻真值时是按下标去取的, 用名字比用 0/1/2/3 稳 —— 想换行的顺序, 改这里
 * 和下面那张表的排列就够了, saveCallbackHandler 里不用动。 */
enum
{
    ROW_UART_BAUD = 0,
    ROW_UART_STOP,
    ROW_UART_DATA,
    ROW_UART_PARITY,
    ROW_UART_COUNT
};
static_assert(ROW_UART_COUNT == 4, "ROWS_UART 的行数改了, 这个枚举也要跟着改");

/* 每种协议要配的几行, 从 box2 最上面那行 (setting1) 往下排。 */
const ProtoOptRow ROWS_UART[4] =
{
    { T_OPTBAUD,   VAL_UART_BAUD,   3 },
    { T_OPTSTOP,   VAL_UART_STOP,   3 },
    { T_OPTDATA,   VAL_UART_DATA,   2 },
    { T_OPTPARITY, VAL_UART_PARITY, 3 },
};

const ProtoOptRow ROWS_SPI[3] =
{
    { T_OPTCPOL, VAL_SPI_LEVEL, 2 },
    { T_OPTCPHA, VAL_SPI_LEVEL, 2 },
    { T_OPTCS,   VAL_SPI_CS,    2 },
};

const ProtoOptRow ROWS_CAN[1] =
{
    { T_OPTBAUD, VAL_CAN_BAUD, 3 },
};

/* ---- 两张表合起来的模式表 -------------------------------------------------- */
struct ProtoModeDef
{
    TEXTS   mode;       /* Mode_Value 上写哪几个字 */
    TEXTS   sig[8];     /* CH1..CH8 各收哪根线 (只有前 used 个有用) */
    uint8_t used;       /* 这个模式用到了前几路 */
    const ProtoOptRow* rows;    /* box2 里要配的几行 —— I2C 没有, 是 0 */
    uint8_t nrows;              /* 有几行 */
};

const ProtoModeDef PROTO_MODE_TAB[LOG_PROTO_COUNT] =
{
    /* 下标必须和 Model.hpp 里 LOG_PROTO_* 的值对得上 —— 查表就是拿那个值当
     * 下标用的。没写满的 sig[] 剩下的位置是 0, 那几路反正被藏起来, 用不到。 */
    { T_PROTOUART, { T_SIGRX }, 1, ROWS_UART, 4 },
    /* I2C 没有可配的 (强制 8 位寻址), 四行全藏起来。 */
    { T_PROTOI2C,  { T_SIGSCL, T_SIGSDA }, 2, 0, 0 },
    { T_PROTOSPI,  { T_SIGCS, T_SIGSCK, T_SIGMISO, T_SIGMOSI }, 4, ROWS_SPI, 3 },
    { T_PROTOCAN,  { T_SIGRX }, 1, ROWS_CAN, 1 },
};

/* box2 里那 4 块 setting 文字区 —— 个数由 Designer 定, 这里只能照着数。
 * 断言是拦"Designer 里加了第五个 setting, 而 Model 那边还只留 4 格"这种不同步:
 * 那时候会编译不过, 而不是第五行悄悄不显示。 */
const uint32_t SETTING_ROWS = 4;
static_assert(SETTING_ROWS == LOG_PROTO_CFG_ROWS,
              "box2 里 setting 文字区的个数和 Model 的 LOG_PROTO_CFG_ROWS 对不上了");

/* 取当前模式那一项。越界兜到 UART —— 只可能是 Model 那边被写坏了。 */
const ProtoModeDef& modeDef(uint32_t mode)
{
    return PROTO_MODE_TAB[mode < LOG_PROTO_COUNT ? mode : 0u];
}

/* 把一条实文本拷进通配缓冲区。走 TypedText 取字, 不在代码里拼字面量 —— 理由见
 * 文件头。缓冲区是 ViewBase 里生成好的那几片, 大小也照它们的 SIZE 传。 */
void copyText(touchgfx::Unicode::UnicodeChar* dst, uint16_t size, TEXTS id)
{
    touchgfx::Unicode::strncpy(dst, touchgfx::TypedText(id).getText(), size);
}

/* 选中那一行的颜色。box2 是白底, 没选中的是黑字 —— 这里挑一个在白底上够醒目、
 * 又和屏上已有的配色不打架的。想换就改这三个数。 */
const uint8_t SEL_RGB[3] = { 20, 151, 197 };
}

Logic_SettingView::Logic_SettingView()
    : modeSwitchCallback(this, &Logic_SettingView::modeSwitchCallbackHandler),
      rowSwitchCallback(this, &Logic_SettingView::rowSwitchCallbackHandler),
      valueSwitchCallback(this, &Logic_SettingView::valueSwitchCallbackHandler),
      saveCallback(this, &Logic_SettingView::saveCallbackHandler),
      cancelCallback(this, &Logic_SettingView::cancelCallbackHandler),
      selRow(0u)
{

}

void Logic_SettingView::setupScreen()
{
    Logic_SettingViewBase::setupScreen();

    /* ---- 第一件事: 已提交的配置整个拷进草稿 ----
     *
     * 必须排在下面摆界面之前 —— updateProtoMode() 读的就是草稿。上一次进来改了
     * 没按 Save 就退出的话草稿还是脏的, 不覆盖就会把上次作废的改动又摆到屏上。 */
    presenter->beginLogicEdit();

    /* ViewBase 里那三个按键只有外观没有 interaction, 这里给它们接上。
     * setupScreen() 每次进这个界面都会被调, setAction 是幂等的。 */
    mode_switch.setAction(modeSwitchCallback);
    upordown.setAction(rowSwitchCallback);
    leftorright.setAction(valueSwitchCallback);

    /* Save / Cancel 在 ViewBase 里是配了 interaction 的 (两句一模一样的空跳转),
     * 所以这里不只是"接管", 是"换掉" —— 跳转那句得在两个 handler 里自己补回来。
     * (和 Logic_DataView 里 cancel 的做法一样。) */
    Save.setAction(saveCallback);
    Cancel.setAction(cancelCallback);

    /* 按 Model 的模式摆一次 —— 摆这一下是必须的: 切屏不改模式, 而且配置的取值
     * 也存在 Model 里, 从别的屏切过来时屏上留着的还是上一次写的那几个字。
     * 里面顺带会把 box2 那几行也写一遍 (renderSettings)。 */
    updateProtoMode(presenter->getLogicProtoMode());
}

void Logic_SettingView::tearDownScreen()
{
    Logic_SettingViewBase::tearDownScreen();
}

void Logic_SettingView::modeSwitchCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &mode_switch)
    {
        return;
    }

    /* 只按一下"轮到下一个", 不在这儿算什么现在是第几个 —— 状态在 Model 里, 那才是
     * 唯一的依据。切完由 Presenter 回来调 updateProtoMode() 写字。 */
    presenter->nextLogicProtoMode();
}

void Logic_SettingView::rowSwitchCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &upordown)
    {
        return;
    }

    const ProtoModeDef& def = modeDef(presenter->getLogicProtoMode());
    if (def.nrows == 0u)
    {
        return;     /* I2C 那样没有可配项的, 按下什么都不做 */
    }

    /* 只动光标, 不碰 Model —— 选中第几行是纯 UI 状态。
     * 单个按键只能单向走, 到最后一行回头。 */
    selRow = (uint8_t)((selRow + 1u) % def.nrows);
    renderSettings();
}

void Logic_SettingView::valueSwitchCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &leftorright)
    {
        return;
    }

    const ProtoModeDef& def = modeDef(presenter->getLogicProtoMode());

    /* 没有选中任何一行 (I2C: nrows 是 0) 就没什么可改的。这一句也顺带挡了
     * selRow 越界, 下面拿它查 def.rows 之前不再单独判一次。 */
    if (selRow >= def.nrows)
    {
        return;
    }

    const ProtoOptRow& opt = def.rows[selRow];

    /* 循环取下一个候选值 —— 同样只能单向走, 到末尾回头。 */
    const uint8_t next =
        (uint8_t)((presenter->getLogicCfgValue(selRow) + 1u) % opt.nvalues);

    presenter->setLogicCfgValue(selRow, next);
    renderSettings();
}

void Logic_SettingView::saveCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &Save)
    {
        return;
    }

    const uint32_t mode = presenter->getLogicProtoMode();

    /* 先把整块清零再填。rsvd 那几个字节和 UART 以外的联合成员都留着 0 ——
     * 以后加 I2C 之类的协议, 那几个字段也得是明确的 0, 不能是栈上的旧东西。 */
    log_ana_cfg_t cfg = {};
    cfg.proto = (uint8_t)mode;

    if (mode == LOG_PROTO_UART)
    {
        /* 按下标取真值。下标理论上已经被 renderSettings() 那边按 nvalues 兜过
         * 越界, 这里再兜一道 —— 表被写坏时读表外是查不出来的那种错。 */
        const uint8_t b = presenter->getLogicCfgValue(ROW_UART_BAUD);
        const uint8_t s = presenter->getLogicCfgValue(ROW_UART_STOP);
        const uint8_t d = presenter->getLogicCfgValue(ROW_UART_DATA);
        const uint8_t p = presenter->getLogicCfgValue(ROW_UART_PARITY);

        cfg.u.uart.baud      = UART_BAUD_VAL  [b < nElem(UART_BAUD_VAL)   ? b : 0u];
        cfg.u.uart.stop_x10  = UART_STOP_VAL  [s < nElem(UART_STOP_VAL)   ? s : 0u];
        cfg.u.uart.data_bits = UART_DATA_VAL  [d < nElem(UART_DATA_VAL)   ? d : 0u];
        cfg.u.uart.parity    = UART_PARITY_VAL[p < nElem(UART_PARITY_VAL) ? p : 0u];

        /* 接线约定: UART 的 RX 接在 CH1 上, 也就是通道 0 (见文件头 ①)。
         * 这个不是这里能配的东西, 所以写死。 */
        cfg.u.uart.rx_ch     = 0u;
    }
    /* I2C / SPI / CAN 这一轮只把模式记下来 —— 解码层那三种还没实现
     * (LOG_Analyze_Run 现在只认 UART)。真值那几个字段留在 0, 等那边做了再按各自
     * 的表填。 */

    presenter->commitLogicConfig(cfg);

    application().gotoLogicScreenSlideTransitionEast();
}

void Logic_SettingView::cancelCallbackHandler(const touchgfx::AbstractButton& src)
{
    if (&src != &Cancel)
    {
        return;
    }

    /* 这里**什么都不用回滚**: 这些改动从头到尾只写在草稿上, 已提交那份没碰过,
     * 解码层那份也没碰过。草稿就让它脏着 —— 下次进这一屏 beginLogicEdit() 会整个
     * 覆盖掉。所以只剩跳转一件事。 */
    application().gotoLogicScreenSlideTransitionEast();
}

void Logic_SettingView::updateProtoMode(uint32_t mode)
{
    /* 兜一道越界。只可能是 Model 那边被写坏了, 但拿它当数组下标之前总得挡一下。 */
    if (mode >= LOG_PROTO_COUNT)
    {
        mode = LOG_PROTO_UART;
    }
    const ProtoModeDef& def = modeDef(mode);

    Mode_Value.setTypedText(touchgfx::TypedText(def.mode));
    Mode_Value.invalidate();

    /* 8 块通道文字区, 按 CH1..CH8 排。名字在 ViewBase 里前 4 个是 CHn_Value、
     * 后 4 个是 CHn_value —— 大小写不一样是生成器照 Designer 里的控件名来的,
     * 别顺手"统一"了, 要改得在 Designer 里改。 */
    touchgfx::TextAreaWithOneWildcard* const value[8] =
    {
        &CH1_Value, &CH2_Value, &CH3_value, &CH4_value,
        &CH5_value, &CH6_value, &CH7_value, &CH8_value,
    };

    for (uint32_t ch = 0u; ch < 8u; ch++)
    {
        const bool used = (ch < def.used);

        /* 先标脏, 再改可见性。
         *
         * invalidate() 对**不可见**的控件是空操作 (Drawable 的约定), 而
         * setVisible(false) 自己又不会标脏 —— 两句顺序反了, 上一次写在这儿的字
         * 就没人擦, 会原地留着, 看着像"切了模式那几个字没动"。
         *
         * 从藏变显也一样: 这时上面那句是空操作, 得在 setVisible(true) 之后再补
         * 一次。所以这一个是两次 invalidate(), 不是写重了。 */
        value[ch]->invalidate();
        value[ch]->setVisible(used);
        if (used)
        {
            value[ch]->setTypedText(touchgfx::TypedText(def.sig[ch]));
            value[ch]->invalidate();
        }
    }

    /* 换了协议 = 换了一组要配的选项, 行数也跟着变 (UART 4 行、CAN 1 行、I2C 0 行),
     * 光标回到第一行, 然后把 box2 那几行按新模式重写。 */
    selRow = 0u;
    renderSettings();
}

void Logic_SettingView::renderSettings()
{
    const ProtoModeDef& def = modeDef(presenter->getLogicProtoMode());

    /* 4 块文字区挨个来。控件名 (setting1..4) 和缓冲区 (settingNBuffer1/2) 都是
     * Designer 定的, 只能照着写 —— 和上面通道那一段一样。 */
    touchgfx::TextAreaWithTwoWildcards* const row[SETTING_ROWS] =
    {
        &setting1, &setting2, &setting3, &setting4,
    };
    touchgfx::Unicode::UnicodeChar* const nameBuf[SETTING_ROWS] =
    {
        setting1Buffer1, setting2Buffer1, setting3Buffer1, setting4Buffer1,
    };
    touchgfx::Unicode::UnicodeChar* const valBuf[SETTING_ROWS] =
    {
        setting1Buffer2, setting2Buffer2, setting3Buffer2, setting4Buffer2,
    };
    const uint16_t nameSize[SETTING_ROWS] =
    {
        SETTING1BUFFER1_SIZE, SETTING2BUFFER1_SIZE, SETTING3BUFFER1_SIZE, SETTING4BUFFER1_SIZE,
    };
    const uint16_t valSize[SETTING_ROWS] =
    {
        SETTING1BUFFER2_SIZE, SETTING2BUFFER2_SIZE, SETTING3BUFFER2_SIZE, SETTING4BUFFER2_SIZE,
    };

    const touchgfx::colortype normalColor = touchgfx::Color::getColorFromRGB(0, 0, 0);
    const touchgfx::colortype selColor =
        touchgfx::Color::getColorFromRGB(SEL_RGB[0], SEL_RGB[1], SEL_RGB[2]);

    for (uint32_t r = 0u; r < SETTING_ROWS; r++)
    {
        const bool used = (r < def.nrows);

        /* 藏/显自己不用操心标脏 —— 下面 box2.invalidate() 会把整块面板重画一遍。 */
        row[r]->setVisible(used);
        if (!used)
        {
            continue;
        }

        const ProtoOptRow& opt = def.rows[r];
        copyText(nameBuf[r], nameSize[r], opt.label);

        /* Model 里存的是"选了第几个", 表长随选项不同 (2 个或 3 个) —— 越界兜到
         * 第一个, 不读表外。 */
        const uint32_t v = presenter->getLogicCfgValue(r);
        copyText(valBuf[r], valSize[r], opt.values[v < opt.nvalues ? v : 0u]);

        /* 选中那一行换个颜色。setColor() 自己不标脏, 但底下 box2.invalidate() 会
         * 把整块面板重画一遍, 所以这里不用另外管。 */
        row[r]->setColor((r == selRow) ? selColor : normalColor);

        /* 这个控件是 AutoSize, 而生成的时候两个缓冲区都还是空的 —— 控件实测只有
         * 28 像素宽 (格式 "<value>    <value>" 就那么宽)。得按刚填进去的字重新量
         * 一次, 否则 "baud    115200" 这一行一百多像素, 塞不进 28 像素里就画不
         * 出来。TextArea::getTextWidth() 是带着通配缓冲区一起算的, 所以这一下量
         * 到的就是真实宽度。 */
        row[r]->resizeToCurrentText();
    }

    /* 整块面板重画: 白底 box2 先把上一次的字盖掉, 可见的那几行再画上去。
     *
     * 这样就不必去对付三件容易错的事: resizeToCurrentText() 前后控件宽度不一样
     * (只画新的一小块会留下旧字的尾巴)、setColor() 不标脏、setVisible() 也不标
     * 脏。按键才触发一次, 这点代价换少三处坑, 值。 */
    box2.invalidate();
}
