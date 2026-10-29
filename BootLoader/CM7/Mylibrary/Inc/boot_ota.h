#ifndef BOOT_OTA_H
#define BOOT_OTA_H

#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** BootLoader 处理 EEPROM 升级请求后的结果。 */
typedef enum
{
    BOOT_OTA_NO_UPDATE = 0,
    BOOT_OTA_PACKAGE_INVALID,
    BOOT_OTA_INSTALLED,
    BOOT_OTA_FAILED
} BootOTA_Result;

/**
 * @brief 检查并安装 EEPROM 中标记为 VALID 的外部 OTA 包。
 *
 * 只有完整验证外部包头和两个镜像后才擦除内部 Flash。
 */
BootOTA_Result BootOTA_Process(void);

/** @brief 返回最近一次 BootOTA_Process 的失败阶段。 */
const char *BootOTA_LastError(void);

/** @brief 返回最近一次失败对应的 HAL 状态码。 */
HAL_StatusTypeDef BootOTA_LastStatus(void);

/** @brief 返回最近一次内部 Flash 失败的 bank 和状态错误位。 */
void BootOTA_LastFlashError(uint32_t *bank, uint32_t *flags);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_OTA_H */
