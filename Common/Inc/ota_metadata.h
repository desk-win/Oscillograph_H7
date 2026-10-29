#ifndef OTA_METADATA_H
#define OTA_METADATA_H

#include <stdint.h>

#include "ota_layout.h"
#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ota_metadata.h
 * @brief CM7 应用与 BootLoader 共用的 OTA 元数据双副本管理接口。
 */

/**
 * @brief 从 EEPROM 两份副本中加载序号最新的有效元数据。
 * @param metadata 输出元数据；无有效记录时清零。
 * @param found 输出是否找到有效记录，1 表示找到，0 表示没有。
 * @return HAL_OK 表示读取完成，其他值表示 EEPROM 读取失败。
 */
HAL_StatusTypeDef OTA_Metadata_Load(OTA_Metadata *metadata, uint8_t *found);

/**
 * @brief 原子提交一条元数据记录。
 *
 * 函数覆盖序号较旧的副本，递增 sequence，重新计算 record_crc32，
 * 写入后回读比对。写入期间另一份旧记录保持不变。
 *
 * @param metadata 输入待提交字段；成功后更新为实际写入的新记录。
 * @return HAL_OK 表示写入并回读校验成功，其他值表示失败。
 */
HAL_StatusTypeDef OTA_Metadata_Commit(OTA_Metadata *metadata);

/**
 * @brief 根据最新元数据选择本次 OTA 应写入的外部 Flash 槽。
 * @param metadata 最新元数据。
 * @param found 是否为有效元数据，0 表示 EEPROM 仍为空。
 * @return 目标槽；返回 OTA_SLOT_ID_NONE 表示当前不允许开始新传输。
 */
OTA_SlotId OTA_Metadata_SelectTargetSlot(const OTA_Metadata *metadata,
                                         uint8_t found);

/** @brief 自检记录 CRC、状态选择和双副本序号规则。 */
HAL_StatusTypeDef OTA_Metadata_SelfTest(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_METADATA_H */
