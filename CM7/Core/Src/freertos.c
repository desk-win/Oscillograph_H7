/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_touchgfx.h"
#include <stdint.h>
#include <sys/_types.h>
#include "uart_printf.h"
#include "osc_probe.h"
#include "encoder.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
TaskHandle_t TouchGfx_TaskHandle = NULL;
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, char *pcTaskName);

/* USER CODE BEGIN 4 */
/* Name of the task whose stack overflowed, kept for the debugger.
   FreeRTOS calls this from vTaskSwitchContext(), i.e. when the damaged task is
   being switched OUT -- the overflow has already happened and the task is still
   in the ready list, so if the hook returns the scheduler will happily resume it
   later on a smashed stack. Trap instead, and let the debugger show who did it:
   read `stackOverflowTaskName`, check the task's stack usage, then reset. */
volatile char *stackOverflowTaskName = NULL;

void vApplicationStackOverflowHook(xTaskHandle xTask, char *pcTaskName)
{
   /* Run time stack overflow checking is performed if
   configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
   called if a stack overflow is detected. */
   (void)xTask;
   stackOverflowTaskName = pcTaskName;

   taskDISABLE_INTERRUPTS();
   for (;;)
   {
   }
}
/* USER CODE END 4 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  xTaskCreate(TouchGFX_Task, "TouchGfxTask", 4096, NULL, osPriorityAboveNormal1, NULL);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  // uint32_t lastPrint = 0;
  // uint8_t  firstPrint = 1;

  for(;;)
  {
   
    int16_t encoder_data = Encoder_GetCount();
    uart1_printf("%d\r\n",(uint32_t)encoder_data);
    osDelay(5);

    // const uint32_t now = OSC_PROBE_CYC;
    // if (firstPrint || ((uint32_t)(now - lastPrint) >= SystemCoreClock))
    // {
    //   firstPrint = 0;
    //   lastPrint  = now;
    //   OSC_Probe_Report();
    // }
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

