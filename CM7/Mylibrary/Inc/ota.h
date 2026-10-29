#ifndef OTA_H
#define OTA_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** OTA 任务收到启动请求。 */
#define OTA_EVENT_START  (1UL << 0)
/** OTA 任务收到串口接收事件。 */
#define OTA_EVENT_RX     (1UL << 1)
/** OTA 任务收到串口错误事件。 */
#define OTA_EVENT_ERROR  (1UL << 2)
/** OTA 任务收到停止请求。 */
#define OTA_EVENT_STOP   (1UL << 3)

/** OTA UI 阶段。 */
typedef enum
{
    OTA_STAGE_IDLE = 0,
    OTA_STAGE_WAITING_M7,
    OTA_STAGE_RECEIVING_M7,
    OTA_STAGE_VERIFYING_M7,
    OTA_STAGE_WAITING_M4,
    OTA_STAGE_RECEIVING_M4,
    OTA_STAGE_VERIFYING_M4,
    OTA_STAGE_FINALIZING,
    OTA_STAGE_DONE,
    OTA_STAGE_FAILED
} OTA_Stage;

/** UI 线程读取的 OTA 状态快照。 */
typedef struct
{
    uint8_t stage;
    uint8_t percent;
} OTA_Status;

/**
 * @brief 创建 OTA 升级任务。
 * @return pdPASS 表示成功，其他值表示创建失败。
 */
BaseType_t OTA_TaskCreate(void);

/**
 * @brief 从应用任务请求进入 OTA 模式。
 * @return pdPASS 表示通知成功，其他值表示任务未创建。
 */
BaseType_t OTA_RequestStart(void);

/**
 * @brief 从应用任务请求退出 OTA 模式。
 * @return 无。
 */
void OTA_RequestStop(void);

/**
 * @brief 从串口中断发送 OTA 任务通知。
 * @param events 要发送的 OTA_EVENT_xxx 事件位。
 * @param higher_priority_task_woken 输出是否唤醒了更高优先级任务。
 * @return pdPASS 表示通知成功，其他值表示失败。
 */
BaseType_t OTA_NotifyFromISR(uint32_t events,
                             BaseType_t *higher_priority_task_woken);

/** @brief 获取当前 OTA 阶段和 0~100 总进度，供 UI 线程读取。 */
void OTA_GetStatus(OTA_Status *status);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
