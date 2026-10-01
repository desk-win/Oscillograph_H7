#ifndef __ENCODER_H
#define __ENCODER_H

#include "gpio.h"
#include "main.h"
#include "stm32h7xx_hal.h"
#include <stdint.h>

/* 这个头会被 C 和 C++ 两边 include (TouchGFX 的 Model 是 C++), 不加这个
 * 守卫链接期会因为名字修饰对不上而报 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 旋钮编码器 (PC1 = A 相, PC2 = B 相) —— 调示波器 / 逻辑分析仪的时基
 *
 * 硬件 (CubeMX 里配的, 见 CM7/Core/Src/gpio.c 的 MX_GPIO_Init):
 *   PC1 / A 相 —— GPIO_MODE_IT_RISING, 走 EXTI1, NVIC 里使能了,
 *                stm32h7xx_it.c 里生成的 EXTI1_IRQHandler 会往下叫本文件的
 *                HAL_GPIO_EXTI_Callback()。
 *   PC2 / B 相 —— 普通输入 (GPIO_MODE_INPUT), 不产生中断, 只当一根电平线读。
 *
 * 带定位感的机械编码器转一格, A/B 各出一个完整周期 = 两个上升沿。这里按
 * "转一格 = ±1" 来: 只在 A 的上升沿动 encoder_count, 方向看那一刻 B 的电平,
 * 所以 B 根本不需要中断。
 *
 * 以后想换成 x2 解码 (抗漏步更好, 但转一格会出 ±2), 把 PC2 改回上升沿触发、
 * 使能 EXTI2、在下面补一个 HAL_GPIO_EXTI_Callback 的分支就行, 上层三个函数
 * 和调用方都不用动。
 * ========================================================================= */

/* 编码器两根线 */
#define ENCODER_A_PORT       GPIOC
#define ENCODER_A_PIN        GPIO_PIN_1
#define ENCODER_B_PORT       GPIOC
#define ENCODER_B_PIN        GPIO_PIN_2

/* encoder_count 的上下限。这个数现在是给时基档位当索引用的, 先给个够宽的范围;
 * 等时基表 (到底几档) 定下来了, 把这两个宏改成档位的上下界, 转到头就自己停住,
 * 调用方不用再夹一次。到界是**卡住**不是回绕 —— 档位到顶了不该跳到另一头去
 * (想要回绕式的转轮交互, 改 Encoder_OnAEdge 里钳位那几行)。 */
#define ENCODER_COUNT_MIN    (-9999)
#define ENCODER_COUNT_MAX    ( 9999)

#if (ENCODER_COUNT_MAX > 32767) || (ENCODER_COUNT_MIN < -32768)
#error "ENCODER_COUNT_MIN/MAX 超出 int16_t 范围了, encoder_count 存不下"
#endif

/* 方向极性。当前约定: A 的上升沿上 B 为低 → +1。要是实际上转反了 (A/B 接反
 * 或者编码器反着装的), 把这个改成 (-1) 就行, 别去动解码逻辑。 */
#define ENCODER_DIR_SIGN     (+1)

/* 抖动抑制窗口 (ms)。机械编码器的触点在过定位点时会弹几下, 转一格可能蹦出好
 * 几个上升沿。HAL 的 tick 是 1 ms 一格, 所以这个数实际分辨到 1~2 ms; 人手最
 * 快也就转到 20~50 格/秒 (20~50 ms 一格), 不会误伤。
 * 填 0u 就是不消抖 (下面那个判断是无符号减法, 差值恒不小于 0)。 */
#define ENCODER_DEBOUNCE_MS  5u

/* EXTI1 的抢占优先级。CubeMX 勾 NVIC 时默认给的是 0 (最高), 那个太扎眼了 ——
 * 编码器只是个输入, 掉一次最多是时基跳一格。这里压到 10: 比 LTDC / DMA2D 的
 * 显示链路 (5) 低, 比 HAL 的 TIM7 时基 (15) 高。
 * 这个中断里不碰任何 FreeRTOS API, 所以不受
 * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 那条限制的约束。
 * 如果哪天在 CubeMX 的 NVIC 面板里也把它改成 10, Encoder_Init() 里那行覆盖
 * 就是多余的, 可以删。 */
#define ENCODER_IRQ_PRIORITY 10u

// 按键状态枚举
typedef enum{
    KEY_STATE_RELEASE = 0,           // 按键按键未按下
    KEY_STATE_PRESS_PREPARE,      // 按下消抖确认
    KEY_STATE_PRESSED,            // 确认按下
    KEY_STATE_RELEASE_PREPARE     // 释放消抖确认
} KeyState_t;

// 按键事件枚举
typedef enum{
    KEY_EVENT_RELEASE = 0,      // 释放事件
    KEY_EVENT_PRESS,            // 单击/按下事件
    KEY_EVENT_LONGPRESS         // 长按事件
} KeyEvent_t;

/**
 * 当前计数值。中断里加减, 外面**只读** —— 16 位对齐访问在 M7 上是原子的,
 * 直接读不会读到半截值。要改它请走下面那几个函数, 别在外面赋值。
 */
extern volatile int16_t encoder_count;

/**
 * 初始化: 计数清零, 并把 EXTI1 的抢占优先级从 CubeMX 默认的 0 压到
 * ENCODER_IRQ_PRIORITY。GPIO 和 NVIC 使能都是 CubeMX 干的, 这里不重复。
 *
 * 必须在 MX_GPIO_Init() 之后调。放 CM7/Core/Src/main.c 的 USER CODE BEGIN 2
 * 里、osKernelStart() 之前, 调一次就好 —— 放在 osKernelStart() 之后也不出错,
 * 只是初始化前转的几格不算数。
 */
void Encoder_Init(void);

/**
 * 读当前计数 (当时基档位索引用)。
 */
int16_t Encoder_GetCount(void);

/**
 * 取自上次调用以来的增量: 正转一格返回 +1, 反转一格返回 -1, 没动返回 0。
 * 两次调用之间连着转了好几格就返回那几格的和, 所以不会漏计数。
 *
 * 只能在主循环 / 任务里调, **不要在中断里调** —— 它要写 last_count。
 * 计数被 ENCODER_COUNT_MIN/MAX 卡住时, 返回值会小于实际转过的格数, 这是故意
 * 的: 档位已经到头顶了, 不该再往外跳。
 */
int16_t Encoder_Increment(void);

KeyEvent_t Encoder_Key(void);


#ifdef __cplusplus
}
#endif

#endif /* __ENCODER_H */
