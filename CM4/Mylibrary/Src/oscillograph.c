#include "oscillograph.h"
#include "stm32h7xx_hal_adc_ex.h"
#include "stm32h7xx_hal_adc.h"
#include "usb_printf.h"
#include <stdint.h>

/* .dma_buffer 段从 RAM_D2 起点 (0x30030000) 开始按声明顺序摆放,
 * 所以这三个依次落在 0x30030000 / 0x30034000 / 0x30036000。
 * 地址契约见 sharedconfig.h。 */
uint32_t osc_ch1_buf[OSC_CH1_BUF_WORDS] __attribute__((section(".dma_buffer"), aligned(32)));
uint16_t osc_ch2_buf[OSC_CH2_BUF_WORDS] __attribute__((section(".dma_buffer"), aligned(32)));
volatile osc_ctrl_t osc_ctrl __attribute__((section(".dma_buffer"), aligned(32)));

/* ===========================================================================
 * 半帧完成计数: 中断里只 +1, 真正的扫描放到主循环 (OSC_CH_Poll)
 *
 * 为什么不留在中断里扫: 两个 ADC 走 DMA1_Stream0/1, 抢占优先级都是 0
 * (CM4/Core/Src/dma.c), 在里面扫 2048 个 word 会把 CM4 上所有别的中断
 * (TIM6 tick、USB) 一次挡住 200 µs 量级。
 *
 * 代价: 主循环必须在半个帧周期(655 µs)内调到 OSC_CH_Poll(), 否则那块半帧
 * 会被 DMA 绕回来覆盖。跟不上的次数记进 diag_miss, 不静默丢数据。
 * ========================================================================= */
static volatile uint32_t s_completed_ch1;   /* HT/TC 中断里 ++ */
static volatile uint32_t s_completed_ch2;
static uint32_t s_processed_ch1;            /* 主循环已经处理到第几次完成 */
static uint32_t s_processed_ch2;

/**
*   @brief DWT 周期计数器, 作为独立于 HAL tick 的时间基准
*
*/
static void dwt_init(void){
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/**
*   @brief osc控制块配置初始化
*/
static void ctrl_defaults(void)
{
    osc_ctrl.magic           = 0u;
    osc_ctrl.seq_ch1         = 0u;
    osc_ctrl.seq_ch2         = 0u;
    osc_ctrl.trig_pos_ch1    = OSC_TRIG_NONE;
    osc_ctrl.trig_pos_ch2    = OSC_TRIG_NONE;
    osc_ctrl.trig_flags      = 0u;

    /* 上电默认走自适应电平, 免得 CM7 还得先猜一个电平 */
    osc_ctrl.cfg_trig_level_ch1 = OSC_LEVEL_AUTO;
    osc_ctrl.cfg_trig_level_ch2 = OSC_LEVEL_AUTO;
    osc_ctrl.cfg_trig_hyst      = 16u;
    osc_ctrl.cfg_trig_edge      = 0u;      /* 上升沿 */
    osc_ctrl.cfg_trig_enable    = 0u;
    osc_ctrl.cfg_run            = 0u;

    osc_ctrl.diag_ovr_ch1    = 0u;
    osc_ctrl.diag_ovr_ch2    = 0u;
    osc_ctrl.diag_miss       = 0u;
    osc_ctrl.diag_rate_ch1   = 0u;
    osc_ctrl.diag_rate_ch2   = 0u;
    osc_ctrl.diag_min_ch1    = 0u;
    osc_ctrl.diag_max_ch1    = 0u;
    osc_ctrl.diag_level_ch1  = 0u;
    osc_ctrl.diag_min_ch2    = 0u;
    osc_ctrl.diag_max_ch2    = 0u;
    osc_ctrl.diag_level_ch2  = 0u;
    osc_ctrl.reserved[0]     = 0u;
}

/**
*   @brief 初始化示波器ADC通道
*
*/
void OSC_CH_Init(void){
    /* --- 地址契约自检。对不上说明 .dma_buffer 的布局被改动了,
     *        CM7 会去读错误的地址, 必须立刻停下来而不是跑出一堆脏数据。 --- */
    if ((uintptr_t)osc_ch1_buf != (uintptr_t)OSC_CH1_BUF_ADDR) Error_Handler();
    if ((uintptr_t)osc_ch2_buf != (uintptr_t)OSC_CH2_BUF_ADDR) Error_Handler();
    if ((uintptr_t)&osc_ctrl   != (uintptr_t)OSC_CTRL_ADDR)    Error_Handler();
    ctrl_defaults();
    __DMB();
    /* ADC校准 */
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();
    if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();
    if (HAL_ADCEx_Calibration_Start(&hadc3, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();

    dwt_init();

    __DMB();
    osc_ctrl.magic = OSC_SHARED_MAGIC;
    __DMB();

    HAL_ADCEx_MultiModeStart_DMA(&hadc1, osc_ch1_buf, OSC_CH1_BUF_WORDS);
    HAL_ADC_Start_DMA(&hadc3, (uint32_t *)osc_ch2_buf, OSC_CH2_BUF_WORDS);

    __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR);
    __HAL_ADC_CLEAR_FLAG(&hadc3, ADC_FLAG_OVR);
}


/* ===========================================================================
 * 诊断
 * ========================================================================= */
#define OSC_CM4_HZ        240000000u
#define RATE_AVG_HALVES   16u

static uint32_t s_cyc_ch1, s_last_ch1;
static uint32_t s_cyc_ch2, s_last_ch2;

void OSC_CH_DiagPrint(void)
{
    usb_printf("[OSC] addr ch1=0x%08lX ch2=0x%08lX ctrl=0x%08lX\r\n",
               (unsigned long)(uintptr_t)osc_ch1_buf,
               (unsigned long)(uintptr_t)osc_ch2_buf,
               (unsigned long)(uintptr_t)&osc_ctrl);
    usb_printf("      magic=0x%08lX seq1=%lu seq2=%lu\r\n",
               (unsigned long)osc_ctrl.magic,
               (unsigned long)osc_ctrl.seq_ch1,
               (unsigned long)osc_ctrl.seq_ch2);
    usb_printf("      rate1=%lu Sa/s rate2=%lu Sa/s\r\n",
               (unsigned long)osc_ctrl.diag_rate_ch1,
               (unsigned long)osc_ctrl.diag_rate_ch2);
    usb_printf("      ovr1=%lu ovr2=%lu miss=%lu\r\n",
               (unsigned long)osc_ctrl.diag_ovr_ch1,
               (unsigned long)osc_ctrl.diag_ovr_ch2,
               (unsigned long)osc_ctrl.diag_miss);
    usb_printf("      trig pos1=%lu pos2=%lu flags=0x%08lX\r\n",
               (unsigned long)osc_ctrl.trig_pos_ch1,
               (unsigned long)osc_ctrl.trig_pos_ch2,
               (unsigned long)osc_ctrl.trig_flags);
    usb_printf("      lvl1=%lu [%lu..%lu] lvl2=%lu [%lu..%lu]\r\n",
               (unsigned long)osc_ctrl.diag_level_ch1,
               (unsigned long)osc_ctrl.diag_min_ch1,
               (unsigned long)osc_ctrl.diag_max_ch1,
               (unsigned long)osc_ctrl.diag_level_ch2,
               (unsigned long)osc_ctrl.diag_min_ch2,
               (unsigned long)osc_ctrl.diag_max_ch2);
    usb_printf("      hyst=%lu edge=%lu auto1=%lu auto2=%lu\r\n",
               (unsigned long)osc_ctrl.cfg_trig_hyst,
               (unsigned long)osc_ctrl.cfg_trig_edge,
               (unsigned long)(osc_ctrl.cfg_trig_level_ch1 == OSC_LEVEL_AUTO),
               (unsigned long)(osc_ctrl.cfg_trig_level_ch2 == OSC_LEVEL_AUTO));
}

/**
*   @brief 采样率估算
*   @note  在主循环里按"每扫一个半帧调一次"的节奏调用。主循环跟得上时它
*          就是真实采样率; 跟不上时读数会跟着掉, 那正好反映有效吞吐。
*/
static void rate_update(uint32_t *last, uint32_t *cnt, uint32_t samples_per_half, volatile uint32_t *out)
{
    uint32_t now = DWT->CYCCNT;
    uint32_t d   = now - *last;
    *last = now;

    if (d == 0u || d > OSC_CM4_HZ) return;      /* 首拍, 或间隔异常 */

    if (++(*cnt) < RATE_AVG_HALVES) return;
    *cnt = 0u;

    *out = (uint32_t)(((uint64_t)samples_per_half * RATE_AVG_HALVES * OSC_CM4_HZ) / (uint64_t)d);
}

/* ===========================================================================
 * 触发电平: 迟滞窗口 + 自适应电平
 *
 * 语义 (和 libtiepie-hw / R&S 的迟滞定义一致): 迟滞只压在"武装"侧,
 * 触发阈值就是 level 本身。
 *     上升沿: 先 v <= level - hyst (确认起点在低位) → 再 v >= level 触发
 *     下降沿: 先 v >= level + hyst                 → 再 v <= level 触发
 *
 * 如果连触发也要求越过 level + hyst, 报出的触发点会整体偏后 hyst 个采样,
 * 慢信号上肉眼可见。抗噪完全来自"必须先站到反侧"这一步。
 *
 * 自适应电平 (cfg_trig_level_* == OSC_LEVEL_AUTO 时由 CM4 算):
 *   目标值取幅度中点 (min+max)/2。不用算术平均: 中点与占空比无关, 而且
 *   正弦在中点处最陡, 触发时刻的抖动 ≈ 噪声/斜率 最小。
 *
 * 纯直流、或峰峰值小于 2*hyst 的信号会退化成噪声触发 —— 本示波器限定
 * 只测周期变化信号, 这是该前提的代价。直流输入下 trig_flags 恒为 0,
 * CM7 据此走自由运行显示即可, 不需要额外的直流判定字段。
 * ========================================================================= */

#define OSC_LEVEL_WIN_SHIFT 4u    /* 统计窗口 = 16 个半帧 ≈ 10.5 ms */
#define OSC_NO_TRIG_WINS    2u    /* 连续这么多窗口没触发 → 电平定歪了, 重算 */
#define OSC_HYST_MIN        2u    /* 迟滞半宽下限, 防止退化成"相邻两拍" */

typedef struct {
    const volatile uint32_t *cfg_level;  /* 指向 osc_ctrl.cfg_trig_level_ch1 / _ch2 */
    uint32_t level;             /* 当前生效电平 */
    uint32_t hyst;              /* 迟滞半宽 */
    uint32_t edge;              /* 0 = 上升沿, 1 = 下降沿 */
    uint32_t arm;               /* 武装阈值 (在触发方向的反侧) */

    uint32_t armed;             /* 迟滞状态: 边沿起点已确认 */
    uint32_t win_min;           /* ↓ 自适应电平的统计窗口 */
    uint32_t win_max;
    uint32_t wins;              /* 本窗口已收的半帧数 */
    uint32_t win_trig;          /* 本窗口是否触发过 */
    uint32_t no_trig;           /* 连续无触发的窗口数 */
} osc_trig_t;

/* no_trig 初值 = OSC_NO_TRIG_WINS, 让第一个窗口无条件先定一次电平 */
static osc_trig_t s_trig_ch1 = { .cfg_level = &osc_ctrl.cfg_trig_level_ch1,
                                 .level = 2047u, .win_min = 4095u,
                                 .no_trig = OSC_NO_TRIG_WINS };
static osc_trig_t s_trig_ch2 = { .cfg_level = &osc_ctrl.cfg_trig_level_ch2,
                                 .level = 2047u, .win_min = 4095u,
                                 .no_trig = OSC_NO_TRIG_WINS };

/**
*   @brief 扫描前同步 CM7 下发的配置, 并算出武装阈值
*   @note  人为改了电平/迟滞/边沿 → 迟滞状态作废。否则会拿旧阈值武装出来的
*          状态去比新阈值, 报出一个 pos=0 的假触发点。
*/
static void trig_sync(osc_trig_t *t)
{
    uint32_t cfg   = *(t->cfg_level);
    uint32_t level = (cfg == OSC_LEVEL_AUTO) ? t->level : cfg;
    uint32_t hyst  = osc_ctrl.cfg_trig_hyst;
    uint32_t edge  = osc_ctrl.cfg_trig_edge & 1u;

    if (hyst < OSC_HYST_MIN) hyst = OSC_HYST_MIN;

    if (level != t->level || hyst != t->hyst || edge != t->edge) t->armed = 0u;

    t->level = level;
    t->hyst  = hyst;
    t->edge  = edge;

    /* 武装阈值在触发方向的反侧: 上升沿要压到 level-hyst 以下, 下降沿反之 */
    if (edge == 0u) {
        t->arm = (level > hyst) ? (level - hyst) : 0u;
    } else {
        uint32_t hi = level + hyst;
        t->arm = (hi > 4095u) ? 4095u : hi;
    }
}

/**
*   @brief 送入一个采样: 更新窗口统计 + 跑迟滞状态机
*   @return 1 = 这一拍就是触发点
*/
static uint32_t trig_feed(osc_trig_t *t, uint32_t v)
{
    /* 窗口统计: 每个采样都要算, 与是否触发无关 */
    if (v < t->win_min) t->win_min = v;
    if (v > t->win_max) t->win_max = v;

    if (!t->armed) {
        /* 先站到触发方向的反侧; 这一拍本身不算触发 */
        if (t->edge == 0u ? (v <= t->arm) : (v >= t->arm)) t->armed = 1u;
        return 0u;
    }

    if (t->edge == 0u ? (v >= t->level) : (v <= t->level)) {
        t->armed = 0u;                  /* 一次边沿只报一次 */
        return 1u;
    }
    return 0u;
}

/* 两个通道都用"锁存位置但不提前 return"的写法: 提前返回会让窗口统计只覆盖到
 * 触发点, min/max 就不完整, 自适应电平会失准。 */

static uint32_t trig_scan_ch2(osc_trig_t *t, const uint16_t *s, uint32_t n)
{
    uint32_t pos = OSC_TRIG_NONE;
    for (uint32_t i = 0; i < n; i++) {
        if (trig_feed(t, s[i]) && pos == OSC_TRIG_NONE) pos = i;
    }
    return pos;                         /* 样本下标 (CH2: 1 word = 1 个样本) */
}

static uint32_t trig_scan_ch1(osc_trig_t *t, const uint32_t *w, uint32_t n)
{
    uint32_t pos = OSC_TRIG_NONE;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t word = w[i];
        if (trig_feed(t, word & 0xFFFFu) && pos == OSC_TRIG_NONE) pos = i;  /* ADC1, 低 16 位, 时间在前 */
        if (trig_feed(t, word >> 16)     && pos == OSC_TRIG_NONE) pos = i;  /* ADC2, 高 16 位, 时间在后 */
    }
    return pos;                         /* word 下标, 不是样本下标 */
}

/**
*   @brief 一个半帧扫完: 收窗口统计, 必要时重算自适应电平, 发布诊断
*   @param ch 0 = CH1 组, 1 = CH2 组 (只用来决定诊断字段往哪写)
*/
static void trig_update(osc_trig_t *t, uint32_t ch)
{
    if (++t->wins < (1u << OSC_LEVEL_WIN_SHIFT)) return;

    /* ---- 一个统计窗口结束 ---- */
    uint32_t wmin = t->win_min;
    uint32_t wmax = t->win_max;

    t->wins     = 0u;
    t->win_min  = 4095u;
    t->win_max  = 0u;

    if (t->win_trig)                t->no_trig = 0u;
    else if (t->no_trig < 0xFFu)    t->no_trig++;
    t->win_trig = 0u;

    /* ---- 自适应电平 ----
     * 触发正常时电平冻住: 电平一直动触发点就跟着动, 画面会左右游走。
     * 只有"第一个窗口"或"连续 OSC_NO_TRIG_WINS 个窗口都触发不上"才动。*/
    if (*(t->cfg_level) == OSC_LEVEL_AUTO && t->no_trig >= OSC_NO_TRIG_WINS) {
        t->level = wmin + (wmax - wmin) / 2u;   /* 幅度中点 */
        t->armed = 0u;                          /* 电平变了 → 迟滞状态作废 */
    }

    /* ---- 发布诊断 ---- */
    if (ch == 0u) {
        osc_ctrl.diag_min_ch1   = wmin;
        osc_ctrl.diag_max_ch1   = wmax;
        osc_ctrl.diag_level_ch1 = t->level;
    } else {
        osc_ctrl.diag_min_ch2   = wmin;
        osc_ctrl.diag_max_ch2   = wmax;
        osc_ctrl.diag_level_ch2 = t->level;
    }
}

/**
*   @brief 扫描一个半帧并发布
*   @param ch   0 = CH1 组, 1 = CH2 组
*   @param half 半帧下标 (0 或 1), 是"刚完成、还没被 DMA 绕回来覆盖"的那一半
*/
static void osc_scan_half(uint32_t ch, uint32_t half)
{
    osc_trig_t *t   = (ch == 0u) ? &s_trig_ch1 : &s_trig_ch2;
    uint32_t    pos = OSC_TRIG_NONE;

    if (osc_ctrl.cfg_trig_enable) {
        trig_sync(t);
        if (ch == 0u) {
            pos = trig_scan_ch1(t, &osc_ch1_buf[half * OSC_HALF_WORDS], OSC_HALF_WORDS);
        } else {
            pos = trig_scan_ch2(t, &osc_ch2_buf[half * OSC_HALF_WORDS], OSC_HALF_WORDS);
        }
        if (pos != OSC_TRIG_NONE) t->win_trig = 1u;
        trig_update(t, ch);
    } else {
        /* 关掉触发时清掉这一轮的暂态, 免得重开时拿旧状态报出 pos=0 的假触发,
         * 或者拿上一次的陈旧 min/max 去定电平。level/no_trig 故意不清: 信号
         * 通常没变, 留着上一轮定好的电平可以省掉重新定一次。 */
        t->armed    = 0u;
        t->wins     = 0u;
        t->win_trig = 0u;
        t->win_min  = 4095u;
        t->win_max  = 0u;
    }

    if (ch == 0u) {
        osc_ctrl.trig_pos_ch1 = pos;
        if (pos != OSC_TRIG_NONE) osc_ctrl.trig_flags |=  1u;
        else                      osc_ctrl.trig_flags &= ~1u;
        rate_update(&s_last_ch1, &s_cyc_ch1, OSC_HALF_WORDS * 2u, &osc_ctrl.diag_rate_ch1);
        __DMB();
        osc_ctrl.seq_ch1++;
        __DMB();
    } else {
        osc_ctrl.trig_pos_ch2 = pos;
        if (pos != OSC_TRIG_NONE) osc_ctrl.trig_flags |=  2u;
        else                      osc_ctrl.trig_flags &= ~2u;
        rate_update(&s_last_ch2, &s_cyc_ch2, OSC_HALF_WORDS,     &osc_ctrl.diag_rate_ch2);
        __DMB();
        osc_ctrl.seq_ch2++;
        __DMB();
    }
    /* 半帧已经完整落在缓冲里了, 先把数据可见性保住, 再通知 CM7 来搬 */
    HAL_HSEM_FastTake(OSC_HSEM_ID);
    HAL_HSEM_Release(OSC_HSEM_ID, 0u);
}

/**
*   @brief 主循环里反复调用: 把中断记下的"刚完成的半帧"扫掉
*   @note  必须在半个帧周期 (2048 word × 320 ns = 655 µs) 内被调到。
*          主循环里不要放阻塞调用 —— USB 打印、HAL_Delay 都会让它跟不上,
*          跟不上的半帧记进 diag_miss, 不静默丢。
*/
void OSC_CH_Poll(void)
{
    if (!osc_ctrl.cfg_run) {
        /* 停止采集: 把游标对齐, 免得恢复时把积压的旧半帧一次性补扫 */
        s_processed_ch1 = s_completed_ch1;
        s_processed_ch2 = s_completed_ch2;
        return;
    }

    /* 落后 ≥2 个半帧 = 想扫的那半已经被 DMA 绕回来覆盖了, 只能丢;
     * 只处理最新那半帧, diag_miss 只多不少(偏保守, 不静默) */
    uint32_t done1 = s_completed_ch1;                   /* 32 位读写原子, 取一次快照 */
    uint32_t lag1  = done1 - s_processed_ch1;
    if (lag1 != 0u) {
        if (lag1 > 1u) osc_ctrl.diag_miss += lag1 - 1u;
        s_processed_ch1 = done1;
        osc_scan_half(0u, (done1 - 1u) & 1u);
    }

    uint32_t done2 = s_completed_ch2;
    uint32_t lag2  = done2 - s_processed_ch2;
    if (lag2 != 0u) {
        if (lag2 > 1u) osc_ctrl.diag_miss += lag2 - 1u;
        s_processed_ch2 = done2;
        osc_scan_half(1u, (done2 - 1u) & 1u);
    }
}

/* 中断里只记数, 扫描交给 OSC_CH_Poll —— 见文件开头那段说明 */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)      s_completed_ch1++;   /* 前半帧 [0,2048) 完成 */
    else if (hadc->Instance == ADC3) s_completed_ch2++;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)      s_completed_ch1++;   /* 后半帧 [2048,4096) 完成 */
    else if (hadc->Instance == ADC3) s_completed_ch2++;
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)      osc_ctrl.diag_ovr_ch1++;
    else if (hadc->Instance == ADC3) osc_ctrl.diag_ovr_ch2++;
}
