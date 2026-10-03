#include <gui/logic_data_screen/Logic_DataView.hpp>

namespace
{
/* 十六进制字符表 —— 只能小写。
 *
 * 字体不是整套 ASCII, 是拿 texts.xml 里那些实文本现生成的 (见
 * generated/fonts/src/Table_verdana_20_4bpp.cpp): 20 号字里一共就 54 个字符,
 * 大写只到 A B C D H I K L M N O P R S T U X。写个大写 F 出来在屏上就是一片
 * 空白 —— 不报错、不告警。小写 a..e 本来就有, f 和 I2C 用的 [] 靠 texts.xml
 * 里那条 hexDigits 撑着 (那条实文本唯一的作用就是喂字模)。 */
const touchgfx::Unicode::UnicodeChar HEX_DIGIT[16] =
{
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

/* 一行写几个字符。
 *
 * 这个数必须自己定: TextArea 默认的 wideTextAction 是 WIDE_TEXT_NONE, 一行
 * 超出控件宽度时它从字符中间直接切掉, 既不折行也不给省略号。
 *
 * 50 是按 800 像素宽和 verdana 20 (一个字约 12~13 像素) 估的, 约 650 像素,
 * 留了余量。对 UART 正好是"每行 16 个字节" —— 2 + 16×3 = 50。 */
const uint32_t LINE_CHARS = 50u;

/* I2C 续行的缩进 —— 宽度正好是 "[50 w] " 那 7 个字符, 续行的字节对齐到第一个
 * 数据字节下面。 */
const uint32_t I2C_INDENT = 7u;

/* 往通配缓冲里写字的小工具。
 *
 * 两种协议的排法不一样 (UART 按行宽断, I2C 一个消息一行), 但"够不够地方""这行
 * 还摆不摆得下""在不在行首"这三件事是一样的, 所以收在这里。
 *
 * **写超了不会写坏**: put() 自己看着容量, 装不下就把 full 支起来, 后面写什么
 * 都丢掉。所以调用方只管往下写, 最后看一次 full 决定要不要补省略号 —— 比每个
 * 调用点各判一遍边界可靠得多, 也不会出现"数据写进去了、省略号反而写不下"。 */
struct TextBuf
{
    touchgfx::Unicode::UnicodeChar* buf;
    uint32_t cap;       /* 可用字符数 (不含结尾那个 0) */
    uint32_t n;         /* 已经写进去几个 */
    uint32_t line;      /* 当前这一行写了几个字符 */
    bool     fresh;     /* 在行首 (刚换过行, 或者还什么都没写) */
    bool     full;      /* 装不下了 */

    /* 每次写之前留这么多位置: 收尾那段 " ..." 用 4 个, 结尾的 0 用 1 个, 再留
     * 一个字节 (2 个字符) 的余量。 */
    static const uint32_t RESERVE = 7u;

    void put(touchgfx::Unicode::UnicodeChar c)
    {
        if (full)
        {
            return;
        }
        if (n + RESERVE > cap)
        {
            full = true;
            return;
        }

        buf[n] = c;
        n++;
        line++;
        fresh = false;
    }

    /* 换行, 再缩进 indent 个空格。缩进是给"消息太长、续行"用的。 */
    void newline(uint32_t indent)
    {
        put('\n');
        line = 0u;

        for (uint32_t i = 0u; i < indent; i++)
        {
            put(' ');
        }

        /* 缩进之后仍然算"行首" —— 调用方靠它决定要不要在前面补分隔空格。
         * put() 会把 fresh 清掉, 所以得放在最后。 */
        fresh = true;
    }

    /* 收尾: 被截断了就补个 " ...", 再写上结尾的 0。
     *
     * 直接写 buf 不走 put —— RESERVE 就是留给这一下的, 这里一定是安全的。 */
    void finish(bool truncated)
    {
        if (truncated && n != 0u)
        {
            buf[n] = ' '; n++;
            buf[n] = '.'; n++;
            buf[n] = '.'; n++;
            buf[n] = '.'; n++;
        }
        buf[n] = 0u;
    }
};
}

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

    /* 把**进来之前就解好**的结果先摆上。
     *
     * 解码是在 Model::tick 里做的, 而且同一轮采集只解一次 (记号对得上就不再解,
     * 也不会再回调) —— 用户第二次进这一屏时 tick 不会再发 logicDataUpdated,
     * 不主动取一次的话屏幕上会一直是空的。 */
    uint32_t cnt = 0u;
    showLogicData(presenter->getLogicItems(&cnt), cnt);
}

void Logic_DataView::tearDownScreen()
{
    Logic_DataViewBase::tearDownScreen();
}

void Logic_DataView::showLogicData(const log_ana_item_t* items, uint32_t count)
{
    /* 竖着能摆几行。
     *
     * TextArea 对放不下的行是**静默裁掉**的 —— 不滚动也不提示。所以这里主动按
     * 能显示的行数截断, 然后在末尾写个省略号: 宁可少显示也别让用户以为屏幕上
     * 这些就是全部。
     *
     * 行高取字体自己那份 (Designer 里给这个 font 配的) 加上控件设的 linespacing,
     * 不写死 —— 白框在 Designer 里改大改小、字号换了, 这里跟着走。
     *
     * 以后要做"滚动查看"时, 这个 maxRows 的截断就是第一个要拆掉的东西。 */
    const uint16_t lineH = (uint16_t)(logic_dataarea.getTypedText().getFont()->getHeight()
                                      + logic_dataarea.getLinespacing());
    const uint32_t maxRows = (lineH != 0u)
                                 ? ((uint32_t)logic_dataarea.getHeight() / (uint32_t)lineH)
                                 : 0u;
    TextBuf out = { logic_dataareaBuffer, (uint32_t)LOGIC_DATAAREA_SIZE - 1u,
                    0u, 0u, true, false };

    uint32_t rows = 1u;     /* 已经开了几行 —— 第一行从开头就算数 */
    uint32_t k    = 0u;

    /* SPI 的每一组是 "MOSI/MISO", 光看数字分不出哪边是哪边 —— 先摆一行图例。
     *
     * 代价是占掉一行 (16 行里少一行数据)。和 I2C 的 [50 w]、CAN 的 ID: 不一样,
     * 那两个格式自己就把含义写全了, 只有 SPI 是两个平级的数字并排。 */
    if (count != 0u && items[0].proto == LOG_ANA_SPI)
    {
        out.put('m');
        out.put('o');
        out.put('/');
        out.put('m');
        out.put('i');

        if (rows < maxRows)
        {
            out.newline(0u);
            rows++;
        }
    }

    for (; k < count; k++)
    {
        if (out.full || rows > maxRows)
        {
            break;
        }

        const log_ana_item_t& it = items[k];

        /* 三种有"一段"这个概念的协议, 各有一个"我是段首"的标记:
         *   I2C  地址字节 (它是起始位之后的第一条)
         *   SPI  CS 选中之后的第一拍
         *   CAN  每一帧本身就是一段, 所以恒为真
         * UART 没有, 一条就是一条。段首一定另起一行。 */
        const bool new_msg = ((it.proto == LOG_ANA_I2C &&
                               (it.flags & LOG_ANA_IT_I2C_ADDR) != 0u) ||
                              (it.proto == LOG_ANA_SPI &&
                               (it.flags & LOG_ANA_IT_SPI_CS) != 0u) ||
                              (it.proto == LOG_ANA_CAN));

        /* 没被应答的字节后面跟个 '?'。
         *
         * I2C 里"器件没回 ACK"是最常要看的诊断 (写错地址、器件没焊上、总线被拉
         * 住), 而它本身在数据里看不出来 —— 第 9 位不是数据位。解码那边已经把它
         * 记成 LOG_ANA_IT_I2C_NACK 了, 这里不显示就等于白解。
         *
         * '?' 字体里有 (它还是 FallbackCharacter, 一定在), 读起来也顺: [50 w]?
         * 就是"叫了 0x50 写, 没人应"。
         *
         * 判 proto 不能省: 标记位是**按协议各自解释**的, 同一个位在别的协议里
         * 是另一个意思 (0x40 对 UART 没定义, 对 CAN 也不是 NACK)。 */
        const uint32_t nack_w = ((it.proto == LOG_ANA_I2C &&
                                  (it.flags & LOG_ANA_IT_I2C_NACK) != 0u)) ? 1u : 0u;

        /* ---- 这一条要写几个字符, 以及它被挤到下一行时缩进多少 ----
         *
         * 缩进是让续行的数据和上一行的第一组对齐 —— 有意义的只有 I2C (对齐到数据
         * 那一列) 和 CAN (对齐到冒号后面), SPI 是一串等宽的小组, 不缩更好认。
         *
         * width 含前面那个分隔空格 (行首没有), 所以行首那一位要单独算。 */
        uint32_t width;
        uint32_t indent;

        if (it.proto == LOG_ANA_CAN)
        {
            /* "<ID>:" 再加 " xx" × 数据字节数。标准帧 ID 补到 3 位、扩展帧补到
             * 8 位 —— 补齐而不是掐掉前导零, 是为了让同一路总线上每一帧的宽度都
             * 一样, 续行缩进才对得齐。 */
            const uint32_t idw   = ((it.flags & LOG_ANA_IT_CAN_EXT) != 0u) ? 8u : 3u;
            const uint32_t ndata = (it.aux <= 8u) ? it.aux : 8u;

            width  = idw + 1u + ndata * 3u;
            indent = idw + 2u;
        }
        else if (it.proto == LOG_ANA_SPI)
        {
            width  = 5u;        /* "mm/ss" —— MOSI 在前, MISO 在后 */
            indent = 0u;
        }
        else if (new_msg)       /* I2C 的地址字节 */
        {
            width  = 6u + nack_w;       /* "[50 w]" */
            indent = 0u;
        }
        else                    /* UART 的帧 / I2C 的数据字节: 一个字节 */
        {
            width  = 2u + nack_w;
            indent = I2C_INDENT;
        }

        /* 行首不用分隔空格, 那一格就不算进宽度里。*/
        if (!out.fresh)
        {
            width++;
        }

        /* ---- 换不换行 ----
         * 段首一定另起一行; 其余按行宽断。行首不用换。 */
        if (new_msg ? !out.fresh : (out.line + width > LINE_CHARS))
        {
            if (rows >= maxRows)
            {
                break;              /* 屏幕上摆不下了, 后面的都别写 */
            }
            out.newline(new_msg ? 0u : indent);
            rows++;
        }

        /* ---- CAN: 一帧整条写在一行里 ----
         *
         * 它不靠上面那套"一条一个小组"的换行: 一帧的 ID 和数据是一件事, 中间断开
         * 反而看不懂。最长 8+1+8×3 = 33 个字符, 一个 LINE_CHARS (50) 装得下。 */
        if (it.proto == LOG_ANA_CAN)
        {
            const uint32_t id = ((uint32_t)it.data[0] << 24)
                              | ((uint32_t)it.data[1] << 16)
                              | ((uint32_t)it.data[2] << 8)
                              |  (uint32_t)it.data[3];
            const uint32_t idw   = ((it.flags & LOG_ANA_IT_CAN_EXT) != 0u) ? 8u : 3u;
            const uint32_t ndata = (it.aux <= 8u) ? it.aux : 8u;

            for (uint32_t d = idw; d > 0u; d--)
            {
                out.put(HEX_DIGIT[(id >> ((d - 1u) * 4u)) & 0x0Fu]);
            }
            out.put(':');

            for (uint32_t b = 0u; b < ndata; b++)
            {
                const uint8_t v = it.data[4u + b];
                out.put(' ');
                out.put(HEX_DIGIT[(v >> 4) & 0x0Fu]);
                out.put(HEX_DIGIT[v & 0x0Fu]);
            }

            if ((it.flags & LOG_ANA_IT_CAN_NOACK) != 0u)
            {
                out.put('?');       /* 总线上没人应答 —— 只有自己在喊 */
            }
            continue;
        }

        if (!out.fresh)
        {
            out.put(' ');
        }

        if (it.proto == LOG_ANA_I2C && new_msg)
        {
            /* 地址那一条显示成 [50 w] —— 地址 + 方向, 而不是它在线上的原样
             * (0xA0)。两者等价, 前者才是人想看的: 哪个器件、读还是写。
             *
             * 地址字节本身的值就不再单独写两个字符了, 这 6 个字符已经把它说清楚;
             * 它也不占"数据字节"的量, 只是占了这行的宽度。 */
            const uint8_t addr = it.aux;

            out.put('[');
            out.put(HEX_DIGIT[(addr >> 4) & 0x0Fu]);
            out.put(HEX_DIGIT[addr & 0x0Fu]);
            out.put(' ');
            out.put(((it.flags & LOG_ANA_IT_I2C_RD) != 0u) ? 'r' : 'w');
            out.put(']');
            if (nack_w != 0u)
            {
                out.put('?');       /* 地址没人应 —— 器件根本不在总线上 */
            }
            continue;
        }

        if (it.proto == LOG_ANA_SPI)
        {
            /* mm/ss —— MOSI 在前 (主机发出去的), MISO 在后 (从机回来的)。
             *
             * 数据段是采样沿那一刻两路上各自的值, 解码那边就摆在 data[0]/data[1]。
             * MISO 没接的话这一路会在线上飘, 画出来是乱的 —— 那本身也是有用的
             * 信息 (那一栏根本没接), 所以照实画。 */
            const uint8_t mo = (it.len > 0u) ? it.data[0] : 0u;
            const uint8_t mi = (it.len > 1u) ? it.data[1] : 0u;

            out.put(HEX_DIGIT[(mo >> 4) & 0x0Fu]);
            out.put(HEX_DIGIT[mo & 0x0Fu]);
            out.put('/');
            out.put(HEX_DIGIT[(mi >> 4) & 0x0Fu]);
            out.put(HEX_DIGIT[mi & 0x0Fu]);
            continue;
        }

        /* 剩下来的就是 UART 的帧和 I2C 的数据字节 —— 都是一个字节落在 data[0]。 */
        const uint8_t b = (it.len != 0u) ? it.data[0] : 0u;

        out.put(HEX_DIGIT[(b >> 4) & 0x0Fu]);
        out.put(HEX_DIGIT[b & 0x0Fu]);

        if (nack_w != 0u)
        {
            out.put('?');
        }
    }

    /* k < count 就是"没摆完" —— 要么行数到顶, 要么缓冲满了, 要么两者都有。
     * count == 0 时 k 和 count 都是 0, 走不到补省略号那一步, 就是个空串。 */
    out.finish(k < count);

    /* ---- 这一句不能省: 重新算 boundingArea ----
     *
     * TextArea 把"这段文字要占多大一块"缓存在 boundingArea 里, 而它判断缓存还
     * 新不新鲜的依据是 **format 串的指针**, 不是 wildcard 缓冲。我们改的是缓冲,
     * 那个指针一动没动 —— 绘制区就还钉在上一次算出来的那一块上, 多出来的行被
     * 无声裁掉。
     *
     * setWideTextAction 是那几个会重算 boundingArea 的接口 (setWidth/setHeight/
     * setTypedText/setLinespacing/setWideTextAction) 里唯一不需要文本 id 的一个:
     * 别的要么会改尺寸, 要么得写 T___SINGLEUSE_317X 这种一重新生成就会变的名字。
     * 读出来再写回去, 值没变, 只为了触发那一次重算。 */
    logic_dataarea.setWideTextAction(logic_dataarea.getWideTextAction());

    logic_dataarea.invalidate();
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
