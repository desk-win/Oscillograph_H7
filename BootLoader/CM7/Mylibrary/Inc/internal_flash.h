#ifndef INTERNAL_FLASH_H
#define INTERNAL_FLASH_H

#include <stdint.h>

#include "stm32h7xx_hal.h"
#include "ota_layout.h"

/** M7 应用槽起始地址，包含。 */
#define INTERNAL_FLASH_M7_APP_START  OTA_INTERNAL_CM7_BASE
/** M7 应用槽结束地址，不包含。 */
#define INTERNAL_FLASH_M7_APP_END    (OTA_INTERNAL_CM7_BASE + \
                                      OTA_INTERNAL_CM7_SIZE)
/** M4 应用槽起始地址，包含。 */
#define INTERNAL_FLASH_M4_APP_START  OTA_INTERNAL_CM4_BASE
/** M4 应用槽结束地址，不包含。 */
#define INTERNAL_FLASH_M4_APP_END    (OTA_INTERNAL_CM4_BASE + \
                                      OTA_INTERNAL_CM4_SIZE)

/** H7 每个 Flash 扇区的大小，128 KB。 */
#define INTERNAL_FLASH_SECTOR_SIZE   0x00020000U
/** H7 单次 Flash 编程粒度，32 字节。 */
#define INTERNAL_FLASH_WORD_SIZE     32U

/*
 * 擦写 M4 应用槽前必须确保 CM4 已停止执行目标 bank。
 */

/**
 * @brief 擦除内部 Flash 的指定扇区范围。
 * @param address 起始地址，必须按 128 KB 对齐。
 * @param size 擦除长度，必须是 128 KB 的整数倍。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef InternalFlash_EraseRange(uint32_t address, uint32_t size);

/**
 * @brief 向内部 Flash 写入数据。
 * @param address 写入起始地址，必须按 32 字节对齐。
 * @param data 待写入数据缓冲区。
 * @param size 写入长度，单位为字节。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 * @note 尾部不足 32 字节时使用 0xFF 补齐；目标区域必须已擦除。
 */
HAL_StatusTypeDef InternalFlash_Write(uint32_t address,
                                      const uint8_t *data,
                                      uint32_t size);

/**
 * @brief 从 QSPI 外部 Flash 复制数据到内部 Flash。
 * @param qspi_address 外部 Flash 源地址。
 * @param internal_address 内部 Flash 目标地址。
 * @param size 要复制的字节数。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 * @note 内部目标区域必须已擦除，复制过程会按 32 字节补齐最后一个字。
 */
HAL_StatusTypeDef InternalFlash_CopyFromQspi(uint32_t qspi_address,
                                             uint32_t internal_address,
                                             uint32_t size);

/**
 * @brief 计算内部 Flash 指定范围的 CRC32。
 * @param address 起始地址。
 * @param size 数据长度，单位为字节。
 * @param crc32 输出 CRC32 值。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef InternalFlash_CalculateCrc32(uint32_t address,
                                               uint32_t size,
                                               uint32_t *crc32);

/**
 * @brief 校验内部 Flash 指定范围的 CRC32。
 * @param address 起始地址。
 * @param size 数据长度，单位为字节。
 * @param expected_crc32 期望的 CRC32 值。
 * @return HAL_OK 表示匹配，HAL_ERROR 表示不匹配或读取失败。
 */
HAL_StatusTypeDef InternalFlash_VerifyCrc32(uint32_t address,
                                            uint32_t size,
                                            uint32_t expected_crc32);

#endif /* INTERNAL_FLASH_H */
