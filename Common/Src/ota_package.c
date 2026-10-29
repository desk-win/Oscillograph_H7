#include "ota_package.h"

#include <stddef.h>

#include "crc32.h"
#include "qspi_flash.h"

uint32_t OTA_Package_SlotBase(OTA_SlotId slot)
{
    if (slot == OTA_SLOT_ID_A)
    {
        return OTA_EXTERNAL_SLOT_A_BASE;
    }
    if (slot == OTA_SLOT_ID_B)
    {
        return OTA_EXTERNAL_SLOT_B_BASE;
    }
    return 0U;
}

uint32_t OTA_Package_HeaderCrc32(const OTA_PackageHeader *header)
{
    return CRC32_Calculate((const uint8_t *)header,
                           (uint32_t)offsetof(OTA_PackageHeader,
                                              header_crc32));
}

uint8_t OTA_Package_HeaderValid(const OTA_PackageHeader *header,
                                OTA_SlotId expected_slot)
{
    if ((header == NULL) ||
        ((expected_slot != OTA_SLOT_ID_A) &&
         (expected_slot != OTA_SLOT_ID_B)) ||
        (header->magic != OTA_PACKAGE_MAGIC) ||
        (header->version != OTA_PACKAGE_VERSION) ||
        (header->header_size != sizeof(OTA_PackageHeader)) ||
        (header->slot != expected_slot) ||
        (header->cm7_offset != OTA_EXTERNAL_CM7_OFFSET) ||
        (header->cm4_offset != OTA_EXTERNAL_CM4_OFFSET) ||
        (header->cm7_size < 8U) ||
        (header->cm7_size > OTA_EXTERNAL_CM7_MAX_SIZE) ||
        (header->cm4_size < 8U) ||
        (header->cm4_size > OTA_EXTERNAL_CM4_MAX_SIZE) ||
        ((header->cm7_offset + header->cm7_size) > header->cm4_offset) ||
        ((header->cm4_offset + header->cm4_size) >
         OTA_EXTERNAL_RESERVED_OFFSET))
    {
        return 0U;
    }

    return (OTA_Package_HeaderCrc32(header) == header->header_crc32)
         ? 1U : 0U;
}

HAL_StatusTypeDef OTA_Package_VerifySlot(OTA_SlotId slot,
                                         OTA_PackageHeader *header)
{
    OTA_PackageHeader readback;
    uint32_t slot_base;
    uint32_t image_crc;
    HAL_StatusTypeDef status;

    if ((header == NULL) ||
        ((slot != OTA_SLOT_ID_A) && (slot != OTA_SLOT_ID_B)))
    {
        return HAL_ERROR;
    }

    status = QSPI_FLASH_Init();
    if (status != HAL_OK)
    {
        return status;
    }

    slot_base = OTA_Package_SlotBase(slot);
    status = QSPI_FLASH_Read(slot_base + OTA_PACKAGE_HEADER_OFFSET,
                             (uint8_t *)&readback,
                             sizeof(readback));
    if (status != HAL_OK)
    {
        return status;
    }
    if (OTA_Package_HeaderValid(&readback, slot) == 0U)
    {
        return HAL_ERROR;
    }

    status = QSPI_FLASH_CalculateCrc32(slot_base + readback.cm7_offset,
                                       readback.cm7_size,
                                       &image_crc);
    if ((status != HAL_OK) || (image_crc != readback.cm7_crc32))
    {
        return HAL_ERROR;
    }

    status = QSPI_FLASH_CalculateCrc32(slot_base + readback.cm4_offset,
                                       readback.cm4_size,
                                       &image_crc);
    if ((status != HAL_OK) || (image_crc != readback.cm4_crc32))
    {
        return HAL_ERROR;
    }

    *header = readback;
    return HAL_OK;
}

HAL_StatusTypeDef OTA_Package_InvalidateSlot(OTA_SlotId slot)
{
    if ((slot != OTA_SLOT_ID_A) && (slot != OTA_SLOT_ID_B))
    {
        return HAL_ERROR;
    }

    return QSPI_FLASH_EraseSector4KB(OTA_Package_SlotBase(slot) +
                                     OTA_PACKAGE_HEADER_OFFSET);
}

HAL_StatusTypeDef OTA_Package_SelfTest(void)
{
    OTA_PackageHeader header = {0};

    header.magic = OTA_PACKAGE_MAGIC;
    header.version = OTA_PACKAGE_VERSION;
    header.header_size = sizeof(header);
    header.slot = OTA_SLOT_ID_A;
    header.cm7_offset = OTA_EXTERNAL_CM7_OFFSET;
    header.cm7_size = 8U;
    header.cm4_offset = OTA_EXTERNAL_CM4_OFFSET;
    header.cm4_size = 8U;
    header.header_crc32 = OTA_Package_HeaderCrc32(&header);

    if (OTA_Package_HeaderValid(&header, OTA_SLOT_ID_A) == 0U)
    {
        return HAL_ERROR;
    }
    if (OTA_Package_HeaderValid(&header, OTA_SLOT_ID_B) != 0U)
    {
        return HAL_ERROR;
    }

    header.cm4_size++;
    return (OTA_Package_HeaderValid(&header, OTA_SLOT_ID_A) == 0U)
         ? HAL_OK : HAL_ERROR;
}
