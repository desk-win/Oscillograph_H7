#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QSPI_FLASH_SIZE_BYTES   (32U * 1024U * 1024U)
#define QSPI_FLASH_PAGE_SIZE    256U
#define QSPI_FLASH_SECTOR_SIZE  4096U
#define QSPI_FLASH_BLOCK_SIZE   65536U

/*
 * QSPI_FLASH_Init()必须在MX_QUADSPI_Init()之后调用
 * These blocking functions are not re-entrant; serialize calls from tasks.
 */
/**
 * @brief 初始化 W25Q256 外部 Flash。
 * @note 必须在 MX_QUADSPI_Init() 之后调用。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef QSPI_FLASH_Init(void);

/**
 * @brief 读取外部 Flash 的 JEDEC ID。
 * @param jedec_id 输出参数，返回 24 位 JEDEC ID。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef QSPI_FLASH_ReadJedecId(uint32_t *jedec_id);

/**
 * @brief 等待外部 Flash 内部操作完成。
 * @param timeout_ms 超时时间，单位为毫秒。
 * @return HAL_OK 表示空闲，HAL_TIMEOUT 表示超时，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef QSPI_FLASH_WaitReady(uint32_t timeout_ms);

/**
 * @brief 从外部 Flash 的指定地址读取数据。
 * @param address 读取起始地址，范围为 0 到 32 MB - 1。
 * @param data 接收数据的缓冲区指针。
 * @param size 要读取的字节数。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef QSPI_FLASH_Read(uint32_t address, uint8_t *data, uint32_t size);

/**
 * @brief 向外部 Flash 的单个页写入数据。
 * @param address 写入起始地址，写入范围不能跨页。
 * @param data 待写入数据的缓冲区指针。
 * @param size 要写入的字节数，最大为 256 字节。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef QSPI_FLASH_PageProgram(uint32_t address,
                                         const uint8_t *data,
                                         uint16_t size);

/**
 * @brief 向外部 Flash 写入任意长度的数据。
 * @param address 写入起始地址。
 * @param data 待写入数据的缓冲区指针。
 * @param size 要写入的字节数。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 * @note 写入区域必须已经擦除。
 */
HAL_StatusTypeDef QSPI_FLASH_Write(uint32_t address,
                                   const uint8_t *data,
                                   uint32_t size);

/**
 * @brief 擦除一个 4 KB 扇区。
 * @param address 扇区起始地址，必须按 4 KB 对齐。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef QSPI_FLASH_EraseSector4KB(uint32_t address);

/**
 * @brief 擦除一个 64 KB 块。
 * @param address 块起始地址，必须按 64 KB 对齐。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
HAL_StatusTypeDef QSPI_FLASH_EraseBlock64KB(uint32_t address);

#ifdef __cplusplus
}
#endif

#endif /* QSPI_FLASH_H */
