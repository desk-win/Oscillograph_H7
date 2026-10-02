#include "encoder.h"
#include "main.h"
#include "stm32h747xx.h"
#include "stm32h7xx_hal_gpio.h"
#include <stdint.h>

/* A 相上升沿 → 动 encoder_count 的唯一入口 */
static void Encoder_OnAEdge(void);

/* Encoder_Increment 的比较基准。只有主循环会写它, 中断不碰 */
static int16_t last_count = 0;

/* 上一次被接受的边沿的时刻, 用于消抖。只在中断里读写 */
static uint32_t last_edge_tick = 0;

volatile int16_t encoder_count = 0;

/* ---------------------------------------------------------------------------
 * 解码
 * ------------------------------------------------------------------------- */

static void Encoder_OnAEdge(void)
{

    /* 消抖: 窗口内的重复边沿直接丢掉。ENCODER_DEBOUNCE_MS 是 0u 的时候, 无符
     * 号减法的差值恒不小于 0, 这个判断永远不成立, 等于没消抖。 */
    if ((uint32_t)(HAL_GetTick() - last_edge_tick) < ENCODER_DEBOUNCE_MS)
    {
        return;
    }
    last_edge_tick = HAL_GetTick();
    
    GPIO_PinState A_P = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_1);
    GPIO_PinState B_P = HAL_GPIO_ReadPin(ENCODER_B_PORT, ENCODER_B_PIN);
    if ((B_P == GPIO_PIN_SET) && (A_P == GPIO_PIN_RESET)){
        encoder_count += ENCODER_DIR_SIGN;
    }
    else if ((B_P == GPIO_PIN_RESET) && (A_P == GPIO_PIN_RESET)) {
        encoder_count -= ENCODER_DIR_SIGN;
    }
        
    /* 边界: 到顶 / 到底就停在边上, 不回绕 */
    if (encoder_count > ENCODER_COUNT_MAX){
        encoder_count = ENCODER_COUNT_MAX;
    }else if (encoder_count < ENCODER_COUNT_MIN){
        encoder_count = ENCODER_COUNT_MIN;
    }

}

/* ---------------------------------------------------------------------------
 * 中断入口
 * ------------------------------------------------------------------------- */

/**
 * EXTI1 的中断回调 (A 相 = PC1)。
 *
 * 这个函数是 HAL 里的全局弱符号 (stm32h7xx_hal_gpio.c), 这里给一个强定义把
 * 它顶掉。链路上游是 CubeMX 生成的:
 *   stm32h7xx_it.c 的 EXTI1_IRQHandler()
 *     → HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_1)   // 查标志、清标志
 *     → 本函数
 * 所以 CubeMX 那边重新生成多少次都不会动到这里, 不用去改 USER CODE 段。
 *
 * 中断里只做: 读一根电平线、加一下、写回去、读一次 tick。没有 FreeRTOS API,
 * 没有 HAL_Delay / HAL_I2C_xxx 这类会等 tick 的阻塞调用。
 *
 * 别的 EXTI 线要是以后启用, 在自己的回调里判 GPIO_Pin 即可 —— 想让编码器这套
 * 东西全留在本文件里, 就别在别处也定义 HAL_GPIO_EXTI_Callback。
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ENCODER_A_PIN)
    {
        Encoder_OnAEdge();
    }
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

void Encoder_Init(void)
{
    encoder_count  = 0;
    last_count     = 0;
    last_edge_tick = HAL_GetTick();

    /* MX_GPIO_Init() 里 CubeMX 给的是 SetPriority(EXTI1_IRQn, 0, 0) —— 0 是最
     * 高优先级, 比 LTDC/DMA2D 的 5 还高。压到 ENCODER_IRQ_PRIORITY。
     * 要是以后在 CubeMX 的 NVIC 面板里也改成 10, 这两行可以删。 */
    HAL_NVIC_SetPriority(EXTI1_IRQn, ENCODER_IRQ_PRIORITY, 0);
}

int16_t Encoder_GetCount(void)
{
    return encoder_count;
}

int16_t Encoder_Increment(void)
{
    /* 先读快照: 16 位读是原子的, 拿到的要么是这次边沿之前的值, 要么是之后的,
     * 不会撕裂。快照之后的边沿算到下一次调用里。 */
    int16_t now   = encoder_count;
    int16_t delta = (int16_t)(now - last_count);

    last_count = now;
    return delta;
}

/* 长按判定时间 (ms)。**用 HAL_GetTick 量, 不是数调用次数** —— 调用方的节拍不
 * 固定 (现在挂在 TouchGFX 的 tick 上, 一次约 16.7 ms —— 那是 VSYNC 驱动的一帧),
 * 数调用次数的话, 换个帧率长按时间就跟着变。 */
#define LONG_PRESS_TIME_MS  2000u

/**
*   @brief 编码器上的开关，状态机，支持长按和单击
*
*   @note  调用节拍不要求固定, 但**至少要比抖动快** —— 消抖是按"连续两次读到同
*          一个电平"判的 (按下确认和抬起确认各一次), 所以调用得比按键抖动快才
*          压得住。挂在 60 Hz 的 tick 上一次 16.7 ms, 两次 33 ms, 够用。
*/
KeyEvent_t Encoder_Key(void) {
    static KeyState_t current_keystate = KEY_STATE_RELEASE;
    KeyEvent_t return_keyevent = KEY_EVENT_RELEASE;

    GPIO_PinState gpio_state = HAL_GPIO_ReadPin(EC_SW_GPIO_Port, EC_SW_Pin);

    static uint32_t press_start_ms = 0;  // 进入"确认按下"状态的时刻 (HAL_GetTick)
    static uint8_t longpress = 0;   // 标记本次按下过程是否已触发长按

    switch (current_keystate) {
        case KEY_STATE_RELEASE:
            if (gpio_state == GPIO_PIN_RESET) { // 检测到按下（低电平）
                current_keystate = KEY_STATE_PRESS_PREPARE;
            }
            break;

        case KEY_STATE_PRESS_PREPARE:
            if (gpio_state == GPIO_PIN_RESET) { // 确认按下消抖完成
                current_keystate = KEY_STATE_PRESSED;
                press_start_ms = HAL_GetTick(); // 进入按下状态时，记下起点
                longpress = 0; // 进入按下状态时，清零长按标志位
            } else {
                current_keystate = KEY_STATE_RELEASE;
            }
            break;

        case KEY_STATE_PRESSED:
            if (gpio_state == GPIO_PIN_SET) { // 检测到抬起
                current_keystate = KEY_STATE_RELEASE_PREPARE;
            } else { // 持续按下中
                /* 只有未触发过长时才判，保证长按只触发一次。用减法比较，跨 32 位
                 * 回绕自动正确。 */
                if (longpress == 0 &&
                    (uint32_t)(HAL_GetTick() - press_start_ms) >= LONG_PRESS_TIME_MS) {
                    longpress = 1;
                    return_keyevent = KEY_EVENT_LONGPRESS; // 触发长按事件
                }
            }
            break;

        case KEY_STATE_RELEASE_PREPARE:
            if (gpio_state == GPIO_PIN_SET) { // 确认抬起消抖完成
                current_keystate = KEY_STATE_RELEASE;
                if (longpress == 0) {
                    return_keyevent = KEY_EVENT_PRESS; // 只有没触发过长按时，松手才响应单击
                }
            } else { // 抬起过程中的干扰抖动，退回按下状态
                current_keystate = KEY_STATE_PRESSED;
            }
            break;

        default:
            current_keystate = KEY_STATE_RELEASE;
            break;
    }

    return return_keyevent;
}
