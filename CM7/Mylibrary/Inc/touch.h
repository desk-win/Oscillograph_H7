#ifndef __TOUCH_H
#define __TOUCH_H

#include "i2c.h"
#include "gpio.h"
#include "main.h"
#include "stm32h7xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void TOUCH_Init(void);
uint8_t TOUCH_Scan(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif



#endif
