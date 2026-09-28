/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LOG5_Pin GPIO_PIN_10
#define LOG5_GPIO_Port GPIOG
#define LOG4_Pin GPIO_PIN_9
#define LOG4_GPIO_Port GPIOG
#define A_S1_Pin GPIO_PIN_5
#define A_S1_GPIO_Port GPIOD
#define A_S0_Pin GPIO_PIN_4
#define A_S0_GPIO_Port GPIOD
#define LOG6_Pin GPIO_PIN_11
#define LOG6_GPIO_Port GPIOG
#define OSC_AC1_Pin GPIO_PIN_3
#define OSC_AC1_GPIO_Port GPIOD
#define LOG7_Pin GPIO_PIN_12
#define LOG7_GPIO_Port GPIOG
#define A_S2_Pin GPIO_PIN_7
#define A_S2_GPIO_Port GPIOD
#define LOG_EN_Pin GPIO_PIN_14
#define LOG_EN_GPIO_Port GPIOG
#define LOG8_Pin GPIO_PIN_13
#define LOG8_GPIO_Port GPIOG
#define M4_KEY_Pin GPIO_PIN_8
#define M4_KEY_GPIO_Port GPIOA
#define LOG3_Pin GPIO_PIN_7
#define LOG3_GPIO_Port GPIOG
#define LOG2_Pin GPIO_PIN_3
#define LOG2_GPIO_Port GPIOG
#define LOG1_Pin GPIO_PIN_2
#define LOG1_GPIO_Port GPIOG
#define B_S0_Pin GPIO_PIN_2
#define B_S0_GPIO_Port GPIOA
#define B_S1_Pin GPIO_PIN_1
#define B_S1_GPIO_Port GPIOA
#define B_S2_Pin GPIO_PIN_7
#define B_S2_GPIO_Port GPIOA
#define OSC_AC2_Pin GPIO_PIN_5
#define OSC_AC2_GPIO_Port GPIOA
#define M4_LED_Pin GPIO_PIN_1
#define M4_LED_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
