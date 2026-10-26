#ifndef BL24C16F_H
#define BL24C16F_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

#define BL24C16F_SIZE_BYTES          2048U
#define BL24C16F_PAGE_SIZE           16U
#define BL24C16F_TEST_RECORD_ADDRESS 0x07F0U

typedef enum
{
    BL24C16F_TEST_OK = 0,
    BL24C16F_TEST_WRITE_FAILED,
    BL24C16F_TEST_READ_FAILED,
    BL24C16F_TEST_CRC_FAILED
} BL24C16F_TestResult;

HAL_StatusTypeDef BL24C16F_Init(void);
uint8_t BL24C16F_GetAddress(void);

HAL_StatusTypeDef BL24C16F_Read(uint16_t address, uint8_t *data, uint16_t size);
HAL_StatusTypeDef BL24C16F_Write(uint16_t address,
                                 const uint8_t *data,
                                 uint16_t size);

BL24C16F_TestResult BL24C16F_RunTest(uint32_t *expected_crc,
                                     uint32_t *read_crc);

#endif /* BL24C16F_H */
