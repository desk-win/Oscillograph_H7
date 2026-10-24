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
#include "logic_analyze.h"
#include "logic_save.h"
#include "qspi_flash.h"
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

  /* 逻辑分析仪协议解码: 只读冻结的记录表, 无期限 —— 放 TouchGFX 之下。
   * 栈 1 KB (解码器里最大的局部量是 CAN 的 128 字节位数组)。 */
  {
    static const osThreadAttr_t logicAna_attributes = {
      .name       = "logicAna",
      .priority   = osPriorityNormal,
      .stack_size = 1024,
    };
    if (osThreadNew(LOG_Analyze_TaskEntry, NULL, &logicAna_attributes) == NULL)
    {
      taskDISABLE_INTERRUPTS();
      for (;;) { }
    }
  }

  /* 存卡: 借记录表 → 写 CSV。只读表、无期限, 也放 TouchGFX 之下;
   * 栈 2 KB (FatFs + SDMMC 轮询 + 行缓冲)。GUI 那边只调 LOG_Save_Request()。 */
  {
    static const osThreadAttr_t logicSave_attributes = {
      .name       = "logicSave",
      .priority   = osPriorityNormal,
      .stack_size = 2048,
    };
    if (osThreadNew(LOG_Save_TaskEntry, NULL, &logicSave_attributes) == NULL)
    {
      taskDISABLE_INTERRUPTS();
      for (;;) { }
    }
  }
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
  uint32_t jedec_id = 0U;
  HAL_StatusTypeDef qspi_flash_status;

  (void)argument;

  uart1_printf("[QSPI] probe start\r\n");

  qspi_flash_status = QSPI_FLASH_ReadJedecId(&jedec_id);
  if (qspi_flash_status == HAL_OK)
  {
    uint8_t manufacturer_id = (uint8_t)(jedec_id >> 16);
    uint8_t memory_type = (uint8_t)(jedec_id >> 8);
    uint8_t capacity_code = (uint8_t)jedec_id;

    uart1_printf("[QSPI] JEDEC ID: 0x%06lX\r\n", (unsigned long)jedec_id);
    uart1_printf("[QSPI] Manufacturer: 0x%02X, memory type: 0x%02X, capacity: 0x%02X\r\n",
                 (unsigned int)manufacturer_id,
                 (unsigned int)memory_type,
                 (unsigned int)capacity_code);
    uart1_printf("[QSPI] Capacity: 32 MB, expected W25Q256\r\n");

    if (jedec_id == 0x00EF4019U)
    {
      qspi_flash_status = QSPI_FLASH_Init();
      uart1_printf("[QSPI] full init: %s\r\n",
                   (qspi_flash_status == HAL_OK) ? "OK" : "FAILED");
    }
  }
  else
  {
    uart1_printf("[QSPI] read JEDEC ID failed: %d\r\n", (int)qspi_flash_status);
  }

  for(;;)
  {
    osDelay(1000);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

