#include "internal_flash.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"
#include "qspi_flash.h"

#define INTERNAL_FLASH_RAMFUNC \
    __attribute__((section(".RamFunc"), noinline))
#define INTERNAL_FLASH_INLINE \
    static inline __attribute__((always_inline))

#define INTERNAL_FLASH_ERRORS_BANK1 \
    (FLASH_FLAG_ALL_ERRORS_BANK1 & 0x7FFFFFFFU)
#define INTERNAL_FLASH_ERRORS_BANK2 \
    (FLASH_FLAG_ALL_ERRORS_BANK2 & 0x7FFFFFFFU)

#define INTERNAL_FLASH_CRC_CHUNK_SIZE      1024U
#define INTERNAL_FLASH_ERASE_TIMEOUT_MS    5000U
#define INTERNAL_FLASH_PROGRAM_TIMEOUT_MS  1000U
#define INTERNAL_FLASH_READY_TIMEOUT_MS    100U

_Static_assert(((INTERNAL_FLASH_M7_APP_END - INTERNAL_FLASH_M7_APP_START) %
                INTERNAL_FLASH_SECTOR_SIZE) == 0U,
               "M7 application slot must be sector aligned");
_Static_assert(((INTERNAL_FLASH_M4_APP_END - INTERNAL_FLASH_M4_APP_START) %
                INTERNAL_FLASH_SECTOR_SIZE) == 0U,
               "M4 application slot must be sector aligned");

static uint32_t s_flash_word[INTERNAL_FLASH_WORD_SIZE / sizeof(uint32_t)]
    __attribute__((aligned(INTERNAL_FLASH_WORD_SIZE)));
static uint32_t s_qspi_word[INTERNAL_FLASH_WORD_SIZE / sizeof(uint32_t)]
    __attribute__((aligned(INTERNAL_FLASH_WORD_SIZE)));
static uint8_t s_crc_buffer[INTERNAL_FLASH_CRC_CHUNK_SIZE];

INTERNAL_FLASH_INLINE uint32_t InternalFlash_Bank(uint32_t address)
{
    if ((address >= FLASH_BANK1_BASE) &&
        (address < (FLASH_BANK1_BASE + FLASH_BANK_SIZE)))
    {
        return FLASH_BANK_1;
    }
    if ((address >= FLASH_BANK2_BASE) &&
        (address < (FLASH_BANK2_BASE + FLASH_BANK_SIZE)))
    {
        return FLASH_BANK_2;
    }
    return 0U;
}

INTERNAL_FLASH_INLINE uint32_t InternalFlash_Sector(uint32_t address)
{
    uint32_t bank_base = (address < FLASH_BANK2_BASE)
                       ? FLASH_BANK1_BASE
                       : FLASH_BANK2_BASE;

    return (address - bank_base) / FLASH_SECTOR_SIZE;
}

INTERNAL_FLASH_INLINE uint32_t InternalFlash_Status(uint32_t bank)
{
    return (bank == FLASH_BANK_1) ? FLASH->SR1 : FLASH->SR2;
}

INTERNAL_FLASH_INLINE uint32_t InternalFlash_Control(uint32_t bank)
{
    return (bank == FLASH_BANK_1) ? FLASH->CR1 : FLASH->CR2;
}

INTERNAL_FLASH_INLINE void InternalFlash_SetControl(uint32_t bank,
                                                    uint32_t value)
{
    if (bank == FLASH_BANK_1)
    {
        FLASH->CR1 = value;
    }
    else
    {
        FLASH->CR2 = value;
    }
}

INTERNAL_FLASH_INLINE void InternalFlash_ClearFlags(uint32_t bank)
{
    if (bank == FLASH_BANK_1)
    {
        __HAL_FLASH_CLEAR_FLAG_BANK1(INTERNAL_FLASH_ERRORS_BANK1 |
                                     FLASH_FLAG_EOP_BANK1);
    }
    else
    {
        __HAL_FLASH_CLEAR_FLAG_BANK2(INTERNAL_FLASH_ERRORS_BANK2 |
                                     FLASH_FLAG_EOP_BANK2);
    }
}

static HAL_StatusTypeDef INTERNAL_FLASH_RAMFUNC
InternalFlash_WaitReady(uint32_t bank, uint32_t timeout_ms)
{
    uint32_t last;
    uint32_t now;
    uint32_t elapsed_ms = 0U;
    uint32_t status;

    if ((bank == 0U) || (SysTick->LOAD == 0U))
    {
        return HAL_ERROR;
    }

    last = SysTick->VAL;
    while ((InternalFlash_Status(bank) & FLASH_SR_QW) != 0U)
    {
        now = SysTick->VAL;
        if (now <= last)
        {
            elapsed_ms++;
        }
        last = now;

        if (elapsed_ms >= timeout_ms)
        {
            return HAL_TIMEOUT;
        }
    }

    status = InternalFlash_Status(bank);
    if ((status & ((bank == FLASH_BANK_1) ? INTERNAL_FLASH_ERRORS_BANK1
                                          : INTERNAL_FLASH_ERRORS_BANK2)) != 0U)
    {
        InternalFlash_ClearFlags(bank);
        return HAL_ERROR;
    }

    InternalFlash_ClearFlags(bank);
    return HAL_OK;
}

static HAL_StatusTypeDef INTERNAL_FLASH_RAMFUNC
InternalFlash_EraseSector(uint32_t address)
{
    HAL_StatusTypeDef status;
    uint32_t bank = InternalFlash_Bank(address);
    uint32_t sector = InternalFlash_Sector(address);
    uint32_t control;

    if (bank == 0U)
    {
        return HAL_ERROR;
    }

    status = InternalFlash_WaitReady(bank, INTERNAL_FLASH_READY_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }

    control = InternalFlash_Control(bank);
    control &= ~(FLASH_CR_PG | FLASH_CR_SER | FLASH_CR_SNB |
                 FLASH_CR_PSIZE);
    control |= FLASH_VOLTAGE_RANGE_1 |
               (sector << FLASH_CR_SNB_Pos) |
               FLASH_CR_SER |
               FLASH_CR_START;
    InternalFlash_SetControl(bank, control);

    __DSB();
    __ISB();

    status = InternalFlash_WaitReady(bank, INTERNAL_FLASH_ERASE_TIMEOUT_MS);

    control = InternalFlash_Control(bank);
    control &= ~(FLASH_CR_SER | FLASH_CR_SNB);
    InternalFlash_SetControl(bank, control);

    return status;
}

static HAL_StatusTypeDef INTERNAL_FLASH_RAMFUNC
InternalFlash_ProgramWord(uint32_t address, const uint32_t *data)
{
    HAL_StatusTypeDef status;
    volatile uint32_t *destination = (volatile uint32_t *)address;
    uint32_t bank = InternalFlash_Bank(address);
    uint32_t control;

    if ((bank == 0U) || (((uint32_t)data & 3U) != 0U))
    {
        return HAL_ERROR;
    }

    status = InternalFlash_WaitReady(bank, INTERNAL_FLASH_READY_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }

    control = InternalFlash_Control(bank);
    control &= ~(FLASH_CR_SER | FLASH_CR_SNB | FLASH_CR_PSIZE |
                 FLASH_CR_START);
    control |= FLASH_VOLTAGE_RANGE_1 | FLASH_CR_PG;
    InternalFlash_SetControl(bank, control);

    __DSB();
    __ISB();

    for (uint32_t i = 0U;
         i < (INTERNAL_FLASH_WORD_SIZE / sizeof(uint32_t));
         i++)
    {
        destination[i] = data[i];
    }

    __DSB();
    __ISB();

    status = InternalFlash_WaitReady(bank, INTERNAL_FLASH_PROGRAM_TIMEOUT_MS);

    control = InternalFlash_Control(bank);
    control &= ~FLASH_CR_PG;
    InternalFlash_SetControl(bank, control);

    return status;
}

static uint8_t InternalFlash_IsAllowedRange(uint32_t address, uint32_t size)
{
    if ((size == 0U) || (address > (0xFFFFFFFFU - size)))
    {
        return 0U;
    }

    return (((address >= INTERNAL_FLASH_M7_APP_START) &&
             ((address + size) <= INTERNAL_FLASH_M7_APP_END)) ||
            ((address >= INTERNAL_FLASH_M4_APP_START) &&
             ((address + size) <= INTERNAL_FLASH_M4_APP_END))) ? 1U : 0U;
}

static HAL_StatusTypeDef InternalFlash_WriteUnlocked(uint32_t address,
                                                      const uint8_t *data,
                                                      uint32_t size)
{
    HAL_StatusTypeDef status = HAL_OK;
    uint32_t chunk_size;

    while ((status == HAL_OK) && (size > 0U))
    {
        chunk_size = (size > INTERNAL_FLASH_WORD_SIZE)
                   ? INTERNAL_FLASH_WORD_SIZE
                   : size;

        memset(s_flash_word, 0xFF, sizeof(s_flash_word));
        memcpy(s_flash_word, data, chunk_size);

        status = InternalFlash_ProgramWord(address, s_flash_word);
        address += INTERNAL_FLASH_WORD_SIZE;
        data += chunk_size;
        size -= chunk_size;
    }

    return status;
}

HAL_StatusTypeDef InternalFlash_EraseRange(uint32_t address, uint32_t size)
{
    HAL_StatusTypeDef status;
    uint32_t primask;

    if ((size == 0U) ||
        ((address % INTERNAL_FLASH_SECTOR_SIZE) != 0U) ||
        ((size % INTERNAL_FLASH_SECTOR_SIZE) != 0U) ||
        (InternalFlash_IsAllowedRange(address, size) == 0U))
    {
        return HAL_ERROR;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    status = HAL_FLASH_Unlock();
    while ((status == HAL_OK) && (size > 0U))
    {
        status = InternalFlash_EraseSector(address);
        address += INTERNAL_FLASH_SECTOR_SIZE;
        size -= INTERNAL_FLASH_SECTOR_SIZE;
    }

    (void)HAL_FLASH_Lock();

    if (primask == 0U)
    {
        __enable_irq();
    }

    return status;
}

HAL_StatusTypeDef InternalFlash_Write(uint32_t address,
                                      const uint8_t *data,
                                      uint32_t size)
{
    HAL_StatusTypeDef status;
    uint32_t primask;

    if ((data == NULL) ||
        ((address % INTERNAL_FLASH_WORD_SIZE) != 0U) ||
        (InternalFlash_IsAllowedRange(address, size) == 0U))
    {
        return HAL_ERROR;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    status = HAL_FLASH_Unlock();
    if (status == HAL_OK)
    {
        status = InternalFlash_WriteUnlocked(address, data, size);
    }
    (void)HAL_FLASH_Lock();

    if (primask == 0U)
    {
        __enable_irq();
    }

    return status;
}

HAL_StatusTypeDef InternalFlash_CopyFromQspi(uint32_t qspi_address,
                                             uint32_t internal_address,
                                             uint32_t size)
{
    HAL_StatusTypeDef status;
    uint32_t primask;
    uint32_t chunk_size;

    if ((size == 0U) ||
        ((internal_address % INTERNAL_FLASH_WORD_SIZE) != 0U) ||
        (InternalFlash_IsAllowedRange(internal_address, size) == 0U))
    {
        return HAL_ERROR;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    status = HAL_FLASH_Unlock();
    while ((status == HAL_OK) && (size > 0U))
    {
        chunk_size = (size > INTERNAL_FLASH_WORD_SIZE)
                   ? INTERNAL_FLASH_WORD_SIZE
                   : size;

        memset(s_qspi_word, 0xFF, sizeof(s_qspi_word));
        status = QSPI_FLASH_Read(qspi_address, (uint8_t *)s_qspi_word,
                                 chunk_size);
        if (status == HAL_OK)
        {
            status = InternalFlash_ProgramWord(internal_address, s_qspi_word);
        }

        qspi_address += chunk_size;
        internal_address += INTERNAL_FLASH_WORD_SIZE;
        size -= chunk_size;
    }

    (void)HAL_FLASH_Lock();

    if (primask == 0U)
    {
        __enable_irq();
    }

    return status;
}

HAL_StatusTypeDef InternalFlash_CalculateCrc32(uint32_t address,
                                               uint32_t size,
                                               uint32_t *crc32)
{
    uint32_t crc;
    uint32_t chunk_size;

    if ((crc32 == NULL) || (InternalFlash_IsAllowedRange(address, size) == 0U))
    {
        return HAL_ERROR;
    }

    crc = CRC32_Init();
    while (size > 0U)
    {
        chunk_size = (size > INTERNAL_FLASH_CRC_CHUNK_SIZE)
                   ? INTERNAL_FLASH_CRC_CHUNK_SIZE
                   : size;

        memcpy(s_crc_buffer, (const void *)address, chunk_size);
        crc = CRC32_Update(crc, s_crc_buffer, chunk_size);

        address += chunk_size;
        size -= chunk_size;
    }

    *crc32 = CRC32_Finalize(crc);
    return HAL_OK;
}

HAL_StatusTypeDef InternalFlash_VerifyCrc32(uint32_t address,
                                            uint32_t size,
                                            uint32_t expected_crc32)
{
    HAL_StatusTypeDef status;
    uint32_t actual_crc32;

    status = InternalFlash_CalculateCrc32(address, size, &actual_crc32);
    if (status != HAL_OK)
    {
        return status;
    }

    return (actual_crc32 == expected_crc32) ? HAL_OK : HAL_ERROR;
}
