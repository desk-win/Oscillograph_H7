/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sdmmc.h
  * @brief   This file contains all the function prototypes for
  *          the sdmmc.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __SDMMC_H__
#define __SDMMC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern SD_HandleTypeDef hsd1;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_SDMMC1_SD_Init(void);

/* USER CODE BEGIN Prototypes */

/* SDMMC1 的中断入口。函数体和声明本来由 CubeMX 生成 (stm32h7xx_it.c / .h), 在
 * NVIC 面板里勾上 SDMMC1 global interrupt 之后就有了。这里再声明一份是给
 * main.c 里那段 SD 自检用的: 它要把向量表里那一格和 &SDMMC1_IRQHandler 对一下,
 * 确认中断真的接上了。两个声明完全一样, 放着不冲突。 */
void SDMMC1_IRQHandler(void);

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __SDMMC_H__ */

