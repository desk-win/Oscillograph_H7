#ifndef __SHAREDCONFIG_H
#define __SHAREDCONFIG_H

#include <stdint.h>

/* ===========================================================================
 * 跨核共享 ABI —— CM4 与 CM7 必须编译同一份
 *
 * 地址契约靠 CM4 的链接脚本保证: 三个符号依次落在
 *     osc_ch1_buf @ 0x30030000   (uint32, 16 KB)
 *     osc_ch2_buf @ 0x30034000   (uint16,  8 KB)
 *     osc_ctrl    @ 0x30036000   (128 B)
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
 * 当前这一版在**末尾**添了放大倍数 (cfg_gain_ch1/2), 结构体从 96 B 长到
 * 128 B。旧的字段偏移一个都没动 —— 漏烧一个核的表现就只是"增益不生效",
 * 而不是"诊断数值乱跳"。
 * ========================================================================= */

#define OSC_SHARED_MAGIC      0x4F534333u     /* 'OSC3': 末尾添了两个模拟前端字段 */

#define OSC_LEVEL_AUTO        0xFFFFFFFFu     /* cfg_trig_level_* 填它 = CM4 自适应定电平 */

#define OSC_CH1_BUF_ADDR      0x30030000u
#define OSC_CH2_BUF_ADDR      0x30034000u
#define OSC_CTRL_ADDR         0x30036000u

#define OSC_CH1_BUF_WORDS     4096u           /* uint32 个数 = 16 KB */
#define OSC_CH2_BUF_WORDS     4096u           /* uint16 个数 =  8 KB */
#define OSC_HALF_WORDS        2048u           /* 半帧 = 环形缓冲的一半 */

#define OSC_WORD_PERIOD_NS    320u            /* 1 word = 16 个 ADC 周期 @50 MHz */
#define OSC_WORDS_PER_SEC     3125000u        /* 1e9 / 320 */

#define OSC_HSEM_ID           1u              /* ID_0 双核 boot 同步, ID_2 逻辑分析仪 */
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

    /* ---- CM7 写, CM4 读: 模拟前端 (放大倍数) ----
     *
     * 加在**末尾**是刻意的: 偏移量是两核之间唯一的契约, 插在中间会把 diag_*
     * 那一整段往后推, 于是"漏烧一个核"的表现从"增益不生效"变成"诊断数值乱跳"——
     * 后者极难往 ABI 上想。加在末尾则旧字段偏移一个都不动。
     *
     * 值就是 S_4051 的档位号: 0..7 → X1, X2, X4, ..., X128 (倍数 = 1 << n)。
     * CM4 那边在主循环里和本地旧值比, 变了才动 MUX 引脚 —— 见 CM4 的
     * frontend_poll()。越界值会被 CM4 按 X1 处理。 */
    uint32_t cfg_gain_ch1;
    uint32_t cfg_gain_ch2;

    /* 补到 128 B。不是为了让结构体好看: 0x30036000 + 128 = 0x30036080 正好是
     * LOG_CTRL_ADDR, 再长一个字节就压到逻辑分析仪的控制块上了。 */
    uint32_t _rsvd_gain[6];
} osc_ctrl_t;                   /* 32 word = 128 B */

#define OSC_CTRL ((volatile osc_ctrl_t *)OSC_CTRL_ADDR)

/* ===========================================================================
 * 模拟前端的量程 —— 把 ADC 计数换算成探头上的电压时用的三个常数
 *
 * 信号进 ADC 之前走了这么一条路:
 *
 *     探头 ──[分压 OSC_FRONTEND_DIV]──> 运放 ──[× (1 << cfg_gain_chN)]──> ADC
 *
 * ADC 是 12 位、参考 OSC_ADC_VREF_MV, 0V 偏置在量程正中 (≈2048 个计数)。
 * 所以探头上的电压和计数的关系是:
 *
 *     V = (计数 - 2048) / OSC_ADC_FULL_SCALE × OSC_ADC_VREF_MV
 *         × OSC_FRONTEND_DIV / (1 << cfg_gain_chN)
 *
 * 峰峰值只用 (hi - lo), 偏置那一项自然消掉, 所以界面上不算它。
 *
 * 放在这儿而不是某个 .c 里, 是因为它就是"cfg_gain_chN 这个数在硬件上到底
 * 乘了多少倍"的另一半 —— 两者是一句话的两截, 分开写迟早会各改一半。
 * 这两个数只有 CM7 的界面在用 (CM4 只管摆引脚), 但它是硬件的事实, 所以
 * 和 cfg_gain_chN 放在一起。 */
#define OSC_ADC_VREF_MV     3300u    /* ADC 参考电压, mV */
#define OSC_ADC_FULL_SCALE  4096u    /* 12 位 */
#define OSC_FRONTEND_DIV    10u      /* 探头那一级的分压比 */

/* 这个头文件将来可能被 C++ 的 Model.cpp 间接包含 */
#ifdef __cplusplus
#define OSC_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define OSC_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

OSC_STATIC_ASSERT(sizeof(osc_ctrl_t) == 128, "osc_ctrl_t 必须正好 128 字节");


/* ===========================================================================
 * 逻辑分析仪跨核 ABI —— 设计见 docs/log_plan.md, 本节实现它的 §5.1 / §6
 *
 * 数据流:
 *   TIM2 ─触发→ DMA2_Stream0 ─写→ D2 采样缓冲 (32 KB, 两个 16 KB 半帧)
 *   CM7 逐个半帧读出去、找跳变点、把 8 B 记录写进 D1 AXI SRAM
 *   记录满 / 空闲超时 → CM7 把记录抽成 520 列画到 Logic 屏的 8 个框里
 *
 * 分工: M4 只碰 D2 (采集 + 记账), M7 碰 D2 (读) 和 D1 (写)。M4 不做跳变检测,
 * 也不碰 D1。
 *
 * 地址都是 #define 出来的绝对地址, 不走链接脚本 —— 理由见 log_plan.md §6.1:
 * 这两块内存都不在 CM4 的链接区里 (D1 那 512 KB 根本不归 CM4 的 .ld 管),
 * 链接脚本帮不上忙, #define 反而更强。
 * ========================================================================= */

/* ---- HSEM 通知通道 (2 号) ----
 *
 * CM4 每完成一个半帧就 take+release 一次这个信号量, CM7 那边就进一次中断去扫
 * 那一格。**为什么非得这样**: 半帧只有 409.6 µs, 而 TouchGFX 的 Model::tick()
 * 是 60 Hz (16.7 ms) —— 挂在 tick 里就是每 41 个半帧才扫 1 个, 97.5% 的时间轴
 * 根本没被看过 (docs/log_plan.md §8.1 原来就是这么写的, 见 §8.10 的说明)。
 *
 * 中断线是**每核一条, 不是每个信号量一条**: 1 号和 2 号都从 CM7 的 HSEM1_IRQn
 * (Interrupt 0) 进来, 使能位写在同一个 C1IER 里。所以这里只是多占一个信号量号,
 * NVIC 那边一个字都不用改 —— 但收方那个 HAL_HSEM_FreeCallback 必须能处理
 * "一次进来两个位" (HAL 传的是整个 C1MISR), 见 CM7 的 oscillograph.c。 */
#define LOG_HSEM_ID           2u
#define LOG_HSEM_MASK         (1u << LOG_HSEM_ID)

#define LOG_CTRL_ADDR         0x30036080u   /* D2, 区域 2 内, 已非缓存 */
#define LOG_SAMPLE_BUF_ADDR   0x30038000u   /* D2, 32 KB */
#define LOG_REC_BUF_ADDR      0x24000000u   /* D1 AXI SRAM, 512 KB, M7 独占 */

/* 采样缓冲的形状。DMA 是 16 位的 (PDATAALIGN/MDATAALIGN 都是 HALFWORD), 所以
 * 同一个缓冲有三个量纲, 别混:
 *     uint32 视角  M7 一读两个样本  → 半帧 4096 个 word,  整块 8192 个
 *     样本视角     时间戳以它为单位  → 半帧 8192 个样本,  整块 16384 个
 *     半字视角     DMA 的传输个数    → 半帧 8192 个半字,  整块 16384 个 (32 KB)
 */
#define LOG_SAMPLE_HALF_WORDS 4096u         /* 每半帧 4096 个 uint32 (= 两个样本/word) */
#define LOG_SAMPLE_HALF_SAMPS 8192u         /* 每半帧 8192 个样本 */
#define LOG_SAMPLE_HALF_BYTES 16384u        /* 每半帧 16 KB —— M7 算半帧地址用 */
#define LOG_SAMPLE_DMA_LEN    (2u * LOG_SAMPLE_HALF_SAMPS)  /* 整块 16384 个半字 = 32 KB */

#define LOG_PIN_MASK          0x3E8Cu       /* PG2/3/7/9/10/11/12/13, 见 log_plan.md §3.1 */

#define LOG_REC_MAX           65536u        /* 512 KB / 8 B */
#define LOG_CH_BIT            {2u, 3u, 7u, 9u, 10u, 11u, 12u, 13u}  /* 逻辑通道 1..8 */

/* TIM2 在 APB1 (PCLK1 = 120 MHz, 分频不为 1 → 定时器时钟翻倍 = 240 MHz)。
 * fs = 240 MHz / (ARR + 1) / (PSC + 1), PSC = 0。
 * ARR = 11 → 20 MSa/s。fs 是目标值不是保证值 (DMA 漏请求时实际更低),
 * 所以 CM4 会实测 fs_hz 发布出来, CM7 算时间轴用它 —— 见 log_plan.md §2.1 / §7.4。 */
#define LOG_TIM2_CLK_HZ       240000000u
#define LOG_SAMPLE_ARR_DEFAULT 11u

/* 控制块状态字 */
#define LOG_ST_IDLE           0u
#define LOG_ST_RUNNING        1u

/* 一条记录 = 一次电平变化。存的是"变化之后的完整电平", 不是"跳变方向" ——
 * 方向由 上升沿 = ~smp[i-1] & smp[i] 推出来, 8 位装不下方向 (log_plan.md §2.3)。
 *
 * ts 是采样序号 (从采集开始按采样周期计数), 单调递增, 不是数组下标。记录 0
 * 的 ts 恒为 0, 所以 last_ts / fs 就是这次采集的真实时长。
 *
 * 结构正好 8 字节, 不是 5: {uint32_t; uint8_t} 受 4 字节对齐约束, 那几个字节
 * 本来就是白送的填充, 所以填满 —— smp 用 uint16_t 还是"热路径零重排"的
 * 需要 (扫描循环本来就在算 s & LOG_PIN_MASK, 结果直接存)。 */
typedef struct {
    uint32_t ts;    /* 采样序号 */
    uint16_t smp;   /* 该采样点的 GPIOG 电平 (已按 LOG_PIN_MASK 屏蔽) */
    uint8_t  flags; /* bit0 = LOG_REC_GAP: 本条之前有丢帧 */
    uint8_t  rsvd;  /* 补齐到 8 字节 */
} log_rec_t;

/* M7 偶尔来不及读某个半帧时, 那 8192 个样本根本没被扫过, 中间可能藏着跳变。
 * 没有这个标志, "漏了一帧"和"这段时间没变化"在数据上完全一样, 波形会画出一段
 * 根本不存在的平线。 */
#define LOG_REC_GAP           0x01u

/* 精简版控制块: M7 自己就是记录的写者, 记录条数/最后 ts/丢帧数全部留在 M7
 * 本地, 这里只放真要跨核的那几项。 */
typedef struct {
    /* ---- CM7 写, CM4 读 ---- */
    uint32_t cfg_run;         /* 0 = 停, 1 = 采 (滑动开关) */
    uint32_t cfg_sample_arr;  /* TIM2 的 ARR: fs = 240e6 / (arr + 1) */
    uint32_t cmd_seq;         /* 配置序号: CM4 看到它变了才整体读一遍 cfg_* */

    /* ---- CM4 写, CM7 读 ---- */
    uint32_t frame_seq;       /* 单调递增: 已完成的半帧数 (HT/TC 各 +1) */
    uint32_t state;           /* LOG_ST_IDLE / LOG_ST_RUNNING */
    uint32_t fs_hz;           /* 实测采样率 (CM4 用 DWT 量出来的, 不是标称值) */
    uint32_t diag_halves;     /* 完成的半帧数 (= frame_seq) */
    uint32_t diag_cycles;     /* 半帧周期的平滑值, DWT 周期 */
    uint32_t diag_scan_max;   /* 统计窗口内最长的那个半帧周期 */

    uint32_t _rsvd[23];       /* 补到 128 字节 */
} log_ctrl_t;

#define LOG_CTRL ((volatile log_ctrl_t *)LOG_CTRL_ADDR)

OSC_STATIC_ASSERT(sizeof(log_rec_t) == 8, "log_rec_t 必须正好 8 字节");
OSC_STATIC_ASSERT(sizeof(log_ctrl_t) == 128, "log_ctrl_t 必须正好 128 字节");

/* ---- 地址契约 ----
 * 控制块末尾不能压到采样缓冲头上; 采样缓冲必须整块落在 D2 的 MPU 区域 2
 * (0x30030000..0x3003FFFF) 里 —— 区域 2 是 CubeMX 里配好且唯一的非缓存区
 * (CM7/Core/Src/main.c 的 MPU_Config), 越出这一块 M7 就会读到缓存里的脏数据。
 * 采样缓冲还要 32 KB 对齐, 目的是不让它跨 MPU 子区域边界。
 *
 * 这三条在 CM4 的 LOG_Init 里还会再跑一遍运行时版本 (见 logic.c)。 */
OSC_STATIC_ASSERT(OSC_CTRL_ADDR + sizeof(osc_ctrl_t) <= LOG_CTRL_ADDR,
                  "osc_ctrl 长过头, 压到 log_ctrl 上了");
OSC_STATIC_ASSERT(LOG_CTRL_ADDR + sizeof(log_ctrl_t) <= LOG_SAMPLE_BUF_ADDR,
                  "log_ctrl 压到采样缓冲头上了");
OSC_STATIC_ASSERT((LOG_SAMPLE_BUF_ADDR & 0x7FFFu) == 0u,
                  "采样缓冲必须 32 KB 对齐");
OSC_STATIC_ASSERT(LOG_SAMPLE_BUF_ADDR >= 0x30030000u &&
                  LOG_SAMPLE_BUF_ADDR + 2u * LOG_SAMPLE_HALF_BYTES <= 0x30040000u,
                  "采样缓冲没有整块落在 MPU 区域 2 (0x30030000..0x3003FFFF) 内");


#endif
