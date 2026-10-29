#include "ota_metadata.h"

#include <stddef.h>
#include <string.h>

#include "bl24c16f.h"
#include "crc32.h"

#define OTA_METADATA_COPY_COUNT 2U

static const uint16_t s_copy_offsets[OTA_METADATA_COPY_COUNT] =
{
    OTA_EEPROM_METADATA_COPY0_OFFSET,
    OTA_EEPROM_METADATA_COPY1_OFFSET
};

static HAL_StatusTypeDef OTA_Metadata_EnsureEeprom(void)
{
    return (BL24C16F_GetAddress() != 0U)
         ? HAL_OK
         : BL24C16F_Init();
}

static uint32_t OTA_Metadata_RecordCrc(const OTA_Metadata *metadata)
{
    return CRC32_Calculate((const uint8_t *)metadata,
                           (uint32_t)offsetof(OTA_Metadata, record_crc32));
}

static uint8_t OTA_Metadata_FieldsValid(const OTA_Metadata *metadata)
{
    if (metadata == NULL ||
        metadata->active_slot > OTA_SLOT_ID_B ||
        metadata->pending_slot > OTA_SLOT_ID_B ||
        metadata->pending_state > OTA_SLOT_STATE_ACTIVE ||
        metadata->confirmed > 1U)
    {
        return 0U;
    }

    if ((metadata->pending_state != OTA_SLOT_STATE_EMPTY) &&
        (metadata->pending_slot == OTA_SLOT_ID_NONE))
    {
        return 0U;
    }

    return 1U;
}

static uint8_t OTA_Metadata_RecordValid(const OTA_Metadata *metadata)
{
    if ((metadata == NULL) ||
        (metadata->magic != OTA_METADATA_MAGIC) ||
        (metadata->version != OTA_METADATA_VERSION) ||
        (metadata->record_size != sizeof(OTA_Metadata)) ||
        (OTA_Metadata_FieldsValid(metadata) == 0U))
    {
        return 0U;
    }

    return (OTA_Metadata_RecordCrc(metadata) ==
            metadata->record_crc32) ? 1U : 0U;
}

static HAL_StatusTypeDef OTA_Metadata_ReadCopies(
    OTA_Metadata copies[OTA_METADATA_COPY_COUNT],
    uint8_t valid[OTA_METADATA_COPY_COUNT],
    uint8_t readable[OTA_METADATA_COPY_COUNT])
{
    HAL_StatusTypeDef status;

    status = OTA_Metadata_EnsureEeprom();
    if (status != HAL_OK)
    {
        return status;
    }

    for (uint32_t i = 0U; i < OTA_METADATA_COPY_COUNT; i++)
    {
        readable[i] = (BL24C16F_Read(s_copy_offsets[i],
                                     (uint8_t *)&copies[i],
                                     sizeof(copies[i])) == HAL_OK) ? 1U : 0U;
        valid[i] = (readable[i] != 0U &&
                    OTA_Metadata_RecordValid(&copies[i]) != 0U) ? 1U : 0U;
    }

    return HAL_OK;
}

static uint8_t OTA_Metadata_FindNewest(
    const OTA_Metadata copies[OTA_METADATA_COPY_COUNT],
    const uint8_t valid[OTA_METADATA_COPY_COUNT],
    uint32_t *newest_index)
{
    if ((copies == NULL) || (valid == NULL) || (newest_index == NULL))
    {
        return 0U;
    }

    if ((valid[0] == 0U) && (valid[1] == 0U))
    {
        return 0U;
    }

    *newest_index = ((valid[1] != 0U) &&
                     ((valid[0] == 0U) ||
                      (copies[1].sequence > copies[0].sequence))) ? 1U : 0U;
    return 1U;
}

static void OTA_Metadata_Finalize(OTA_Metadata *metadata,
                                  uint32_t sequence)
{
    metadata->magic = OTA_METADATA_MAGIC;
    metadata->version = OTA_METADATA_VERSION;
    metadata->record_size = sizeof(OTA_Metadata);
    metadata->sequence = sequence;
    memset(metadata->reserved, 0, sizeof(metadata->reserved));
    metadata->record_crc32 = OTA_Metadata_RecordCrc(metadata);
}

HAL_StatusTypeDef OTA_Metadata_Load(OTA_Metadata *metadata, uint8_t *found)
{
    OTA_Metadata copies[OTA_METADATA_COPY_COUNT];
    uint8_t valid[OTA_METADATA_COPY_COUNT];
    uint8_t readable[OTA_METADATA_COPY_COUNT];
    HAL_StatusTypeDef status;
    uint32_t selected;

    if ((metadata == NULL) || (found == NULL))
    {
        return HAL_ERROR;
    }

    memset(copies, 0, sizeof(copies));
    status = OTA_Metadata_ReadCopies(copies, valid, readable);
    if (status != HAL_OK)
    {
        return status;
    }

    if (OTA_Metadata_FindNewest(copies, valid, &selected) == 0U)
    {
        memset(metadata, 0, sizeof(*metadata));
        *found = 0U;
        return ((readable[0] != 0U) || (readable[1] != 0U))
             ? HAL_OK
             : HAL_ERROR;
    }

    *metadata = copies[selected];
    *found = 1U;
    return HAL_OK;
}

HAL_StatusTypeDef OTA_Metadata_Commit(OTA_Metadata *metadata)
{
    OTA_Metadata copies[OTA_METADATA_COPY_COUNT];
    OTA_Metadata prepared;
    OTA_Metadata readback;
    uint8_t valid[OTA_METADATA_COPY_COUNT];
    uint8_t readable[OTA_METADATA_COPY_COUNT];
    uint32_t latest_sequence = 0U;
    uint32_t newest_index;
    uint32_t write_index;
    HAL_StatusTypeDef status;

    if ((metadata == NULL) || (OTA_Metadata_FieldsValid(metadata) == 0U))
    {
        return HAL_ERROR;
    }

    memset(copies, 0, sizeof(copies));
    status = OTA_Metadata_ReadCopies(copies, valid, readable);
    if (status != HAL_OK)
    {
        return status;
    }
    if ((valid[0] == 0U) && (valid[1] == 0U) &&
        ((readable[0] == 0U) || (readable[1] == 0U)))
    {
        return HAL_ERROR;
    }

    if (OTA_Metadata_FindNewest(copies, valid, &newest_index) != 0U)
    {
        latest_sequence = copies[newest_index].sequence;
        write_index = newest_index ^ 1U;
    }
    else
    {
        write_index = 0U;
    }

    prepared = *metadata;
    OTA_Metadata_Finalize(&prepared, latest_sequence + 1U);
    if (OTA_Metadata_RecordValid(&prepared) == 0U)
    {
        return HAL_ERROR;
    }

    status = BL24C16F_Write(s_copy_offsets[write_index],
                            (const uint8_t *)&prepared,
                            sizeof(prepared));
    if (status != HAL_OK)
    {
        return status;
    }

    status = BL24C16F_Read(s_copy_offsets[write_index],
                           (uint8_t *)&readback,
                           sizeof(readback));
    if (status != HAL_OK)
    {
        return status;
    }
    if ((OTA_Metadata_RecordValid(&readback) == 0U) ||
        (memcmp(&prepared, &readback, sizeof(prepared)) != 0))
    {
        return HAL_ERROR;
    }

    *metadata = prepared;
    return HAL_OK;
}

OTA_SlotId OTA_Metadata_SelectTargetSlot(const OTA_Metadata *metadata,
                                         uint8_t found)
{
    if ((found == 0U) || (metadata == NULL) ||
        (metadata->active_slot == OTA_SLOT_ID_NONE &&
         metadata->pending_state == OTA_SLOT_STATE_EMPTY))
    {
        return OTA_SLOT_ID_A;
    }

    if (metadata->pending_state == OTA_SLOT_STATE_PENDING)
    {
        return (metadata->pending_slot != metadata->active_slot &&
                metadata->pending_slot != OTA_SLOT_ID_NONE)
             ? (OTA_SlotId)metadata->pending_slot
             : OTA_SLOT_ID_NONE;
    }
    if (metadata->pending_state == OTA_SLOT_STATE_VALID)
    {
        return OTA_SLOT_ID_NONE;
    }
    if (metadata->active_slot == OTA_SLOT_ID_A)
    {
        return OTA_SLOT_ID_B;
    }
    if (metadata->active_slot == OTA_SLOT_ID_B)
    {
        return OTA_SLOT_ID_A;
    }

    return OTA_SLOT_ID_A;
}

HAL_StatusTypeDef OTA_Metadata_SelfTest(void)
{
    OTA_Metadata metadata = {0};
    OTA_Metadata copies[OTA_METADATA_COPY_COUNT] = {0};
    uint8_t valid[OTA_METADATA_COPY_COUNT] = {0};
    uint32_t newest_index = 0U;

    copies[0].sequence = 5U;
    valid[0] = 1U;
    if (OTA_Metadata_FindNewest(copies, valid, &newest_index) == 0U ||
        newest_index != 0U)
    {
        return HAL_ERROR;
    }

    copies[1].sequence = 6U;
    valid[1] = 1U;
    if (OTA_Metadata_FindNewest(copies, valid, &newest_index) == 0U ||
        newest_index != 1U)
    {
        return HAL_ERROR;
    }

    if (OTA_Metadata_SelectTargetSlot(&metadata, 0U) != OTA_SLOT_ID_A)
    {
        return HAL_ERROR;
    }

    metadata.active_slot = OTA_SLOT_ID_A;
    metadata.pending_slot = OTA_SLOT_ID_B;
    metadata.pending_state = OTA_SLOT_STATE_EMPTY;
    OTA_Metadata_Finalize(&metadata, 1U);
    if (OTA_Metadata_RecordValid(&metadata) == 0U ||
        OTA_Metadata_SelectTargetSlot(&metadata, 1U) != OTA_SLOT_ID_B)
    {
        return HAL_ERROR;
    }

    metadata.active_slot = OTA_SLOT_ID_B;
    metadata.pending_slot = OTA_SLOT_ID_A;
    metadata.pending_state = OTA_SLOT_STATE_EMPTY;
    OTA_Metadata_Finalize(&metadata, 2U);
    if (OTA_Metadata_SelectTargetSlot(&metadata, 1U) != OTA_SLOT_ID_A)
    {
        return HAL_ERROR;
    }

    metadata.active_slot = OTA_SLOT_ID_A;
    metadata.pending_slot = OTA_SLOT_ID_B;
    metadata.pending_state = OTA_SLOT_STATE_PENDING;
    OTA_Metadata_Finalize(&metadata, 3U);
    if (OTA_Metadata_SelectTargetSlot(&metadata, 1U) != OTA_SLOT_ID_B)
    {
        return HAL_ERROR;
    }

    metadata.pending_state = OTA_SLOT_STATE_VALID;
    OTA_Metadata_Finalize(&metadata, 4U);
    if (OTA_Metadata_SelectTargetSlot(&metadata, 1U) != OTA_SLOT_ID_NONE)
    {
        return HAL_ERROR;
    }

    metadata.record_crc32 ^= 1U;
    return (OTA_Metadata_RecordValid(&metadata) == 0U) ? HAL_OK : HAL_ERROR;
}
