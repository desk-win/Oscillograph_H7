#include "uart_printf.h"
#include "stm32h7xx_hal_uart.h"
#include "usart.h"
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

volatile uint8_t g_uart1_printf_enabled = 1U;

void UART1_PrintfSetEnabled(uint8_t enabled)
{
    g_uart1_printf_enabled = enabled;
}

void uart1_printf(char *fmt, ...)
{
    if (g_uart1_printf_enabled == 0U)
    {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    char buf[128];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0)
    {
        
        int send_len = (len < (int)sizeof(buf)) ? len : (int)sizeof(buf) - 1;
        HAL_UART_Transmit(&huart1, (uint8_t *)buf, send_len, 200);
    }
}

int __io_putchar(int ch)
{
    if (g_uart1_printf_enabled != 0U)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 100);
    }
    return ch;
}
