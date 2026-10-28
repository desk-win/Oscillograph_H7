#ifndef OTA_LAYOUT_H
#define OTA_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file ota_layout.h
 * @brief CM7 应用与 BootLoader 共用的 OTA 固定布局和持久化数据结构。
 *
 * 这里的地址、结构体大小和 CRC 字段偏移会直接写入外部 Flash 和 EEPROM。
 * 修改任一布局后，必须同时兼容已发布固件，否则下面的编译期断言会阻止构建。
 */

/* 按编译语言选择静态断言实现，保证 C 和 C++ 工程都能检查共享布局。 */
#ifdef __cplusplus
#define OTA_LAYOUT_STATIC_ASSERT(condition, message) \
    static_assert(condition, message)
#else
#define OTA_LAYOUT_STATIC_ASSERT(condition, message) \
    _Static_assert(condition, message)
#endif

/* 内部 Flash 固定分区，单位：字节。 */
#define OTA_INTERNAL_BOOTLOADER_BASE       0x08000000U
#define OTA_INTERNAL_BOOTLOADER_SIZE       (128U * 1024U)
#define OTA_INTERNAL_CM7_BASE              0x08020000U
#define OTA_INTERNAL_CM7_SIZE              (1280U * 1024U)
#define OTA_INTERNAL_RESERVED_BASE         0x08160000U
#define OTA_INTERNAL_RESERVED_SIZE         (128U * 1024U)
#define OTA_INTERNAL_CM4_BASE              0x08180000U
#define OTA_INTERNAL_CM4_SIZE              (512U * 1024U)
#define OTA_INTERNAL_FLASH_END             0x08200000U

/* 外部 Flash A/B 槽及单槽内部偏移，单位：字节。 */
#define OTA_EXTERNAL_SLOT_A_BASE           0x00000000U
#define OTA_EXTERNAL_SLOT_B_BASE           0x00200000U
#define OTA_EXTERNAL_SLOT_SIZE             (2U * 1024U * 1024U)

/* 单槽内依次存放包头、CM7 镜像、CM4 镜像和保留区。 */
#define OTA_PACKAGE_HEADER_OFFSET          0x00000000U
#define OTA_PACKAGE_HEADER_AREA_SIZE       (4U * 1024U)
#define OTA_EXTERNAL_CM7_OFFSET            0x00010000U
#define OTA_EXTERNAL_CM7_MAX_SIZE          (1280U * 1024U)
#define OTA_EXTERNAL_CM4_OFFSET            0x00150000U
#define OTA_EXTERNAL_CM4_MAX_SIZE          (512U * 1024U)
#define OTA_EXTERNAL_RESERVED_OFFSET       0x001D0000U

/* EEPROM 中两份元数据副本的位置和单份容量，单位：字节。 */
#define OTA_EEPROM_SIZE                    2048U
#define OTA_EEPROM_METADATA_COPY0_OFFSET   0x0000U
#define OTA_EEPROM_METADATA_COPY1_OFFSET   0x0080U
#define OTA_EEPROM_METADATA_COPY_SIZE      128U

/* 用于识别有效包和元数据记录的固定魔数，并保留格式版本号。 */
#define OTA_PACKAGE_MAGIC                  0x4F544150U /* "OTAP" */
#define OTA_PACKAGE_VERSION                1U
#define OTA_METADATA_MAGIC                 0x4F54414DU /* "OTAM" */
#define OTA_METADATA_VERSION               1U

/* OTA 槽编号，NONE 表示尚未选择有效槽。 */
typedef enum
{
    OTA_SLOT_ID_NONE = 0,
    OTA_SLOT_ID_A = 1,
    OTA_SLOT_ID_B = 2
} OTA_SlotId;

/* 槽状态：接收中为 PENDING，外部包校验通过为 VALID，安装完成为 ACTIVE。 */
typedef enum
{
    OTA_SLOT_STATE_EMPTY = 0,
    OTA_SLOT_STATE_PENDING = 1,
    OTA_SLOT_STATE_VALID = 2,
    OTA_SLOT_STATE_ACTIVE = 3
} OTA_SlotState;

/**
 * @brief 外部 Flash 单槽包头，必须在两个镜像都写入并校验后最后提交。
 *
 * header_crc32 覆盖该字段之前的全部包头字节，防止包头字段被部分写入后误用。
 */
typedef struct
{
    uint32_t magic;          /* 固定为 OTA_PACKAGE_MAGIC */
    uint16_t version;        /* 包头格式版本 */
    uint16_t header_size;    /* 包头实际长度，当前为 sizeof(OTA_PackageHeader) */
    uint32_t slot;           /* 所在槽编号，取 OTA_SlotId */
    uint32_t cm7_offset;     /* CM7 镜像相对槽基址的偏移 */
    uint32_t cm7_size;       /* CM7 镜像实际长度 */
    uint32_t cm7_crc32;      /* CM7 镜像 CRC32 */
    uint32_t cm4_offset;     /* CM4 镜像相对槽基址的偏移 */
    uint32_t cm4_size;       /* CM4 镜像实际长度 */
    uint32_t cm4_crc32;      /* CM4 镜像 CRC32 */
    uint32_t header_crc32;   /* 包头前 36 字节的 CRC32 */
} OTA_PackageHeader;

/**
 * @brief EEPROM 双副本中的一条 OTA 元数据记录。
 *
 * sequence 用于在掉电后从两份副本中选择最新有效记录；record_crc32
 * 只覆盖该字段之前的记录内容，reserved 必须写零以保证未来兼容。
 */
typedef struct
{
    uint32_t magic;          /* 固定为 OTA_METADATA_MAGIC */
    uint16_t version;        /* 元数据格式版本 */
    uint16_t record_size;    /* 单条记录长度，当前为 128 字节 */
    uint32_t sequence;       /* 单调递增序号，用于选择最新副本 */
    uint8_t active_slot;     /* 当前可启动的内部固件槽 */
    uint8_t pending_slot;    /* 待安装或待确认的外部固件槽 */
    uint8_t pending_state;   /* pending_slot 的 OTA_SlotState */
    uint8_t confirmed;       /* 新固件是否已由 CM7 应用确认 */
    uint32_t boot_attempts;  /* 新固件连续启动尝试次数，用于后续回滚 */
    uint32_t record_crc32;   /* 记录前 20 字节的 CRC32 */
    uint8_t reserved[104];   /* 零填充，保留给后续格式升级 */
} OTA_Metadata;

/* 内部 Flash 各分区必须首尾相接，任何越界或重叠都会在编译期报错。 */
OTA_LAYOUT_STATIC_ASSERT(
    OTA_INTERNAL_BOOTLOADER_BASE + OTA_INTERNAL_BOOTLOADER_SIZE ==
        OTA_INTERNAL_CM7_BASE,
    "Bootloader and CM7 regions must not overlap");
OTA_LAYOUT_STATIC_ASSERT(
    OTA_INTERNAL_CM7_BASE + OTA_INTERNAL_CM7_SIZE ==
        OTA_INTERNAL_RESERVED_BASE,
    "CM7 and reserved internal regions must not overlap");
OTA_LAYOUT_STATIC_ASSERT(
    OTA_INTERNAL_RESERVED_BASE + OTA_INTERNAL_RESERVED_SIZE ==
        OTA_INTERNAL_CM4_BASE,
    "Reserved and CM4 internal regions must not overlap");
OTA_LAYOUT_STATIC_ASSERT(
    OTA_INTERNAL_CM4_BASE + OTA_INTERNAL_CM4_SIZE == OTA_INTERNAL_FLASH_END,
    "CM4 internal region must end at Flash end");

/* A/B 槽以及单槽内部的包头、镜像和保留区不能互相覆盖。 */
OTA_LAYOUT_STATIC_ASSERT(
    OTA_EXTERNAL_SLOT_A_BASE + OTA_EXTERNAL_SLOT_SIZE ==
        OTA_EXTERNAL_SLOT_B_BASE,
    "External slots must not overlap");
OTA_LAYOUT_STATIC_ASSERT(
    OTA_PACKAGE_HEADER_OFFSET + OTA_PACKAGE_HEADER_AREA_SIZE <=
        OTA_EXTERNAL_CM7_OFFSET,
    "Package header must not overlap CM7 image");
OTA_LAYOUT_STATIC_ASSERT(
    OTA_EXTERNAL_CM7_OFFSET + OTA_EXTERNAL_CM7_MAX_SIZE <=
        OTA_EXTERNAL_CM4_OFFSET,
    "CM7 and CM4 external images must not overlap");
OTA_LAYOUT_STATIC_ASSERT(
    OTA_EXTERNAL_CM4_OFFSET + OTA_EXTERNAL_CM4_MAX_SIZE <=
        OTA_EXTERNAL_RESERVED_OFFSET,
    "CM4 external image must not overlap reserved area");
OTA_LAYOUT_STATIC_ASSERT(
    OTA_EXTERNAL_RESERVED_OFFSET <= OTA_EXTERNAL_SLOT_SIZE,
    "Slot layout must fit in one external slot");

/* 两份 EEPROM 元数据副本必须位于 EEPROM 容量以内且互不重叠。 */
OTA_LAYOUT_STATIC_ASSERT(
    OTA_EEPROM_METADATA_COPY0_OFFSET + OTA_EEPROM_METADATA_COPY_SIZE <=
        OTA_EEPROM_METADATA_COPY1_OFFSET,
    "EEPROM metadata copies must not overlap");
OTA_LAYOUT_STATIC_ASSERT(
    OTA_EEPROM_METADATA_COPY1_OFFSET + OTA_EEPROM_METADATA_COPY_SIZE <=
        OTA_EEPROM_SIZE,
    "EEPROM metadata must fit in EEPROM");

/* 固定持久化 ABI，避免不同编译器填充规则导致应用与 BootLoader 解析错位。 */
OTA_LAYOUT_STATIC_ASSERT(sizeof(OTA_PackageHeader) == 40U,
                         "OTA_PackageHeader size changed");
OTA_LAYOUT_STATIC_ASSERT(
    offsetof(OTA_PackageHeader, header_crc32) == 36U,
    "OTA_PackageHeader CRC offset changed");
OTA_LAYOUT_STATIC_ASSERT(sizeof(OTA_Metadata) ==
                             OTA_EEPROM_METADATA_COPY_SIZE,
                         "OTA_Metadata must fill one EEPROM copy");
OTA_LAYOUT_STATIC_ASSERT(offsetof(OTA_Metadata, record_crc32) == 20U,
                         "OTA_Metadata CRC offset changed");

#endif /* OTA_LAYOUT_H */
