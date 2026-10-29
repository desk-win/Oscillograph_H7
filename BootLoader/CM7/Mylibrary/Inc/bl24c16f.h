#ifndef BL24C16F_H
#define BL24C16F_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

/** EEPROM 总容量，BL24C16F 为 2 KB。 */
#define BL24C16F_SIZE_BYTES          2048U
/** EEPROM 页大小，页写操作不能跨越该边界。 */
#define BL24C16F_PAGE_SIZE           16U
/** 自检记录地址，位于 EEPROM 最后一页。 */
#define BL24C16F_TEST_RECORD_ADDRESS 0x07F0U

/** EEPROM 自检结果。 */
typedef enum
{
    /** 自检成功。 */
    BL24C16F_TEST_OK = 0,
    /** 测试记录写入失败。 */
    BL24C16F_TEST_WRITE_FAILED,
    /** 测试记录读回失败。 */
    BL24C16F_TEST_READ_FAILED,
    /** 读回内容或 CRC 校验失败。 */
    BL24C16F_TEST_CRC_FAILED
} BL24C16F_TestResult;

/**
 * @brief 扫描 I2C4 并初始化 BL24C16F。
 * @return HAL_OK 表示找到并初始化成功，HAL_ERROR 表示未找到设备。
 */
HAL_StatusTypeDef BL24C16F_Init(void);

/**
 * @brief 获取当前 BL24C16F 的 7 位 I2C 地址。
 * @return 7 位 I2C 地址，未初始化时返回 0。
 */
uint8_t BL24C16F_GetAddress(void);

/**
 * @brief 从 EEPROM 指定地址读取数据。
 * @param address EEPROM 内部 11 位字节地址，范围 0~2047。
 *                高 3 位会自动映射到 I2C 从机地址 0x50~0x57。
 * @param data 接收数据的缓冲区。
 * @param size 要读取的字节数。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef BL24C16F_Read(uint16_t address, uint8_t *data, uint16_t size);

/**
 * @brief 向 EEPROM 指定地址写入数据。
 * @param address EEPROM 内部 11 位起始地址，范围 0~2047。
 *                高 3 位会自动映射到 I2C 从机地址 0x50~0x57。
 * @param data 待写入数据缓冲区。
 * @param size 要写入的字节数。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 * @note 函数会自动按 16 字节页拆分并在每页后等待写周期。
 */
HAL_StatusTypeDef BL24C16F_Write(uint16_t address,
                                 const uint8_t *data,
                                 uint16_t size);

/**
 * @brief 写入并读回一条带 CRC32 的 EEPROM 测试记录。
 * @param expected_crc 输出期望 CRC，可为 NULL。
 * @param read_crc 输出读回记录中的 CRC，可为 NULL。
 * @return 自检结果，BL24C16F_TEST_OK 表示成功。
 */
BL24C16F_TestResult BL24C16F_RunTest(uint32_t *expected_crc,
                                     uint32_t *read_crc);

#endif /* BL24C16F_H */
