#include "uart_printf.h"
#include "stm32h7xx_hal_uart.h"
#include "usart.h"
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

void uart1_printf(char *fmt, ...)
{
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
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 100);
    return ch;
}
