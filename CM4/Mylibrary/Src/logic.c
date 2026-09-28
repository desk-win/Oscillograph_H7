/* ===========================================================================
 * CM4 侧逻辑分析仪采集 —— TIM2 触发 DMA2_Stream0 把 GPIOG->IDR 搬进 D2
 *
 * 这一侧很薄, 故意的: CM4 只负责"保证采样时序"这一件有硬期限的事, 它
 *   - 配好/启停 TIM2 + DMA2 (配置本身在 CubeMX 生成的 tim.c 里)
 *   - 收 HT/TC, 只记账 (frame_seq++), 不扫描、不检测、不碰 D1
 *   - 实测采样率并发布
 * 跳变检测没有硬期限 (32 KB 双缓冲买到 819 KB 的余量), 所以全在 M7 那边。
 *
 * 设计见 docs/log_plan.md 的 §4 / §7。
 * ========================================================================= */

#include "logic.h"
#include "main.h"
#include "oscillograph.h"
#include "sharedconfig.h"
#include "usb_printf.h"     /* bring-up 诊断打印, 见 LOG_DBG_HALF_PRINT */
#include "tim.h"
#include "dma.h"
#include "stm32h7xx_hal_dma.h"
#include "stm32h7xx_hal_gpio.h"
#include "stm32h7xx_hal_tim.h"
#include <stdint.h>

/* TIM2_UP 那条 DMA 流。它定义在 tim.c 里, 但 tim.h 没导出 (那是 CubeMX 生成
 * 的文件, 不能加东西 —— 下次生成会被覆盖), 所以和 stm32h7xx_it.c 一样自己
 * 声明一份。 */
extern DMA_HandleTypeDef hdma_tim2_up;

/* LOG_EN (PG14) 驱动的是 8 路开关芯片的使能端, **低电平的时候 8 个通道才通**。
 *
 * Logic_EN() 是"把 level 原样写到引脚上", 不替谁翻极性 —— 所以在本模块里这个
 * 参数的含义和函数名看着是反的: 0 = 打开前端, 1 = 关掉。用下面两个名字写调用
 * 点, 免得下次读代码的人 (包括我自己) 又顺手写成 Logic_EN(1) = 开。 */
#define LOG_EN_ON   0u      /* 拉低 = 通道通 */
#define LOG_EN_OFF  1u      /* 拉高 = 通道断 */

/* 半帧周期的平均窗口。单发抖动来自别的中断插进来 (USB / TIM6 tick), 平均到
 * 16 个半帧 (20 MSa/s 下 6.5 ms) 读数才稳, 又快到能跟上采样率的变化。 */
#define LOG_RATE_AVG_HALVES   16u

/* 超过"期望周期 × 这个倍数"的那一拍不进平均 —— 它不是采样率变了, 是别的
 * 东西把中断挡了或者 DMA 漏了请求, 混进平均值会把读数拉低很久。 */
#define LOG_CYC_OUTLIER_MUL   4u

/* ---- 记账状态 (只有本文件的函数和 DMA 中断碰) ---- */
static uint32_t s_last_tc;          /* 上一拍 HT/TC 的 DWT 周期数 */
static uint32_t s_half_cyc_expect;  /* 期望的半帧周期, DWT 周期 */
static uint32_t s_cyc_acc;          /* ↓ 统计窗口 */
static uint32_t s_cyc_max;
static uint32_t s_cyc_cnt;

/* 上一拍看到的 cfg_run。只用来抓 0→1 / 1→0 这两个沿。 */
static uint32_t s_run_prev;

/* ===========================================================================
 * bring-up 诊断: 每 N 个半帧把"刚完成那半帧的第一个样本"打出来
 *
 * 什么时候用: 合上开关之后一点反应都没有 —— 用它区分三种情况
 *     seq 一直不涨        → DMA 根本没起 (TIM2 没跑 / DMA 没配好 / HT TC 中断没开)
 *     seq 在涨但 first/m 全是 0 或不随信号变
 *                         → DMA 在跑, 但 8 路开关没通 (LOG_EN 极性!) 或者信号没接上
 *     first 随信号翻、seq 也涨
 *                         → 采集侧是好的, 问题在 M7 那边的消费或显示
 *
 * 为什么不是"每个半帧都打": 半帧 409.6 µs = 2441 个/秒, 而 usb_printf 走的
 * 是 USB CDC, 一次调用到端点腾空要毫秒级 —— 2441 行/秒物理上打不完。所以抽着
 * 打。LOG_DBG_DECIM 是抽 1/N: 1024 → 约 2.4 行/秒, 人眼盯着看刚好;
 * 64 → 约 38 行/秒, 刷屏快得看不清, 只适合往文件里灌。
 *
 * 为什么打印在主循环而不是中断里: usb_printf 是阻塞的。在 409.6 µs 一次的
 * 中断里调它, 中断会被关住几毫秒, 后面的 HT/TC 全堆在一起 —— 那不是"少打几
 * 行", 是把采集时序整个搞垮。
 *
 * 打完关: 现在是 0。**这里不是"占点时间"那么轻, 它能把启动整个搞垮** ——
 *
 * usb_printf 底下 (见 usb_printf.c) 的 cdc_write 在 CDC 端点没腾空时会自旋
 * 最长 CDC_TX_TIMEOUT_MS = 100 ms。这个 100 ms 和 M7 那边逻辑分析仪的启动门
 * LOG_START_TIMEOUT_MS = 100 ms (CM7/Mylibrary/Inc/logic_trace.h) 是同一个数:
 * 打印期间 CM4 走不到 LOG_Poll(), 看不到 cfg_run 的 0→1 边沿; M7 的启动门却
 * 已经在计时, 到点就把开关收回去 —— 表现成"拨了开关一点反应都没有, 要反复点
 * 几次才起得来"。而且失败那一次会打两行 (start + start FAILED), 自己给下一次
 * 点击制造同样的条件。
 *
 * 实测判据: **插着 USB 线**时必现, 拔了线就完全正常 —— 拔线之后 cdc_write 在
 * dev_state 那一句就早退了。所以这不叫"USB 供电的问题", 是"线插没插"的问题。
 *
 * 除了启动门这一条, 每行还有毫秒级的 CDC 传输时间, 主循环走不动, 示波器的扫描
 * (在主循环里, 有 655 µs 期限) 也会成片丢帧。
 *
 * 要再开: 先确保 usb_printf.c 里的总闸 USB_PRINTF_ENABLE = 1, 再看一眼那两个
 * 100 有没有脱钩。 */
#define LOG_DBG_HALF_PRINT   0
/* 抽 1/N 打一行。**要用奇数**: 打的都是 frame_seq 的倍数, 半格号是
 * (frame_seq - 1) & 1, N 是偶数的话这个式子恒等于 1 —— 永远只看后半格,
 * 前半格是什么样一次都看不到。 */
#define LOG_DBG_DECIM        1023u

/* 实时引脚打印的间隔 (ms)。和采集状态**无关**, 采不采都在打 —— 见下面那段。 */
#define LOG_DBG_IDR_MS       500u

#if LOG_DBG_HALF_PRINT
/* 全都是打印用的中转, 关掉打印就一起消失 —— 留着会被 -Wunused-variable 报到。 */
static uint16_t s_dbg_raw;      /* 原始 16 位 (没屏蔽) */
static uint32_t s_dbg_seq;      /* 它属于第几个半帧 */
static uint32_t s_dbg_half;     /* 哪个半格: 0 或 1 */
static volatile uint32_t s_dbg_ready;
static uint32_t s_dbg_idr_tick; /* 上一次打实时引脚电平的时刻 */
#endif

/**
*   @brief 使能逻辑分析仪模块 (PG14)
*
*   @note  PG14 是 LOG_EN 输出, 它不在 LOG_PIN_MASK (0x3E8C) 里 —— 采集回来的
*          数据是屏蔽过的, 所以这根线怎么翻都进不了记录。
*
*          必须在启动 DMA 之前置起, 让输入侧的施密特触发器先稳定下来。
*/
void Logic_EN(uint8_t level)
{
    if (level) {
        HAL_GPIO_WritePin(LOG_EN_GPIO_Port, LOG_EN_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(LOG_EN_GPIO_Port, LOG_EN_Pin, GPIO_PIN_RESET);
    }
}

/* ===========================================================================
 * 地址契约自检
 *
 * 这三个地址都是 sharedconfig.h 里 #define 出来的绝对地址, 所以在那里已经用
 * OSC_STATIC_ASSERT 卡过一遍了。这里再跑一遍运行时版本, 是因为它们不好查:
 * 采样缓冲是 DMA 直接写的绝对地址, 越界不会报任何错, 只会静静地冲掉 D2 里的
 * 别的东西 (首当其冲是示波器的 osc_ch2_buf), 表现是示波器波形莫名其妙花掉,
 * 而"逻辑分析仪"看起来一切正常。
 * ========================================================================= */
static void log_addr_selfcheck(void)
{
    if ((uintptr_t)LOG_CTRL_ADDR + sizeof(log_ctrl_t) > (uintptr_t)LOG_SAMPLE_BUF_ADDR) {
        Error_Handler();        /* 控制块压到采样缓冲头上了 */
    }
    if (((uintptr_t)LOG_SAMPLE_BUF_ADDR & 0x7FFFu) != 0u) {
        Error_Handler();        /* 不是 32 KB 对齐 */
    }
    if (((uintptr_t)LOG_SAMPLE_BUF_ADDR < (uintptr_t)0x30030000u) ||
        ((uintptr_t)LOG_SAMPLE_BUF_ADDR + 2u * LOG_SAMPLE_HALF_BYTES >
                                                       (uintptr_t)0x30040000u)) {
        Error_Handler();        /* 跑出 MPU 区域 2 = 跑出非缓存区 */
    }
}

/* ===========================================================================
 * 启停
 * ========================================================================= */

/* DMA 完成回调。正文在文件末尾 (半帧记账那一节旁边), 这里先声明是因为
 * log_start() 要拿它们的地址去注册。为什么不注册在 LOG_Init 里: 见 log_start
 * 里的注释 —— 注册和 HAL_DMA_Start_IT 必须紧挨着, 中间不能隔任何可能失败或
 * 被重排的东西。 */
static void log_dma_half(DMA_HandleTypeDef *h);
static void log_dma_full(DMA_HandleTypeDef *h);

/* 起采。顺序有讲究, 见 docs/log_plan.md §7.2:
 *   1) 先停定时器和 DMA, 清掉上一次留下的状态 (重入时不这样做会拿到残破的
 *      NDTR/传输计数)
 *   2) ARR 按 CM7 下发的 cfg_sample_arr 重设 —— 采样率是 CM7 说了算的
 *   3) 打开 LOG_EN 在 DMA 之前 —— 让开关芯片 (和它后面的输入通路) 先稳定
 *   4) DMA 先就绪, 定时器最后启动 —— 反过来的话有一小段"DMA 还没就绪但已经在
 *      发请求"的时间, 那几个请求会丢 */
static void log_start(void)
{
    LOG_CTRL->state = LOG_ST_IDLE;
    __DMB();

    HAL_TIM_Base_Stop(&htim2);

    /* ---- 这一下 Abort 是"清上一次留下的 DMA 句柄状态"的唯一手段 ----
     *
     * 为什么必须盯住它: HAL_DMA_Start_IT 的**成功路径不释放 Lock**
     * (stm32h7xx_hal_dma.c: 只有 757 那个失败分支有 __HAL_UNLOCK), 而
     * HAL_DMA_Abort 又只在 State == BUSY 时才动手 —— State 是别的一律早退,
     * 连 State 都不改。两个凑在一起就是: 这里没清干净, 下面 Start_IT 就再也
     * 起不来, 而且不报任何错。
     *
     * 更死的是一条路: Abort 里那个"等 EN 清零"超时 (5 ms) 或 IRQHandler 的 TE
     * 分支会把 State 打成 HAL_DMA_STATE_ERROR, 那之后 Abort 永远走早退分支、
     * State 永远回不到 READY, 除了重新上电没救。
     *
     * 所以先把 Abort 之前的状态抓下来 (那是上一次采集留的), 再打 Abort 之后
     * 的状态 (那是这次 Start_IT 要面对的起点)。一行, 一次采集打一次。 */
#if LOG_DBG_HALF_PRINT
    const uint32_t pre_state = (uint32_t)hdma_tim2_up.State;
    const uint32_t pre_lock  = (uint32_t)hdma_tim2_up.Lock;
    const uint32_t pre_err   = hdma_tim2_up.ErrorCode;
    const HAL_StatusTypeDef abort_ret = HAL_DMA_Abort(&hdma_tim2_up);
#else
    /* 返回值只有上面的诊断打印用。但**这一下 Abort 本身不能省** —— 它是清上
     * 一次采集留下的 DMA 句柄状态的唯一手段 (见上)。 */
    (void)HAL_DMA_Abort(&hdma_tim2_up);
#endif

#if LOG_DBG_HALF_PRINT
    /* st/lock/err 都是 HAL 句柄里的字段, 不是外设寄存器 —— 想知道 CR.EN 要另读。
     * 正常情况: pre st=1 (READY, 上一次 log_stop 收干净了), abort=0, lock=0。
     * 异常时怎么读:
     *   pre st=3 (ERROR)                 → 死路, 只有重新上电
     *   pre st=2 (BUSY) + abort=0        → 上一次 log_stop 没跑, 这一下补上了
     *   abort=1 + lock=1                 → Lock 卡住, Start_IT 会返回 HAL_BUSY */
    usb_printf("[LOG] start: pre st=%lu lock=%lu err=0x%04X | abort=%lu -> st=%lu lock=%lu\r\n",
               (unsigned long)pre_state,
               (unsigned long)pre_lock,
               (unsigned)pre_err,
               (unsigned long)abort_ret,
               (unsigned long)hdma_tim2_up.State,
               (unsigned long)hdma_tim2_up.Lock);
#endif

    const uint32_t arr = LOG_CTRL->cfg_sample_arr;
    __HAL_TIM_SET_AUTORELOAD(&htim2, arr);
    __HAL_TIM_SET_COUNTER(&htim2, 0u);

    /* 期望的半帧周期 (DWT 周期) = 每半帧样本数 / 采样率 fs, 而
     * fs = TIM2 时钟 / (arr + 1)。写成一个式子就是
     *     SystemCoreClock × 8192 × (arr + 1) / LOG_TIM2_CLK_HZ
     * 也就是 docs/log_plan.md §7.4 那条 "240 MHz × 8192 / fs"。
     *
     * 本工程里 CPU 和 TIM2 都是 240 MHz, 所以它化简成 8192 × (arr + 1) =
     * 98304 —— 判据 2 要的就是这个数。保留两个时钟项是免得哪天 CPU 分频变了,
     * 这个换算悄悄跟着错。
     *
     * 实测明显大于它 = DMA 在漏请求, 实际采样率低于标称值 —— 这就是"实测达不
     * 到可降"的落地方式: 不是猜, 是量。 */
    s_half_cyc_expect = (uint32_t)(((uint64_t)LOG_SAMPLE_HALF_SAMPS
                                    * (uint64_t)SystemCoreClock * (uint64_t)(arr + 1u))
                                   / (uint64_t)LOG_TIM2_CLK_HZ);

    s_last_tc   = DWT->CYCCNT;
    s_cyc_acc   = 0u;
    s_cyc_max   = 0u;
    s_cyc_cnt   = 0u;

    /* 记账字段先清零, 再开 DMA —— 反过来的话 CM7 可能先看到 frame_seq 涨了,
     * 却配上还没清干净的诊断字段。 */
    LOG_CTRL->frame_seq     = 0u;
    LOG_CTRL->diag_halves   = 0u;
    LOG_CTRL->diag_cycles   = 0u;
    LOG_CTRL->diag_scan_max = 0u;
    LOG_CTRL->fs_hz         = 0u;
    __DMB();

    /* 打开 8 路开关 (低有效), 必须在 DMA 之前 —— 让输入通路先稳定下来, 否则
     * 最前面几个样本是它还没进入工作状态时采的。 */
    Logic_EN(LOG_EN_ON);
    __DMB();

    /* ---- 注册完成回调, 必须在 HAL_DMA_Start_IT 之前 ----
     *
     * HT 中断的使能位是 HAL_DMA_Start_IT 根据 XferHalfCpltCallback 是否非空来
     * 决定的 —— 晚一步注册, 这一次就只剩 TC, 半帧全跳过。详见 log_dma_half
     * 上面那段。 */
    hdma_tim2_up.XferHalfCpltCallback = log_dma_half;
    hdma_tim2_up.XferCpltCallback     = log_dma_full;

    /* 源地址必须写 &GPIOG->IDR (不是 GPIOG->IDR): PeriphInc = DISABLE, DMA 每次
     * 都读同一个地址, 要的是那个地址本身。
     * 传输个数按"半字"算 (PDATAALIGN/MDATAALIGN 都是 HALFWORD), 整块 32 KB =
     * 16384 个半字。 */
    

        
    if (hdma_tim2_up.State != HAL_DMA_STATE_READY) {
        hdma_tim2_up.Lock = HAL_UNLOCKED;       /* HAL_DMA_Init 不碰 Lock, 得自己来 */
        HAL_DMA_Init(&hdma_tim2_up);        /* 无条件 State=READY, ErrorCode=NONE, 重写 CR */
    }
    
    const HAL_StatusTypeDef dma_ret = HAL_DMA_Start_IT(&hdma_tim2_up,
                                                       (uint32_t)&GPIOG->IDR,
                                                       LOG_SAMPLE_BUF_ADDR,
                                                       LOG_SAMPLE_DMA_LEN);
    if (dma_ret != HAL_OK) {
#if LOG_DBG_HALF_PRINT
        /* 走到这儿 = "开关拨上去了但采不到"。以前这里是全静默的: 唯一的后果是
         * state 停在 IDLE, M7 那边 100 ms 的启动门超时之后把开关拨回 OFF ——
         * 界面上看不出和"没信号"有任何区别。这一行就是把那个静默填上。
         *
         * 怎么读:
         *   ret=2 (HAL_BUSY)  → __HAL_LOCK 卡住了, Lock 还是上一次 Start_IT
         *                       留下的 LOCKED, State 一个字没动
         *   ret=1 (HAL_ERROR) → State 不是 READY, 这时 err 应该是 0x0800
         *                       (HAL_DMA_ERROR_BUSY)
         *   err 的低位另说: 0x0001 = TE, 0x0020 = TIMEOUT —— 那两个是 Abort
         *   或者 IRQHandler 留下的, 说明问题在更早的地方。 */
        usb_printf("[LOG] start FAILED ret=%lu st=%lu lock=%lu err=0x%04X\r\n",
                   (unsigned long)dma_ret,
                   (unsigned long)hdma_tim2_up.State,
                   (unsigned long)hdma_tim2_up.Lock,
                   (unsigned)hdma_tim2_up.ErrorCode);
#endif
        Logic_EN(LOG_EN_OFF);   /* DMA 起不来就把开关关上, 别让它空开着 */
        LOG_CTRL->state = LOG_ST_IDLE;
        __DMB();
        return;
    }

    /* ---- 打开 TIM2 的 UP DMA 请求 (DIER 的 UDE 位) ----
     *
     * 这一句不能省, 也换不掉: **HAL_TIM_Base_Start() 只置 CEN, 一个 DIER 位都
     * 不碰**。没有 UDE, TIM2 的更新事件就不往外发 DMA 请求 —— DMA 配得再对也
     * 一个字节都不搬、一个中断都不来, 而且不报任何错。
     *
     * 只有 HAL_TIM_Base_Start_DMA() 会替你开这一位, 但它顺手就把句柄回调覆盖
     * 成它自己的 TIM_DMAPeriodElapsedCplt / HalfCplt, 我们上面刚注册的那两个
     * 就白注册了。所以这里自己开, 启动仍然用普通的 HAL_TIM_Base_Start()。
     *
     * 顺序必须在 HAL_TIM_Base_Start 之前: 反过来的话计数器先跑起来, 头几个
     * 更新事件时 UDE 还没开, 那几个请求直接丢。 */
    __HAL_TIM_ENABLE_DMA(&htim2, TIM_DMA_UPDATE);

    HAL_TIM_Base_Start(&htim2);
    __DMB();
    LOG_CTRL->state = LOG_ST_RUNNING;
}

/* 停采。先停源再停搬运, 顺序和启动相反。 */
static void log_stop(void)
{
    HAL_TIM_Base_Stop(&htim2);

    /* UDE 也一起关掉。HAL_TIM_Base_Stop 只清 CEN, 不碰 DIER —— 留着它虽然因为
     * 计数器停了也不会再发请求, 但下次 log_start 万一走岔了, 就会变成"定时器
     * 在跑、请求也开着、DMA 却没就绪"的那种最难查的状态。 */
    __HAL_TIM_DISABLE_DMA(&htim2, TIM_DMA_UPDATE);

    HAL_DMA_Abort(&hdma_tim2_up);
    Logic_EN(LOG_EN_OFF);
    __DMB();
    LOG_CTRL->state = LOG_ST_IDLE;
}

/* ===========================================================================
 * 主循环入口
 * ========================================================================= */
void LOG_Poll(void)
{
    const uint32_t run = LOG_CTRL->cfg_run;

    if (run != 0u && s_run_prev == 0u) {
        /* ---- 0 → 1: 开关合上 ----
         *
         * 互斥 (docs/log_plan.md §7.6): 两个屏幕各自有个开关, 切屏不改变对方的
         * 状态, 所以不能指望"用户不会同时开" —— 在这里强制。示波器的 ADC 先
         * 让路: 它和逻辑采集抢的是同一片 D2 带宽, 而且 ADC 的迟滞扫描会把
         * CM4 主循环吃掉一大块, 两个一起跑两边都跑不好。
         *
         * 反过来那一半在 OSC_CH_Poll 里: 示波器重新被打开 (CM7 置了
         * cfg_run_chN) 时 ADC 会被补启。两边只动自己那半边, 不需要谁等谁。 */
        OSC_CH_StopAdc();
        log_start();
    } else if (run == 0u && s_run_prev != 0u) {
        /* ---- 1 → 0: 开关断开, 或者 CM7 那边判定记录满 / 空闲超时 ---- */
        log_stop();
    }

    s_run_prev = (run != 0u) ? 1u : 0u;

#if LOG_DBG_HALF_PRINT
    /* ---- 半帧诊断打印 ----
     *
     * 中断那边放好一个就这里打一个。放主循环打是因为 usb_printf 阻塞 (见文件
     * 上方那段): 打的时候主循环停下, 但 HT/TC 中断照常跑, frame_seq 不受影响
     * —— 这一点和示波器不同, 它的扫描在主循环里, 停了就是真丢帧。
     *
     * 字段怎么读:
     *   seq    一直涨 = DMA 在跑。不涨 = 采集根本没起 (这时连这一行都不会有)
     *   st     CM4 侧的状态字, 正常采集中是 1 (LOG_ST_RUNNING)
     *   half   刚完成的是哪半格, 应该 0/1 交替 (0 是前半, 1 是后半)
     *   first  第一个样本的**原始** 16 位 —— 顺便能看出 PG14 (bit14) 是什么电平
     *   m      屏蔽后的 8 个通道位 (LOG_PIN_MASK = 0x3E8C), 这才是信号本身 */
    if (s_dbg_ready != 0u) {
        s_dbg_ready = 0u;
        usb_printf("[LOG] seq=%lu st=%lu half=%lu first=0x%04X m=0x%04X\r\n",
                   (unsigned long)s_dbg_seq,
                   (unsigned long)LOG_CTRL->state,
                   (unsigned long)s_dbg_half,
                   (unsigned)s_dbg_raw,
                   (unsigned)(s_dbg_raw & LOG_PIN_MASK));
    }

    /* ---- 实时引脚电平 ----
     *
     * 这一行**不经过 DMA、不经过缓冲、不经过采集**, 是 CPU 直接读 GPIOG->IDR。
     * 存在的意义是把"引脚上到底有没有信号"和"采集链路有没有把它搬过来"拆成两
     * 个独立的问题 —— 不然两边都是 0 的时候没法判断是哪一边。
     *
     * 怎么读:
     *   pin.m  跟着手上的信号变   → 引脚是通的, 往 DMA / 消费那一段查
     *   pin.m  一直是 0 不动      → 引脚上根本没看到信号。注意这里的"没看到"是
     *                               真的没看到: 同一个寄存器 DMA 也在读, 地址
     *                               已经在反汇编里核对过是 0x58021810 (= IDR)。
     *                               那就只剩三种可能:
     *                                 (a) 节点被别的东西拖着 —— 8 路开关芯片
     *                                     使能着的时候它的输出是低阻的, 会把
     *                                     外部加进来的电平顶掉
     *                                 (b) 加的不是这 8 个脚 (掩码 0x3E8C)
     *                                 (c) 没共地 / 信号幅度不够
     *   pin.idr 里的 bit14 是 PG14 (开关芯片使能) 的实际电平, 顺便确认它到底是
     *   高还是低 —— 别信代码写了什么, 信这里读出来的。
     *
     * 它和上面的半帧打印都走 usb_printf, 都是阻塞的: 一起开的时候主循环里每
     * 半秒多停一次, 正式跑之前记得把 LOG_DBG_HALF_PRINT 整个关掉。 */
    if ((HAL_GetTick() - s_dbg_idr_tick) >= LOG_DBG_IDR_MS) {
        const uint16_t idr = (uint16_t)GPIOG->IDR;
        s_dbg_idr_tick = HAL_GetTick();
        usb_printf("[LOG] pin idr=0x%04X m=0x%04X en=%u\r\n",
                   (unsigned)idr,
                   (unsigned)(idr & LOG_PIN_MASK),
                   (unsigned)((idr >> 14) & 1u));
    }
#endif
}

/* ===========================================================================
 * HT / TC: 只做记账
 *
 * 这两个是 HAL 的**全局**回调, 会和 ADC 的 DMA 撞车 (CM4 的 ADC1/ADC3 走
 * DMA1_Stream0/1)。目前 hdma_adc1 / hdma_adc3 没注册 .XferHalfCpltCallback /
 * .XferCpltCallback, 所以现在不会冲突 —— 但必须写成"先判 Instance 再干活"的
 * 形式, 否则哪天给 ADC 加上回调就是静默的状态错乱。
 *
 * 中断体是常数级的: 不扫描、不检测、不碰 D1。耗时不会因为信号密集而变长。
 * ========================================================================= */
static void log_half_done(void)
{
    const uint32_t now = DWT->CYCCNT;
    const uint32_t d   = now - s_last_tc;   /* 减法, 跨 32 位回绕自动正确 */
    s_last_tc = now;

    /* 落在"像话"范围内的才进平均: 期望 ±4 倍。太小 = 不可能 (除非时钟变了),
     * 太大 = 别的中断把这一拍挡了, 不是采样率真的掉了。 */
    if (d != 0u && d < (s_half_cyc_expect * LOG_CYC_OUTLIER_MUL)) {
        s_cyc_acc += d;
        if (d > s_cyc_max) {
            s_cyc_max = d;
        }
        if (++s_cyc_cnt >= LOG_RATE_AVG_HALVES) {
            const uint32_t avg = s_cyc_acc / s_cyc_cnt;

            LOG_CTRL->diag_cycles   = avg;
            LOG_CTRL->diag_scan_max = s_cyc_max;

            /* fs = 每半帧样本数 / 半帧周期 = DWT 时钟 × 每半帧样本数 / 实测周
             * 期。用实测周期, 不用标称周期 —— DMA 漏请求时实际速率低于标称值,
             * 而屏幕上的时间轴必须是真时间。 */
            LOG_CTRL->fs_hz = (avg != 0u)
                ? (uint32_t)(((uint64_t)SystemCoreClock * LOG_SAMPLE_HALF_SAMPS) / avg)
                : 0u;

            s_cyc_acc = 0u;
            s_cyc_max = 0u;
            s_cyc_cnt = 0u;
        }
    }

    /* 发布。frame_seq 是 CM7 的消费游标: 它比自己记的 consumed 大 1 = 正常,
     * 大过 1 = 中间那半帧已经被 DMA 绕回来覆盖了, 追丢了。 */
    LOG_CTRL->frame_seq++;
    __DMB();
    LOG_CTRL->diag_halves = LOG_CTRL->frame_seq;

#if LOG_DBG_HALF_PRINT
    /* 抽 1/LOG_DBG_DECIM 抓一个样本, 剩下的交给主循环去打 (理由见文件上方那段)。
     * 这里只有一次比较、两次内存读、四次赋值 —— 和扫描无关, 中断体仍是常数级。 */
    if ((LOG_CTRL->frame_seq % LOG_DBG_DECIM) == 0u) {
        /* 刚完成的是哪半格: 和 CM7 那边读的是同一个算法 (sharedconfig.h §6.3)。
         * frame_seq 是"已完成的半帧数", 所以刚写完的那一格编号是 frame_seq-1。 */
        const uint32_t half = (LOG_CTRL->frame_seq - 1u) & 1u;
        const uint16_t *s   = (const uint16_t *)(uintptr_t)LOG_SAMPLE_BUF_ADDR
                              + (half * LOG_SAMPLE_HALF_SAMPS);

        s_dbg_raw   = s[0];
        s_dbg_half  = half;
        s_dbg_seq   = LOG_CTRL->frame_seq;
        s_dbg_ready = 1u;
    }
#endif

    /* 通知 CM7 来扫这一格 (2 号信号量)。
     *
     * 这一下就是"每个半帧一次"的那个节拍。没有它, CM7 只能等 TouchGFX 的
     * Model::tick() 来问, 而那是 60 Hz —— 半帧只有 409.6 µs, 一个 tick 之间
     * DMA 已经把整块缓冲绕回来 40 圈了。
     *
     * take + release 就是"发通知"这个动作本身, 和示波器那边发布半帧时的收尾
     * (osc_scan_half 末尾) 是同一个写法, 照抄。它不是"抢锁": 这个信号量没有谁
     * 在等, 只是借它把 CM7 的 HSEM1_IRQn 拉起来一次。
     *
     * 中断体里做, 但只有几条指令 —— 还是那句话, 这里不扫描、不检测、不碰 D1。 */
    HAL_HSEM_FastTake(LOG_HSEM_ID);
    HAL_HSEM_Release(LOG_HSEM_ID, 0u);
}

/* ---------------------------------------------------------------------------
 * 这两个函数是怎么被叫到的 —— 踩过两个坑, 都写在这儿
 *
 * ── 一、不能用 HAL_DMA_XferHalfCpltCallback / HAL_DMA_ConvCpltCallback ──
 *
 * 本工程这份 HAL 里, 那几个全局弱回调**已经没有任何人调用**: 翻遍
 * stm32h7xx_hal_dma.c, HAL_DMA_IRQHandler 只认挂在句柄上的
 * hdma->XferCpltCallback / hdma->XferHalfCpltCallback。定义一个同名全局函数
 * 照样能编过、能链接, 但永远不被调用, 且不报任何错 —— 是彻头彻尾的死代码。
 *
 * ── 二、HT 中断是"HAL_DMA_Start_IT 看回调非空才打开"的 ──
 *
 * HAL_DMA_Start_IT 里:
 *     MODIFY_REG(CR, (DMA_IT_TC|DMA_IT_TE|DMA_IT_DME|DMA_IT_HT),
 *                    (DMA_IT_TC|DMA_IT_TE|DMA_IT_DME));
 *     if (hdma->XferHalfCpltCallback != NULL) CR |= DMA_IT_HT;
 *
 * 也就是说**不注册 XferHalfCpltCallback, 半传输中断根本不会被使能** —— 只剩
 * TC, 每 819.2 µs 才醒一次, 一半的缓冲永远没被看过。而且它同样不报错。
 *
 * 所以序必须在 HAL_DMA_Start_IT **之前**注册 (见 log_start)。
 * ------------------------------------------------------------------------- */
static void log_dma_half(DMA_HandleTypeDef *h)
{
    if (h->Instance != DMA2_Stream0) return;
    log_half_done();          /* 前半帧 [0, 8192) 个样本 完成 */
}

static void log_dma_full(DMA_HandleTypeDef *h)
{
    if (h->Instance != DMA2_Stream0) return;
    log_half_done();          /* 后半帧 [8192, 16384) 个样本 完成 */
}

/* ===========================================================================
 * 初始化
 * ========================================================================= */

/* 把 DWT 的周期计数器打开。
 *
 * oscillograph.c 里有一份功能一样的 dwt_init(), 但那个是 static、而且只在
 * OSC_CH_Init() 里调 —— 那份初始化在 main.c 里是注释掉的 (示波器这会儿不跑),
 * 于是整个 CM4 上 DWT->CYCCNT 一直是 0。后果不是崩溃, 是"静默地量不出东西":
 * diag_cycles 恒等于 0, fs_hz 被 0 除的保护挡成 0, 判据 2 (半帧周期) 和判据 3
 * (实测采样率) 就永远是 0 而不是测出来的值。
 *
 * 所以逻辑分析仪这边自己开一份, 不依赖示波器那边init 没 init。
 * 两个核各有一个 DWT, 这是 CM4 的那个, 和 CM7 互不影响。重入无害 (就是置位)。 */
static void log_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
}

void LOG_Init(void)
{
    log_addr_selfcheck();
    log_dwt_init();

    LOG_CTRL->cfg_run        = 0u;
    LOG_CTRL->cfg_sample_arr = LOG_SAMPLE_ARR_DEFAULT;
    LOG_CTRL->cmd_seq        = 0u;

    LOG_CTRL->frame_seq     = 0u;
    LOG_CTRL->state         = LOG_ST_IDLE;
    LOG_CTRL->fs_hz         = 0u;
    LOG_CTRL->diag_halves   = 0u;
    LOG_CTRL->diag_cycles   = 0u;
    LOG_CTRL->diag_scan_max = 0u;
    __DMB();

    s_run_prev     = 0u;
#if LOG_DBG_HALF_PRINT
    s_dbg_ready    = 0u;
    s_dbg_idr_tick = HAL_GetTick();
#endif
    s_last_tc      = DWT->CYCCNT;
    /* 和 log_start() 里同一个换算, 只是先用默认 ARR 填一个初值 —— 免得第一次
     * 采集在算出来之前, 异常值过滤的上下界是个未初始化的数。 */
    s_half_cyc_expect = (uint32_t)(((uint64_t)LOG_SAMPLE_HALF_SAMPS
                                    * (uint64_t)SystemCoreClock
                                    * (uint64_t)(LOG_SAMPLE_ARR_DEFAULT + 1u))
                                   / (uint64_t)LOG_TIM2_CLK_HZ);

    /* 关掉 8 路开关 —— 不采的时候不让输入通路一直开着, 免得 PG2..PG13 上挂着
     * 的信号白白地在开关芯片里换来换去。
     *
     * 注意 MX_GPIO_Init 里已经把它写成了 RESET (低), 而低是"开" —— 所以开机
     * 到 LOG_Init() 跑完这一小段里前端是开着的。无害 (没人采), 但要说清楚,
     * 免得哪天看到"没开采集电流却偏高"往别处找。 */
    Logic_EN(LOG_EN_OFF);
}
