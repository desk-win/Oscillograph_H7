#ifndef BL24C16F_H
#define BL24C16F_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

/** BL24C16F 总容量，2 KB。 */
#define BL24C16F_SIZE_BYTES 2048U
/** BL24C16F 页大小，16 字节。 */
#define BL24C16F_PAGE_SIZE  16U

/**
 * @brief 扫描 I2C4 并初始化 BL24C16F。
 * @return HAL_OK 表示成功，HAL_ERROR 表示未找到设备。
 */
HAL_StatusTypeDef BL24C16F_Init(void);

/**
 * @brief 获取当前 BL24C16F 的 7 位 I2C 地址。
 * @return 7 位 I2C 地址，未初始化时返回 0。
 */
uint8_t BL24C16F_GetAddress(void);

/**
 * @brief 从 EEPROM 指定地址读取数据。
 * @param address EEPROM 内部 8 位地址。
 * @param data 接收数据缓冲区。
 * @param size 读取字节数。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef BL24C16F_Read(uint16_t address,
                                uint8_t *data,
                                uint16_t size);

/**
 * @brief 向 EEPROM 指定地址写入数据。
 * @param address EEPROM 内部 8 位起始地址。
 * @param data 待写入数据缓冲区。
 * @param size 写入字节数。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef BL24C16F_Write(uint16_t address,
                                 const uint8_t *data,
                                 uint16_t size);

#endif /* BL24C16F_H */
