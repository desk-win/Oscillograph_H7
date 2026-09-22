#include "logic.h"
#include "main.h"
#include "stm32h7xx_hal_gpio.h"
#include <stdint.h>


/**
*   @brief 使能逻辑分析仪模块
*/
void Logic_EN(uint8_t level){
    if (level) {
        HAL_GPIO_WritePin(LOG_EN_GPIO_Port, LOG_EN_Pin, GPIO_PIN_SET);
    }else {
        HAL_GPIO_WritePin(LOG_EN_GPIO_Port, LOG_EN_Pin, GPIO_PIN_RESET);
    }
}
