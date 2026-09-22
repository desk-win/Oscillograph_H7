#ifndef __OSCILLOGRAPH_H
#define __OSCILLOGRAPH_H

#include "stm32h7xx_hal.h"
#include "adc.h"
#include "sharedconfig.h"
/* ADC 循环 DMA 的两个半帧缓冲, 落在 D2 SRAM 的固定地址上:
 *     osc_ch1_buf @ 0x30030000   uint32, 16 KB   CH1 = ADC1+ADC2 交错
 *     osc_ch2_buf @ 0x30034000   uint16,  8 KB   CH2 = ADC3
 * CM7 侧按同样这两个地址读。跨核 ABI 在 Common/Inc/sharedconfig.h。
 *
 * 交错模式下 1 word = ADC1(低16位) + ADC2(高16位), 且 ADC1 在时间上在前。
 * 依据 CMSIS: ADC_CDR_RDATA_MST_Pos = 0 (主/ADC1 低半字),
 *             ADC_CDR_RDATA_SLV_Pos = 16 (从/ADC2 高半字)。
 * 对应的取数宏在任务 2 里加。 */

extern uint32_t osc_ch1_buf[OSC_CH1_BUF_WORDS];
extern uint16_t osc_ch2_buf[OSC_CH2_BUF_WORDS];

void OSC_CH_Init(void);

/* 触发扫描在这里做, 不在 ADC 中断里 (中断只记半帧计数)。
 * 必须在半个帧周期 655 µs 内被调到, 主循环里不要放阻塞调用。 */
void OSC_CH_Poll(void);






void OSC_CH_DiagPrint(void);

#endif
