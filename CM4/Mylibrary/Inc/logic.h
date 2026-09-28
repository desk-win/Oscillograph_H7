#ifndef __LOGIC_H
#define __LOGIC_H


#include "stm32h7xx_hal.h"
#include "gpio.h"
#include "main.h"
#include "stm32h7xx_hal_gpio.h"
#include <stdint.h>


/**
*   @brief 使能逻辑分析仪模块 (PG14 = LOG_EN)
*
*   @note  施密特触发器的使能。必须在启动 DMA 之前置起 —— 让触发器先稳定下来,
*          否则最前面几个样本可能是它还没进入工作状态时采的。
*/
void Logic_EN(uint8_t level);

/**
*   @brief 逻辑分析仪采集侧初始化
*   @note  MX_TIM2_Init() 之后、主循环开始之前调一次 (CM4/Core/Src/main.c 的
*          USER CODE BEGIN 2 里, 紧跟 OSC_CH_Init())。
*
*          地址契约自检 + 控制块填初值 + 关掉 8 路开关 (LOG_EN 拉**高** ——
*          低有效, 见 logic.c 里的 LOG_EN_ON / LOG_EN_OFF)。硬件 (TIM2 /
*          DMA2_Stream0) 的初始化全部由 CubeMX 生成, 这里一个字都不改 ——
*          `.ioc` 才是唯一该改定时器/DMA 的地方。
*/
void LOG_Init(void);

/**
*   @brief 主循环里反复调用, 和 OSC_CH_Poll() 并列
*
*   它只做一件事: 看 CM7 写的 cfg_run 是不是变了, 变了就按 log_start/log_stop
*   启停 TIM2 + DMA2。中间不回传任何东西 —— 半帧完成计数由 DMA 中断直接写进
*   控制块的 frame_seq, CM7 自己去轮询那个字段。
*
*   和 OSC_CH_Poll 一样, 这个函数不许阻塞 (理由见 CM4/Core/Src/main.c 主循环
*   上面那段), 但它本来就是几行判断, 没有期限压力。
*/
void LOG_Poll(void);


#endif
