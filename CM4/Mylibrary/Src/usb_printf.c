#include "usb_printf.h"
#include "usbd_cdc_if.h"
#include "stm32h7xx_hal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

/* ---- 总闸 ----
 *
 * 1 = 打开, 0 = 整个打印路径编译成空 (usb_printf / __io_putchar / cdc_write
 * 一个都不再碰 USB)。
 *
 * **为什么要这么一道总闸, 而不是只在各个调用点 #if:** 下面 cdc_write 在 CDC
 * 端点腾空之前会**自旋最长 CDC_TX_TIMEOUT_MS = 100 ms**。而这 100 ms 正好撞
 * 上 M7 那边逻辑分析仪的启动门 LOG_START_TIMEOUT_MS = 100 ms (见
 * CM7/Mylibrary/Inc/logic_trace.h) —— 打印期间 CM4 走不到 LOG_Poll(), 看不到
 * cfg_run 的 0→1 边沿; M7 的启动门却已经在计时, 到点就把开关收回去。表现就
 * 是"拨了开关一点反应都没有, 要反复点几次才起得来", 而且每次失败的 start 又
 * 打两行, 自己给下一次点击制造了同样的条件。
 *
 * 实测判据: 插着 USB 线 (CDC 已枚举) 时必现; 拔了线则完全正常 —— 因为下面
 * cdc_write 第一句就按 dev_state 早退了, 一次 USB 都不碰。**差别在"线插没插",
 * 不在供电质量。** 所以"用电池就好了"这个结论只在**线也拔了**的前提下成立。
 *
 * **这两个 100 必须脱钩** —— 要么关打印 (现在这样), 要么把 CDC_TX_TIMEOUT_MS
 * 压到 0 (端点忙就丢这一行, 不阻塞), 二者至少做一个。 */
#define USB_PRINTF_ENABLE   0

/* 上一包没发完时 CDC_Transmit_FS 直接返回 USBD_BUSY 且什么都不做, 必须等端点腾空。
 *
 * **这个数不许和 M7 的 LOG_START_TIMEOUT_MS 相等或更大** —— 相等就是上面说的
 * 那条死路, 更大更糟。改打印之前先看那段注释。 */
#define CDC_TX_TIMEOUT_MS   100

#if USB_PRINTF_ENABLE

static void cdc_write(const uint8_t *buf, uint16_t len){
    USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    uint32_t t0;

    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED || hcdc == NULL) return;

    t0 = HAL_GetTick();
    while (CDC_Transmit_FS((uint8_t *)buf, len) == USBD_BUSY){
        if (HAL_GetTick() - t0 >= CDC_TX_TIMEOUT_MS) return;
    }
}

#endif /* USB_PRINTF_ENABLE */

void usb_printf(char *fmt, ...){
#if USB_PRINTF_ENABLE
    va_list ap;
    va_start(ap, fmt);
    char buf[128];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0){
        int send_len = (len < (int)sizeof(buf)) ? len : (int)sizeof(buf) - 1;
        cdc_write((const uint8_t*)buf, (uint16_t)send_len);
    }
#else
    /* 总闸关着 —— 连着 vsnprintf 一起省掉, 调用点连参数求值都不用改。 */
    (void)fmt;
#endif
}

int __io_putchar(int ch){
#if USB_PRINTF_ENABLE
    cdc_write((const uint8_t *)&ch, 1);
#endif
    return ch;
}
