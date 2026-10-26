#include "qspi_flash.h"

#include <stddef.h>

#include "crc32.h"
#include "quadspi.h"

#define QSPI_FLASH_W25Q256_JEDEC_ID      0x00EF4019U

#define QSPI_FLASH_CMD_WRITE_ENABLE      0x06U
#define QSPI_FLASH_CMD_READ_STATUS1      0x05U
#define QSPI_FLASH_CMD_READ_STATUS2      0x35U
#define QSPI_FLASH_CMD_WRITE_STATUS2     0x31U
#define QSPI_FLASH_CMD_READ_DATA         0x03U
#define QSPI_FLASH_CMD_PAGE_PROGRAM      0x02U
#define QSPI_FLASH_CMD_SECTOR_ERASE      0x20U
#define QSPI_FLASH_CMD_BLOCK_ERASE       0xD8U
#define QSPI_FLASH_CMD_ENTER_4BYTE_MODE  0xB7U
#define QSPI_FLASH_CMD_READ_JEDEC_ID     0x9FU

#define QSPI_FLASH_STATUS1_BUSY          0x01U
#define QSPI_FLASH_STATUS2_QE            0x02U

#define QSPI_FLASH_COMMAND_TIMEOUT_MS    100U
#define QSPI_FLASH_WRITE_TIMEOUT_MS      1000U
#define QSPI_FLASH_ERASE_TIMEOUT_MS      5000U
#define QSPI_FLASH_CRC_CHUNK_SIZE        1024U

static uint8_t s_crc_buffer[QSPI_FLASH_CRC_CHUNK_SIZE];

static HAL_StatusTypeDef QSPI_FLASH_Command(uint8_t instruction,
                                             uint32_t address,
                                             uint32_t address_mode,
                                             uint32_t data_mode,
                                             uint32_t data_size)
{
    QSPI_CommandTypeDef command = {0};

    command.Instruction = instruction;
    command.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    command.Address = address;
    command.AddressSize = QSPI_ADDRESS_32_BITS;
    command.AddressMode = address_mode;
    command.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    command.AlternateBytes = 0U;
    command.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
    command.DummyCycles = 0U;
    command.DataMode = data_mode;
    command.NbData = data_size;
    command.DdrMode = QSPI_DDR_MODE_DISABLE;
    command.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    command.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;

    return HAL_QSPI_Command(&hqspi, &command, QSPI_FLASH_COMMAND_TIMEOUT_MS);
}

static uint32_t QSPI_FLASH_ReadTimeout(uint32_t size)
{
    return QSPI_FLASH_COMMAND_TIMEOUT_MS + (size / 1024U);
}

static HAL_StatusTypeDef QSPI_FLASH_ReadStatus(uint8_t instruction,
                                                uint8_t *status)
{
    HAL_StatusTypeDef result;

    result = QSPI_FLASH_Command(instruction, 0U, QSPI_ADDRESS_NONE,
                                QSPI_DATA_1_LINE, 1U);
    if (result != HAL_OK)
    {
        return result;
    }

    return HAL_QSPI_Receive(&hqspi, status, QSPI_FLASH_COMMAND_TIMEOUT_MS);
}

static HAL_StatusTypeDef QSPI_FLASH_WriteEnable(void)
{
    HAL_StatusTypeDef result;

    result = QSPI_FLASH_WaitReady(QSPI_FLASH_WRITE_TIMEOUT_MS);
    if (result != HAL_OK)
    {
        return result;
    }

    return QSPI_FLASH_Command(QSPI_FLASH_CMD_WRITE_ENABLE, 0U,
                              QSPI_ADDRESS_NONE, QSPI_DATA_NONE, 0U);
}

static HAL_StatusTypeDef QSPI_FLASH_SetQuadEnable(void)
{
    HAL_StatusTypeDef result;
    uint8_t status2;

    result = QSPI_FLASH_ReadStatus(QSPI_FLASH_CMD_READ_STATUS2, &status2);
    if (result != HAL_OK)
    {
        return result;
    }
    if ((status2 & QSPI_FLASH_STATUS2_QE) != 0U)
    {
        return HAL_OK;
    }

    status2 |= QSPI_FLASH_STATUS2_QE;
    result = QSPI_FLASH_WriteEnable();
    if (result != HAL_OK)
    {
        return result;
    }

    result = QSPI_FLASH_Command(QSPI_FLASH_CMD_WRITE_STATUS2, 0U,
                                QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE, 1U);
    if (result != HAL_OK)
    {
        return result;
    }

    result = HAL_QSPI_Transmit(&hqspi, &status2,
                               QSPI_FLASH_COMMAND_TIMEOUT_MS);
    if (result != HAL_OK)
    {
        return result;
    }

    result = QSPI_FLASH_WaitReady(QSPI_FLASH_WRITE_TIMEOUT_MS);
    if (result != HAL_OK)
    {
        return result;
    }

    result = QSPI_FLASH_ReadStatus(QSPI_FLASH_CMD_READ_STATUS2, &status2);
    if (result != HAL_OK)
    {
        return result;
    }

    return ((status2 & QSPI_FLASH_STATUS2_QE) != 0U) ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef QSPI_FLASH_Enter4ByteMode(void)
{
    return QSPI_FLASH_Command(QSPI_FLASH_CMD_ENTER_4BYTE_MODE, 0U,
                              QSPI_ADDRESS_NONE, QSPI_DATA_NONE, 0U);
}

static HAL_StatusTypeDef QSPI_FLASH_Erase(uint32_t address,
                                           uint32_t alignment,
                                           uint8_t instruction)
{
    HAL_StatusTypeDef result;

    if ((address >= QSPI_FLASH_SIZE_BYTES) || ((address % alignment) != 0U))
    {
        return HAL_ERROR;
    }

    result = QSPI_FLASH_WriteEnable();
    if (result != HAL_OK)
    {
        return result;
    }

    result = QSPI_FLASH_Command(instruction, address, QSPI_ADDRESS_1_LINE,
                                QSPI_DATA_NONE, 0U);
    if (result != HAL_OK)
    {
        return result;
    }

    return QSPI_FLASH_WaitReady(QSPI_FLASH_ERASE_TIMEOUT_MS);
}

HAL_StatusTypeDef QSPI_FLASH_Init(void)
{
    HAL_StatusTypeDef result;
    uint32_t jedec_id;

    result = QSPI_FLASH_ReadJedecId(&jedec_id);
    if (result != HAL_OK)
    {
        return result;
    }
    if (jedec_id != QSPI_FLASH_W25Q256_JEDEC_ID)
    {
        return HAL_ERROR;
    }

    result = QSPI_FLASH_SetQuadEnable();
    if (result != HAL_OK)
    {
        return result;
    }

    return QSPI_FLASH_Enter4ByteMode();
}

HAL_StatusTypeDef QSPI_FLASH_ReadJedecId(uint32_t *jedec_id)
{
    HAL_StatusTypeDef result;
    uint8_t id[3];

    if (jedec_id == NULL)
    {
        return HAL_ERROR;
    }

    result = QSPI_FLASH_Command(QSPI_FLASH_CMD_READ_JEDEC_ID, 0U,
                                QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE,
                                sizeof(id));
    if (result != HAL_OK)
    {
        return result;
    }

    result = HAL_QSPI_Receive(&hqspi, id, QSPI_FLASH_COMMAND_TIMEOUT_MS);
    if (result != HAL_OK)
    {
        return result;
    }

    *jedec_id = ((uint32_t)id[0] << 16) |
                ((uint32_t)id[1] << 8) |
                (uint32_t)id[2];
    return HAL_OK;
}

HAL_StatusTypeDef QSPI_FLASH_WaitReady(uint32_t timeout_ms)
{
    HAL_StatusTypeDef result;
    uint32_t start = HAL_GetTick();
    uint8_t status1;

    for (;;)
    {
        result = QSPI_FLASH_ReadStatus(QSPI_FLASH_CMD_READ_STATUS1, &status1);
        if (result != HAL_OK)
        {
            return result;
        }
        if ((status1 & QSPI_FLASH_STATUS1_BUSY) == 0U)
        {
            return HAL_OK;
        }
        if ((HAL_GetTick() - start) >= timeout_ms)
        {
            return HAL_TIMEOUT;
        }
        HAL_Delay(1U);
    }
}

HAL_StatusTypeDef QSPI_FLASH_Read(uint32_t address, uint8_t *data, uint32_t size)
{
    HAL_StatusTypeDef result;

    if (size == 0U)
    {
        return HAL_OK;
    }
    if ((data == NULL) || (address >= QSPI_FLASH_SIZE_BYTES) ||
        (size > (QSPI_FLASH_SIZE_BYTES - address)))
    {
        return HAL_ERROR;
    }

    result = QSPI_FLASH_Command(QSPI_FLASH_CMD_READ_DATA, address,
                                QSPI_ADDRESS_1_LINE, QSPI_DATA_1_LINE, size);
    if (result != HAL_OK)
    {
        return result;
    }

    return HAL_QSPI_Receive(&hqspi, data, QSPI_FLASH_ReadTimeout(size));
}

HAL_StatusTypeDef QSPI_FLASH_PageProgram(uint32_t address,
                                         const uint8_t *data,
                                         uint16_t size)
{
    HAL_StatusTypeDef result;

    if (size == 0U)
    {
        return HAL_OK;
    }
    if ((data == NULL) || (address >= QSPI_FLASH_SIZE_BYTES) ||
        ((uint32_t)size > (QSPI_FLASH_SIZE_BYTES - address)) ||
        (((address % QSPI_FLASH_PAGE_SIZE) + size) > QSPI_FLASH_PAGE_SIZE))
    {
        return HAL_ERROR;
    }

    result = QSPI_FLASH_WriteEnable();
    if (result != HAL_OK)
    {
        return result;
    }

    result = QSPI_FLASH_Command(QSPI_FLASH_CMD_PAGE_PROGRAM, address,
                                QSPI_ADDRESS_1_LINE, QSPI_DATA_1_LINE, size);
    if (result != HAL_OK)
    {
        return result;
    }

    result = HAL_QSPI_Transmit(&hqspi, (uint8_t *)data,
                               QSPI_FLASH_WRITE_TIMEOUT_MS);
    if (result != HAL_OK)
    {
        return result;
    }

    return QSPI_FLASH_WaitReady(QSPI_FLASH_WRITE_TIMEOUT_MS);
}

HAL_StatusTypeDef QSPI_FLASH_Write(uint32_t address,
                                   const uint8_t *data,
                                   uint32_t size)
{
    HAL_StatusTypeDef result;
    uint32_t chunk_size;

    if (size == 0U)
    {
        return HAL_OK;
    }
    if ((data == NULL) || (address >= QSPI_FLASH_SIZE_BYTES) ||
        (size > (QSPI_FLASH_SIZE_BYTES - address)))
    {
        return HAL_ERROR;
    }

    while (size > 0U)
    {
        chunk_size = QSPI_FLASH_PAGE_SIZE - (address % QSPI_FLASH_PAGE_SIZE);
        if (chunk_size > size)
        {
            chunk_size = size;
        }

        result = QSPI_FLASH_PageProgram(address, data, (uint16_t)chunk_size);
        if (result != HAL_OK)
        {
            return result;
        }

        address += chunk_size;
        data += chunk_size;
        size -= chunk_size;
    }

    return HAL_OK;
}

HAL_StatusTypeDef QSPI_FLASH_EraseSector4KB(uint32_t address)
{
    return QSPI_FLASH_Erase(address, QSPI_FLASH_SECTOR_SIZE,
                            QSPI_FLASH_CMD_SECTOR_ERASE);
}

HAL_StatusTypeDef QSPI_FLASH_EraseBlock64KB(uint32_t address)
{
    return QSPI_FLASH_Erase(address, QSPI_FLASH_BLOCK_SIZE,
                            QSPI_FLASH_CMD_BLOCK_ERASE);
}

HAL_StatusTypeDef QSPI_FLASH_CalculateCrc32(uint32_t address,
                                            uint32_t size,
                                            uint32_t *crc32)
{
    HAL_StatusTypeDef result;
    uint32_t crc;
    uint32_t chunk_size;

    if ((crc32 == NULL) || (address > QSPI_FLASH_SIZE_BYTES) ||
        (size > (QSPI_FLASH_SIZE_BYTES - address)))
    {
        return HAL_ERROR;
    }

    crc = CRC32_Init();

    while (size > 0U)
    {
        chunk_size = (size > QSPI_FLASH_CRC_CHUNK_SIZE)
                   ? QSPI_FLASH_CRC_CHUNK_SIZE
                   : size;

        result = QSPI_FLASH_Read(address, s_crc_buffer, chunk_size);
        if (result != HAL_OK)
        {
            return result;
        }

        crc = CRC32_Update(crc, s_crc_buffer, chunk_size);
        address += chunk_size;
        size -= chunk_size;
    }

    *crc32 = CRC32_Finalize(crc);
    return HAL_OK;
}

HAL_StatusTypeDef QSPI_FLASH_VerifyCrc32(uint32_t address,
                                         uint32_t size,
                                         uint32_t expected_crc32)
{
    HAL_StatusTypeDef result;
    uint32_t actual_crc32;

    result = QSPI_FLASH_CalculateCrc32(address, size, &actual_crc32);
    if (result != HAL_OK)
    {
        return result;
    }

    return (actual_crc32 == expected_crc32) ? HAL_OK : HAL_ERROR;
}
