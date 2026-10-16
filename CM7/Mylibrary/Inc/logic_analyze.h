#ifndef __LOGIC_ANALYZE_H
#define __LOGIC_ANALYZE_H

#include <stdint.h>
#include "sharedconfig.h"       /* log_rec_t */

/* 这个头会被 C 和 C++ 两边 include (TouchGFX 的 Model 是 C++), 不加这个
 * 守卫链接期会因为名字修饰对不上而报 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * CM7 侧逻辑分析仪 —— 协议解码层
 *
 * 上游是 logic_trace.c 的记录表 (跳变表), 下游是 Logic_Data 屏上那块文字区。
 * 这一层不依赖 TouchGFX, 也不碰任何硬件外设 —— 只做"跳变表 → 一串字节"这件事。
 *
 * ── 为什么能在跳变表上解码 (而不是要原始采样缓冲) ──
 * log_rec_t 存的是"变化之后的完整电平" (见 sharedconfig.h), 两个跳变之间电平
 * 恒定, 所以**任意时刻的电平都能精确还原** —— 对二值数字线来说跳变表是无损的。
 * 原始采样缓冲做不到这件事: 它只有 32 KB 双缓冲, 早被 DMA 绕圈盖掉了。
 *
 * ── 位宽从哪来 ──
 * 用 LOG_CTRL->fs_hz (CM4 用 DWT 实测发布的采样率), 不是标称的 20 MHz。fs_hz
 * 为 0 (CM4 还没发布过) 时才退到标称值。20 MSa/s 下 9600 baud 一位 = 2083 个
 * 采样点, 115200 baud 一位 = 174 个 —— 都远够解码。
 *
 * ── 什么时候能解 ──
 * 只有采集**停了**才能解: 采集期间记录表还在被半帧中断追加, 读出来是半截的。
 * 这一条在解码任务的入口挡着 (借不到记录表就不解), 调用方不用自己判。
 *
 * ── 配置从哪来 ──
 * LOG_Analyze_SetConfig() 是唯一的写入口, 只有 Logic_Setting 屏按下 Save 才走
 * 这条路。Cancel 不调它, 所以本次改动不生效 —— 这正是要的语义。
 * ========================================================================= */

/* 协议 —— 下标就是设置屏模式表的顺序, 必须和 Model.hpp 的 LOG_PROTO_* 对得上
 * (Model.cpp 里有一条 static_assert 看着它们)。 */
#define LOG_ANA_UART          0u
#define LOG_ANA_I2C           1u
#define LOG_ANA_SPI           2u
#define LOG_ANA_CAN           3u
#define LOG_ANA_PROTO_COUNT   4u

/* UART 校验。顺序就是设置屏候选值表的顺序 (none / odd / even)。 */
#define LOG_UART_PARITY_NONE  0u
#define LOG_UART_PARITY_ODD   1u
#define LOG_UART_PARITY_EVEN  2u

/* UART 一帧怎么切 —— 全部是**用户可配项**, 和设置屏 box2 那四行一一对应。 */
typedef struct {
    uint32_t baud;      /* 9600 / 38400 / 115200 */
    uint8_t  data_bits; /* 7 / 8 */
    uint8_t  stop_x10;  /* 停止位 ×10: 10 = 1 位, 15 = 1.5 位, 20 = 2 位 */
    uint8_t  parity;    /* LOG_UART_PARITY_* */
    uint8_t  rx_ch;     /* 逻辑通道 0..7 (UART 接线约定 CH1 收 RX, 所以是 0) */
} log_ana_uart_cfg_t;

/* SPI —— 完全同步, 时钟线 (SCK) 就在采集里, 所以和 I2C 一样没有波特率可配。
 *
 * 接线约定 (见 Logic_SettingView.cpp 的通道表):
 *   CH1 = CS, CH2 = SCK, CH3 = MISO, CH4 = MOSI
 *
 * 一个字按 8 个采样沿算。16/32 位字长的器件解出来会按字节两两颠倒 —— 这一版
 * 不做, 真碰上了再说。 */
typedef struct {
    uint8_t cpol;       /* 空闲时 SCK 的电平: 0 或 1 */
    uint8_t cpha;       /* 0 = 前沿采样, 1 = 后沿采样 */
    uint8_t cs_active;  /* CS **选中**时的电平: 0 = 低有效 (常规), 1 = 高有效 */
} log_ana_spi_cfg_t;

/* CAN —— 和 UART 一样是异步的 (线上没有时钟线, 位定时全靠约定), 所以要有波特率。
 *
 * 接线约定: CH1 = RX, 也就是收发器给 MCU 的那根 RXD。它的极性是**隐性 (总线空闲)
 * 为高、显性 (逻辑 0) 为低** —— 和 UART 的空闲电平同一个约定, 所以"起始位 =
 * 下降沿"这句话两边都成立, 别让差分那两个字把人绕进去。
 *
 * 波特率是**仲裁段的位速率**: 采样点跑在 125k/250k/500k 三档里。 */
typedef struct {
    uint32_t baud;      /* 125k / 250k / 500k */
} log_ana_can_cfg_t;

/* 一条解出来的帧。
 *
 * 四种协议共用这一种条目, 而不是各写各的: Model / View / 格式化那一整条通路
 * 只认这一个结构。加一种协议时只是往 union 里加一个结构、往 Run() 里加一个分支、
 * 往格式化里加一种排法。
 *
 * data[] 各协议的用法:
 *   UART  data[0]                    一条 = 一帧 = 一个字节
 *   I2C   data[0]                    一条 = 一个字节 (地址字节也是其中一条)
 *   SPI   data[0] = MOSI, data[1] = MISO
 *                                    一条 = 一个 8 位的字, 两个方向一起交出去
 *   CAN   data[0..3] = ID (大端), data[4..] = 数据
 *                                    一条 = 一帧; 4 + 8 正好把 12 用满 */
#define LOG_ANA_ITEM_MAX   12u

typedef struct {
    uint32_t ts;                      /* 该帧第一个跳变的采样序号 */
    uint8_t  proto;                   /* LOG_ANA_* */
    uint8_t  flags;                   /* LOG_ANA_IT_* */
    uint8_t  len;                     /* data[] 有效字节数 */
    uint8_t  aux;                     /* 协议相关: I2C = 这一字节归属的 7 位地址,
                                       * CAN = DLC, SPI/UART 不用 (0)。见各协议说明 */
    uint8_t  data[LOG_ANA_ITEM_MAX];
} log_ana_item_t;

#define LOG_ANA_IT_ERR  0x01u   /* 帧本身有问题 (UART: 校验位对不上) */
#define LOG_ANA_IT_GAP  0x02u   /* 帧的时间里跨过丢帧区间, 内容不可信 */

/* ---- I2C 专用 (proto == LOG_ANA_I2C) ----
 *
 * I2C 是**一个字节一条**: 起始位之后每 9 个 SCL 上升沿就是一个字节 + 它的应答位,
 * 一条刚好装进 data[0]。地址不单独占一条 —— 它就是这一段的第一个字节, 只是多
 * 打了个 ADDR 标记; 后面每个数据字节的 aux 里存着"我属于哪个地址", 所以就算
 * 显示时按地址分了行, 单独看一条也能知道它是谁的数据。
 *
 * 地址那一条显示出来是 [50 w] 这种 (地址 + 方向), 而不是它在线上的原样 (0xA0)。
 * 两者等价, 前者是给人看的。 */
#define LOG_ANA_IT_I2C_ADDR   0x10u   /* 这一条是地址字节 (一段里的第一个) */
#define LOG_ANA_IT_I2C_RD     0x20u   /* 地址字节的 R/W 位 = 1 (读) */
#define LOG_ANA_IT_I2C_NACK   0x40u   /* 这一字节没被应答 (第 9 位是 1) */

/* aux 的"还不知道属于哪个地址"。7 位地址只占 0..127, 所以 0xFF 是安全的哨兵。 */
#define LOG_ANA_I2C_NOADDR    0xFFu

/* ---- SPI 专用 (proto == LOG_ANA_SPI) ----
 *
 * SPI 没有帧界 —— CS 一拉低就是一段传输的开始, 拉高就是结束, 段内按 8 个采样沿
 * 一个字一直数下去。所以显示时按 CS 分段, 靠的就是这个标记: 它打在**一段传输的
 * 第一个字**上, 后面的字都没有。
 *
 * 和 I2C 的 LOG_ANA_IT_I2C_ADDR 是同一个位 —— 标记位是**按协议各自解释**的
 * (看 flags 之前先看 proto), 不共用一套语义, 所以位值可以重。 */
#define LOG_ANA_IT_SPI_CS     0x10u   /* 这一条是 CS 选中之后的第一拍 (一段的开头) */

/* ---- CAN 专用 (proto == LOG_ANA_CAN) ----
 *
 * DLC 单独有个字段放 (aux), 所以这里只有"怎么解释 ID"和"有没有人应答"两件事。
 *
 * ACK 槽是发方发隐性、**任何一个收对了的节点**把它拉成显性。所以:
 *   显性 (0) = 总线上有别人, 正常
 *   隐性 (1) = 只有自己在喊, 没有从机 —— 接线错了、对方没上电、或者波特率不对
 * 这和 I2C 的 NACK 是同一类信息, 也是排查 CAN 最常要看的一条。 */
#define LOG_ANA_IT_CAN_EXT    0x10u   /* 扩展帧: ID 是 29 位, 不是 11 位 */
#define LOG_ANA_IT_CAN_NOACK  0x20u   /* 没人应答 (ACK 槽是隐性) */

/* 解出来的帧数上限。到顶就截断, 剩下的不存 (log_ana_diag_trunc 记一笔)。
 *
 * 512 是按菜屏上那块文字区定的: 白框 (0,91,800,389) + verdana 20 大约放得下
 * 16 行、每行 22 字节 ≈ 350 字节, 512 已经盖过屏幕能显示的量 —— 再大只是白占
 * 内存, 用户也看不到。 */
/* 现在解码在独立任务里跑, 结果要双缓冲发布 (交换指针), 所以砍到 256:
 * 两份 256 × 20 B = 10 KB。 */
#define LOG_ANA_MAX_ITEMS  256u

/* 一次解码需要的全部配置。union 里按协议各一份 —— I2C 那份是空的 (它没得可配),
 * 所以没有成员, 只有 proto 那个数。 */
typedef struct {
    uint8_t proto;          /* LOG_ANA_* */
    uint8_t rsvd[3];        /* 补齐 + 以后要加的标记位 */
    union {
        log_ana_uart_cfg_t uart;
        log_ana_spi_cfg_t  spi;
        log_ana_can_cfg_t  can;
    } u;
} log_ana_cfg_t;

/**
 * 一次性初始化: 配置回到"默认 UART + 设置屏候选值里第一个"。
 *
 * 在 Model 的构造函数里调 (那时还没进 main, 但这一层不碰任何硬件, 安全)。
 */
void LOG_Analyze_Init(void);

/**
 * 收下一份新配置。**只有 Logic_Setting 屏按下 Save 才调**。
 *
 * 传 0 或者 proto 越界一律忽略 (配置保持原样), 不会读到联合体外面。
 */
void LOG_Analyze_SetConfig(const log_ana_cfg_t *cfg);

/** 取当前生效的配置。传 0 就直接返回。 */
void LOG_Analyze_GetConfig(log_ana_cfg_t *out);

/**
 * 现在这一刻, 解码的前提够了没有。
 *
 * 判据和解码任务里那句一模一样: 采集停了 + 记录表冻结 + 真有跳变
 * 可解。**0 和"解出 0 条"是两件事** —— 这一条问的是"能不能解", Run 的返回值
 * 说的是"解出来几条", 记录表就绪但一条合法帧都没有的时候是"能解, 0 条"。
 *
 * Model::tick 拿它决定"现在就解"还是"再等一拍": 用户点进 Logic_Data 屏的时候
 * 采集完全可能还在跑 (或者干脆还没采过), 那时候不能解 —— 记录表还在变, 解出来
 * 的是半截数据。该做的是把请求**挂在那儿**, 每个 tick 问一次, 等它稳定。
 */
uint8_t LOG_Analyze_Ready(void);

/**
 * 请求解码某一代记录 (GUI 侧, 非阻塞)。
 *
 * 请求连着**配置快照**一起放进请求槽; 计数上限 1 的信号量 + 最新覆盖 = 旧的
 * 请求不排队。同一代记录只解一次 (记号就是 LOG_Trace_GetResultSeq)。
 *
 * 解码期间记录表被"借用": 采集不会重开, Cancel 也挂到借用结束之后。
 */
void LOG_Analyze_Request(uint32_t result_gen);

/** 结果代数: GUI 在 tick 里比它, 变了才去 GetItems() 取 (和 GetCols 一个套路)。 */
uint32_t LOG_Analyze_GetPubSeq(void);

/** 读发布缓冲期间置起 —— 解码任务换指针前会看它 (任务侧只等不咬)。 */
void LOG_Analyze_PubLock(void);
void LOG_Analyze_PubUnlock(void);

/**
 * 解码任务入口: 由 CM7/Core/Src/freertos.c 里 osThreadNew 创建。
 * 优先级放 TouchGFX 之下 (无期限的活); 栈 1 KB 够用 —— 解码器里最大的局部量
 * 是 CAN 的 128 字节位数组 (CAN_BITS_MAX)。
 */
void LOG_Analyze_TaskEntry(void *argument);

/**
 * 取上一次解出来的结果 (只读, 不用释放)。*count 收条数, 传 0 就不写。
 *
 * 返回的是**发布缓冲**的指针: 调用方要用 LOG_Analyze_PubLock/Unlock 包住
 * "取指针 + 用完整段数据", 否则解码任务可能正好在换指针。
 */
const log_ana_item_t *LOG_Analyze_GetItems(uint32_t *count);

/** 丢掉解码结果, 回到"还没解过" (在途的那一次解码也会被丢弃)。
 *  Logic_Data 屏的 Cancel 走这条路。 */
void LOG_Analyze_Clear(void);

/* ---- 诊断 ---- */

/** 最近一次解码解出几条 */
extern volatile uint32_t log_ana_diag_items;
/** 最近一次 Run 花的 CPU 周期数 (DWT)。÷ 480 就是微秒 */
extern volatile uint32_t log_ana_diag_cyc;
/** 最近一次 Run 里内容不可信的条目数 (打了 LOG_ANA_IT_GAP 的)。
 *  含义随协议略不同: UART 是"帧头撞上丢帧区间、整帧放弃"的数量, I2C 是"这一
 *  字节所在的这一段的采样有缺口"的数量。非 0 都说明记录表在采集中途满过。 */
extern volatile uint32_t log_ana_diag_gap;
/** 最近一次 Run 因为条目到顶而没存下来的帧数 (非 0 = 结果被截断了) */
extern volatile uint32_t log_ana_diag_trunc;

/** 检测点: 发布被占用/被 Cancel 丢弃的次数 */
extern volatile uint32_t log_ana_diag_pub_drop;
/** 检测点: 记录表借不到 (采集还在跑) 而重试的次数 */
extern volatile uint32_t log_ana_diag_borrow_wait;

#ifdef __cplusplus
}
#endif

#endif
