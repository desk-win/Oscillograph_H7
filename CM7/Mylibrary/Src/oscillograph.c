/* ===========================================================================
 * CM7 侧深存储搬运 —— 用 MDMA 把 CM4 发布的半帧从 D2 SRAM 搬进 SDRAM 环形缓冲
 *
 * 地址表、环结构、对外接口都在 oscillograph.h 里。这里只有搬运逻辑。
 *
 * ========================================================================= */

#include "oscillograph.h"
#include "mdma.h"
#include "osc_probe.h"   /* OSC_ISR_Enter / Exit —— 只用于中断计时, 调试完一起删 */
#include <stdint.h>

/* ---- 对外可见的诊断计数 (结构见 oscillograph.h) ---- */
m7_mdma_diag_t m7_diag_ch1;
m7_mdma_diag_t m7_diag_ch2;

/* ---- 每个通道各自的进度 ---- */

/* 上一次已经搬走的发布序号。
 * 初值必须是 0, 且要和 CM4 那边 seq_ch1 / seq_ch2 的清零值一致 —— 因为
 * 判据是 "seq != last_seq": seq 还是 0 时两者相等, 直接返回, 于是
 * slot = seq - 1 永远不会下溢成 0xFFFFFFFF。 */
static volatile uint32_t s_ch1_last_seq;
static volatile uint32_t s_ch2_last_seq;

/* 通道忙标志: Start 函数置起, MDMA 中断回调清掉 */
static volatile uint8_t  s_ch1_busy;
static volatile uint8_t  s_ch2_busy;

/* ---------------------------------------------------------------------------
 * MDMA 中断回调
 *
 * HAL 的顺序是: 先把 State 复位成 READY, 再调 XferCpltCallback —— 所以完成
 * 回调里可以立刻装填下一发, 不会撞上 HAL_BUSY。
 *
 * 出错时 HAL 不会自己报错, 它把原因写进 hmdma->ErrorCode, 关掉通道, 然后调
 * XferErrorCallback。所以 ErrorCode 必须在这里读 —— CESR 里的细节会被清掉,
 * 事后再去看已经是空的。
 *
 * 别去注册 XferBlockCpltCallback —— 那会让 HAL 使能 BT 中断, 这里用不上。
 * ------------------------------------------------------------------------- */
static void ch1_cplt (MDMA_HandleTypeDef *h) { (void)h; s_ch1_busy = 0u; }
static void ch2_cplt (MDMA_HandleTypeDef *h) { (void)h; s_ch2_busy = 0u; }

static void ch1_error(MDMA_HandleTypeDef *h)
{
    m7_diag_ch1.err_code = h->ErrorCode;
    m7_diag_ch1.err++;
    s_ch1_busy = 0u;
}

static void ch2_error(MDMA_HandleTypeDef *h)
{
    m7_diag_ch2.err_code = h->ErrorCode;
    m7_diag_ch2.err++;
    s_ch2_busy = 0u;
}

/* ===========================================================================
 * 初始化 + HSEM 通知驱动
 * ========================================================================= */

/* 中断进来过几次。bring-up 时第一个要看的数。 */
volatile uint32_t m7_diag_irq;

void M7_OSC_Init(void)
{
    hmdma_mdma_channel0_sw_0.XferCpltCallback  = ch1_cplt;
    hmdma_mdma_channel0_sw_0.XferErrorCallback = ch1_error;
    hmdma_mdma_channel1_sw_0.XferCpltCallback  = ch2_cplt;
    hmdma_mdma_channel1_sw_0.XferErrorCallback = ch2_error;

    /* 优先级 5: 比 MDMA_IRQn (0) 低, 让完成回调能插进来及时清忙标志。
     * 别调高 —— 那样 MDMA 中断会被拖住, s_chN_busy 清不掉, 下一发就误判成
     * overrun。 */
    HAL_NVIC_SetPriority(HSEM1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(HSEM1_IRQn);

    /* 武装信号量 1 的释放通知。CM4 那边在发布完半帧后 Release 1 号,
     * 这一句就是让那个释放变成 CM7 的 HSEM1_IRQn。
     *
     * 只开 1 号: 0 号上还压着 CM7 自己在 boot 时 Release 的那一下
     * (main.c Boot_Mode_Sequence_2), 开进来也没用。 */
    HAL_HSEM_ActivateNotification(OSC_HSEM_MASK);
}

/* 中断入口。启动文件里的向量叫 HSEM1_IRQHandler (CM7 的 Interrupt 0 线),
 * 名字不能改 —— 改了就静默落回 Default_Handler 死循环。 */
void HSEM1_IRQHandler(void)
{
    OSC_ISR_Enter();         /* 计时用, 见 osc_probe.h */
    HAL_HSEM_IRQHandler();   /* 读 C1MISR → 清使能位 → 清标志 → 回调 */
    OSC_ISR_Exit();
}

/* HAL 在 IRQHandler 里已经把触发的那一位从 C1IER 清掉了
 * (stm32h7xx_hal_hsem.c:386), 不在这里重新武装的话, 只会进这一次中断。 */
void HAL_HSEM_FreeCallback(uint32_t SemMask)
{
    if ((SemMask & OSC_HSEM_MASK) == 0u) return;   /* 不是我们的信号量 */

    /* 先武装再搬运: 反过来的话, 搬运那几微秒里来的发布会被漏掉, 那个半帧
     * 就成了环上的空洞。 */
    HAL_HSEM_ActivateNotification(OSC_HSEM_MASK);

    m7_diag_irq++;
    M7_OSC_Poll();

    /* M7_OSC_Poll 只碰静态变量和 MDMA 寄存器, 不调任何 RTOS 接口 —— 所以
     * 这个中断不占 FreeRTOS 的临界区, 也不需要 portYIELD_FROM_ISR。 */
}

/* ===========================================================================
 * CH1 —— 源 osc_ch1_buf (D2 SRAM, uint32, 乒乓 2 格), 一格 8192 B
 * ========================================================================= */
void M7_OSC_CH1Start(void)
{
    uint32_t seq = OSC_CTRL->seq_ch1;

    /* 1) 没有新半帧 —— 绝大多数调用走到这里就返回了 */
    if (seq == s_ch1_last_seq) return;

    /* 2) 上一发还在飞。丢弃本次, 记一笔; 那个半帧会成为环上的空洞。
     *    这里故意不推进 s_ch1_last_seq, 于是下次进来 seq 差得更大, 会直接
     *    跳到最新那个半帧重试 —— 自愈, 不用额外处理。 */
    if (s_ch1_busy) { m7_diag_ch1.overrun++; return; }

    /* 3) seq 跳跃的幅度。>1 说明两次调用之间 CM4 发布了不止一个半帧, 中间
     *    那些搬不到了 (D2 只有 2 格, 早的已经被 DMA 覆盖) */
    uint32_t step = seq - s_ch1_last_seq;    /* >1 = 我们漏了帧 */

    /* 4) D2 侧是乒乓: 第 n 号发布对应半帧 (n-1)&1。
     *
     *    这不是"猜" —— CM4 那边 seq 跟着 DMA 完成计数递增 (见 CM4 的
     *    osc_scan_half), 它扫的也正好是 (done-1)&1, 两者同奇偶是硬保证。
     *    换句话: 这里搬到的一定是"刚扫过、没有被 DMA 覆盖"的那一格。
     *
     *    以前这条靠"每个半帧恰好发一次"间接成立, 只要 CM4 漏掉过半帧
     *    (主循环跟不上, 或者每秒一次的阻塞式串口打印) 就永久错开一格 ——
     *    搬到 DMA 正在写的那一格 (半新半旧), 而且触发点在环上的位置整体偏
     *    2048 word = 655 µs = 1/4 屏, 每翻一次奇偶横跳一次。
     *
     *    SDRAM 侧是环形: 同一个 n 对应槽号 (n-1) —— 因为每个半帧占死
     *    655.36 µs, 槽号本身就是时间戳。 */
    uint32_t half = (seq - 1u) & 1u;
    uint32_t slot = seq - 1u;                /* 在 SDRAM 里面要写入的位置 */

    uint32_t src = OSC_CH1_BUF_ADDR + half * OSC_CH1_HALF_BYTES;
    uint32_t dst = OSC_SDRAM_CH1_BASE
                 + (slot & OSC_SDRAM_HALF_MASK) * OSC_CH1_HALF_BYTES;

    /* 5) 启动。第 4 参是"字节数"不是 word 数; 第 5 参是块数(不是超时), 单块传 1 */
    s_ch1_busy = 1u;
    if (HAL_MDMA_Start_IT(&hmdma_mdma_channel0_sw_0, src, dst,
                          OSC_CH1_HALF_BYTES, 1u) != HAL_OK) {
        s_ch1_busy = 0u;
        m7_diag_ch1.err++;       /* 通道没进 READY, 见文件头注释 */
        return;                  /* 不推进 last_seq, 下次重试 */
    }

    s_ch1_last_seq  = seq;
    m7_diag_ch1.pushed++;
    m7_diag_ch1.missed += step - 1u;   /* step-1 个半帧成了空洞 (正常时是 0) */
}

/* ===========================================================================
 * CH2 —— 源 osc_ch2_buf (D2 SRAM, uint16, 乒乓 2 格), 一格 4096 B
 *
 * 与 CH1 逐行对应, 只有地址、长度和序号字段不同 —— 对照着看即可。
 * 两个通道各占一个 MDMA 通道, 互不等待, 天然并行。
 * ========================================================================= */
void M7_OSC_CH2Start(void)
{
    uint32_t seq = OSC_CTRL->seq_ch2;

    if (seq == s_ch2_last_seq) return;
    if (s_ch2_busy) { m7_diag_ch2.overrun++; return; }

    uint32_t step = seq - s_ch2_last_seq;

    uint32_t half = (seq - 1u) & 1u;
    uint32_t slot = seq - 1u;

    uint32_t src = OSC_CH2_BUF_ADDR + half * OSC_CH2_HALF_BYTES;
    uint32_t dst = OSC_SDRAM_CH2_BASE
                 + (slot & OSC_SDRAM_HALF_MASK) * OSC_CH2_HALF_BYTES;

    s_ch2_busy = 1u;
    if (HAL_MDMA_Start_IT(&hmdma_mdma_channel1_sw_0, src, dst,
                          OSC_CH2_HALF_BYTES, 1u) != HAL_OK) {
        s_ch2_busy = 0u;
        m7_diag_ch2.err++;
        return;
    }

    s_ch2_last_seq  = seq;
    m7_diag_ch2.pushed++;
    m7_diag_ch2.missed += step - 1u;
}

/* ===========================================================================
 * 便利入口: 两个通道各推一遍
 * ========================================================================= */
/* ===========================================================================
 * 重新开始采集之前: 把搬运游标对齐到当前序号 (通道号: 0 = CH1, 1 = CH2)
 *
 * 用途见 oscillograph.h。要点是 s_chN_last_seq 平时只有 HSEM 中断在写, 这里
 * 从任务里写, 所以短暂关中断 —— 就一条赋值。
 *
 * 一次只对齐一路: 两个通道各自启停, 重启 CH1 的时候 CH2 可能正跑着, 把它
 * 一起对齐会凭空吃掉它一个半帧 (655 µs 的旧数据被拼进新波形)。
 * ========================================================================= */
void M7_OSC_SyncCh(uint32_t ch)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (ch == 0u) { s_ch1_last_seq = OSC_CTRL->seq_ch1; }
    else          { s_ch2_last_seq = OSC_CTRL->seq_ch2; }

    __set_PRIMASK(primask);
}

void M7_OSC_Poll(void)
{
    M7_OSC_CH1Start();
    M7_OSC_CH2Start();
}
