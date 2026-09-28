#ifndef __LOGIC_TRACE_H
#define __LOGIC_TRACE_H

#include <stdint.h>

/* 这个头会被 C 和 C++ 两边 include (TouchGFX 的 Model 是 C++), 不加这个
 * 守卫链接期会因为名字修饰对不上而报 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * CM7 侧逻辑分析仪 —— 读半帧、找跳变、写记录、抽成列
 *
 * 数据流 (完整设计见 docs/log_plan.md §8):
 *   CM4 用 TIM2 + DMA2 把 GPIOG->IDR 采进 D2 的 32 KB 双缓冲, 每个半帧
 *   (16 KB = 8192 个样本 = 409.6 µs @20 MSa/s) 完成后把 frame_seq 加一、
 *   并 take+release 一次 2 号信号量;
 *   CM7 在 HSEM1_IRQn 里被这一下叫醒 (LOG_Trace_OnHalfFrame), 读掉那一格、
 *   找跳变点、把 8 字节一条的记录写进 D1 AXI SRAM;
 *   记录满或者空闲超时之后把记录抽成 LOG_TRACE_COLS 列交给屏幕 (LOG_Trace_Poll,
 *   在 TouchGFX 的 tick 里)。
 *
 * ── 为什么扫描在中断里、归约在 tick 里 ──
 * 扫描有硬期限: 半帧 409.6 µs, 整块缓冲 819.2 µs, 慢了 DMA 就把那一格绕回来
 * 覆盖掉。挂 60 Hz 的 tick 上就是每 41 格才看 1 格。
 * 归约 (记录 → 520 列) 没有期限 (采完才做一次), 而且它写的那两片列数组会被
 * 屏幕任务读走 —— 放在中断里就得考虑"读到一半被下一次采集覆盖"。
 *
 * 这个模块不依赖 TouchGFX —— Model 那边只是把结果喂给控件而已。
 * ========================================================================= */

/* 屏幕上波形区的宽度: x 从 96 到 616, 一共 520 列, 一列一个像素。
 * 8 个通道的框位置见 LogicView.cpp 里的 LOG_BOX_TOP。 */
#define LOG_TRACE_COLS  520u

/* 一条记录都没有 / 一条变化都没有时, 只画 8 条平线 (电平取记录 0)。 */

/* 空闲超时: 最后一个跳变之后又过去了这么多**采样点**, 就认为信号停了, 停止
 * 采集并出图。20 MSa/s 下 20e6 个样本 = 1 秒 (docs/log_plan.md §10)。 */
#define LOG_IDLE_TIMEOUT_SAMPS  20000000u

/* 启动门超时 (ms): 合上开关之后等 CM4 把 state 置成 RUNNING 的上限。正常是
 * 几十微秒, 给到 100 ms 是为了容忍 CM7 这边正在跑别的耗时活 (TouchGFX 刷屏)。
 * 超了就把开关弹回去 —— 那说明 CM4 起不来了 (DMA 起不来 / 采集被别的挡了)。 */
#define LOG_START_TIMEOUT_MS    100u

/**
 * 一次性初始化。在 CM7/Core/Src/main.c 的 USER CODE BEGIN 2 里调一次
 * (M7_OSC_Init() 旁边), 必须在 osKernelStart() 之前。
 */
void LOG_Trace_Init(void);

/**
 * 打开采集开关: 把示波器停掉 (互斥), 写 cfg_* + cmd_seq++, 再合上 cfg_run。
 *
 * 记录 0 在这里就写下去 (ts = 0, 电平 = 合上开关那一刻 GPIOG 的输入) ——
 * 它不是可选项: 没有它, "第一个跳变点是下降沿说明之前一直是高电平"这个
 * 推理就没有起点 (docs/log_plan.md §2.5)。
 *
 * 重复调用没有副作用 (已经在跑就直接返回)。
 */
void LOG_Trace_Start(void);

/**
 * 断开采集开关 (cfg_run = 0)。CM4 那边看到就停 TIM2 + DMA 并把 LOG_EN 拉低。
 * 已经采到的记录不动 —— 出图是 GetCols 的事。
 */
void LOG_Trace_Stop(void);

/**
 * 丢掉上一次采集的结果, 回到"还没采过"的状态 (屏幕上的 Cancel 按钮)。
 *
 * 采集中的话先停掉, 再清记录数 / 起点电平 / 已经算好的那一屏。清完之后:
 *   - GetCols 不会再交出任何东西 (屏幕该被擦掉, 这是 View 的事)
 *   - 下一次 LOG_Trace_Start 是从零开始的一轮, 和开机后第一次一样
 *
 * **不动 s_build_seq / s_built_seq**: 那一对是"归约到第几号"的单调记号,
 * tick 本地存了 s_built_seq, 把它清成 0 有可能和本地那份撞上而漏掉一次归约。
 *
 * 从屏幕任务调 (按键回调), 不要在中断里调。
 */
void LOG_Trace_Clear(void);

/** 采集是否在跑 */
int LOG_Trace_IsRunning(void);

/**
 * 中断上下文: CM4 每完成一个半帧就在 HSEM1_IRQn 里叫一次这个函数
 * (由 CM7 的 HAL_HSEM_FreeCallback 调, 见 Mylibrary/Src/oscillograph.c)。
 *
 * 干的事: 判有没有追丢 (打 GAP 标志) → 扫那一格 → 把跳变点写成记录 →
 * 判记录满 / 空闲超时, 是就停下来并置一个"该出图了"的记号。
 *
 * **不做归约**(记录 → 520 列), 那是 LOG_Trace_Poll 的事 —— 理由见文件头。
 * 不在采集中就直接返回 (比如上一次采集结束后又飘进来一次通知)。
 *
 * 调用周期必须跟得上半帧周期 (409.6 µs), 否则记进 log_diag_gap_recs。
 */
void LOG_Trace_OnHalfFrame(void);

/**
 * 每个 tick 调一次 (TouchGFX 的 Model::tick)。
 *
 *   - 有算好的新一屏就归约出来 (记录 → LOG_TRACE_COLS 列), 等 GetCols 来取
 *   - 摆 LED (采集中有跳变 → 亮)
 *   - 等 CM4 把采集真正拉起来 (带超时, 免得界面卡在"在跑")
 *
 * 每次都要调, 不管采集在不在跑 —— 采集是在中断里停的, 停完那一次通知得靠这里
 * 归约出来。开销是几个读和比较。
 */
void LOG_Trace_Poll(void);

/**
 * 取一份算好的列数据。**取走式**: 每次出图之后只有第一次调用返回 1, 之后
 * 返回 0 —— 所以 Model 那边是"返回值变成 1 就 invalidate() 一次", 而不是
 * 每个 tick 都白重绘一遍。
 *
 *   *level  每列末尾电平, 位映射同 LOG_CH_BIT (所以是 uint16_t, 不是 uint8_t:
 *           最高那位是 bit 13, 8 位装不下 —— docs/log_plan.md §2.3)
 *   *change 每列内发生跳变的通道掩码, 位映射同上
 *   *ncols  列数, 恒等于 LOG_TRACE_COLS
 *
 * 返回 0 时三个出参都不动。
 */
uint8_t LOG_Trace_GetCols(const uint16_t **level, const uint16_t **change, uint32_t *ncols);

/* ---- 诊断 ---- */

/** 追丢的半帧数 (那次采集里 frame_seq 一次跨了不止一格)。判据 6 就看它: */
extern volatile uint32_t log_diag_gap_recs;
/** 最近一次采集写下的记录条数 */
extern volatile uint32_t log_diag_recs;
/** 最近一次采集覆盖的采样点数 (= 最后一条记录的 ts) */
extern volatile uint32_t log_diag_ts_span;

#ifdef __cplusplus
}
#endif

#endif
