#include "oscillograph.h"
#include "stm32h7xx_hal_adc_ex.h"
#include "stm32h7xx_hal_adc.h"
#include "usb_printf.h"
#include <stdint.h>

/* 三个对象各占一个具名段, 由链接脚本按绝对地址钉死在 RAM_D2 里:
 *     0x30030000  osc_ch1_buf   (16 KB)
 *     0x30034000  osc_ch2_buf   ( 8 KB)
 *     0x30036000  osc_ctrl      (96 B)
 * 地址契约见 sharedconfig.h, OSC_CH_Init 里还会自检一道。
 *
 * 不靠"谁先声明谁在前": 那个顺序是编译器定的, 换个优化级别就可能变。以前三个
 * 共用 .dma_buffer 段就是这样, -O2 下 GCC 把 osc_ctrl 排到了最前面, 自检当场
 * Error_Handler。 */
uint32_t osc_ch1_buf[OSC_CH1_BUF_WORDS] __attribute__((section(".osc_ch1_buf"), aligned(32)));
uint16_t osc_ch2_buf[OSC_CH2_BUF_WORDS] __attribute__((section(".osc_ch2_buf"), aligned(32)));
volatile osc_ctrl_t osc_ctrl __attribute__((section(".osc_ctrl"), aligned(32)));

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

/* 上一拍 cfg_run_chN 的值。只用来抓"刚被打开"那一刻 —— 要在那时候把发布序号
 * 的奇偶对齐一次, 理由见 OSC_CH_Poll。 */
static uint8_t  s_run_ch1_prev;
static uint8_t  s_run_ch2_prev;

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
    osc_ctrl.cfg_run_ch1        = 0u;      /* 两个通道各自启停, 上电都是停 */
    osc_ctrl.cfg_run_ch2        = 0u;

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
}

/**
*   @brief 初始化示波器ADC通道
*
*/
void OSC_CH_Init(void){
    /* --- 地址契约自检。对不上说明链接脚本里的绝对地址被改了,
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

static uint32_t s_cyc_ch1, s_last_ch1, s_acc_ch1;
static uint32_t s_cyc_ch2, s_last_ch2, s_acc_ch2;

/* 每行只留四个数, 一行打完, 其余全部砍掉。
 *
 * 为什么砍: 打印是阻塞的, 每打一次必然丢一个半帧 —— 它自己在制造环上的空洞,
 * 也就是我们正在查的那个东西。原来 7 行 (两百多字符) 在 115200 下要 20 ms,
 * 那期间 DMA 绕回来一整圈。行数越少, 仪器本身对被测对象的扰动越小。
 *
 * 留下的四个:
 *   seq1  换算 miss 的速率要用 (miss 的绝对值和增速是两回事)
 *   miss  扫描来不及的次数 —— 这是"环上有没有洞"的直接来源
 *   pos1 / flags
 *         触发点落点和触发标志。只有接了信号才有意义; 空载时恒为
 *         0 / OSC_TRIG_NONE, 留着是为了接上信号的那一刻不用重新烧程序。
 *
 * 砍掉的:
 *   addr/magic     链接脚本钉死后是常数, 只在开机那一次值得看
 *   rate1/rate2    公式原来错了 16 倍 (已修, 见 rate_update), 而且可由
 *                  seq1 的斜率算出来, 不必单占一行
 *   ovr1/ovr2      ADC 溢出。到目前为止恒为 0 —— 真出现时再加回来,
 *                  它和 miss 是两种不同的失效 (一个是没排上队, 一个是
 *                  DMA 没搬完就又被触发)
 *   lvl/min/max    自适应电平的中间量, 只在专门调电平的时候要
 *   hyst/edge/auto 配置回读, 看一眼 ctrl_defaults 就知道
 *
 * 位置没变: 仍在主循环里每秒一次。 */
void OSC_CH_DiagPrint(void)
{
    usb_printf("[OSC] seq1=%lu miss=%lu pos1=%lu flags=0x%lX\r\n",
               (unsigned long)osc_ctrl.seq_ch1,
               (unsigned long)osc_ctrl.diag_miss,
               (unsigned long)osc_ctrl.trig_pos_ch1,
               (unsigned long)osc_ctrl.trig_flags);
}

/**
*   @brief 采样率估算
*   @note  在主循环里按"每扫一个半帧调一次"的节奏调用。主循环跟得上时它
*          就是真实采样率; 跟不上时读数会跟着掉, 那正好反映有效吞吐。
*
*          算式说明 (旧版这里是错的, 读数恒为真实值的 16 倍):
*          采样率 = 每半帧样本数 / 半帧周期。*acc 累的是 RATE_AVG_HALVES 个
*          半帧的总周期, 平均一下才是"一个半帧周期", 所以分子上样本数也得
*          乘 RATE_AVG_HALVES 才对得起来 —— 两边都乘 16, 约掉, 结果就是
*          samples_per_half * OSC_CM4_HZ / 平均半帧周期。
*
*          旧版只把样本数乘了 16, 分母却仍是"相邻两次调用的间隔"(相邻两次
*          调用就是相邻两个半帧, 本来就只差一个半帧), 于是白白放大 16 倍。
*          它自己看不出来, 因为打印出来只是个数字 —— 但拿它当验收判据就会
*          把"跟得上"误判成"快了 16 倍"。这也是它不再单独占一行日志的原因。
*/
static void rate_update(uint32_t *last, uint32_t *acc, uint32_t *cnt,
                        uint32_t samples_per_half, volatile uint32_t *out)
{
    uint32_t now = DWT->CYCCNT;
    uint32_t d   = now - *last;
    *last = now;

    /* 首拍 (d==0), 或间隔异常 (超出一个半帧的一整秒, 说明被别的东西卡住了,
     * 这个样本不能混进平均)。 */
    if (d == 0u || d > OSC_CM4_HZ) return;

    *acc += d;
    if (++(*cnt) < RATE_AVG_HALVES) return;

    *out = (uint32_t)(((uint64_t)samples_per_half * RATE_AVG_HALVES * OSC_CM4_HZ)
                      / (uint64_t)*acc);
    *acc = 0u;
    *cnt = 0u;
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
*
*   @note  always_inline 是必须的, 不是"顺手优化"。这个函数每个半帧要被调用
*          6144 次 (CH1 每 word 两次 + CH2 每 word 一次), 而它每次都要把
*          win_min / win_max / armed 从内存读改写一遍 —— 不内联的话光这一项
*          就占掉扫描的 85%。实测 -O2 下 GCC 依然拒绝内联 (两个调用点, 内联会
*          让循环变大), 结果是每半帧要跑约 34.6 万条指令 ≈ 1.7 ms, 而预算只有
*          655 µs —— 主循环永远跟不上, 环里七成以上是空洞。
*
*          内联之后编译器能把这几个状态提进寄存器跨整个循环复用, 指令数掉到
*          几万条。代码变大的代价无所谓: FLASH 才用掉 5%。
*/
static inline __attribute__((always_inline)) uint32_t trig_feed(osc_trig_t *t, uint32_t v)
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
    /* 状态先拷进局部变量再扫, 扫完写回 —— 见 trig_scan_ch1 里的说明。 */
    osc_trig_t loc = *t;
    uint32_t pos = OSC_TRIG_NONE;

    for (uint32_t i = 0; i < n; i++) {
        if (trig_feed(&loc, s[i]) && pos == OSC_TRIG_NONE) pos = i;
    }

    t->win_min = loc.win_min;
    t->win_max = loc.win_max;
    t->armed   = loc.armed;
    return pos;                         /* 样本下标 (CH2: 1 word = 1 个样本) */
}

/**
*   @brief 扫一个半帧的 CH1 缓冲
*   @return 第一个触发点的 word 下标, 或 OSC_TRIG_NONE
*
*   @note  trig_feed 只动 win_min / win_max / armed 三个字段, 所以扫描期间把这
*          三个拷进局部变量, 扫完写回 —— 语义和直接改 t 完全一样。
*
*          为什么要多这一道: trig_feed 内联之后这几个字段仍然是通过指针访问的,
*          编译器证明不了 t 指向的结构和输入缓冲不重叠, 于是每个采样都要"读
*          win_min/win_max → 比较 → 条件写回", 4 次内存访问一次都省不掉。
*          一个半帧有 6144 个采样, 光这一项就是几万条指令, 而预算只有 655 µs。
*          换成局部变量后能被提到寄存器里跨整个循环复用。
*
*          level / hyst / arm / edge 扫描期间只读, 不用写回。
*/
static uint32_t trig_scan_ch1(osc_trig_t *t, const uint32_t *w, uint32_t n)
{
    osc_trig_t loc = *t;
    uint32_t pos = OSC_TRIG_NONE;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t word = w[i];
        if (trig_feed(&loc, word & 0xFFFFu) && pos == OSC_TRIG_NONE) pos = i;  /* ADC1, 低 16 位, 时间在前 */
        if (trig_feed(&loc, word >> 16)     && pos == OSC_TRIG_NONE) pos = i;  /* ADC2, 高 16 位, 时间在后 */
    }

    t->win_min = loc.win_min;
    t->win_max = loc.win_max;
    t->armed   = loc.armed;
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
*   @param done 这一拍的 DMA 完成计数 (s_completed_chN 的快照)
*   @param lag  这一拍比上次多完成了几个半帧 (>= 1)
*
*   @note  半帧下标是 (done-1)&1 —— 刚完成、还没被绕回来覆盖的那一格。
*
*          发布序号按 lag 累加 (seq += lag), 不是简单的 ++。这是相位对齐的
*          全部前提: CM7 用 (seq-1)&1 决定从 D2 的哪一格搬数据、用
*          (seq-1)*OSC_HALF_WORDS + pos 定位触发点在环里的位置。两件事都要求
*          seq 和 done 同奇偶。写成 ++ 的话, 只要有一次 lag != 1 (主循环漏掉
*          半帧), done 加 2 而 seq 只加 1, 奇偶就永久错开 —— CM7 从此搬错一格
*          (那一格正被 DMA 覆盖, 搬进来的是撕裂的数据), 而且触发点在环上的
*          位置整体偏 2048 word = 655 µs = 1/4 屏; 之后每翻一次奇偶就横跳一次。
*
*          同奇偶由 OSC_CH_Poll 在通道被打开那一刻对齐一次; 之后两边都按 lag
*          递增, 这个关系自然一直保持 (见那边的注释)。
*/
static void osc_scan_half(uint32_t ch, uint32_t done, uint32_t lag)
{
    osc_trig_t *t    = (ch == 0u) ? &s_trig_ch1 : &s_trig_ch2;
    uint32_t    half = (done - 1u) & 1u;
    uint32_t    pos  = OSC_TRIG_NONE;

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
        rate_update(&s_last_ch1, &s_acc_ch1, &s_cyc_ch1, OSC_HALF_WORDS * 2u, &osc_ctrl.diag_rate_ch1);
        __DMB();
        /* += lag 而不是 ++: 中间被跳掉的半帧也占一个序号, seq 才能一直和
         * DMA 完成计数同奇偶 —— 理由见本函数头。 */
        osc_ctrl.seq_ch1 += lag;
        __DMB();
    } else {
        osc_ctrl.trig_pos_ch2 = pos;
        if (pos != OSC_TRIG_NONE) osc_ctrl.trig_flags |=  2u;
        else                      osc_ctrl.trig_flags &= ~2u;
        rate_update(&s_last_ch2, &s_acc_ch2, &s_cyc_ch2, OSC_HALF_WORDS,     &osc_ctrl.diag_rate_ch2);
        __DMB();
        osc_ctrl.seq_ch2 += lag;
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
    /* 两个通道各有各的闸。停掉的那一路什么都不做 —— 不扫描、不发布、不通知
     * CM7, 于是它的 seq 不涨、SDRAM 环里也不再进新数据。屏幕上那一路怎么
     * 处理是另一回事 (由 View 直接擦掉, 不等这里)。
     *
     * 注意 ADC 和 DMA 从 OSC_CH_Init() 起就一直开着, 这里说的"停"只是不去
     * 看它 —— 硬件照常在采、D2 缓冲照常被绕回来覆盖, 都无所谓。
     *
     * 停着的那一路仍然要把游标对齐, 否则重新打开时会把积压的半帧一次性补扫。 */
    if (!osc_ctrl.cfg_run_ch1) {
        s_processed_ch1 = s_completed_ch1;
        s_run_ch1_prev  = 0u;
    } else {
        /* 刚被打开的那一拍: 把发布序号的奇偶对齐到完成计数。
         *
         * 停着的这段时间 DMA 照常在完成, done 一直在涨, 而 seq 是冻住的 ——
         * 两者奇偶一样是碰运气。CM7 用 (seq-1)&1 决定搬 D2 的哪一格, 而这里扫
         * 的是 (done-1)&1, 奇偶不对就搬错一格, 整屏的触发点偏 2048 word
         * (1/4 屏)。开着的时候 seq 按 lag 递增, 奇偶自然一直保持, 所以只需要
         * 在开机门这一刻对一次。
         *
         * 调 seq 而不是调 done: done 是 DMA 中断在写, 动不得。加一(而不是减一)
         * 是为了不让 CM7 那边算出 step=0 —— 那会让 missed 计数器下溢。 */
        if (s_run_ch1_prev == 0u) {
            if (((osc_ctrl.seq_ch1 ^ s_processed_ch1) & 1u) != 0u) {
                osc_ctrl.seq_ch1++;
            }
            s_run_ch1_prev = 1u;
        }

        /* 落后 ≥2 个半帧 = 想扫的那半已经被 DMA 绕回来覆盖了, 只能丢;
         * 只处理最新那半帧, diag_miss 只多不少(偏保守, 不静默) */
        uint32_t done1 = s_completed_ch1;               /* 32 位读写原子, 取一次快照 */
        uint32_t lag1  = done1 - s_processed_ch1;
        if (lag1 != 0u) {
            if (lag1 > 1u) osc_ctrl.diag_miss += lag1 - 1u;
            s_processed_ch1 = done1;
            osc_scan_half(0u, done1, lag1);
        }
    }

    if (!osc_ctrl.cfg_run_ch2) {
        s_processed_ch2 = s_completed_ch2;
        s_run_ch2_prev  = 0u;
    } else {
        if (s_run_ch2_prev == 0u) {
            if (((osc_ctrl.seq_ch2 ^ s_processed_ch2) & 1u) != 0u) {
                osc_ctrl.seq_ch2++;
            }
            s_run_ch2_prev = 1u;
        }

        uint32_t done2 = s_completed_ch2;
        uint32_t lag2  = done2 - s_processed_ch2;
        if (lag2 != 0u) {
            if (lag2 > 1u) osc_ctrl.diag_miss += lag2 - 1u;
            s_processed_ch2 = done2;
            osc_scan_half(1u, done2, lag2);
        }
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
