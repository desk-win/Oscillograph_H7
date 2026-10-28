#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

/**
 * @file crc32.h
 * @brief CM7 应用与 BootLoader 共用的 CRC-32/ISO-HDLC 算法。
 *
 * 算法与 zlib crc32() 兼容，用于镜像、包头和 EEPROM 元数据的完整性校验。
 */

/** @brief 获取流式 CRC32 计算的初始中间值。 */
uint32_t CRC32_Init(void);

/**
 * @brief 在已有中间值后追加一段数据。
 * @param crc 上一段数据的 CRC32 中间值。
 * @param data 待计算数据缓冲区。
 * @param size 数据长度，单位：字节。
 * @return 更新后的 CRC32 中间值。
 */
uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t size);

/** @brief 将流式计算的中间值转换为最终 CRC32。 */
uint32_t CRC32_Finalize(uint32_t crc);

/** @brief 一次计算完整连续缓冲区的 CRC32。 */
uint32_t CRC32_Calculate(const uint8_t *data, uint32_t size);

/** @brief 使用标准字符串 "123456789" 自检，结果应为 0xCBF43926。 */
HAL_StatusTypeDef CRC32_SelfTest(void);

#endif /* CRC32_H */
