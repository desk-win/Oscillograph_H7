#ifndef __SHAREDCONFIG_H
#define __SHAREDCONFIG_H

#include <stdint.h>

/* ===========================================================================
 * 跨核共享 ABI —— CM4 与 CM7 必须编译同一份
 *
 * 地址契约靠 CM4 的链接脚本保证: 三个符号依次落在
 *     osc_ch1_buf @ 0x30030000   (uint32, 16 KB)
 *     osc_ch2_buf @ 0x30034000   (uint16,  8 KB)
 *     osc_ctrl    @ 0x30036000   (96 B)
 * CM4 在 OSC_CH_Init() 里自检, 对不上就 Error_Handler。
 *
 * 链接脚本里 RAM_D2 = 0x30030000 + 96 KB, 现在只用掉 24 KB,
 * 将来把环形缓冲加深到 4 个半帧(48 KB)也放得下。
 *
 * 通道定义: CH1 = ADC1+ADC2 交替采同一路 (1 word = 2 个样本, ADC1 在低 16 位且时间在前)
 *           CH2 = ADC3 (1 word = 1 个样本)
 *
 * 写者约定: 一个字段只有一个核在写。诊断字段按通道拆成独立的字,
 * 不要写成"两路共用一个字、各改各的位" —— 目前两个 ADC 走 DMA1_Stream0/1
 * 且同为抢占优先级 0 (CM4/Core/Src/dma.c), 不会互相打断所以读改写侥幸安全,
 * 但这条约束一旦被改动就会变成静默丢位。
 *
 * ── 动这个结构体的时候 ──
 * 两个核各编译一份, 靠偏移量对上, 而 magic 目前没人检查 (只打印出来看一眼)。
 * 所以改了字段就得两个核一起重新烧 —— 只烧一个不会报任何错, 表现是各种数值
 * 莫名其妙地乱跳, 很难往"ABI 对不上"上想。
 *
 * 当前这一版把原来的 cfg_run 拆成了 cfg_run_ch1 / cfg_run_ch2 (两个通道分别
 * 启停), 用的是末尾那格 reserved —— 总大小还是 96 字节, 但 diag_* 的偏移
 * 全都前移了一个字。
 * ========================================================================= */

#define OSC_SHARED_MAGIC      0x4F534332u     /* 'OSC2': 拆出每通道电平 + 自适应电平诊断 */

#define OSC_LEVEL_AUTO        0xFFFFFFFFu     /* cfg_trig_level_* 填它 = CM4 自适应定电平 */

#define OSC_CH1_BUF_ADDR      0x30030000u
#define OSC_CH2_BUF_ADDR      0x30034000u
#define OSC_CTRL_ADDR         0x30036000u

#define OSC_CH1_BUF_WORDS     4096u           /* uint32 个数 = 16 KB */
#define OSC_CH2_BUF_WORDS     4096u           /* uint16 个数 =  8 KB */
#define OSC_HALF_WORDS        2048u           /* 半帧 = 环形缓冲的一半 */

#define OSC_WORD_PERIOD_NS    320u            /* 1 word = 16 个 ADC 周期 @50 MHz */
#define OSC_WORDS_PER_SEC     3125000u        /* 1e9 / 320 */

#define OSC_HSEM_ID           1u              /* ID_0 被双核 boot 同步占用 */
/* 信号量号 → 使能寄存器里的位。等价于 HAL 自己的 __HAL_HSEM_SEMID_TO_MASK,
 * 写在这里是为了让 CM4 那边也能用 (CM4 不 include HAL 的 hsem 头)。 */
#define OSC_HSEM_MASK         (1u << OSC_HSEM_ID)
#define OSC_TRIG_NONE         0xFFFFFFFFu

typedef struct {
    /* ---- CM4 写, CM7 读 ---- */
    uint32_t magic;             /* 必须 == OSC_SHARED_MAGIC */

    /* seq_chN —— 相位对齐全靠这一条, 改之前先读 CM4 osc_scan_half 的头注释。
     *
     * 它跟着 DMA 完成计数走: 每个半帧 +1, 主循环漏掉的半帧也算进去, 所以不是
     * "发布了几次"。这样 (seq-1)&1 才等于刚扫过、可以安全搬走的那一格 D2
     * 缓冲 —— 也就是 CM7 该搬的那一格; (seq-1)*OSC_HALF_WORDS + pos 才是这个
     * 触发点在环里的绝对位置。
     *
     * 代价是两边必须一直同奇偶: CM4 在通道刚被打开时对齐一次, 之后两边都按
     * lag 递增, 关系自然保持。 */
    uint32_t seq_ch1;
    uint32_t seq_ch2;
    uint32_t trig_pos_ch1;      /* 半帧内触发点下标; OSC_TRIG_NONE = 本帧无 */
    uint32_t trig_pos_ch2;      /* CH1 是 word 下标, CH2 是样本下标 —— 量纲不同! */
    uint32_t trig_flags;        /* bit0: CH1 本半帧检测到触发; bit1: CH2 */

    /* ---- CM7 写, CM4 读 ---- */
    uint32_t cfg_trig_level_ch1;/* 0..4095, 或 OSC_LEVEL_AUTO = 自适应 */
    uint32_t cfg_trig_level_ch2;
    uint32_t cfg_trig_hyst;     /* 迟滞半宽, LSB; 只压在"武装"侧, 不压触发阈值 */
    uint32_t cfg_trig_edge;     /* 0 = 上升沿, 1 = 下降沿 */
    uint32_t cfg_trig_enable;
    uint32_t cfg_run_ch1;       /* 0 = 停止 CH1 采集 */
    uint32_t cfg_run_ch2;       /* 0 = 停止 CH2 采集 */

    /* ---- 诊断: 每通道一份, 各自只有本通道的中断/主循环在写 ---- */
    uint32_t diag_ovr_ch1;
    uint32_t diag_ovr_ch2;
    uint32_t diag_miss;         /* CM4 主循环来不及扫的半帧数 (跟不上就记这里) */
    uint32_t diag_rate_ch1;     /* Sa/s, CM4 用 DWT 实测 */
    uint32_t diag_rate_ch2;

    uint32_t diag_min_ch1;      /* 上一个统计窗口的最小值 */
    uint32_t diag_max_ch1;      /* 上一个统计窗口的最大值 (CM7 可拿它做垂直量程) */
    uint32_t diag_level_ch1;    /* 实际生效电平; 自适应模式下是平滑后的值 */
    uint32_t diag_min_ch2;
    uint32_t diag_max_ch2;
    uint32_t diag_level_ch2;
} osc_ctrl_t;                   /* 24 word = 96 B */

#define OSC_CTRL ((volatile osc_ctrl_t *)OSC_CTRL_ADDR)

/* 这个头文件将来可能被 C++ 的 Model.cpp 间接包含 */
#ifdef __cplusplus
#define OSC_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define OSC_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

OSC_STATIC_ASSERT(sizeof(osc_ctrl_t) == 96, "osc_ctrl_t 必须正好 96 字节");


#endif
