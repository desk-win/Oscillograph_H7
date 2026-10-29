#ifndef OTA_PACKAGE_H
#define OTA_PACKAGE_H

#include <stdint.h>

#include "ota_layout.h"
#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ota_package.h
 * @brief CM7 应用与 BootLoader 共用的外部 Flash 包校验接口。
 */

/** @brief 获取 A/B 槽在外部 Flash 中的基地址。 */
uint32_t OTA_Package_SlotBase(OTA_SlotId slot);

/** @brief 计算包头前 36 字节的 CRC32。 */
uint32_t OTA_Package_HeaderCrc32(const OTA_PackageHeader *header);

/** @brief 校验包头字段、目标槽编号、区域边界和 header_crc32。 */
uint8_t OTA_Package_HeaderValid(const OTA_PackageHeader *header,
                                OTA_SlotId expected_slot);

/**
 * @brief 回读并验证指定槽的包头、CM7 镜像和 CM4 镜像。
 * @param slot 待验证槽。
 * @param header 输出回读并验证通过的包头。
 * @return HAL_OK 表示全部有效，其他值表示读取或校验失败。
 */
HAL_StatusTypeDef OTA_Package_VerifySlot(OTA_SlotId slot,
                                         OTA_PackageHeader *header);

/** @brief 擦除指定槽的包头区，使该槽不能被当作有效包使用。 */
HAL_StatusTypeDef OTA_Package_InvalidateSlot(OTA_SlotId slot);

/** @brief 自检包头 CRC、槽编号和字段损坏检测。 */
HAL_StatusTypeDef OTA_Package_SelfTest(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_PACKAGE_H */
