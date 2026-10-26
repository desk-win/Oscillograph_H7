#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

#define QSPI_FLASH_SIZE_BYTES   (32U * 1024U * 1024U)
#define QSPI_FLASH_PAGE_SIZE    256U
#define QSPI_FLASH_SECTOR_SIZE  4096U
#define QSPI_FLASH_BLOCK_SIZE   65536U

HAL_StatusTypeDef QSPI_FLASH_Init(void);
HAL_StatusTypeDef QSPI_FLASH_ReadJedecId(uint32_t *jedec_id);
HAL_StatusTypeDef QSPI_FLASH_WaitReady(uint32_t timeout_ms);

HAL_StatusTypeDef QSPI_FLASH_Read(uint32_t address, uint8_t *data, uint32_t size);
HAL_StatusTypeDef QSPI_FLASH_PageProgram(uint32_t address,
                                         const uint8_t *data,
                                         uint16_t size);
HAL_StatusTypeDef QSPI_FLASH_Write(uint32_t address,
                                   const uint8_t *data,
                                   uint32_t size);

HAL_StatusTypeDef QSPI_FLASH_EraseSector4KB(uint32_t address);
HAL_StatusTypeDef QSPI_FLASH_EraseBlock64KB(uint32_t address);

HAL_StatusTypeDef QSPI_FLASH_CalculateCrc32(uint32_t address,
                                            uint32_t size,
                                            uint32_t *crc32);
HAL_StatusTypeDef QSPI_FLASH_VerifyCrc32(uint32_t address,
                                         uint32_t size,
                                         uint32_t expected_crc32);

#endif /* QSPI_FLASH_H */
