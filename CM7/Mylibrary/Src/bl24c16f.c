#include "bl24c16f.h"

#include <stddef.h>

#include "i2c.h"

/** BL24C16F 7 位地址扫描范围。 */
#define BL24C16F_FIRST_ADDRESS  0x50U
#define BL24C16F_LAST_ADDRESS   0x57U
/** EEPROM 页写后的内部写周期等待时间。 */
#define BL24C16F_WRITE_DELAY_MS 5U
/** 单次 I2C 操作超时时间。 */
#define BL24C16F_TIMEOUT_MS     100U

_Static_assert(((BL24C16F_SIZE_BYTES - 1U) >> 8U) ==
               (BL24C16F_LAST_ADDRESS - BL24C16F_FIRST_ADDRESS),
               "BL24C16F page address mapping mismatch");

/** 当前探测到的 7 位 I2C 地址，0 表示未初始化。 */
static uint8_t s_device_address;

HAL_StatusTypeDef BL24C16F_Init(void)
{
    for (uint8_t address = BL24C16F_FIRST_ADDRESS;
         address <= BL24C16F_LAST_ADDRESS;
         address++)
    {
        if (HAL_I2C_IsDeviceReady(&hi2c4, (uint16_t)(address << 1U),
                                  3U, BL24C16F_TIMEOUT_MS) == HAL_OK)
        {
            s_device_address = address;
            return HAL_OK;
        }
    }

    s_device_address = 0U;
    return HAL_ERROR;
}

uint8_t BL24C16F_GetAddress(void)
{
    return s_device_address;
}

HAL_StatusTypeDef BL24C16F_Read(uint16_t address,
                                uint8_t *data,
                                uint16_t size)
{
    HAL_StatusTypeDef status;
    uint16_t chunk_size;
    uint16_t device_address;

    if ((s_device_address == 0U) || (data == NULL) ||
        (address >= BL24C16F_SIZE_BYTES) ||
        ((uint32_t)size > (BL24C16F_SIZE_BYTES - address)))
    {
        return HAL_ERROR;
    }

    if (size == 0U)
    {
        return HAL_OK;
    }

    /* 2 KB EEPROM 的高 3 位地址由 0x50~0x57 从机地址选择。 */
    while (size > 0U)
    {
        chunk_size = (uint16_t)(256U - (address & 0xFFU));
        if (chunk_size > size)
        {
            chunk_size = size;
        }

        device_address = (uint16_t)(s_device_address + (address >> 8U));
        if (device_address > BL24C16F_LAST_ADDRESS)
        {
            return HAL_ERROR;
        }

        status = HAL_I2C_Mem_Read(&hi2c4,
                                  (uint16_t)(device_address << 1U),
                                  (uint16_t)(address & 0xFFU),
                                  I2C_MEMADD_SIZE_8BIT,
                                  data, chunk_size, BL24C16F_TIMEOUT_MS);
        if (status != HAL_OK)
        {
            return status;
        }

        address += chunk_size;
        data += chunk_size;
        size -= chunk_size;
    }

    return HAL_OK;
}

HAL_StatusTypeDef BL24C16F_Write(uint16_t address,
                                 const uint8_t *data,
                                 uint16_t size)
{
    HAL_StatusTypeDef status;
    uint16_t chunk_size;

    if ((s_device_address == 0U) || (data == NULL) ||
        (address >= BL24C16F_SIZE_BYTES) ||
        ((uint32_t)size > (BL24C16F_SIZE_BYTES - address)))
    {
        return HAL_ERROR;
    }

    while (size > 0U)
    {
        chunk_size = (uint16_t)(BL24C16F_PAGE_SIZE -
                               (address % BL24C16F_PAGE_SIZE));
        if (chunk_size > size)
        {
            chunk_size = size;
        }

        if ((uint16_t)(s_device_address + (address >> 8U)) >
            BL24C16F_LAST_ADDRESS)
        {
            return HAL_ERROR;
        }

        status = HAL_I2C_Mem_Write(&hi2c4,
                                   (uint16_t)((s_device_address +
                                               (address >> 8U)) << 1U),
                                   (uint16_t)(address & 0xFFU),
                                   I2C_MEMADD_SIZE_8BIT,
                                   (uint8_t *)data, chunk_size,
                                   BL24C16F_TIMEOUT_MS);
        if (status != HAL_OK)
        {
            return status;
        }

        HAL_Delay(BL24C16F_WRITE_DELAY_MS);
        address += chunk_size;
        data += chunk_size;
        size -= chunk_size;
    }

    return HAL_OK;
}
