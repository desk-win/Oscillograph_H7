#ifndef __UART_PRINTF_H
#define __UART_PRINTF_H

#include "stm32h7xx_hal.h"

/** 非零时允许 uart1_printf 和 printf 重定向输出。 */
extern volatile uint8_t g_uart1_printf_enabled;

/**
 * @brief 打开或关闭 USART1 普通日志输出。
 * @param enabled 0 表示关闭，非 0 表示打开。
 */
void UART1_PrintfSetEnabled(uint8_t enabled);

void uart1_printf(char *fmt, ...);

#endif
