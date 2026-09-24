#ifndef OSC_PROBE_H
#define OSC_PROBE_H

/* ===========================================================================
 * 调优探针 —— 把一帧各段耗时和搬运计数器通过 USART1 打出来
 *
 * 存在的理由: 打开采集开关之后界面变卡、按键要等一会儿才有反应。处理点击
 * 的代码和 Model::tick() 在同一个任务里, 所以"卡"必然是 tick 那一帧里多花
 * 掉的时间造成的。但光看代码估不出到底花在哪段, 得量。
 *
 * 怎么用:
 *   1) main() 那边不用管, OSC_Probe_Init() 在 Model 的构造函数里调 (只一次)。
 *   2) StartDefaultTask 里每秒调一次 OSC_Probe_Report()。
 *   3) 串口 (USART1, 115200-8N1) 上就会每秒出一行。
 *
 * 打出来的东西:
 *   dt     这一行距上一行过了多久 (us)。累计量 (irq / pushed) 必须除它才能
 *          换算成速率 —— defaultTask 优先级比 TouchGFX 低, 它到底隔多久才
 *          被轮到一次, 光看代码看不出来。
 *   frame  上一帧到这一帧隔了多久 = 当前实际帧周期。不采集时也应该在
 *          15500us 上下 (64 Hz), 采集时它涨多少就是慢了多少。
 *   max    帧周期的历史最大值。若是帧周期的整数倍, 说明是"整帧整帧地没 tick",
 *          不是"每帧慢一点" —— 这两种毛病完全不是一回事。
 *   hdt    HAL 毫秒时基这一窗口走了多少毫秒 (TIM7 驱动)。正常应该 ≈ dt/1000。
 *          对不上就说明 TIM7 被饿过, 所有靠 HAL_GetTick() 的超时 ——
 *          HAL_Delay / 串口 / I2C —— 都会跟着失准, 那正是"卡住"的另一种成因。
 *   ntick  Model::tick() 的累计调用次数。除以 dt 就是真实帧率。
 *   uart   上一次 HAL_UART_Transmit 花了多久 (us)。dt 大的时候用它分辨是
 *          "任务没被调度"还是"发送本身慢"。
 *   isr    CPU 待在中断里的时间 (us), 并集, 见下面 OSC_ISR_Enter 的说明
 *   isrbp  上面那个占 dt 的比例, 单位万分之一 (10000 = 100%)。
 *          实测关采集约 5, 开采集约 53 —— 中断只占 0.05% 和 0.55%, 是清白的。
 *   isrcnt 从任务态进入中断的次数。和 irq 比一下就知道中断是不是全来自 HSEM
 *   build  OSC_Trace_Build × 2, 也就是从 SDRAM 读 16384 个 word 的时间
 *   graph  两个折线图灌点 (clear + 1024×addDataPoint) 的时间
 *   rend   框架自己往显存上画的时间 (beginFrame→endFrame), 上面那三个的余数
 *          的正面证据。采集关掉时是基准值 (几毫秒), 采集开着时看它涨多少
 *   rmax   rend 的历史最大值
 *   stall  帧周期超过 50 ms 的次数。区分"偶发卡一下"和"每隔一会儿就来一下"
 *   irq    CM4 的 HSEM 通知进来了多少次。采集时应该 ≈ 1526×2/秒
 *   chN    pushed / overrun / err / missed, 定义见 oscillograph.h
 *
 * 调完请把这份一起删掉, 一共四处:
 *   1) CM7/Mylibrary/{Inc/osc_probe.h, Src/osc_probe.c} 两个文件
 *   2) CM7/CMakeLists.txt 里 Src/osc_probe.c 那一行
 *   3) Model.cpp 里的一处调用 + freertos.c 里 StartDefaultTask 那一段
 *   4) TouchGFX/target/TouchGFXHAL.cpp 里的 include 和 beginFrame/endFrame
 *      (这两个是 USER CODE 段, 重新生成不会被覆盖, 但也就要手动删)
 * ========================================================================= */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DWT 周期计数器。Cortex-M7 内核里的固定地址, 不走总线矩阵, 读它本身几乎
 * 不花时间, 所以探针不会把要量的事情本身搅乱。
 *
 * 32 位, 按主频回绕: 480 MHz 下 8.9 秒绕一圈。所有差值一律用减法算, 跨回绕
 * 自动正确, 但两次取值之间不能超过一圈。 */
#define OSC_PROBE_CYC (*(volatile uint32_t *)0xE0001004u)

/* 单位都是 CPU 周期。用减法比较, 所以可以跨回绕。 */
extern volatile uint32_t g_dbg_gap_cyc;      /* 上一个 tick 到这一个 */
extern volatile uint32_t g_dbg_gap_max_cyc;  /* 历史最大值 */
extern volatile uint32_t g_dbg_gap_min_cyc;  /* 历史最小值, 0 = 还没有样本 */
extern volatile uint32_t g_dbg_build_cyc;    /* 最近一次 OSC_Trace_Build ×2 */
extern volatile uint32_t g_dbg_graph_cyc;    /* 最近一次两个折线图灌点 */

/* ---------------------------------------------------------------------------
 * 渲染 pass 的耗时 —— 从 TouchGFXHAL::beginFrame() 到 endFrame()
 *
 * 这一对探针是为了把"一次重画 2.67 秒"这件事从排除法变成直接证据。
 *
 * 已经知道的: tick 里的 build (2.1 ms) + graph (0.4 ms) 加起来才 2.5 ms, 而
 * 两次 tick 之间隔了 2670 ms —— 中间剩下的只可能是框架自己往显存上画。
 * 但"只剩它"是排除法。beginFrame/endFrame 正好把"一帧的画"整个夹在中间
 * (HAL.hpp 里这两个函数的注释: 一个叫 "beginning to rendering a frame",
 * 一个叫 "a rendering pass is completed"), 一头一尾掐上表, 这一段有多长
 * 就是直接的证据, 不用再推。
 *
 * 它包含 Model::tick() 自己那 2.5 ms, 相对 2670 ms 可以忽略。
 * 采集关掉时这一列就是基准值 (应该只有几毫秒)。
 * ------------------------------------------------------------------------- */
extern volatile uint32_t g_dbg_render_cyc;      /* 最近一次 beginFrame→endFrame */
extern volatile uint32_t g_dbg_render_max_cyc;  /* 上面那个的历史最大值 */
extern volatile uint32_t g_dbg_begin_cyc;       /* 最近一次 beginFrame 的时刻 */

/* 想重新统计 max/min, 在调试器里把 g_dbg_gap_max_cyc / g_dbg_gap_min_cyc
 * 都写成 0 就行 —— 更新判据对 0 是"重新开始"。 */

/* 一次 tick 间隔超过 g_osc_probe_stall_cyc 的次数。用来区分"偶发卡一下"和
 * "每隔一会儿就来一下" —— 光看 max 分不出这两者。 */
extern volatile uint32_t g_dbg_stall_cnt;

/* "卡了一回"的判据: 一次 tick 间隔超过这么多周期才算数。
 *
 * 这里取 50 ms (约 3 帧), 不是 1 ms —— 正常帧本来就有 15.46 ms, 阈值设成
 * 1 ms 的话每一帧都会被记成一次停滞, 计数器退化成"tick 被调用了几次"。
 *
 * 之所以做成变量而不是让调用方写 SystemCoreClock / 50: 调用方是 C++ 的
 * Model.cpp, 它看不见 SystemCoreClock (那是 CMSIS 的符号, 定义在 HAL 那套
 * 头里)。为了一个阈值把 stm32h7xx.h 拖进 TouchGFX 的文件不划算。
 * OSC_Probe_Init() 里按主频算好。 */
extern uint32_t g_osc_probe_stall_cyc;

/* ---------------------------------------------------------------------------
 * 中断耗时 —— CPU 被中断占住的时长 (并集, 不是各中断之和)
 *
 * 这一段是为"开采集后整个系统冻住约 3 秒"这件事加的。现象是: TouchGFX 任务
 * (优先级 26) 和 defaultTask (24) 会同时停摆, 而 HSEM 中断在那 3 秒里照常
 * 以 3052/s 往里进。两个优先级不同的任务一起停, 只可能是被比任务都高的中断
 * 抢光了 CPU —— 但"进了几次"和"每次多久"是两回事, 光数次数看不出来, 得量。
 *
 * 还有个更硬的旁证: HAL 的毫秒时基走 TIM7, 优先级 15; FreeRTOS 的节拍走
 * SysTick, 优先级也是 15。两个都在最低一级。优先级 0 或 5 的中断只要占住
 * CPU, 这两个节拍就一起停 —— 一个让 osDelay 不走, 一个让 HAL_GetTick 不走,
 * 于是两个任务同时冻住。这正是观察到的症状。
 *
 * 用法: 在每个中断处理函数首尾各包一次 OSC_ISR_Enter / OSC_ISR_Exit。
 * 用嵌套计数, 所以高优先级中断抢占低优先级中断时那段时间只算一次 —— 量的
 * 是"CPU 有多少时间不在任务里", 这个数才和任务被饿死直接对应。
 *
 * 没插桩的中断不计入。CM7 上开着的只有 5 个, 都插了:
 *   MDMA(0) / LTDC(5) / DMA2D(5) / HSEM1(5) / TIM7(15)
 * ------------------------------------------------------------------------- */
extern volatile uint32_t g_dbg_isr_cyc;   /* 累计: CPU 待在中断里的周期数 */
extern volatile uint32_t g_dbg_isr_cnt;   /* 从任务态进入中断的次数 */
extern volatile uint32_t g_dbg_isr_depth; /* 嵌套深度, 中断内部用 */

/* 上一次 HAL_UART_Transmit 花了多少个周期。
 *
 * 这一列是为了把"dt 为什么是 2.9 秒"这件事劈开:
 *   dt 就是 Report 两次进入之间的间隔, 而 Report 里最后一步是阻塞发送。
 *   所以 dt 大有两种可能 —— 任务真的没被调度 (等 2.9 秒才轮到), 还是
 *   发送本身就花了 2.9 秒。这两个的修法完全不同, 但光看 dt 分不出来。
 *   uart 小 (几十 µs 级) 就是前者, uart 赶上 dt 就是后者。 */
extern volatile uint32_t g_dbg_uart_cyc;

/* Model::tick() 被调用的累计次数。除以 dt 就是真实帧率 ——
 * max / stall 只能看出"有没有长间隔", 数不出"到底跑了几帧"。 */
extern volatile uint32_t g_dbg_tick_cnt;

/* 最外层进入时的时刻。中断里用, 外部不要碰。 */
extern volatile uint32_t g_dbg_isr_enter_cyc;

static inline void OSC_ISR_Enter(void)
{
    if (g_dbg_isr_depth == 0u)
    {
        g_dbg_isr_enter_cyc = OSC_PROBE_CYC;
        g_dbg_isr_cnt++;
    }
    g_dbg_isr_depth++;
}

static inline void OSC_ISR_Exit(void)
{
    g_dbg_isr_depth--;
    if (g_dbg_isr_depth == 0u)
    {
        g_dbg_isr_cyc += OSC_PROBE_CYC - g_dbg_isr_enter_cyc;
    }
}

/* 解锁 DWT 并打开周期计数器。不调它的话 CYCCNT 恒为 0, 打出来全是 0。 */
void OSC_Probe_Init(void);

/* 组一行文本从 USART1 发出去。只在 defaultTask 里调 —— 它是阻塞发送, 会占用
 * 调用它的任务好几毫秒, 不能放在 TouchGFX 任务里 (那正是要量时间的地方)。 */
void OSC_Probe_Report(void);

#ifdef __cplusplus
}
#endif

#endif /* OSC_PROBE_H */
