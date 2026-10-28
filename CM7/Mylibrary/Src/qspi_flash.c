#include "qspi_flash.h"

#include "crc32.h"
#include "quadspi.h"
#include "stm32h7xx_hal_def.h"

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

/**
 * @brief 配置并发送一条 QSPI 命令。
 * @param instruction 指令码。
 * @param address 地址值；无地址阶段时忽略。
 * @param address_mode 地址阶段模式，取值为 QSPI_ADDRESS_xxx。
 * @param data_mode 数据阶段模式，取值为 QSPI_DATA_xxx。
 * @param data_size 数据阶段字节数。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
static HAL_StatusTypeDef qspi_flash_command(uint8_t instruction,
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

/**
 * @brief 根据读取长度计算接收超时时间。
 * @param size 要读取的字节数。
 * @return 接收超时时间，单位为毫秒。
 */
static uint32_t qspi_flash_read_timeout(uint32_t size)
{
    return QSPI_FLASH_COMMAND_TIMEOUT_MS + (size / 1024U);
}

/**
 * @brief 读取外部 Flash 的状态寄存器。
 * @param instruction 状态寄存器读取指令码。
 * @param status 输出参数，返回状态寄存器值。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
static HAL_StatusTypeDef qspi_flash_read_status(uint8_t instruction, uint8_t *status)
{
    HAL_StatusTypeDef result;

    result = qspi_flash_command(instruction, 0U, QSPI_ADDRESS_NONE,
                                QSPI_DATA_1_LINE, 1U);
    if (result != HAL_OK) {
        return result;
    }

    return HAL_QSPI_Receive(&hqspi, status, QSPI_FLASH_COMMAND_TIMEOUT_MS);
}

/**
 * @brief 发送写使能命令并等待 Flash 空闲。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
static HAL_StatusTypeDef qspi_flash_write_enable(void)
{
    HAL_StatusTypeDef result;

    result = QSPI_FLASH_WaitReady(QSPI_FLASH_WRITE_TIMEOUT_MS);
    if (result != HAL_OK) {
        return result;
    }

    return qspi_flash_command(QSPI_FLASH_CMD_WRITE_ENABLE, 0U,
                              QSPI_ADDRESS_NONE, QSPI_DATA_NONE, 0U);
}

/**
 * @brief 检查并设置状态寄存器 2 的 QE 位。
 * @return HAL_OK 表示 QE 已置位，其他 HAL 状态表示失败。
 */
static HAL_StatusTypeDef qspi_flash_set_quad_enable(void)
{
    HAL_StatusTypeDef result;
    uint8_t status2;

    result = qspi_flash_read_status(QSPI_FLASH_CMD_READ_STATUS2, &status2);
    if (result != HAL_OK) {
        return result;
    }
    if ((status2 & QSPI_FLASH_STATUS2_QE) != 0U) {
        return HAL_OK;
    }

    status2 |= QSPI_FLASH_STATUS2_QE;
    result = qspi_flash_write_enable();
    if (result != HAL_OK) {
        return result;
    }

    result = qspi_flash_command(QSPI_FLASH_CMD_WRITE_STATUS2, 0U,
                                QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE, 1U);
    if (result != HAL_OK) {
        return result;
    }

    result = HAL_QSPI_Transmit(&hqspi, &status2,
                               QSPI_FLASH_COMMAND_TIMEOUT_MS);
    if (result != HAL_OK) {
        return result;
    }

    result = QSPI_FLASH_WaitReady(QSPI_FLASH_WRITE_TIMEOUT_MS);
    if (result != HAL_OK) {
        return result;
    }

    result = qspi_flash_read_status(QSPI_FLASH_CMD_READ_STATUS2, &status2);
    if (result != HAL_OK) {
        return result;
    }

    return ((status2 & QSPI_FLASH_STATUS2_QE) != 0U) ? HAL_OK : HAL_ERROR;
}

/**
 * @brief 使外部 Flash 进入 4 字节地址模式。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
static HAL_StatusTypeDef qspi_flash_enter_4byte_mode(void)
{
    return qspi_flash_command(QSPI_FLASH_CMD_ENTER_4BYTE_MODE, 0U,
                              QSPI_ADDRESS_NONE, QSPI_DATA_NONE, 0U);
}

/**
 * @brief 执行指定类型的擦除操作。
 * @param address 擦除起始地址。
 * @param alignment 擦除区域的对齐大小，单位为字节。
 * @param instruction 擦除指令码。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
static HAL_StatusTypeDef qspi_flash_erase(uint32_t address,
                                          uint32_t alignment,
                                          uint8_t instruction)
{
    HAL_StatusTypeDef result;

    if ((address >= QSPI_FLASH_SIZE_BYTES) || ((address % alignment) != 0U)) {
        return HAL_ERROR;
    }

    result = qspi_flash_write_enable();
    if (result != HAL_OK) {
        return result;
    }

    result = qspi_flash_command(instruction, address, QSPI_ADDRESS_1_LINE,
                                QSPI_DATA_NONE, 0U);
    if (result != HAL_OK) {
        return result;
    }

    return QSPI_FLASH_WaitReady(QSPI_FLASH_ERASE_TIMEOUT_MS);
}

HAL_StatusTypeDef QSPI_FLASH_Init(void)
{
    HAL_StatusTypeDef result;
    uint32_t jedec_id;

    result = QSPI_FLASH_ReadJedecId(&jedec_id);
    if (result != HAL_OK) {
        return result;
    }
    if (jedec_id != QSPI_FLASH_W25Q256_JEDEC_ID) {
        return HAL_ERROR;
    }

    result = qspi_flash_set_quad_enable();
    if (result != HAL_OK) {
        return result;
    }

    return qspi_flash_enter_4byte_mode();
}

HAL_StatusTypeDef QSPI_FLASH_ReadJedecId(uint32_t *jedec_id)
{
    HAL_StatusTypeDef result;
    uint8_t id[3];

    if (jedec_id == NULL) {
        return HAL_ERROR;
    }

    result = qspi_flash_command(QSPI_FLASH_CMD_READ_JEDEC_ID, 0U,
                                QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE,
                                sizeof(id));
    if (result != HAL_OK) {
        return result;
    }

    result = HAL_QSPI_Receive(&hqspi, id, QSPI_FLASH_COMMAND_TIMEOUT_MS);
    if (result != HAL_OK) {
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

    for (;;) {
        result = qspi_flash_read_status(QSPI_FLASH_CMD_READ_STATUS1, &status1);
        if (result != HAL_OK) {
            return result;
        }
        if ((status1 & QSPI_FLASH_STATUS1_BUSY) == 0U) {
            return HAL_OK;
        }
        if ((HAL_GetTick() - start) >= timeout_ms) {
            return HAL_TIMEOUT;
        }
        HAL_Delay(1U);
    }
}

HAL_StatusTypeDef QSPI_FLASH_Read(uint32_t address, uint8_t *data, uint32_t size)
{
    HAL_StatusTypeDef result;

    if (size == 0U) {
        return HAL_OK;
    }
    if ((data == NULL) || (address >= QSPI_FLASH_SIZE_BYTES) ||
        (size > (QSPI_FLASH_SIZE_BYTES - address))) {
        return HAL_ERROR;
    }

    result = qspi_flash_command(QSPI_FLASH_CMD_READ_DATA, address,
                                QSPI_ADDRESS_1_LINE, QSPI_DATA_1_LINE, size);
    if (result != HAL_OK) {
        return result;
    }

    return HAL_QSPI_Receive(&hqspi, data, qspi_flash_read_timeout(size));
}

HAL_StatusTypeDef QSPI_FLASH_PageProgram(uint32_t address,
                                         const uint8_t *data,
                                         uint16_t size)
{
    HAL_StatusTypeDef result;

    if (size == 0U) {
        return HAL_OK;
    }
    if ((data == NULL) || (address >= QSPI_FLASH_SIZE_BYTES) ||
        ((uint32_t)size > (QSPI_FLASH_SIZE_BYTES - address)) ||
        (((address % QSPI_FLASH_PAGE_SIZE) + size) > QSPI_FLASH_PAGE_SIZE)) {
        return HAL_ERROR;
    }

    result = qspi_flash_write_enable();
    if (result != HAL_OK) {
        return result;
    }

    result = qspi_flash_command(QSPI_FLASH_CMD_PAGE_PROGRAM, address,
                                QSPI_ADDRESS_1_LINE, QSPI_DATA_1_LINE, size);
    if (result != HAL_OK) {
        return result;
    }

    result = HAL_QSPI_Transmit(&hqspi, (uint8_t *)data,
                               QSPI_FLASH_WRITE_TIMEOUT_MS);
    if (result != HAL_OK) {
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

    if (size == 0U) {
        return HAL_OK;
    }
    if ((data == NULL) || (address >= QSPI_FLASH_SIZE_BYTES) ||
        (size > (QSPI_FLASH_SIZE_BYTES - address))) {
        return HAL_ERROR;
    }

    while (size > 0U) {
        chunk_size = QSPI_FLASH_PAGE_SIZE - (address % QSPI_FLASH_PAGE_SIZE);
        if (chunk_size > size) {
            chunk_size = size;
        }

        result = QSPI_FLASH_PageProgram(address, data, (uint16_t)chunk_size);
        if (result != HAL_OK) {
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
    return qspi_flash_erase(address, QSPI_FLASH_SECTOR_SIZE,
                            QSPI_FLASH_CMD_SECTOR_ERASE);
}

HAL_StatusTypeDef QSPI_FLASH_EraseBlock64KB(uint32_t address)
{
    return qspi_flash_erase(address, QSPI_FLASH_BLOCK_SIZE,
                            QSPI_FLASH_CMD_BLOCK_ERASE);
}

HAL_StatusTypeDef QSPI_FLASH_CalculateCrc32(uint32_t address,
                                            uint32_t size,
                                            uint32_t *crc32)
{
    HAL_StatusTypeDef result;
    uint32_t crc;
    uint32_t chunk_size;

    if ((crc32 == NULL) || (size == 0U) ||
        (address >= QSPI_FLASH_SIZE_BYTES) ||
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
