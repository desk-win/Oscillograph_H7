#include "ota.h"

#include <stddef.h>
#include <string.h>

#include "bl24c16f.h"
#include "cmsis_os2.h"
#include "crc32.h"
#include "ota_layout.h"
#include "qspi_flash.h"
#include "usart.h"
#include "uart_printf.h"

#ifndef OTA_DEBUG_LOG
#define OTA_DEBUG_LOG 1U
#endif

/** OTA 任务栈大小，单位为字。 */
#define OTA_TASK_STACK_WORDS 1024U

/*
 * 0x38000000 是现有 MPU 配置的非缓存 D3 SRAM。
 * DMA 写入后 CPU 可直接读取，不需要清理 D-Cache。
 */
#define OTA_DMA_BUFFER_ADDRESS 0x38000000U
#define OTA_DMA_BUFFER_SIZE    1032U

#define OTA_FILE_CM7_MASK  0x01U
#define OTA_FILE_CM4_MASK  0x02U
#define OTA_FILES_REQUIRED (OTA_FILE_CM7_MASK | OTA_FILE_CM4_MASK)
#define OTA_CM7_FILENAME   "M7.bin"
#define OTA_CM4_FILENAME   "M4.bin"

#define OTA_UART_TIMEOUT_MS        200U
#define OTA_HANDSHAKE_TIMEOUT_MS   1000U
#define OTA_DATA_TIMEOUT_MS        3000U

#define OTA_YMODEM_SOH 0x01U
#define OTA_YMODEM_STX 0x02U
#define OTA_YMODEM_EOT 0x04U
#define OTA_YMODEM_ACK 0x06U
#define OTA_YMODEM_NAK 0x15U
#define OTA_YMODEM_CAN 0x18U
#define OTA_YMODEM_C   0x43U

typedef enum
{
    OTA_STATE_IDLE = 0,
    OTA_STATE_WAIT_BLOCK0,
    OTA_STATE_RECEIVING,
    OTA_STATE_DONE,
    OTA_STATE_FAILED
} OTA_State;

typedef enum
{
    OTA_FILE_NONE = 0,
    OTA_FILE_CM7,
    OTA_FILE_CM4
} OTA_File;

typedef enum
{
    OTA_FRAME_INVALID = 0,
    OTA_FRAME_DATA,
    OTA_FRAME_EOT,
    OTA_FRAME_CANCEL
} OTA_FrameType;

typedef struct
{
    OTA_FrameType type;
    uint8_t sequence;
    const uint8_t *data;
    uint16_t data_size;
} OTA_Frame;

/** 非缓存 DMA 接收缓冲区。 */
static uint8_t *const s_dma_buffer =
    (uint8_t *)OTA_DMA_BUFFER_ADDRESS;
/** UART 中断记录的接收长度。 */
static volatile uint16_t s_rx_size;
/** UART 中断记录的接收事件类型。 */
static volatile uint32_t s_rx_event_type;
/** OTA 当前是否处于接收状态。 */
static volatile uint8_t s_ota_active;
/** OTA 任务句柄。 */
static TaskHandle_t s_ota_task;
/** Ymodem 状态机当前状态。 */
static OTA_State s_state = OTA_STATE_IDLE;
/** 固件总长度，来自 Ymodem Block 0。 */
static uint32_t s_image_size;
/** 已写入外部 Flash 的固件字节数。 */
static uint32_t s_received_size;
/** 下一个期望收到的数据块序号。 */
static uint8_t s_expected_sequence;
/** 当前文件在外部 Flash 中的起始地址。 */
static uint32_t s_image_address;
/** 当前文件接收过程中的 CRC32 中间值。 */
static uint32_t s_receive_crc;
/** 本次会话已完整接收并校验的文件位图。 */
static uint8_t s_received_files;
/** 当前正在接收的文件。 */
static OTA_File s_current_file;
/** 本次写入的 A/B 槽，来自 EEPROM 中最新有效元数据。 */
static OTA_SlotId s_target_slot = OTA_SLOT_ID_A;
/** 当前接收阶段，用于失败后定位。 */
static const char *s_stage = "idle";
/** 当前阶段内的错误细节。 */
static const char *s_detail = "none";
/** 最近一次 UART 错误码。 */
static volatile uint32_t s_uart_error;

#if OTA_DEBUG_LOG
#define OTA_SET_STAGE(value)  (s_stage = (value))
#define OTA_SET_DETAIL(value) (s_detail = (value))
#else
#define OTA_SET_STAGE(value)  ((void)0)
#define OTA_SET_DETAIL(value) ((void)0)
#endif

_Static_assert(OTA_DMA_BUFFER_SIZE >= 1029U,
               "OTA DMA buffer must fit one Ymodem frame");

/** 校验 EEPROM 元数据记录，避免把掉电残留记录当成有效槽信息。 */
static uint8_t OTA_MetadataValid(const OTA_Metadata *metadata)
{
    uint32_t record_crc;

    if ((metadata == NULL) ||
        (metadata->magic != OTA_METADATA_MAGIC) ||
        (metadata->version != OTA_METADATA_VERSION) ||
        (metadata->record_size != sizeof(OTA_Metadata)) ||
        (metadata->active_slot > OTA_SLOT_ID_B))
    {
        return 0U;
    }

    record_crc = CRC32_Calculate((const uint8_t *)metadata,
                                 (uint32_t)offsetof(OTA_Metadata,
                                                    record_crc32));
    return (record_crc == metadata->record_crc32) ? 1U : 0U;
}

/**
 * @brief 从 EEPROM 两份元数据中选择最新有效记录，返回其非活动槽。
 *
 * 阶段四会提供正式的共享元数据模块；当前无有效记录时默认使用 A 槽，
 * 这样第一次升级和尚未写入元数据的设备仍可完成阶段二传输。
 */
static OTA_SlotId OTA_SelectTargetSlot(void)
{
    static const uint16_t copy_offsets[] =
    {
        OTA_EEPROM_METADATA_COPY0_OFFSET,
        OTA_EEPROM_METADATA_COPY1_OFFSET
    };
    OTA_Metadata metadata;
    OTA_Metadata newest;
    uint8_t newest_valid = 0U;

    if (BL24C16F_Init() != HAL_OK)
    {
        OTA_SET_DETAIL("eeprom-init");
        return OTA_SLOT_ID_A;
    }

    for (uint32_t i = 0U;
         i < (sizeof(copy_offsets) / sizeof(copy_offsets[0]));
         i++)
    {
        if ((BL24C16F_Read(copy_offsets[i], (uint8_t *)&metadata,
                           sizeof(metadata)) == HAL_OK) &&
            (OTA_MetadataValid(&metadata) != 0U) &&
            ((newest_valid == 0U) || (metadata.sequence > newest.sequence)))
        {
            newest = metadata;
            newest_valid = 1U;
        }
    }

    if ((newest_valid == 0U) ||
        (newest.active_slot == OTA_SLOT_ID_NONE))
    {
        return OTA_SLOT_ID_A;
    }

    return (newest.active_slot == OTA_SLOT_ID_A)
         ? OTA_SLOT_ID_B
         : OTA_SLOT_ID_A;
}

static uint32_t OTA_SlotBase(OTA_SlotId slot)
{
    return (slot == OTA_SLOT_ID_A)
         ? OTA_EXTERNAL_SLOT_A_BASE
         : OTA_EXTERNAL_SLOT_B_BASE;
}

/**
 * @brief 通过 USART1 发送一个 Ymodem 控制字符。
 * @param value 要发送的字节。
 * @return 无。
 */
static void OTA_SendByte(uint8_t value)
{
    (void)HAL_UART_Transmit(&huart1, &value, 1U, OTA_UART_TIMEOUT_MS);
}

/**
 * @brief 重新启动 USART1 IDLE + DMA 接收。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
static HAL_StatusTypeDef OTA_ArmDma(void)
{
    (void)HAL_UART_AbortReceive(&huart1);
    return HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_dma_buffer,
                                        OTA_DMA_BUFFER_SIZE);
}

/**
 * @brief 计算 Ymodem 使用的 CRC16-CCITT。
 * @param data 数据缓冲区。
 * @param size 数据长度。
 * @return CRC16 值。
 */
static uint16_t OTA_Crc16(const uint8_t *data, uint16_t size)
{
    uint16_t crc = 0U;

    for (uint16_t i = 0U; i < size; i++)
    {
        crc ^= (uint16_t)data[i] << 8U;
        for (uint8_t bit = 0U; bit < 8U; bit++)
        {
            crc = ((crc & 0x8000U) != 0U)
                ? (uint16_t)((crc << 1U) ^ 0x1021U)
                : (uint16_t)(crc << 1U);
        }
    }

    return crc;
}

/**
 * @brief 解析并校验一帧 Ymodem 数据。
 * @param size 接收字节数。
 * @param frame 输出解析结果。
 * @return 1 表示帧有效，0 表示无效。
 */
static uint8_t OTA_ParseFrame(uint16_t size, OTA_Frame *frame)
{
    uint16_t data_size;
    uint16_t expected_size;
    uint16_t received_crc;

    OTA_SET_DETAIL("none");
    if ((frame == NULL) || (size == 0U))
    {
        OTA_SET_DETAIL("empty");
        return 0U;
    }

    if ((size == 1U) && (s_dma_buffer[0] == OTA_YMODEM_EOT))
    {
        frame->type = OTA_FRAME_EOT;
        return 1U;
    }

    if ((size >= 2U) && (s_dma_buffer[0] == OTA_YMODEM_CAN) &&
        (s_dma_buffer[1] == OTA_YMODEM_CAN))
    {
        frame->type = OTA_FRAME_CANCEL;
        return 1U;
    }

    if (s_dma_buffer[0] == OTA_YMODEM_SOH)
    {
        data_size = 128U;
    }
    else if (s_dma_buffer[0] == OTA_YMODEM_STX)
    {
        data_size = 1024U;
    }
    else
    {
        OTA_SET_DETAIL("marker");
        return 0U;
    }

    expected_size = (uint16_t)(data_size + 5U);
    if (size != expected_size)
    {
        OTA_SET_DETAIL("length");
        return 0U;
    }
    if ((uint8_t)(s_dma_buffer[1] ^ s_dma_buffer[2]) != 0xFFU)
    {
        OTA_SET_DETAIL("header");
        return 0U;
    }

    received_crc = ((uint16_t)s_dma_buffer[3U + data_size] << 8U) |
                   s_dma_buffer[4U + data_size];
    if (OTA_Crc16(&s_dma_buffer[3], data_size) != received_crc)
    {
        OTA_SET_DETAIL("crc");
        return 0U;
    }

    frame->type = OTA_FRAME_DATA;
    frame->sequence = s_dma_buffer[1];
    frame->data = &s_dma_buffer[3];
    frame->data_size = data_size;
    return 1U;
}

/**
 * @brief 从 Ymodem Block 0 的文件名字段识别 CM7 或 CM4 镜像。
 * @param data Block 0 数据区。
 * @param size 数据区长度。
 * @return 识别到的文件类型，未知文件名返回 OTA_FILE_NONE。
 */
static OTA_File OTA_ParseFileName(const uint8_t *data, uint16_t size)
{
    uint16_t name_length = 0U;

    if ((data == NULL) || (size == 0U))
    {
        return OTA_FILE_NONE;
    }

    while ((name_length < size) && (data[name_length] != 0U))
    {
        name_length++;
    }
    if (name_length >= size)
    {
        return OTA_FILE_NONE;
    }

    if ((name_length == (sizeof(OTA_CM7_FILENAME) - 1U)) &&
        (memcmp(data, OTA_CM7_FILENAME, name_length) == 0))
    {
        return OTA_FILE_CM7;
    }
    if ((name_length == (sizeof(OTA_CM4_FILENAME) - 1U)) &&
        (memcmp(data, OTA_CM4_FILENAME, name_length) == 0))
    {
        return OTA_FILE_CM4;
    }

    return OTA_FILE_NONE;
}

/**
 * @brief 从 Ymodem Block 0 中解析文件长度。
 * @param data Block 0 数据区。
 * @param size 数据区长度。
 * @param image_size 输出固件长度。
 * @return 1 表示解析成功，0 表示失败。
 */
static uint8_t OTA_ParseImageSize(const uint8_t *data,
                                  uint16_t size,
                                  uint32_t *image_size)
{
    uint16_t pos = 0U;
    uint32_t value = 0U;
    uint8_t digits = 0U;

    while ((pos < size) && (data[pos] != 0U))
    {
        pos++;
    }
    if (pos >= size)
    {
        return 0U;
    }
    pos++;

    while ((pos < size) && (data[pos] != 0U))
    {
        if (data[pos] == (uint8_t)' ')
        {
            break;
        }
        if ((data[pos] < (uint8_t)'0') || (data[pos] > (uint8_t)'9'))
        {
            return 0U;
        }
        if (value > ((UINT32_MAX - (data[pos] - (uint8_t)'0')) / 10U))
        {
            return 0U;
        }
        value = (value * 10U) + (data[pos] - (uint8_t)'0');
        digits = 1U;
        pos++;
    }

    if ((digits == 0U) || (value == 0U) ||
        (value > QSPI_FLASH_SIZE_BYTES))
    {
        return 0U;
    }

    *image_size = value;
    return 1U;
}

static uint8_t OTA_ParseBlock0(const uint8_t *data,
                               uint16_t size,
                               OTA_File *file,
                               uint32_t *image_size)
{
    OTA_File parsed_file;

    if ((file == NULL) || (image_size == NULL))
    {
        return 0U;
    }

    parsed_file = OTA_ParseFileName(data, size);
    if ((parsed_file == OTA_FILE_NONE) ||
        (OTA_ParseImageSize(data, size, image_size) == 0U))
    {
        return 0U;
    }

    *file = parsed_file;
    return 1U;
}

/** 在进入协议前验证文件名和长度解析器，避免上位机格式固定时静默写错槽。 */
static uint8_t OTA_Block0ParserSelfTest(void)
{
    static const uint8_t cm7_block0[] = "M7.bin\0" "1";
    static const uint8_t cm4_block0[] = "M4.bin\0" "1";
    OTA_File file;
    uint32_t image_size;

    if ((OTA_ParseBlock0(cm7_block0, sizeof(cm7_block0),
                         &file, &image_size) == 0U) ||
        (file != OTA_FILE_CM7) || (image_size != 1U))
    {
        return 0U;
    }

    return ((OTA_ParseBlock0(cm4_block0, sizeof(cm4_block0),
                             &file, &image_size) != 0U) &&
            (file == OTA_FILE_CM4) && (image_size == 1U)) ? 1U : 0U;
}

/**
 * @brief 擦除外部 Flash 中存放新固件的区域。
 * @param address 擦除起始地址。
 * @param image_size 固件长度。
 * @return HAL_OK 表示成功，其他 HAL 状态表示失败。
 */
static HAL_StatusTypeDef OTA_EraseImage(uint32_t address, uint32_t image_size)
{
    HAL_StatusTypeDef status = HAL_OK;
    uint32_t remaining = (image_size + 4095U) & ~4095U;

    while ((status == HAL_OK) && (remaining >= QSPI_FLASH_BLOCK_SIZE))
    {
        status = QSPI_FLASH_EraseBlock64KB(address);
        address += QSPI_FLASH_BLOCK_SIZE;
        remaining -= QSPI_FLASH_BLOCK_SIZE;
    }

    while ((status == HAL_OK) && (remaining > 0U))
    {
        status = QSPI_FLASH_EraseSector4KB(address);
        address += QSPI_FLASH_SECTOR_SIZE;
        remaining -= QSPI_FLASH_SECTOR_SIZE;
    }

    return status;
}

/**
 * @brief 结束当前 OTA 会话。
 * @param state 最终状态。
 * @return 无。
 */
static void OTA_Finish(OTA_State state)
{
    uint32_t received_size = s_received_size;
    uint32_t image_size = s_image_size;
    uint32_t rx_event = s_rx_event_type;
    uint32_t uart_error = s_uart_error;
    uint16_t last_rx_size = s_rx_size;
    uint8_t sequence = s_expected_sequence;

    (void)HAL_UART_AbortReceive(&huart1);
    s_ota_active = 0U;
    s_state = state;
    UART1_PrintfSetEnabled(1U);

#if OTA_DEBUG_LOG
    /* 只在 Ymodem 会话结束后输出，避免文本混入协议流。 */
    if (state == OTA_STATE_DONE)
    {
        OTA_SET_STAGE("done");
        uart1_printf("[OTA] TRANSFER OK st=%s rx=%lu/%lu\r\n",
                     s_stage, (unsigned long)received_size,
                     (unsigned long)image_size);
    }
    else if (state == OTA_STATE_FAILED)
    {
        uart1_printf("[OTA] TRANSFER FAILED st=%s d=%s sq=%u\r\n",
                     s_stage, s_detail, sequence);
        uart1_printf("[OTA] rx=%lu/%lu last=%u ev=%lu first=%02X err=0x%08lX\r\n",
                     (unsigned long)received_size, (unsigned long)image_size,
                     last_rx_size, (unsigned long)rx_event,
                     (last_rx_size > 0U) ? s_dma_buffer[0] : 0U,
                     (unsigned long)uart_error);
    }
#endif
}

/**
 * @brief 发送 Ymodem 取消并结束会话。
 * @return 无。
 */
static void OTA_Fail(void)
{
    static const uint8_t cancel[] = { OTA_YMODEM_CAN, OTA_YMODEM_CAN };

    (void)HAL_UART_Transmit(&huart1, (uint8_t *)cancel,
                            sizeof(cancel), OTA_UART_TIMEOUT_MS);
    OTA_Finish(OTA_STATE_FAILED);
}

/**
 * @brief 重新挂起 DMA，失败时结束会话。
 * @return 1 表示成功，0 表示失败。
 */
static uint8_t OTA_RearmOrFail(void)
{
    if (OTA_ArmDma() != HAL_OK)
    {
        OTA_SET_DETAIL("dma-arm");
        OTA_Fail();
        return 0U;
    }
    return 1U;
}

/**
 * @brief 开始一次新的 OTA 会话。
 * @return 无。
 */
static void OTA_StartSession(void)
{
    OTA_SET_STAGE("init");
    OTA_SET_DETAIL("none");
    s_image_size = 0U;
    s_received_size = 0U;
    s_image_address = 0U;
    s_receive_crc = CRC32_Init();
    s_expected_sequence = 1U;
    s_received_files = 0U;
    s_current_file = OTA_FILE_NONE;
    s_target_slot = OTA_SLOT_ID_A;
    s_state = OTA_STATE_WAIT_BLOCK0;
    s_ota_active = 1U;
    s_uart_error = 0U;

    OTA_SET_STAGE("parser-selftest");
    if (OTA_Block0ParserSelfTest() == 0U)
    {
        OTA_SET_DETAIL("block0-parser");
        OTA_Fail();
        return;
    }

    OTA_SET_STAGE("qspi-init");
    if (QSPI_FLASH_Init() != HAL_OK)
    {
        OTA_SET_DETAIL("qspi-init");
        OTA_Fail();
        return;
    }

    OTA_SET_STAGE("slot-select");
    s_target_slot = OTA_SelectTargetSlot();

    OTA_SET_STAGE("dma-arm");
    if (!OTA_RearmOrFail())
    {
        return;
    }

    OTA_SET_STAGE("wait-block0");
    OTA_SendByte(OTA_YMODEM_C);
}

/**
 * @brief 处理 Ymodem Block 0。
 * @param frame 已校验的数据帧。
 * @return 无。
 */
static void OTA_HandleBlock0(const OTA_Frame *frame)
{
    OTA_File file;
    uint32_t image_size;
    uint32_t max_size;
    uint32_t slot_base;
    uint8_t expected_mask;

    if (s_state != OTA_STATE_WAIT_BLOCK0)
    {
        OTA_SET_STAGE("block0-state");
        OTA_Fail();
        return;
    }

    if ((frame->data == NULL) || (frame->data_size == 0U))
    {
        OTA_SET_STAGE("block0-empty-data");
        OTA_Fail();
        return;
    }

    if (frame->data[0] == 0U)
    {
        if (s_received_files != OTA_FILES_REQUIRED)
        {
            OTA_SET_STAGE("block0-incomplete");
            OTA_Fail();
            return;
        }

        OTA_SET_STAGE("final-block");
        (void)HAL_UART_AbortReceive(&huart1);
        OTA_SendByte(OTA_YMODEM_ACK);
        OTA_Finish(OTA_STATE_DONE);
        return;
    }

    if (!OTA_ParseBlock0(frame->data, frame->data_size,
                         &file, &image_size))
    {
        OTA_SET_STAGE("block0-parse");
        OTA_SET_DETAIL("filename-or-size");
        OTA_Fail();
        return;
    }

    expected_mask = (s_received_files == 0U)
                  ? OTA_FILE_CM7_MASK
                  : ((s_received_files == OTA_FILE_CM7_MASK)
                     ? OTA_FILE_CM4_MASK : 0U);
    if ((expected_mask == 0U) ||
        (((file == OTA_FILE_CM7) ? OTA_FILE_CM7_MASK
                                 : OTA_FILE_CM4_MASK) != expected_mask))
    {
        OTA_SET_STAGE("block0-file");
        OTA_SET_DETAIL("M7-then-M4-required");
        OTA_Fail();
        return;
    }

    slot_base = OTA_SlotBase(s_target_slot);
    if (file == OTA_FILE_CM7)
    {
        s_image_address = slot_base + OTA_EXTERNAL_CM7_OFFSET;
        max_size = OTA_EXTERNAL_CM7_MAX_SIZE;
    }
    else
    {
        s_image_address = slot_base + OTA_EXTERNAL_CM4_OFFSET;
        max_size = OTA_EXTERNAL_CM4_MAX_SIZE;
    }
    if (image_size > max_size)
    {
        OTA_SET_STAGE("block0-size");
        OTA_SET_DETAIL("image-too-large");
        OTA_Fail();
        return;
    }

    if (s_received_files == 0U)
    {
        OTA_SET_STAGE("header-invalidate");
        if (QSPI_FLASH_EraseSector4KB(slot_base +
                                      OTA_PACKAGE_HEADER_OFFSET) != HAL_OK)
        {
            OTA_SET_DETAIL("qspi-header-erase");
            OTA_Fail();
            return;
        }
    }

    s_current_file = file;
    s_image_size = image_size;
    s_receive_crc = CRC32_Init();

    OTA_SET_STAGE("block0-ack");
    (void)HAL_UART_AbortReceive(&huart1);
    (void)xTaskNotifyStateClear(NULL);
    OTA_SendByte(OTA_YMODEM_ACK);

    OTA_SET_STAGE("erase");
    if (OTA_EraseImage(s_image_address, s_image_size) != HAL_OK)
    {
        OTA_SET_DETAIL("qspi-erase");
        OTA_Fail();
        return;
    }
    s_received_size = 0U;
    s_expected_sequence = 1U;
    s_state = OTA_STATE_RECEIVING;

    OTA_SET_STAGE("block0-c");
    if (!OTA_RearmOrFail())
    {
        return;
    }
    OTA_SendByte(OTA_YMODEM_C);
    OTA_SET_STAGE("receive-data");
}

/**
 * @brief 处理一个 Ymodem 固件数据块。
 * @param frame 已校验的数据帧。
 * @return 无。
 */
static void OTA_HandleData(const OTA_Frame *frame)
{
    uint32_t remaining;
    uint32_t write_size;

    if (s_state != OTA_STATE_RECEIVING)
    {
        OTA_SET_STAGE("data-state");
        OTA_Fail();
        return;
    }

    if (frame->sequence == (uint8_t)(s_expected_sequence - 1U))
    {
        OTA_SET_STAGE("dup-block");
        if (OTA_RearmOrFail())
        {
            OTA_SendByte(OTA_YMODEM_ACK);
        }
        return;
    }

    if (frame->sequence != s_expected_sequence)
    {
        OTA_SET_STAGE("seq-mismatch");
        if (OTA_RearmOrFail())
        {
            OTA_SendByte(OTA_YMODEM_NAK);
        }
        return;
    }

    remaining = s_image_size - s_received_size;
    if ((frame->data_size == 0U) || (remaining == 0U))
    {
        OTA_SET_STAGE("data-length");
        OTA_Fail();
        return;
    }

    write_size = ((uint32_t)frame->data_size > remaining)
               ? remaining
               : frame->data_size;
    OTA_SET_STAGE("qspi-write");
    if (QSPI_FLASH_Write(s_image_address + s_received_size,
                         frame->data, write_size) != HAL_OK)
    {
        OTA_SET_DETAIL("qspi-write");
        OTA_Fail();
        return;
    }

    s_receive_crc = CRC32_Update(s_receive_crc, frame->data, write_size);
    s_received_size += write_size;
    s_expected_sequence++;

    OTA_SET_STAGE("data-ack");
    if (!OTA_RearmOrFail())
    {
        return;
    }
    OTA_SendByte(OTA_YMODEM_ACK);
    OTA_SET_STAGE("receive-data");
}

/**
 * @brief 处理 Ymodem EOT。
 * @return 无。
 */
static void OTA_HandleEot(void)
{
    uint32_t flash_crc;
    uint32_t received_crc;

    if (s_state == OTA_STATE_RECEIVING)
    {
        if (s_received_size != s_image_size)
        {
            OTA_SET_STAGE("eot-size");
            OTA_Fail();
            return;
        }

        received_crc = CRC32_Finalize(s_receive_crc);
        OTA_SET_STAGE("verify-crc");
        if ((QSPI_FLASH_CalculateCrc32(s_image_address, s_image_size,
                                       &flash_crc) != HAL_OK) ||
            (flash_crc != received_crc))
        {
            OTA_SET_DETAIL("qspi-crc");
            OTA_Fail();
            return;
        }

        if (s_current_file == OTA_FILE_CM7)
        {
            s_received_files |= OTA_FILE_CM7_MASK;
        }
        else if (s_current_file == OTA_FILE_CM4)
        {
            s_received_files |= OTA_FILE_CM4_MASK;
        }
        else
        {
            OTA_SET_STAGE("eot-file");
            OTA_Fail();
            return;
        }

        s_current_file = OTA_FILE_NONE;
        s_state = OTA_STATE_WAIT_BLOCK0;
        OTA_SET_STAGE("next-block0");
        if (!OTA_RearmOrFail())
        {
            return;
        }
        OTA_SendByte(OTA_YMODEM_ACK);
        OTA_SendByte(OTA_YMODEM_C);
    }
    else if (s_state == OTA_STATE_WAIT_BLOCK0)
    {
        /* 上位机未收到 EOT 应答时会重发 EOT，这里重复 ACK + C。 */
        OTA_SET_STAGE("eot-retry");
        if (OTA_RearmOrFail())
        {
            OTA_SendByte(OTA_YMODEM_ACK);
            OTA_SendByte(OTA_YMODEM_C);
        }
    }
}

/**
 * @brief 处理一次 UART IDLE 或 DMA 完成通知。
 * @param size 本次接收长度。
 * @return 无。
 */
static void OTA_ProcessRx(uint16_t size)
{
    OTA_Frame frame = {0};

    if (!OTA_ParseFrame(size, &frame))
    {
        OTA_SET_STAGE("frame-parse");
        if (OTA_RearmOrFail())
        {
            OTA_SendByte(OTA_YMODEM_NAK);
        }
        return;
    }

    switch (frame.type)
    {
    case OTA_FRAME_DATA:
        if ((frame.sequence == 0U) &&
            (s_state == OTA_STATE_WAIT_BLOCK0))
        {
            OTA_HandleBlock0(&frame);
        }
        else
        {
            OTA_HandleData(&frame);
        }
        break;

    case OTA_FRAME_EOT:
        OTA_HandleEot();
        break;

    case OTA_FRAME_CANCEL:
        OTA_Finish(OTA_STATE_FAILED);
        break;

    case OTA_FRAME_INVALID:
    default:
        OTA_SET_STAGE("frame-invalid");
        OTA_Finish(OTA_STATE_FAILED);
        break;
    }
}

/**
 * @brief OTA 升级任务主循环。
 * @param argument 任务参数，当前未使用。
 * @return 无。
 */
static void OTA_Task(void *argument)
{
    uint32_t events;
    TickType_t wait_ticks;

    (void)argument;

    for (;;)
    {
        if (s_state == OTA_STATE_WAIT_BLOCK0)
        {
            wait_ticks = pdMS_TO_TICKS(OTA_HANDSHAKE_TIMEOUT_MS);
        }
        else if (s_state == OTA_STATE_RECEIVING)
        {
            wait_ticks = pdMS_TO_TICKS(OTA_DATA_TIMEOUT_MS);
        }
        else
        {
            wait_ticks = portMAX_DELAY;
        }

        if (xTaskNotifyWait(0U, UINT32_MAX, &events, wait_ticks) != pdTRUE)
        {
            if (s_state == OTA_STATE_WAIT_BLOCK0)
            {
                OTA_SET_STAGE("block0-timeout");
                OTA_SendByte(OTA_YMODEM_C);
            }
            else if (s_state == OTA_STATE_RECEIVING)
            {
                OTA_SET_STAGE("data-timeout");
                OTA_SendByte(OTA_YMODEM_NAK);
            }
            continue;
        }

        if ((events & OTA_EVENT_START) != 0U)
        {
            OTA_StartSession();
        }

        if ((events & OTA_EVENT_RX) != 0U)
        {
            if ((s_rx_event_type == HAL_UART_RXEVENT_IDLE) ||
                (s_rx_event_type == HAL_UART_RXEVENT_TC))
            {
                OTA_ProcessRx(s_rx_size);
            }
        }

        if ((events & OTA_EVENT_ERROR) != 0U)
        {
            OTA_SET_STAGE("uart-error");
            OTA_SET_DETAIL("uart");
            OTA_Finish(OTA_STATE_FAILED);
        }

        if ((events & OTA_EVENT_STOP) != 0U)
        {
            OTA_SET_STAGE("stop");
            OTA_Finish(OTA_STATE_IDLE);
        }
    }
}

BaseType_t OTA_TaskCreate(void)
{
    return xTaskCreate(OTA_Task, "ota", OTA_TASK_STACK_WORDS, NULL,
                       osPriorityNormal, &s_ota_task);
}

BaseType_t OTA_RequestStart(void)
{
    if (s_ota_task == NULL)
    {
        return pdFAIL;
    }

    /* 先关闭普通日志，确保后续 Ymodem 控制字符不会与调试文本混在一起。 */
    UART1_PrintfSetEnabled(0U);
    s_ota_active = 1U;
    return xTaskNotify(s_ota_task, OTA_EVENT_START, eSetBits);
}

void OTA_RequestStop(void)
{
    /* 先停止 DMA 并恢复日志，任务侧再做状态收尾。 */
    (void)HAL_UART_AbortReceive(&huart1);
    s_ota_active = 0U;
    UART1_PrintfSetEnabled(1U);

    if (s_ota_task != NULL)
    {
        (void)xTaskNotify(s_ota_task, OTA_EVENT_STOP, eSetBits);
    }
}

BaseType_t OTA_NotifyFromISR(uint32_t events,
                             BaseType_t *higher_priority_task_woken)
{
    if (s_ota_task == NULL)
    {
        return pdFAIL;
    }

    return xTaskNotifyFromISR(s_ota_task, events, eSetBits,
                              higher_priority_task_woken);
}

/**
 * @brief UART IDLE、半传输和全传输事件回调。
 * @param huart 触发回调的 UART 句柄。
 * @param Size 本次事件对应的接收字节数。
 * @return 无。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if ((huart == &huart1) && (s_ota_active != 0U))
    {
        if ((huart->RxEventType == HAL_UART_RXEVENT_IDLE) ||
            (huart->RxEventType == HAL_UART_RXEVENT_TC))
        {
            /* 中断只记录完整帧长度，不在中断中解析或写 Flash。 */
            s_rx_size = Size;
            s_rx_event_type = huart->RxEventType;
            __DMB();
            (void)OTA_NotifyFromISR(OTA_EVENT_RX,
                                    &higher_priority_task_woken);
            portYIELD_FROM_ISR(higher_priority_task_woken);
        }
    }
}

/**
 * @brief UART 错误回调。
 * @param huart 触发错误的 UART 句柄。
 * @return 无。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (huart == &huart1)
    {
        s_uart_error = huart->ErrorCode;
        (void)OTA_NotifyFromISR(OTA_EVENT_ERROR,
                                &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}
