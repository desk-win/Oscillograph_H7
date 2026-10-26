#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

/*
 * CRC-32/ISO-HDLC，与 zlib crc32() 兼容。
 * 多项式 0xEDB88320，初始值 0xFFFFFFFF，结果异或 0xFFFFFFFF。
 */

/**
 * @brief 获取 CRC32 流式计算的初始值。
 * @return CRC32 初始状态。
 */
uint32_t CRC32_Init(void);

/**
 * @brief 继续计算一段数据的 CRC32。
 * @param crc 上一段数据的 CRC32 中间状态。
 * @param data 待计算数据缓冲区。
 * @param size 数据长度，单位为字节。
 * @return 更新后的 CRC32 中间状态。
 */
uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t size);

/**
 * @brief 完成 CRC32 计算。
 * @param crc CRC32 中间状态。
 * @return 最终 CRC32 值。
 */
uint32_t CRC32_Finalize(uint32_t crc);

/**
 * @brief 一次计算完整数据缓冲区的 CRC32。
 * @param data 待计算数据缓冲区。
 * @param size 数据长度，单位为字节。
 * @return 数据对应的 CRC32 值。
 */
uint32_t CRC32_Calculate(const uint8_t *data, uint32_t size);

/**
 * @brief 使用标准字符串 "123456789" 自检 CRC32 实现。
 * @return HAL_OK 表示结果正确，HAL_ERROR 表示实现异常。
 */
HAL_StatusTypeDef CRC32_SelfTest(void);

#endif /* CRC32_H */
