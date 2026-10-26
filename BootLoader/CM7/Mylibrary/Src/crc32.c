#include "crc32.h"

#include <stddef.h>
#include <string.h>

uint32_t CRC32_Init(void)
{
    return 0xFFFFFFFFU;
}

uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t size)
{
    if ((data == NULL) && (size != 0U))
    {
        return crc;
    }

    for (uint32_t i = 0U; i < size; i++)
    {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ 0xEDB88320U)
                                     : (crc >> 1U);
        }
    }

    return crc;
}

uint32_t CRC32_Finalize(uint32_t crc)
{
    return ~crc;
}

uint32_t CRC32_Calculate(const uint8_t *data, uint32_t size)
{
    return CRC32_Finalize(CRC32_Update(CRC32_Init(), data, size));
}

HAL_StatusTypeDef CRC32_SelfTest(void)
{
    static const uint8_t test_data[] = "123456789";
    const uint32_t expected = 0xCBF43926U;

    return (CRC32_Calculate(test_data, sizeof(test_data) - 1U) == expected)
         ? HAL_OK : HAL_ERROR;
}
