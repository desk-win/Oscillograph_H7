#include "mylcd.h"
#include "stm32h7xx_hal_gpio.h"



void LCD_BackLight(uint8_t level){
    if (level) {
        HAL_GPIO_WritePin(GPIOI, GPIO_PIN_11, GPIO_PIN_SET); 
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
    }else {
        HAL_GPIO_WritePin(GPIOI, GPIO_PIN_11, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET); 
    }
}
