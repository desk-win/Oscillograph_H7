#ifndef __LOGIC_TRACE_H
#define __LOGIC_TRACE_H

#include <stdint.h>

/* LOG_Trace_GetRecs 的参数用到了 log_rec_t, 所以这个头得自己把它拉进来 ——
 * 以前只有 .c 那边用到, 靠 logic_trace.c 自己 include 就够了; 现在 C++ 侧
 * (Model) 也 include 这个头, 缺了它就是 "'log_rec_t' does not name a type"。 */
#include "sharedconfig.h"

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
 *   中断 (HSEM1_IRQn) 里只置位叫醒扫描任务 (LOG_Trace_NotifyFromISR), 任务里
 *   读掉那一格、找跳变点、把 8 字节一条的记录写进 D1 AXI SRAM;
 *   记录满或者空闲超时之后把记录抽成 LOG_TRACE_COLS 列交给屏幕 (LOG_Trace_Poll,
 *   在 TouchGFX 的 tick 里)。
 *
 * ── 为什么扫描做成任务、归约留在 tick 里 ──
 * 扫描有硬期限: 半帧 409.6 µs, 整块缓冲 819.2 µs, 慢了 DMA 就把那一格绕回来
 * 覆盖掉。挂 60 Hz 的 tick 上就是每 41 格才看 1 格; 放中断里则会把 prio 15 的
 * SysTick/TIM7 一起挡住, 所以它有独立的最高优先级任务 (LogicScan_Task)。
 * 归约 (记录 → 520 列) 没有期限 (采完才做一次), 而且它写的那两片列数组会被
 * 屏幕任务读走 —— 放在中断里就得考虑"读到一半被下一次采集覆盖"。
 *
 * 这个模块不依赖 TouchGFX —— Model 那边只是把结果喂给控件而已。
 * ========================================================================= */

/* 屏幕上波形区的宽度: x 从 96 到 616, 一共 520 列, 一列一个像素。
 * 8 个通道的框位置见 LogicView.cpp 里的 LOG_BOX_TOP。 */
#define LOG_TRACE_COLS  520u

/* 时基的下限: 一屏最少铺这么多采样点, 也就是"一列正好一个采样点"。
 *
 * 再细下去没有意义 —— 列到时间的映射是 ts_c = span * c / (COLS-1), span 小于
 * 这个数就会有好几列算在同一个采样点上, 画出来只是把同一个台阶拉宽, 不是真
 * 的放大。@20 MSa/s 下 519 个采样点 ≈ 26 µs 一屏 (50 ns 一列)。
 *
 * 注意它是 COLS-1 不是 COLS: ts_c 的取值范围是 [0, span] 闭区间, 所以
 * span = 519 时 ts_c 正好走遍 0..519 共 520 个采样点, 一列一个。
 * 见 logic_trace.c 的 view_span()。 */
#define LOG_TRACE_SPAN_MIN  (LOG_TRACE_COLS - 1u)

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
 * 请求式: 记录 0 和 cfg_* 的实际下发在扫描任务里 (scan_do_start); 它优先级
 * 更高, 请求一发就抢占执行, 调用方看到的效果和同步一样。
 */
void LOG_Trace_Start(void);

/**
 * 断开采集开关 (cfg_run = 0)。CM4 那边看到就停 TIM2 + DMA 并把 LOG_EN 拉低。
 * 已经采到的记录不动 —— 出图是 GetCols 的事。
 * 请求式: 实际由扫描任务执行 (同上, 微秒级生效)。
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

/* ===========================================================================
 * 视图模式 —— 编码器在这一屏上是干什么的
 *
 * 两种, 用编码器上那个开关**长按**来回切; 界面上右下角那个开关的图跟着翻面,
 * 表示现在是哪一种:
 *   LOG_VIEW_TIMEBASE  调时基 —— 一屏铺多长一段采样区间
 *   LOG_VIEW_SCROLL    滚窗口 —— 时基不动, 把窗口 [t0, t0+span] 前后挪
 *
 * 模式是**用户偏好**, 不是这一轮采集的状态: LOG_Trace_Start / LOG_Trace_Clear
 * 都不会把它复位, 只有 ToggleViewMode 能改。切屏当然也不影响它 —— 界面回来时
 * 按 GetViewMode 的结果把开关摆回去就行。
 * ========================================================================= */
#define LOG_VIEW_TIMEBASE  0u
#define LOG_VIEW_SCROLL    1u

/** 当前视图模式 (LOG_VIEW_TIMEBASE / LOG_VIEW_SCROLL)。 */
uint32_t LOG_Trace_GetViewMode(void);

/**
 * 在两种视图模式之间来回切。编码器上那个开关长按走这条路。
 *
 * 右下角那个开关的**图**不归这个函数管 —— 那是 View 按 GetViewMode 的结果自己
 * 摆的 (和 toggleButton1 一样的做法: 以 Model 为准, 不读开关当前显示的是哪一面)。
 *
 * 采集中也能切, 而且切了不重算那一屏: 它不碰记录区, 画面也不变 (同样的 span、
 * 同样的起点), 只是"下一格转动是干什么的"变了。
 */
void LOG_Trace_ToggleViewMode(void);

/**
 * 编码器转了 delta 格 (正 = 顺时针), 按**当前视图模式**分派:
 *
 *   时基模式 (默认) —— 逐档走档位表, 正 = 放大 (一屏铺的时间变短)。
 *                     整屏档在一头, 表尾 (26 µs) 在另一头。
 *   滚动模式       —— 把窗口前后挪, span 不变。一格 1/4 屏, 但乘的是净格数,
 *                     所以转得快就挪得远 (天然的加速, 没有另做一套)。
 *
 * 传"净格数"而不是单格的 ±1: 一帧里转了好几格的话, 上层不必自己拆成一格一格
 * 调, 而且这一层可以在跳完之后只置一次"该重算"的记号 —— 归约是 O(记录数) 的
 * 活, 一帧里重算好几遍纯属浪费。
 *
 * **两种模式都只对已经采完的那一屏起作用**: s_running 非 0 时直接返回。理由有
 * 两条 —— 记录区正被半帧中断追加 (归约要读整片记录区, 这是它一直被放在 tick 里
 * 做的原因), 而且屏幕上那一屏本来就是上一次采完的旧图, 动一张旧图没有意义。
 *
 * 窗口的两个自由度:
 *   t0   起点 (采样序号), 恒夹在 [0, max(0, s_last_ts - span)] —— 窗口右沿不
 *        越过数据末尾。整屏档下 span == s_last_ts, 可动范围恒为 0, 也就是
 *        **整屏档滚不动** (全都在屏幕上了, 本来也没得滚)。
 *   span 长度 = "一屏覆盖多少个采样点", 见 s_span_tab。
 *
 * 缩放的锚点是**左沿**: t0 不动, 只有 span 变。放大 (span 变小) 后看到的是原来
 * 那一段的前半截; 缩小 (span 变大) 后如果 t0+span 越过了 s_last_ts, t0 会被夹回
 * 去 —— 表现为窗口整体滑到采集末尾贴住, 而不是画出一段没采过的空白。
 *
 * 档位到顶 / 到底 / 没有数据 / 这次采集短到 LOG_TRACE_SPAN_MIN 个采样点都铺
 * 不满, 一律直接返回, 不产生任何副作用。
 *
 * 从屏幕任务调 (Model::tick)。这里只改档位/起点并置一个记号, 真正重算那一屏仍
 * 然在 LOG_Trace_Poll 里做 —— 所以从调用到 GetCols 交出新的列数据, 最坏隔一个
 * tick (同一 tick 内如果 Poll 还没跑到, 就是当帧生效)。
 */
void LOG_Trace_Turn(int delta);

/**
 * 由扫描任务调用: CM4 每完成一个半帧置一次 LOG_REQ_HALF, 中断侧只做这一件事
 * (见 Mylibrary/Src/oscillograph.c 的 HAL_HSEM_FreeCallback)。
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
 * 中断上下文 (HAL_HSEM_FreeCallback 调): 只置一个线程标志叫醒扫描任务。
 *
 * 调用方所在中断的优先级数值必须 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
 * (= 5)。这是本工程唯一一处从中断里调 RTOS API, HSEM1_IRQn 现在正是 5。
 */
void LOG_Trace_NotifyFromISR(void);

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

/**
 * 取跳变记录表的只读视图 —— 这是协议解码那一层 (logic_analyze.c) 的输入。
 *
 * 列数据 (GetCols) 是给**画波形**用的, 它已经把记录归约成 520 列了 —— 那是
 * 有损的, 解不出协议。解码要的是原始跳变表: 每个跳变的时间和当时的完整电平。
 *
 * **返回 0 (三个出参一个都不动) 的三种情况**:
 *   - 采集还在跑: 记录表正被半帧中断追加, 读出来是半截的
 *   - 这一次上电还没采过 (下面 GetResultSeq 那个记号还是 0)
 *   - 记录条数 <= 1, 也就是只有记录 0 那条起点电平, 一个跳变都没有
 *
 * *recs 指到 LOG_REC_BUF (D1 AXI SRAM 里那 512 KB), 不用释放。*count 是**有效
 * 条数**不是容量。*last_rec_ts 是最后一条的采样序号, 不需要就传 0。
 *
 * **拿到指针之后不能让采集再跑起来** —— 上面的判断是"那一刻"的, 中间开了采集
 * 的话记录会从 LOG_REC_BUF[0] 开始被覆写, 边读边盖, 解出来是垃圾。
 * (调用点现在是 Model::tick, 同期不会有别的线程动这个闸; 以后解码挪进独立
 * RTOS 任务时, 这一条要重新想一遍 —— 见 logic_analyze.h 里 Run 的说明。)
 */
uint8_t LOG_Trace_GetRecs(const log_rec_t **recs, uint32_t *count,
                          uint32_t *last_rec_ts);

/**
 * 借用冻结的记录表 (任务上下文): 借用期间采集不许重开, Cancel 挂起到释放之后。
 * 解码/存卡这类"要读整片记录区"的活必须先借到它。
 *
 * 返回 0 = 借不到 (采集还在跑 / 已被借用 / 没有可解的数据)。
 * 借到之后必须配一次 LOG_Trace_ReleaseRecs()。
 */
int  LOG_Trace_BorrowRecs(const log_rec_t **recs, uint32_t *count, uint32_t *last_rec_ts);
void LOG_Trace_ReleaseRecs(void);

/**
 * 采集结果的代数: 每收工一轮 (记录满 / 空闲超时 / 用户拨开关停) 加一, 开机是 0。
 *
 * 和 s_build_seq 是同一个数 —— 那个记号本来就在"采集收工"时自增 (tick 拿它
 * 判该不该归约), 这里只是把它交出来。只增不减, Start / Clear 都不清它。
 *
 * 解码那一层拿它判"这一轮解过没有", 免得每次点进 Logic_Data 都把同一批记录
 * 重解一遍。
 */
uint32_t LOG_Trace_GetResultSeq(void);

/* ---- 诊断 ---- */

/** 追丢的半帧数 (那次采集里 frame_seq 一次跨了不止一格)。判据 6 就看它: */
extern volatile uint32_t log_diag_gap_recs;
/** 最近一次采集写下的记录条数 */
extern volatile uint32_t log_diag_recs;
/** 最近一次采集覆盖的采样点数 (= 最后一条记录的 ts) */
extern volatile uint32_t log_diag_ts_span;

/** 检测点: 最近一次 / 最大一次半帧扫描耗时 (DWT 周期, ÷480 是 µs)。
 *  只记数不打印 (打印会拖慢采集); LOG_SCAN_DIAG = 0 时这两个符号不存在。 */
extern volatile uint32_t log_scan_diag_cyc;
extern volatile uint32_t log_scan_diag_max_cyc;

#ifdef __cplusplus
}
#endif

#endif
