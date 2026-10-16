#ifndef __LOGIC_SAVE_H
#define __LOGIC_SAVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 逻辑分析仪"存卡"任务 —— 把跳变记录表写成 SD 卡上的 CSV
 *
 * 分工 (和扫描/解码任务一个路子):
 *   GUI 只调 LOG_Save_Request() 通知一声, 然后立刻跳回 Logic 屏;
 *   任务这边借表 (LOG_Trace_BorrowRecs) → 用 sd.c 的入口写文件 → 还表。
 *   借用期间采集不会重开 (见 logic_trace.h), 所以存卡和采集不抢那张表;
 *   解码任务读的也是同一张表 (都只读), 两边可以同时借。
 *
 * 一次 Save 的流程 (全在任务里, GUI 帧里一个文件操作都没有):
 *   1. SD_Init()   —— 第一次存卡时挂卷 (sd.h: 必须在任务里调)
 *   2. SD_NextIndex() —— 首次挂上之后扫一遍目录, 取下一个可用编号
 *   3. 借记录表     —— 采集还在跑就等它收工 (最多 LOG_SAVE_WAIT_MS)
 *   4. SD_Create → SD_WriteRecs → SD_Close
 *   5. 还表 + 更新状态 (代数加一)
 *
 * 结果状态用代数发布 (和 LOG_Analyze_GetPubSeq 一个套路): 界面想显示就读
 * LOG_Save_GetStatus()。现在界面没做显示, 失败只在 sd_diag_* / 状态代数里看得见。
 * ========================================================================= */

/** 建请求信号量 (在 Model 构造函数里调一次, 和 LOG_Analyze_Init 并排)。 */
void LOG_Save_Init(void);

/** GUI 侧: 请求存卡 (非阻塞, 只放一次信号量)。存卡期间再按会被丢掉 —— 一次
 *  只写一份, 不会把同一张表写两遍。 */
void LOG_Save_Request(void);

/**
 * 读最近一次存卡的结果 (只读快照)。
 *   *res  = SD_OK / SD_ERR_* (见 sd.h); 还没存过是 SD_ERR_STATE
 *   *name = "LOG0003.CSV" (指向内部的静态字符串, 不用释放; 可以传 0)
 *   *rows = 写下的行数 (= sd_diag_rows; 可以传 0)
 * 返回值是**状态代数**: 变了说明有新的结果 (没存过是 0)。
 */
uint32_t LOG_Save_GetStatus(uint8_t *res, const char **name, uint32_t *rows);

/** 任务入口: 由 CM7/Core/Src/freertos.c 里 osThreadNew 创建。
 *  优先级要低于 TouchGFX; 栈 2 KB (FatFs + SDMMC 轮询 + 行缓冲)。 */
void LOG_Save_TaskEntry(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* __LOGIC_SAVE_H */
