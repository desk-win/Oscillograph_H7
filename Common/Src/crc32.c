/**
 * @file crc32.c
 * @brief CRC-32/ISO-HDLC 流式实现，供 CM7 应用和 BootLoader 共用。
 */

#include "crc32.h"

#include <stddef.h>

/* 标准 CRC32 初值为全 1，最终结果再按位取反。 */
uint32_t CRC32_Init(void)
{
    return 0xFFFFFFFFU;
}

uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t size)
{
    /* 输入非法时保持原中间值，避免空指针且长度为 0 的合法调用被误判。 */
    if ((data == NULL) && (size != 0U))
    {
        return crc;
    }

    for (uint32_t i = 0U; i < size; i++)
    {
        crc ^= data[i];
        /* 使用反射多项式 0xEDB88320 逐位计算，避免占用查表内存。 */
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
    /* "123456789" 是 CRC-32/ISO-HDLC 的标准校验向量。 */
    static const uint8_t test_data[] = "123456789";
    const uint32_t expected = 0xCBF43926U;

    return (CRC32_Calculate(test_data, sizeof(test_data) - 1U) == expected)
         ? HAL_OK : HAL_ERROR;
}
