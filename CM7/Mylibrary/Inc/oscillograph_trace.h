#ifndef __OSCILLOGRAPH_TRACE_H
#define __OSCILLOGRAPH_TRACE_H

#include <stdint.h>

/* 这个头会被 C 和 C++ 两边 include (TouchGFX 的 Model 是 C++), 不加这个
 * 守卫链接期会因为名字修饰对不上而报 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 屏幕波形取数 —— 从 SDRAM 深存储环里抠出一屏, 抽点成折线图能画的点数
 *
 * 环 (oscillograph.h 里那张 SDRAM 映射表) 是 CM4 一直采、CM7 一直搬的,
 * 没有"写满了"这个状态。所以这里是边写边读: 每次只读写指针后面已经落定的
 * 那些 word。
 *
 * 一屏怎么切
 *   时间窗   512 列 × W word/列, W 是**每通道一个**的档位 (见下面的时基档位表),
 *            1 word = 320 ns (OSC_WORD_PERIOD_NS)。默认档 W=16 → 2.62 ms。
 *   对齐     触发点固定停在第 128 列, 也就是屏幕上左边 25% 的位置
 *   输出     每列留一个最小值和一个最大值 → 1024 个点。折线图容量就是 1024
 *
 * 两路各调各的档位, 但共用同一条横轴和同一个触发列 —— 所以这是个"双时基"
 * 视图: 屏上两条曲线的**横向量纲可以不一样**。看单路的形状没问题, 拿两路的
 * 横向位置互相比时间就没意义了。
 *
 * 这个模块不依赖 TouchGFX, 只依赖 sharedconfig.h 和 SDRAM 地址宏 ——
 * Model 那边只是把结果喂给 graph 而已。
 * ========================================================================= */

#define OSC_TRACE_COLUMNS        512u
#define OSC_TRACE_POINTS         (OSC_TRACE_COLUMNS * 2u)          /* 1024 */

/* ---- 时基档位 ----
 *
 * 档位号 i 对应"每列吃 1<<i 个 word"。9 档:
 *
 *   i    W      一屏时长     抽一屏读的 word 数 (两路合计)   重画节流
 *   0    1      163.84 µs    1024                          3 tick /  50 ms
 *   1    2      327.68 µs    2048                          3
 *   2    4      655.36 µs    4096                          3
 *   3    8        1.31 ms    8192                          3
 *   4   16        2.62 ms   16384      ← 改档位之前的行为   3
 *   5   32        5.24 ms   32768                          3
 *   6   64       10.49 ms   65536                          4 tick /  67 ms
 *   7  128       20.97 ms  131072                          6 tick / 100 ms
 *   8  256       41.94 ms  262144                          8 tick / 134 ms
 *
 * 档位表 (W、节流、时长) 全在 .c 里的 s_tb, 这里是"有几档"和默认值。
 *
 * W 全部是 2 的幂且整除半帧 (2048 word) —— 一列因此永远不跨半帧边界,
 * "读到的每一段都落在已经搬完的半帧里"这条保证不用重证。
 *
 * 环里有 4194304 个 word = 1.34 s, 最慢档的 131072 word 也远在历史里 ——
 * 所以切档不用重启采集, 深存储里的数据够。 */
#define OSC_TB_STEPS             9u
#define OSC_TB_DEFAULT           4u      /* W=16, 2.62 ms */
#define OSC_TB_MAX_WPC           (1u << (OSC_TB_STEPS - 1u))   /* 256 */

/* 编码器上那个开关的两档: 调时基 / 调幅度 */
#define OSC_MODE_TIME            0u
#define OSC_MODE_SCOPE           1u

/* 放大档位: 0..7 → X1, X2, X4, ..., X128 (倍数 = 1 << n)。
 * 和 CM4 那边 S_4051 的档位号是同一个编码, 跨核传的就是这个数。 */
#define OSC_GAIN_STEPS           8u
#define OSC_GAIN_DEFAULT         0u      /* X1 */

/* 示波器界面当前的全部设置。Model 拼出来交给 View —— View 不该自己去问
 * "哪一路在跑" 之类的状态, 那是 Model 的事。 */
typedef struct {
    uint8_t selected;    /* 0 = CH1, 1 = CH2: 编码器短按选中的那一路 */
    uint8_t mode[2];     /* OSC_MODE_* */
    uint8_t tb[2];       /* 时基档位 0..OSC_TB_STEPS-1 */
    uint8_t gain[2];     /* 放大档位 0..OSC_GAIN_STEPS-1 */
} osc_ui_state_t;

/* 触发点放在第几列。放 25% 处 = 屏幕上能看到 1/4 屏触发前过程 + 3/4 屏触发后过程。
 *
 * 这个常量还决定了"从触发发生到画到屏幕上"至少滞后多久: 触发点右边那
 * POST_WORDS = (512-128)×W 个 word 得等后面的半帧采出来、再搬进环里才存在。
 * 默认档是 6144 word = 3 个半帧 = 1.97 ms; 最慢档是 98304 word = 31.5 ms。
 * 这是原理上的下限, 绕不过去。 */
#define OSC_TRACE_TRIG_COL       128u

/* 读窗口统一往后退几个半帧, 当作"最新那几格还不存在"。
 *
 * 为什么需要: seq_chN 是 CM4 在 D2 SRAM 里写完半帧就发出来的, 而它进 SDRAM 环
 * 还得等 CM7 这边发一次 MDMA —— 发车的地方就在 Model::tick() 里, 也就是
 * "刚发完车转头就去读"。所以最新那一格必然还在搬, 读它就会读到搬了一半的数据
 * (半新半旧, 屏幕上表现为一小段错位)。
 *
 * 退后一格 = 655 µs, 比搬运本身 (8192 B, 约 160 µs) 宽松得多; 而这 655 µs 的
 * 额外显示延迟在屏幕上根本看不出来。 */
#define OSC_TRACE_SAFE_HALVES    1u

/* 每隔几个 TouchGFX tick 重画一次 —— **基准档 (W=16) 的值**, tick 是 60 Hz,
 * 除 3 就是 20 Hz。抽一屏要读 16384 个 word (两个通道加起来), 约 650 µs,
 * 每个 tick 都做会白白吃掉 4% 的帧时间, 20 Hz 对眼睛已经足够连续了。
 *
 * 慢档要读的 word 数按 W 线性涨 (W=256 时 10.4 ms, 塞进一个 16.7 ms 的帧里
 * 会明显顿一下), 所以真正用的是 OSC_Trace_RefreshTicks(), 它按当前档位从
 * 表里取, 且**不会小于这个数**。 */
#define OSC_TRACE_REFRESH_TICKS  3u

typedef enum {
    OSC_TRACE_CH1 = 0,   /* 折线图 1 */
    OSC_TRACE_CH2 = 1    /* 折线图 2 */
} osc_trace_ch_t;

/* 开始采集指定通道: 清掉它的触发锁存, 打开 CM4 的触发扫描 (cfg_trig_enable),
 * 再把对应的 cfg_run_chN 置上。重复调用没有副作用。另一路在不在跑不受影响。
 *
 * cfg_trig_enable 是全局一位 (不分通道): 关着的话 CM4 只采不找触发点, 屏幕上
 * 的波形永远处于自由运行状态 —— 相位每帧随机换一个, 看起来就是在跳。 */
void OSC_Trace_Start(osc_trace_ch_t ch);

/* 停止指定通道: 只清它的 cfg_run_chN。环里的数据不动 —— 屏幕上那一路怎么
 * 处理是 View 的事 (OscTraceWidget 那边是直接擦掉, 不留残影)。
 * 另一路在不在跑不受影响。 */
void OSC_Trace_Stop(osc_trace_ch_t ch);

/* 指定通道的采集是否在跑 */
int  OSC_Trace_IsRunning(osc_trace_ch_t ch);

/* 每个 tick 调一次 —— 更新**在跑的那几个**通道的触发锁存。
 *
 * 很便宜 (只是读几个共享变量), 但必须比"重画"调得勤: 候选触发点要冻着等
 * 它变老 (见 .c 里的说明), 而变老只要 2 ms。只在重画时调的话, 显示会白白
 * 多等一个重画周期。 */
void OSC_Trace_Poll(void);

/* 抠一屏出来, 写成 OSC_TRACE_POINTS 个 Y 值:
 *   y_out[2c]   = 第 c 列的最小值
 *   y_out[2c+1] = 第 c 列的最大值
 * 折线按 [min, max, min, max, ...] 连起来, 就是一条有厚度的包络线 —— 这比
 * 每列随便取一个点强, 因为它不会把窄脉冲漏掉。
 *
 * 返回 0 表示这一路没有可显示的数据 —— 要么刚 Start、环里还没凑够一屏, 要么
 * 这一路已经停了。两种情况下 y_out 都不动 (调用方那边留着上一屏的内容)。
 * 单通道开销约 330 µs (8192 个 word, SDRAM 非 cache 标量读约 19 周期 @480 MHz)。
 */
int OSC_Trace_Build(osc_trace_ch_t ch, uint16_t *y_out);

/* ---- 时基档位 ---- */

/* 换某一档的时基。idx 超出 0..OSC_TB_STEPS-1 会被夹到边界 (不回绕: 转到头
 * 就该停住, 从 42 ms 一把跳到 164 µs 不是用户想要的)。
 *
 * 值没变就直接返回。变了会**丢掉这一路的触发锁存** —— 锁存里存的是绝对
 * word 号, 而"触发点落在窗口的第几列"是按当前档位算的, 不丢的话触发点会
 * 停在错误的列上, 而且看起来"有触发", 不会自己恢复。
 *
 * 环里有 1.34 s 的历史数据, 所以切档不需要重启采集, 也不会空屏。 */
void OSC_Trace_SetTimebase(osc_trace_ch_t ch, int idx);

/* 当前档位 (0..OSC_TB_STEPS-1) */
int  OSC_Trace_GetTimebase(osc_trace_ch_t ch);

/* 档位 idx 一屏的时长 (ns)。界面那边拿它显示 "time 2.62ms" ——
 * 用定点整数拼, 不要走浮点。idx 越界会被夹到最慢那一档。 */
uint32_t OSC_Trace_SpanNs(uint32_t idx);

/* 现在该隔几个 tick 重画一次: 取两路里**在跑**的较大者, 都不在跑就返回基准档
 * 的值。返回值恒 >= 1, 可以直接拿去当分频数用。 */
uint8_t  OSC_Trace_RefreshTicks(void);

#ifdef __cplusplus
}
#endif

#endif
