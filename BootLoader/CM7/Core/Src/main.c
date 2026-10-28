/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "crc.h"
#include "i2c.h"
#include "quadspi.h"
#include "usart.h"
#include "gpio.h"
#include <stdarg.h>
#include <stdio.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "bl24c16f.h"
#include "crc32.h"
#include "ota_layout.h"
#include "qspi_flash.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define APP_BASE_ADDRESS  OTA_INTERNAL_CM7_BASE
#define APP_END_ADDRESS   (OTA_INTERNAL_CM7_BASE + OTA_INTERNAL_CM7_SIZE)
#define APP_SRAM_START    0x20000000U
#define APP_SRAM_END      0x20020000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

static void BootLoader_Printf(const char *format, ...);
static void BootLoader_JumpToApplication(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

typedef void (*app_entry_t)(void);

static void BootLoader_Printf(const char *format, ...)
{
  char buffer[128];
  va_list args;
  int length;

  va_start(args, format);
  length = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  if (length > 0)
  {
    uint16_t send_length = (length < (int)sizeof(buffer))
                         ? (uint16_t)length
                         : (uint16_t)(sizeof(buffer) - 1U);

    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buffer, send_length, 200U);
  }
}

static void BootLoader_JumpToApplication(void)
{
  uint32_t app_stack = *(volatile uint32_t *)APP_BASE_ADDRESS;
  uint32_t app_reset = *(volatile uint32_t *)(APP_BASE_ADDRESS + 4U);

  if ((app_stack < APP_SRAM_START) ||
      (app_stack > APP_SRAM_END) ||
      ((app_stack & 0x7U) != 0U) ||
      (app_reset < APP_BASE_ADDRESS) ||
      (app_reset >= APP_END_ADDRESS) ||
      ((app_reset & 0x1U) == 0U))
  {
    while (1)
    {
    }
  }

  __disable_irq();

  HAL_QSPI_DeInit(&hqspi);
  HAL_I2C_DeInit(&hi2c4);
  HAL_UART_DeInit(&huart1);

  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;

  for (uint32_t i = 0U; i < 8U; i++)
  {
    NVIC->ICER[i] = 0xFFFFFFFFU;
    NVIC->ICPR[i] = 0xFFFFFFFFU;
  }

  SCB->VTOR = APP_BASE_ADDRESS;
  __DSB();
  __ISB();

  __set_MSP(app_stack);
  __enable_irq();
  ((app_entry_t)app_reset)();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */
  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CRC_Init();
  MX_I2C4_Init();
  MX_QUADSPI_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */

  BootLoader_Printf("[BL] CRC32 self-test: %s\r\n",
                    (CRC32_SelfTest() == HAL_OK) ? "OK" : "FAILED");

  {
    HAL_StatusTypeDef qspi_status = QSPI_FLASH_Init();

    if (qspi_status == HAL_OK)
    {
      uint32_t jedec_id = 0U;
      uint32_t sample_crc = 0U;

      if ((QSPI_FLASH_ReadJedecId(&jedec_id) == HAL_OK) &&
          (QSPI_FLASH_CalculateCrc32(0U, 256U, &sample_crc) == HAL_OK))
      {
        BootLoader_Printf("[QSPI] JEDEC=0x%06lX, sample CRC=0x%08lX\r\n",
                          (unsigned long)jedec_id,
                          (unsigned long)sample_crc);
      }
      else
      {
        BootLoader_Printf("[QSPI] read/CRC FAILED\r\n");
      }
    }
    else
    {
      BootLoader_Printf("[QSPI] init FAILED\r\n");
    }
  }

  {
    HAL_StatusTypeDef eeprom_status;
    BL24C16F_TestResult test_result;
    uint32_t expected_crc = 0U;
    uint32_t read_crc = 0U;

    BootLoader_Printf("[BL] EEPROM test start\r\n");

    eeprom_status = BL24C16F_Init();
    if (eeprom_status == HAL_OK)
    {
      BootLoader_Printf("[EEPROM] found at 0x%02X\r\n",
                        (unsigned int)BL24C16F_GetAddress());

      test_result = BL24C16F_RunTest(&expected_crc, &read_crc);
      if (test_result == BL24C16F_TEST_WRITE_FAILED)
      {
        BootLoader_Printf("[EEPROM] write FAILED\r\n");
      }
      else
      {
        BootLoader_Printf("[EEPROM] write OK\r\n");

        if (test_result == BL24C16F_TEST_READ_FAILED)
        {
          BootLoader_Printf("[EEPROM] read FAILED\r\n");
        }
        else
        {
          BootLoader_Printf("[EEPROM] read OK\r\n");
          BootLoader_Printf("[EEPROM] CRC expected=0x%08lX read=0x%08lX\r\n",
                            (unsigned long)expected_crc,
                            (unsigned long)read_crc);
          BootLoader_Printf("[EEPROM] CRC %s\r\n",
                            (test_result == BL24C16F_TEST_OK) ? "OK" : "FAILED");
        }
      }
    }
    else
    {
      BootLoader_Printf("[EEPROM] not found on I2C4\r\n");
    }
  }

  BootLoader_JumpToApplication();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 60;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
