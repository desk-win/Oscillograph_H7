#include "touch.h"
#include <stdbool.h>


#define GT1151_RST_PORT          GPIOB
#define GT1151_RST_PIN           GPIO_PIN_12

#define GT1151_INT_PORT          GPIOB
#define GT1151_INT_PIN           GPIO_PIN_5

/* GT1151Q I2C address */
#define GT1151_I2C_ADDR          0x28   /* 8-bit write address */

/* Registers */
#define GT1151_REG_STATUS        0x814E
#define GT1151_REG_FIRST_POINT   0x814F

static HAL_StatusTypeDef GT1151_Read(uint16_t reg, uint8_t *buf, uint16_t len);
static HAL_StatusTypeDef GT1151_Write(uint16_t reg, const uint8_t *buf, uint16_t len);
static void GT1151_GPIO_InitForReset(void);

static void GT1151_GPIO_InitForReset(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* PB12: RST output */
    GPIO_InitStruct.Pin = GT1151_RST_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GT1151_RST_PORT, &GPIO_InitStruct);

    /* PB5: INT input
       你当前资料说明 INT 不参与地址选择，
       所以这里先直接作为输入即可 */
    GPIO_InitStruct.Pin = GT1151_INT_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GT1151_INT_PORT, &GPIO_InitStruct);
}

static HAL_StatusTypeDef GT1151_Read(uint16_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t reg_buf[2];

    reg_buf[0] = (uint8_t)(reg >> 8);
    reg_buf[1] = (uint8_t)(reg & 0xFF);

    if (HAL_I2C_Master_Transmit(&hi2c2, GT1151_I2C_ADDR, reg_buf, 2, 100) != HAL_OK)
    {
        return HAL_ERROR;
    }

    if (HAL_I2C_Master_Receive(&hi2c2, GT1151_I2C_ADDR, buf, len, 100) != HAL_OK)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
}

static HAL_StatusTypeDef GT1151_Write(uint16_t reg, const uint8_t *buf, uint16_t len)
{
    uint8_t tx_buf[32];
    uint16_t i;

    if ((len + 2U) > sizeof(tx_buf))
    {
        return HAL_ERROR;
    }

    tx_buf[0] = (uint8_t)(reg >> 8);
    tx_buf[1] = (uint8_t)(reg & 0xFF);

    for (i = 0; i < len; i++)
    {
        tx_buf[2 + i] = buf[i];
    }

    if (HAL_I2C_Master_Transmit(&hi2c2, GT1151_I2C_ADDR, tx_buf, len + 2U, 100) != HAL_OK)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
}

void TOUCH_Init(void)
{
    GT1151_GPIO_InitForReset();

    /* Reset sequence */
    HAL_GPIO_WritePin(GT1151_RST_PORT, GT1151_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(10);

    HAL_GPIO_WritePin(GT1151_RST_PORT, GT1151_RST_PIN, GPIO_PIN_SET);

    /* According to your timing requirement */
    HAL_Delay(60);

    
}

uint8_t TOUCH_Scan(uint16_t *x, uint16_t *y)
{
    uint8_t status;
    uint8_t point_buf[8];
    uint8_t clear_val = 0;

    if ((x == 0) || (y == 0))
    {
        return 0;
    }

    if (GT1151_Read(GT1151_REG_STATUS, &status, 1) != HAL_OK)
    {
        return 0;
    }

    /* Bit7 = 1 means data ready */
    if ((status & 0x80U) == 0U)
    {
        return 0;
    }

    /* Bit3:0 = touch point count */
    if ((status & 0x0FU) == 0U)
    {
        (void)GT1151_Write(GT1151_REG_STATUS, &clear_val, 1);
        return 0;
    }

    /* Read first point, 8 bytes per point */
    if (GT1151_Read(GT1151_REG_FIRST_POINT, point_buf, 8) != HAL_OK)
    {
        (void)GT1151_Write(GT1151_REG_STATUS, &clear_val, 1);
        return 0;
    }

    /* Goodix point data format:
       [0] Track ID
       [1] X_L
       [2] X_H
       [3] Y_L
       [4] Y_H
       [5] W_L
       [6] W_H
       [7] reserved
    */
    *x = ((uint16_t)point_buf[2] << 8) | point_buf[1];
    *y = ((uint16_t)point_buf[4] << 8) | point_buf[3];

    /* Must clear status after reading */
    (void)GT1151_Write(GT1151_REG_STATUS, &clear_val, 1);

    return 1;
}