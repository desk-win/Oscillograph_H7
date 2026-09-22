#include "osc_operate.h"


/********************************
*   S2      S1      S0      CH  *
*   0       0       0       A0  *
*   0       0       1       A1  *
*   0       1       0       A2  *
*   0       1       1       A3  *
*   1       0       0       A4  *
*   1       0       1       A5  *
*   1       1       0       A6  *
*   1       1       1       A7  *
*********************************/

void S_4051_A(GPIO_PinState s2, GPIO_PinState s1, GPIO_PinState s0){
    HAL_GPIO_WritePin(A_S2_GPIO_Port, A_S2_Pin, s2);
    HAL_GPIO_WritePin(A_S1_GPIO_Port, A_S1_Pin, s1);
    HAL_GPIO_WritePin(A_S0_GPIO_Port, A_S0_Pin, s0);
}

void S_4051_B(GPIO_PinState s2, GPIO_PinState s1, GPIO_PinState s0){
    HAL_GPIO_WritePin(B_S2_GPIO_Port, B_S2_Pin, s2);
    HAL_GPIO_WritePin(B_S1_GPIO_Port, B_S1_Pin, s1);
    HAL_GPIO_WritePin(B_S0_GPIO_Port, B_S0_Pin, s0);
}

void OSC_Ch1_Multiple(uint8_t multiple, uint8_t DC){
    switch (multiple) {
        case 1:
            S_4051_A(GPIO_PIN_RESET, GPIO_PIN_RESET, GPIO_PIN_RESET);
        break;
        case 2:
            S_4051_A(GPIO_PIN_RESET, GPIO_PIN_RESET, GPIO_PIN_SET);
        break;
        case 4:
            S_4051_A(GPIO_PIN_RESET, GPIO_PIN_SET, GPIO_PIN_RESET);
        break;
        case 8:
            S_4051_A(GPIO_PIN_RESET, GPIO_PIN_SET, GPIO_PIN_SET);
        break;
        case 16:
            S_4051_A(GPIO_PIN_SET, GPIO_PIN_RESET, GPIO_PIN_RESET);
        break;
        case 32:
            S_4051_A(GPIO_PIN_SET, GPIO_PIN_RESET, GPIO_PIN_SET);
        break;
        case 64:
            S_4051_A(GPIO_PIN_SET, GPIO_PIN_SET, GPIO_PIN_RESET);
        break;
        case 128:
            S_4051_A(GPIO_PIN_SET, GPIO_PIN_SET, GPIO_PIN_SET);
        break;
        default:
            S_4051_A(GPIO_PIN_RESET, GPIO_PIN_RESET, GPIO_PIN_RESET);
    }
    if (DC == 0) {
        HAL_GPIO_WritePin(OSC_AC1_GPIO_Port, OSC_AC1_Pin, GPIO_PIN_RESET);
    }else {
        HAL_GPIO_WritePin(OSC_AC1_GPIO_Port, OSC_AC1_Pin, GPIO_PIN_SET);
    }
}

void OSC_Ch2_Multiple(uint8_t multiple, uint8_t DC){
    switch (multiple) {
        case 1:
            S_4051_B(GPIO_PIN_RESET, GPIO_PIN_RESET, GPIO_PIN_RESET);
        break;
        case 2:
            S_4051_B(GPIO_PIN_RESET, GPIO_PIN_RESET, GPIO_PIN_SET);
        break;
        case 4:
            S_4051_B(GPIO_PIN_RESET, GPIO_PIN_SET, GPIO_PIN_RESET);
        break;
        case 8:
            S_4051_B(GPIO_PIN_RESET, GPIO_PIN_SET, GPIO_PIN_SET);
        break;
        case 16:
            S_4051_B(GPIO_PIN_SET, GPIO_PIN_RESET, GPIO_PIN_RESET);
        break;
        case 32:
            S_4051_B(GPIO_PIN_SET, GPIO_PIN_RESET, GPIO_PIN_SET);
        break;
        case 64:
            S_4051_B(GPIO_PIN_SET, GPIO_PIN_SET, GPIO_PIN_RESET);
        break;
        case 128:
            S_4051_B(GPIO_PIN_SET, GPIO_PIN_SET, GPIO_PIN_SET);
        break;
        default:
            S_4051_B(GPIO_PIN_RESET, GPIO_PIN_RESET, GPIO_PIN_RESET);
    }
    if (DC == 0) {
        HAL_GPIO_WritePin(OSC_AC2_GPIO_Port, OSC_AC2_Pin, GPIO_PIN_RESET);
    }else {
        HAL_GPIO_WritePin(OSC_AC2_GPIO_Port, OSC_AC2_Pin, GPIO_PIN_SET);
    }
}
