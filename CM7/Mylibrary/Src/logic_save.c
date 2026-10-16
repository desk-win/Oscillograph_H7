/* ===========================================================================
 * CM7 侧存卡任务 —— 见 logic_save.h 的文件头
 *
 * 这一层只做"借表 / 定文件名 / 调 sd.c 的三个入口 / 还表", 不碰采集状态, 也不
 * 直接碰 FatFs (那是 sd.c 的地盘)。
 * ========================================================================= */

#include "logic_save.h"
#include "logic_trace.h"        /* LOG_Trace_BorrowRecs / ReleaseRecs / IsRunning */
#include "sharedconfig.h"       /* LOG_CTRL (fs_hz) */
#include "sd.h"                 /* SD_Init / NextIndex / Create / WriteRecs / Close */
#include "cmsis_os2.h"
#include <stdint.h>

/* 等采集收工的上限。记录满大约几秒, 空闲超时 1 秒 —— 给 3 秒够了; 超了就别再
 * 按着用户 (状态报 SD_ERR_STATE, 界面那边能看出来)。 */
#define LOG_SAVE_WAIT_MS   3000u

static osSemaphoreId_t s_req_sem;       /* 计数上限 1: 存卡期间的重复请求被丢掉 */

/* ---- 状态 (只任务写, GUI 读) ---- */
static volatile uint32_t s_seq;         /* 状态代数: 变了就是有新结果 */
static volatile uint8_t  s_res;         /* SD_OK / SD_ERR_* */
static char              s_name[SD_NAME_MAX];

/* ===========================================================================
 * GUI 侧
 * ========================================================================= */

void LOG_Save_Init(void)
{
    s_req_sem = osSemaphoreNew(1u, 0u, NULL);
    s_seq     = 0u;
    s_res     = SD_ERR_STATE;
    s_name[0] = '\0';
}

void LOG_Save_Request(void)
{
    (void)osSemaphoreRelease(s_req_sem);
}

uint32_t LOG_Save_GetStatus(uint8_t *res, const char **name, uint32_t *rows)
{
    if (res  != 0) { *res  = s_res; }
    if (name != 0) { *name = s_name; }
    if (rows != 0) { *rows = sd_diag_rows; }
    return s_seq;
}

/* ===========================================================================
 * 任务侧
 * ========================================================================= */

static void save_report(uint8_t res, const char *name)
{
    if (name != 0) {
        uint32_t i = 0u;
        while (i < (SD_NAME_MAX - 1u) && name[i] != '\0') {
            s_name[i] = name[i];
            i++;
        }
        s_name[i] = '\0';
    } else {
        s_name[0] = '\0';
    }

    s_res = res;
    s_seq++;
}

static void save_once(uint32_t *next_idx)
{
    /* 1) 挂卷。插拔过卡 / 上次没挂上, 这里会重新认一次 */
    if (SD_Init() != SD_OK) {
        save_report(SD_ERR_MOUNT, 0);
        return;
    }
    if (*next_idx == 0u) {
        *next_idx = SD_NextIndex();
    }

    /* 2) 借记录表: 采集还在跑就等它收工; 确实没数据 (没采过 / 被 Cancel) 就直接
     *    放弃 —— 别在这儿空转。 */
    const log_rec_t *recs = 0;
    uint32_t nrec  = 0u;
    uint32_t waited = 0u;

    while (LOG_Trace_BorrowRecs(&recs, &nrec, 0) == 0) {
        if (LOG_Trace_IsRunning() == 0u || waited >= LOG_SAVE_WAIT_MS) {
            save_report(SD_ERR_STATE, 0);
            return;
        }
        osDelay(5);
        waited += 5u;
    }

    /* 3) 建文件 → 写 → 收尾。中途失败也要 Close, 不能把句柄漏在那儿 */
    char name[SD_NAME_MAX];
    SD_MakeName(name, *next_idx);

    uint8_t res = SD_Create(name);
    if (res == SD_OK) {
        res = SD_WriteRecs(recs, nrec, LOG_CTRL->fs_hz);
    }
    {
        const uint8_t rc = SD_Close();
        if (res == SD_OK) {
            res = rc;
        }
    }

    LOG_Trace_ReleaseRecs();

    if (res == SD_OK) {
        *next_idx = (*next_idx >= 9999u) ? 1u : (*next_idx + 1u);
    }
    save_report(res, name);
}

void LOG_Save_TaskEntry(void *argument)
{
    (void)argument;

    uint32_t next_idx = 0u;         /* 0 = 还没扫过目录 */

    for (;;) {
        if (osSemaphoreAcquire(s_req_sem, osWaitForever) != osOK) {
            continue;
        }

        save_once(&next_idx);

        /* 存卡期间多按的那几次不算数: 把积压的请求丢掉 (一次只写一份) */
        (void)osSemaphoreAcquire(s_req_sem, 0);
    }
}
