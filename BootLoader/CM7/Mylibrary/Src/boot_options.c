#include "boot_options.h"

#include "ota_layout.h"

static HAL_StatusTypeDef s_last_status = HAL_OK;


// bcm4_enabled为0时，BCM4 关闭，CM4 由 CM7 显式启动
HAL_StatusTypeDef BootOptions_GetCM4Info(BootOptions_CM4Info *info)
{
    FLASH_OBProgramInitTypeDef options = {0};

    if (info == NULL)
    {
        return HAL_ERROR;
    }

    options.Banks = FLASH_BANK_1;
    HAL_FLASHEx_OBGetConfig(&options);
    info->bcm4_enabled =
        ((options.USERConfig & FLASH_OPTSR_BCM4) != 0U) ? 1U : 0U;
    info->cm4_boot_addr0 = options.CM4BootAddr0;
    return HAL_OK;
}

BootOptions_Result BootOptions_EnsureCM4Held(BootOptions_CM4Info *info)
{
    FLASH_OBProgramInitTypeDef options = {0};
    FLASH_OBProgramInitTypeDef readback = {0};
    HAL_StatusTypeDef status;

    if (info == NULL)
    {
        s_last_status = HAL_ERROR;
        return BOOT_OPTIONS_ERROR;
    }

    if (BootOptions_GetCM4Info(info) != HAL_OK)
    {
        s_last_status = HAL_ERROR;
        return BOOT_OPTIONS_ERROR;
    }
    if (info->bcm4_enabled == 0U)
    {
        s_last_status = HAL_OK;
        return BOOT_OPTIONS_ALREADY_DISABLED;
    }

    /* 地址不正确时绝不单独关闭 BCM4，避免 CM4 永远无法由 CM7 启动。 */
    if (info->cm4_boot_addr0 != OTA_INTERNAL_CM4_BASE)
    {
        s_last_status = HAL_ERROR;
        return BOOT_OPTIONS_BAD_ADDRESS;
    }

    options.OptionType = OPTIONBYTE_USER;
    options.USERType = OB_USER_BCM4;
    options.USERConfig = OB_BCM4_DISABLE;

    status = HAL_FLASH_OB_Unlock();
    if (status == HAL_OK)
    {
        status = HAL_FLASHEx_OBProgram(&options);
    }
    if (status == HAL_OK)
    {
        status = HAL_FLASH_OB_Launch();
    }
    (void)HAL_FLASH_OB_Lock();

    if (status != HAL_OK)
    {
        s_last_status = status;
        return BOOT_OPTIONS_ERROR;
    }

    readback.Banks = FLASH_BANK_1;
    HAL_FLASHEx_OBGetConfig(&readback);
    if ((readback.USERConfig & FLASH_OPTSR_BCM4) != 0U)
    {
        s_last_status = HAL_ERROR;
        return BOOT_OPTIONS_ERROR;
    }

    s_last_status = HAL_OK;
    return BOOT_OPTIONS_PROGRAMMED;
}

HAL_StatusTypeDef BootOptions_LastStatus(void)
{
    return s_last_status;
}
