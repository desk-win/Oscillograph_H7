#include "usb_printf.h"
#include "usbd_cdc_if.h"
#include "stm32h7xx_hal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

/* 上一包没发完时 CDC_Transmit_FS 直接返回 USBD_BUSY 且什么都不做, 必须等端点腾空 */
#define CDC_TX_TIMEOUT_MS   100

static void cdc_write(const uint8_t *buf, uint16_t len){
    USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    uint32_t t0;

    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED || hcdc == NULL) return;

    t0 = HAL_GetTick();
    while (CDC_Transmit_FS((uint8_t *)buf, len) == USBD_BUSY){
        if (HAL_GetTick() - t0 >= CDC_TX_TIMEOUT_MS) return;
    }
}

void usb_printf(char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    char buf[128];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0){
        int send_len = (len < (int)sizeof(buf)) ? len : (int)sizeof(buf) - 1;
        cdc_write((const uint8_t*)buf, (uint16_t)send_len);
    }
}

int __io_putchar(int ch){
    cdc_write((const uint8_t *)&ch, 1);
    return ch;
}
