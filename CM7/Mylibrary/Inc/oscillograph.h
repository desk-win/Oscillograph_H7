#ifndef __OSCILLOGRAPH_H
#define __OSCILLOGRAPH_H

#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_mdma.h"
#include "sharedconfig.h"

/* 这个头会被 C 和 C++ 两边 include (TouchGFX 的 Model 是 C++), 不加这个
 * 守卫链接期会因为名字修饰对不上而报 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

/* ── M7_OSC_Poll() 必须跑多快 —— 这是这套东西最容易被搞错的一点 ──
 *
 * CM4 每 655 µs 发布一个半帧 (2048 word × 320 ns)。M7_OSC_Poll() 每次只发一
 * 发 MDMA, 而且设计上就是"跳到最新那一帧, 中间漏掉的记 missed" —— 所以调用
 * 周期必须明显小于 655 µs, 否则:
 *
 *     60 Hz (16.7 ms) 调用 →  每次都跳 25 个半帧, 只搬到 1/25,
 *                             环里 96% 是空洞
 *
 * 而屏幕一屏是 4 个连续半帧 (2.62 ms), 空洞化之后整屏基本全是旧数据, 波形
 * 是断的。所以 M7_OSC_Poll() 千万不能挂在 TouchGFX 的 Model::tick() 里。
 *
 * 正确做法是让 CM4 的发布事件来驱动 —— CM4 每次发布完都会
 * HAL_HSEM_FastTake(OSC_HSEM_ID) / HAL_HSEM_Release()。这条链路已经在
 * oscillograph.c 里接好了 (M7_OSC_Init 负责武装, HAL_HSEM_FreeCallback 负责
 * 搬运), 节奏天然跟着 CM4 走, 没有轮询延迟。改的时候留意这几条:
 *
 *   1) 信号量号用 OSC_HSEM_ID = 1。0 号被双核 boot 同步占着 (CM7 在 main.c
 *      里 Release 0 号来唤醒 CM4, CM4 开 C2IER 等这个释放), 不要混用。
 *
 *   2) CM7 的中断线是 HSEM1_IRQn (Interrupt 0), CM4 的是 HSEM2_IRQn
 *      (Interrupt 1) —— 两个核各有一份使能寄存器: CM7 写 C1IER (偏移
 *      0x100), CM4 写 C2IER (0x110), 互不干扰。所以启动文件里的向量名字是
 *      HSEM1_IRQHandler, 没有 HSEM_IRQHandler 这个名字。
 *      好在 HAL 的 HSEM_COMMON 宏已经按核选好了地址 (stm32h747xx.h:2840:
 *      CM7 → HSEM_BASE+0x100, CM4 → HSEM_BASE+0x110), 所以
 *      HAL_HSEM_ActivateNotification / HAL_HSEM_IRQHandler 可以直接用。
 *
 *   3) HAL_HSEM_IRQHandler() 会顺手把触发的那一位从使能寄存器里清掉
 *      (stm32h7xx_hal_hsem.c:386), 所以回调里必须重新武装, 否则只会进一次
 *      中断。而且要在搬运之前武装 —— 反过来的话, 搬运期间来的发布会被漏掉。
 *
 *   4) 优先级设成 5: 比 MDMA_IRQn (0) 低, 让 MDMA 完成回调能插进来及时清忙
 *      标志; 又正好等于 FreeRTOS 的 configLIBRARY_MAX_SYSCALL_INTERRUPT_
 *      PRIORITY, 以后要在里面调 FromISR 接口也不用再改。
 *
 * ── 另一条约束: 搬运本身要跑得赢 ──
 * 8192 B 的 MDMA 拷贝实测约 160 µs, 655 µs 里做得完 (占用约 25%)。但 MDMA 是
 * "上一发没搬完就直接丢弃本次"的, 所以要是 SDRAM 被 LTDC 抢得太狠导致单发超
 * 过 655 µs, 表现是环上开始成片出现空洞 —— 看 m7_diag_ch1.overrun / .missed。 */

/* ── SDRAM 映射 (FMC Bank2 @0xD0000000, 32 MB) —— 仅 CM7 使用 ──
 *   0xD0000000  帧缓冲 0     768000 B   ltdc.c:71
 *   0xD00BB800  帧缓冲 1     768000 B   TouchGFXGeneratedHAL.cpp:39
 *   0xD0177000  空闲          561 KB
 *   0xD0200000  CH1 深存储环   16 MB  = 0x400000 个 uint32 槽
 *   0xD1200000  CH2 深存储环    8 MB  = 0x400000 个 uint16 槽
 *   0xD1A00000  空闲            6 MB
 *
 * 两环槽数相同 → 半帧数相同 → 绕回永远同步。
 * 基址 2 MB 对齐 + 槽数 2 的幂 → 环形取模就是按位与。
 */
#define OSC_SDRAM_CH1_BASE   0xD0200000u
#define OSC_SDRAM_CH2_BASE   0xD1200000u
#define OSC_SDRAM_CH1_BYTES  (16u * 1024u * 1024u)          /* 0x01000000 */
#define OSC_SDRAM_CH2_BYTES  ( 8u * 1024u * 1024u)          /* 0x00800000 */
#define OSC_SDRAM_HALVES     (0x400000u / OSC_HALF_WORDS)   /* 2048 */
#define OSC_SDRAM_HALF_MASK  (OSC_SDRAM_HALVES - 1u)
#define OSC_CH1_HALF_BYTES   (OSC_HALF_WORDS * 4u)          /* 8192 */
#define OSC_CH2_HALF_BYTES   (OSC_HALF_WORDS * 2u)          /* 4096 */

/* 编译期把上面这张账对平。这些错要是留到运行时才暴露, 表现是"波形莫名其妙
 * 拼错一格", 非常难查 —— 所以宁可编译不过。 */
OSC_STATIC_ASSERT(OSC_SDRAM_CH1_BASE >= 0xD0177000u, "CH1 环压到 LTDC 显存了");
OSC_STATIC_ASSERT(OSC_SDRAM_CH2_BASE >= 0xD0177000u, "CH2 环压到 LTDC 显存了");
OSC_STATIC_ASSERT((OSC_SDRAM_CH1_BYTES / OSC_CH1_HALF_BYTES) == OSC_SDRAM_HALVES,
                  "CH1 环容量除不下整数个半帧");
OSC_STATIC_ASSERT((OSC_SDRAM_CH2_BYTES / OSC_CH2_HALF_BYTES) == OSC_SDRAM_HALVES,
                  "CH2 环容量除不下整数个半帧");
OSC_STATIC_ASSERT((OSC_SDRAM_HALVES & OSC_SDRAM_HALF_MASK) == 0u,
                  "槽数必须是 2 的幂, 否则 & 掩码取模不成立");
OSC_STATIC_ASSERT(OSC_SDRAM_CH1_BASE + OSC_SDRAM_CH1_BYTES <= OSC_SDRAM_CH2_BASE,
                  "CH1 环和 CH2 环重叠了");
OSC_STATIC_ASSERT(OSC_SDRAM_CH2_BASE + OSC_SDRAM_CH2_BYTES <= 0xD2000000u,
                  "CH2 环超出 SDRAM 容量");

/* ---- 诊断计数 ---- */
typedef struct {
    volatile uint32_t pushed;     /* 成功启动的搬运次数 */
    volatile uint32_t overrun;    /* 上一发还没搬完, 本次没发出去 */
    volatile uint32_t missed;     /* 被漏掉的半帧数 (CM4 发布了但我们没搬) */
    volatile uint32_t err;        /* 启动失败 + MDMA 错误回调的次数 */
    volatile uint32_t err_code;   /* 最后一次的 hmdma->ErrorCode */
} m7_mdma_diag_t;

extern m7_mdma_diag_t m7_diag_ch1;
extern m7_mdma_diag_t m7_diag_ch2;

/* CM4 的 HSEM 通知进来了多少次。bring-up 时最有用的一条: 按下 Start 之后
 * 这个数不涨, 说明 CM4 那边根本没在发布 (或者那一路的 cfg_run_chN 没置上),
 * 不用去怀疑 MDMA。 */
extern volatile uint32_t m7_diag_irq;

/* ---- 对外接口 ---- */

/* 一次性的初始化: 注册 MDMA 完成/错误回调 + 开 HSEM1_IRQn + 武装通知。
 * 必须在 MX_MDMA_Init() 之后、osKernelStart() 之前调用一次。
 * 不注册回调的话, MDMA 出错时 HAL 会完全静默地把通道关掉。 */
void M7_OSC_Init(void);

/* 把"搬到哪儿了"的游标对齐到 CM4 当前的发布序号。某一路重新开始采集之前调一次。
 * ch: 0 = CH1, 1 = CH2 (和 OSC_TRACE_CH1/CH2 同值)。
 *
 * 停着的那一路 seq 是冻住的 (CM4 只在 cfg_run_chN 置着时才发布半帧), 所以正常
 * 情况下这里等于没做。它兜的是另一种情况: CM7 漏掉了 HSEM 通知, 游标落后了 ——
 * 那样重启后第一次搬运算出的 step 是一段积压, m7_diag_chN.missed 会被灌进一个
 * 巨大的值, 那个计数器之后就没法用来判断"到底有没有真的漏帧"了。
 *
 * 对齐成"当前这一格算搬过了": 于是下一次发布 (seq+1) 算出来 step = 1, 记 0 个
 * 漏帧, 而且那个新半帧照样会被搬走 (不会把重启后的第一帧吃掉)。
 *
 * 只有一路被对齐 —— 另一路正跑着的话, 一起对齐会让它丢一个半帧。
 *
 * 会短暂关中断 —— s_chN_last_seq 平时只有 HSEM 中断在写。只有几条指令, 无所谓。 */
void M7_OSC_SyncCh(uint32_t ch);

/* 每通道一个: 发现 CM4 发布了新半帧就启动一次搬运。
 *   没有新数据   → 什么都不做
 *   上一发还在飞 → 丢弃本次并记 overrun
 * 正常路径下由 HSEM 中断调用 (见文件头), 不需要自己轮询。 */
void M7_OSC_CH1Start(void);
void M7_OSC_CH2Start(void);

/* 便利入口: 把两个通道各推一遍 */
void M7_OSC_Poll(void);

/* 换某一路的模拟前端放大倍数。gain_code: 0..7 → X1, X2, ..., X128。
 *
 * 只是往共享控制块里写一个字, 引脚是 CM4 动的 (那边的 MUX 地址线 CM7 根本
 * 没配)。CM4 在下一次主循环迭代 (≤655 µs) 里看到它并摆引脚。
 * ch: 0 = CH1, 1 = CH2 (和 OSC_TRACE_CH1/CH2 同值)。
 *
 * 越界值会被 CM4 按 X1 处理, 这里不再夹一次 —— 夹两道的话, 出现"夹的规则不
 * 一致"时反而更难查。调用方保证 0..7。 */
void M7_OSC_SetFrontend(uint32_t ch, uint32_t gain_code);

#ifdef __cplusplus
}
#endif

#endif
