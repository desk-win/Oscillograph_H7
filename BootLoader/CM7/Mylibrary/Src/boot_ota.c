#include "boot_ota.h"

#include <stdint.h>

#include "internal_flash.h"
#include "ota_layout.h"
#include "ota_metadata.h"
#include "ota_package.h"
#include "qspi_flash.h"

/* 当前 CM7 应用栈位于 AXI SRAM 128 KB 内。 */
#define BOOT_OTA_CM7_STACK_START  0x20000000U
#define BOOT_OTA_CM7_STACK_END    0x20020000U
/* 当前 CM4 应用栈位于 D2 SRAM 192 KB 内。 */
#define BOOT_OTA_CM4_STACK_START  0x10000000U
#define BOOT_OTA_CM4_STACK_END    0x10030000U
#define BOOT_OTA_BOOT_ATTEMPT_LIMIT 3U

static const char *s_last_error = "none";
static HAL_StatusTypeDef s_last_status = HAL_OK;

static uint8_t BootOTA_VectorValid(uint32_t stack,
                                   uint32_t reset,
                                   uint32_t flash_start,
                                   uint32_t flash_end,
                                   uint32_t stack_start,
                                   uint32_t stack_end)
{
    return ((stack >= stack_start) && (stack <= stack_end) &&
            ((stack & 0x7U) == 0U) &&
            (reset >= flash_start) && (reset < flash_end) &&
            ((reset & 0x1U) != 0U)) ? 1U : 0U;
}

static uint8_t BootOTA_InternalVectorValid(uint32_t address,
                                           uint32_t flash_end,
                                           uint32_t stack_start,
                                           uint32_t stack_end)
{
    uint32_t stack = *(volatile uint32_t *)address;
    uint32_t reset = *(volatile uint32_t *)(address + 4U);

    return BootOTA_VectorValid(stack, reset, address, flash_end,
                               stack_start, stack_end);
}

static HAL_StatusTypeDef BootOTA_ExternalVectorValid(
    uint32_t qspi_address,
    uint32_t image_size,
    uint32_t internal_start,
    uint32_t internal_end,
    uint32_t stack_start,
    uint32_t stack_end)
{
    uint32_t vectors[2];
    HAL_StatusTypeDef status;

    if (image_size < sizeof(vectors))
    {
        return HAL_ERROR;
    }

    status = QSPI_FLASH_Read(qspi_address, (uint8_t *)vectors,
                             sizeof(vectors));
    if (status != HAL_OK)
    {
        return status;
    }

    return (BootOTA_VectorValid(vectors[0], vectors[1],
                                internal_start, internal_end,
                                stack_start, stack_end) != 0U)
         ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef BootOTA_InstallImage(
    uint32_t qspi_address,
    uint32_t internal_address,
    uint32_t image_size,
    uint32_t expected_crc32,
    uint32_t maximum_size,
    const char *erase_error,
    const char *copy_error,
    const char *verify_error)
{
    uint32_t erase_size;
    HAL_StatusTypeDef status;

    if ((image_size == 0U) || (image_size > maximum_size))
    {
        s_last_error = "image-size";
        s_last_status = HAL_ERROR;
        return HAL_ERROR;
    }

    erase_size = (image_size + INTERNAL_FLASH_SECTOR_SIZE - 1U) &
                 ~(INTERNAL_FLASH_SECTOR_SIZE - 1U);

    s_last_error = erase_error;
    status = InternalFlash_EraseRange(internal_address, erase_size);
    if (status != HAL_OK)
    {
        s_last_status = status;
        return status;
    }

    s_last_error = copy_error;
    status = InternalFlash_CopyFromQspi(qspi_address, internal_address,
                                        image_size);
    if (status != HAL_OK)
    {
        s_last_status = status;
        return status;
    }

    s_last_error = verify_error;
    status = InternalFlash_VerifyCrc32(internal_address, image_size,
                                       expected_crc32);
    if (status != HAL_OK)
    {
        s_last_status = status;
        return status;
    }

    return HAL_OK;
}

/**
 * @brief 外部包无效时清除包头并改回 PENDING，允许应用重新上传。
 *
 * 此时绝不擦除内部 Flash，旧应用仍可启动。
 */
static BootOTA_Result BootOTA_RejectPackage(OTA_Metadata *metadata)
{
    OTA_SlotId slot = (OTA_SlotId)metadata->pending_slot;

    s_last_error = "package-reject";
    s_last_status = HAL_ERROR;
    (void)OTA_Package_InvalidateSlot(slot);
    metadata->pending_state = OTA_SLOT_STATE_PENDING;
    metadata->confirmed = 0U;
    metadata->boot_attempts = 0U;

    return (OTA_Metadata_Commit(metadata) == HAL_OK)
         ? BOOT_OTA_PACKAGE_INVALID
         : BOOT_OTA_FAILED;
}

static BootOTA_Result BootOTA_HandleUnconfirmedBoot(OTA_Metadata *metadata)
{
    OTA_PackageHeader header;
    OTA_SlotId active_slot = (OTA_SlotId)metadata->active_slot;
    OTA_SlotId rollback_slot;

    if ((active_slot != OTA_SLOT_ID_A) && (active_slot != OTA_SLOT_ID_B))
    {
        s_last_error = "active-slot";
        s_last_status = HAL_ERROR;
        return BOOT_OTA_FAILED;
    }

    if (metadata->boot_attempts >= BOOT_OTA_BOOT_ATTEMPT_LIMIT)
    {
        rollback_slot = (active_slot == OTA_SLOT_ID_A)
                      ? OTA_SLOT_ID_B : OTA_SLOT_ID_A;

        /* 回滚前先验证旧槽，避免把无效旧包排队安装。 */
        if (OTA_Package_VerifySlot(rollback_slot, &header) != HAL_OK)
        {
            s_last_error = "rollback-package";
            s_last_status = HAL_ERROR;
            return BOOT_OTA_FAILED;
        }

        metadata->pending_slot = (uint8_t)rollback_slot;
        metadata->pending_state = OTA_SLOT_STATE_VALID;
        metadata->confirmed = 1U;
        metadata->boot_attempts = 0U;
        if (OTA_Metadata_Commit(metadata) != HAL_OK)
        {
            s_last_error = "rollback-queue";
            s_last_status = HAL_ERROR;
            return BOOT_OTA_FAILED;
        }

        /* 当前运行中先用复位返回 BootLoader，下一次走正常安装流程。 */
        s_last_error = "rollback-queued";
        return BOOT_OTA_ROLLBACK_QUEUED;
    }

    metadata->boot_attempts++;
    if (OTA_Metadata_Commit(metadata) != HAL_OK)
    {
        s_last_error = "boot-attempt";
        s_last_status = HAL_ERROR;
        return BOOT_OTA_FAILED;
    }

    return BOOT_OTA_NO_UPDATE;
}

BootOTA_Result BootOTA_Process(void)
{
    OTA_Metadata metadata;
    OTA_PackageHeader header;
    OTA_SlotId slot;
    uint32_t slot_base;
    uint8_t found = 0U;

    s_last_error = "none";
    s_last_status = HAL_OK;
    if ((OTA_Metadata_SelfTest() != HAL_OK) ||
        (OTA_Package_SelfTest() != HAL_OK))
    {
        s_last_error = "selftest";
        s_last_status = HAL_ERROR;
        return BOOT_OTA_FAILED;
    }

    if (OTA_Metadata_Load(&metadata, &found) != HAL_OK)
    {
        s_last_error = "metadata-load";
        s_last_status = HAL_ERROR;
        return BOOT_OTA_NO_UPDATE;
    }
    if (found == 0U)
    {
        return BOOT_OTA_NO_UPDATE;
    }
    if ((metadata.pending_state == OTA_SLOT_STATE_ACTIVE) &&
        (metadata.confirmed == 0U))
    {
        return BootOTA_HandleUnconfirmedBoot(&metadata);
    }
    if (metadata.pending_state != OTA_SLOT_STATE_VALID)
    {
        return BOOT_OTA_NO_UPDATE;
    }

    slot = (OTA_SlotId)metadata.pending_slot;
    if ((slot != OTA_SLOT_ID_A) && (slot != OTA_SLOT_ID_B))
    {
        s_last_error = "pending-slot";
        s_last_status = HAL_ERROR;
        return BootOTA_RejectPackage(&metadata);
    }

    /* 先完整验证外部包头、偏移、长度、CRC32 和两个入口向量。 */
    if (OTA_Package_VerifySlot(slot, &header) != HAL_OK)
    {
        s_last_error = "package-header-or-crc";
        s_last_status = HAL_ERROR;
        return BootOTA_RejectPackage(&metadata);
    }

    slot_base = OTA_Package_SlotBase(slot);
    if ((BootOTA_ExternalVectorValid(slot_base + header.cm7_offset,
                                     header.cm7_size,
                                     OTA_INTERNAL_CM7_BASE,
                                     OTA_INTERNAL_CM7_BASE +
                                         OTA_INTERNAL_CM7_SIZE,
                                     BOOT_OTA_CM7_STACK_START,
                                     BOOT_OTA_CM7_STACK_END) != HAL_OK) ||
        (BootOTA_ExternalVectorValid(slot_base + header.cm4_offset,
                                     header.cm4_size,
                                     OTA_INTERNAL_CM4_BASE,
                                     OTA_INTERNAL_CM4_BASE +
                                         OTA_INTERNAL_CM4_SIZE,
                                     BOOT_OTA_CM4_STACK_START,
                                     BOOT_OTA_CM4_STACK_END) != HAL_OK))
    {
        s_last_error = "image-vector";
        s_last_status = HAL_ERROR;
        return BootOTA_RejectPackage(&metadata);
    }

    /* 到这里外部包已完整验证，才开始修改当前可运行的内部 Flash。 */
    if (BootOTA_InstallImage(slot_base + header.cm7_offset,
                             OTA_INTERNAL_CM7_BASE,
                             header.cm7_size,
                             header.cm7_crc32,
                             OTA_INTERNAL_CM7_SIZE,
                             "cm7-erase",
                             "cm7-copy",
                             "cm7-verify") != HAL_OK)
    {
        return BOOT_OTA_FAILED;
    }
    if (BootOTA_InternalVectorValid(OTA_INTERNAL_CM7_BASE,
                                    OTA_INTERNAL_CM7_BASE +
                                        OTA_INTERNAL_CM7_SIZE,
                                    BOOT_OTA_CM7_STACK_START,
                                    BOOT_OTA_CM7_STACK_END) == 0U)
    {
        s_last_error = "cm7-vector";
        s_last_status = HAL_ERROR;
        return BOOT_OTA_FAILED;
    }

    /*
     * 系统复位后 CM4 启动代码会先停在 STOP/WFE 等待 HSEM0。
     * BootLoader 从不释放 HSEM0，所以此处 CM4 不会执行 CM4 应用区。
     * 阶段六会把 BCM4 改为 Disable，再由 CM7 应用显式释放 CM4。
     */
    if (BootOTA_InstallImage(slot_base + header.cm4_offset,
                             OTA_INTERNAL_CM4_BASE,
                             header.cm4_size,
                             header.cm4_crc32,
                             OTA_INTERNAL_CM4_SIZE,
                             "cm4-erase",
                             "cm4-copy",
                             "cm4-verify") != HAL_OK)
    {
        return BOOT_OTA_FAILED;
    }
    if (BootOTA_InternalVectorValid(OTA_INTERNAL_CM4_BASE,
                                    OTA_INTERNAL_CM4_BASE +
                                        OTA_INTERNAL_CM4_SIZE,
                                    BOOT_OTA_CM4_STACK_START,
                                    BOOT_OTA_CM4_STACK_END) == 0U)
    {
        s_last_error = "cm4-vector";
        s_last_status = HAL_ERROR;
        return BOOT_OTA_FAILED;
    }

    metadata.active_slot = (uint8_t)slot;
    metadata.pending_slot = (uint8_t)slot;
    metadata.pending_state = OTA_SLOT_STATE_ACTIVE;
    metadata.boot_attempts = 0U;

    if (OTA_Metadata_Commit(&metadata) != HAL_OK)
    {
        s_last_error = "metadata-active";
        s_last_status = HAL_ERROR;
        return BOOT_OTA_FAILED;
    }

    return BOOT_OTA_INSTALLED;
}

const char *BootOTA_LastError(void)
{
    return s_last_error;
}

HAL_StatusTypeDef BootOTA_LastStatus(void)
{
    return s_last_status;
}

void BootOTA_LastFlashError(uint32_t *bank, uint32_t *flags)
{
    InternalFlash_GetLastError(bank, flags);
}
