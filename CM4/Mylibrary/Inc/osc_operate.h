#ifndef __OSC_OPERATE_H
#define __OSC_OPERATE_H



#include "stm32h7xx_hal.h"
#include "gpio.h"
#include "main.h"
#include "stm32h7xx_hal_gpio.h"
#include <stdint.h>


void OSC_Ch1_Multiple(uint8_t multiple, uint8_t DC);
void OSC_Ch2_Multiple(uint8_t multiple, uint8_t DC);


#endif
