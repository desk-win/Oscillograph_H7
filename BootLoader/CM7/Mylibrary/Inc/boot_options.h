#ifndef BOOT_OPTIONS_H
#define BOOT_OPTIONS_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 当前 CM4 启动相关 Option Byte 状态。 */
typedef struct
{
    uint8_t bcm4_enabled;
    uint32_t cm4_boot_addr0;
} BootOptions_CM4Info;

/** BCM4 配置结果。 */
typedef enum
{
    BOOT_OPTIONS_ALREADY_DISABLED = 0,
    BOOT_OPTIONS_PROGRAMMED,
    BOOT_OPTIONS_BAD_ADDRESS,
    BOOT_OPTIONS_ERROR
} BootOptions_Result;

HAL_StatusTypeDef BootOptions_GetCM4Info(BootOptions_CM4Info *info);

/**
 * @brief 确保 BCM4=Disable，同时保持 CM4 boot 地址为 0x08180000。
 * @param info 输出修改前的 CM4 启动配置。
 * @return 配置结果。
 */
BootOptions_Result BootOptions_EnsureCM4Held(BootOptions_CM4Info *info);

/** @brief 返回最近一次 Option Byte 操作的 HAL 状态。 */
HAL_StatusTypeDef BootOptions_LastStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_OPTIONS_H */
