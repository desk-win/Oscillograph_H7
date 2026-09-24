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
 *   时间窗   512 列 × 16 word/列 = 8192 word = 2.62 ms   (OSC_WORD_PERIOD_NS = 320)
 *   对齐     触发点固定停在第 128 列, 也就是屏幕上左边 25% 的位置
 *   输出     每列留一个最小值和一个最大值 → 1024 个点。折线图容量就是 1024
 *
 * 这个模块不依赖 TouchGFX, 只依赖 sharedconfig.h 和 SDRAM 地址宏 ——
 * Model 那边只是把结果喂给 graph 而已。
 * ========================================================================= */

#define OSC_TRACE_COLUMNS        512u
#define OSC_TRACE_POINTS         (OSC_TRACE_COLUMNS * 2u)          /* 1024 */
#define OSC_TRACE_WORDS_PER_COL  16u
#define OSC_TRACE_WORDS          (OSC_TRACE_COLUMNS * OSC_TRACE_WORDS_PER_COL)  /* 8192 */

/* 触发点放在第几列。放 25% 处 = 屏幕上能看到 1/4 屏触发前过程 + 3/4 屏触发后过程。
 *
 * 这个常量还决定了"从触发发生到画到屏幕上"至少滞后多久: 触发点右边那
 * OSC_TRACE_POST_WORDS 个 word 得等后面的半帧采出来、再搬进环里才存在。
 * 这里是 6144 word = 3 个半帧 = 1.97 ms, 是原理上的下限, 绕不过去。 */
#define OSC_TRACE_TRIG_COL       128u
#define OSC_TRACE_PRE_WORDS      (OSC_TRACE_TRIG_COL * OSC_TRACE_WORDS_PER_COL)  /* 2048 = 1 个半帧 */
#define OSC_TRACE_POST_WORDS     (OSC_TRACE_WORDS - OSC_TRACE_PRE_WORDS)         /* 6144 = 3 个半帧 */

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

/* 每隔几个 TouchGFX tick 重画一次。tick 是 60 Hz, 除 3 就是 20 Hz。
 * 抽一屏要读 16384 个 word (两个通道加起来), 约 650 µs —— 每个 tick 都做
 * 会白白吃掉 4% 的帧时间, 20 Hz 对眼睛已经足够连续了。 */
#define OSC_TRACE_REFRESH_TICKS  3u

typedef enum {
    OSC_TRACE_CH1 = 0,   /* 折线图 1 */
    OSC_TRACE_CH2 = 1    /* 折线图 2 */
} osc_trace_ch_t;

/* 开始采集指定通道: 清掉它的触发锁存, 再把 CM4 那边对应的 cfg_run_chN 置上。
 * 重复调用没有副作用。另一路在不在跑不受影响。 */
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

#ifdef __cplusplus
}
#endif

#endif
