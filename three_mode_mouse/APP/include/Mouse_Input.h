#ifndef __MOUSE_INPUT_H
#define __MOUSE_INPUT_H

#include "CH58x_common.h"

// --- 核心按键引脚定义 (GPIOB) ---
#define BTN_LEFT_PIN    GPIO_Pin_0   // PB0
#define BTN_RIGHT_PIN   GPIO_Pin_1   // PB1
#define BTN_MIDDLE_PIN  GPIO_Pin_2   // PB2
#define BTN_DPI_PIN     GPIO_Pin_5   // PB5

// --- 侧键引脚定义 (GPIOB) ---
#define BTN_BACK_PIN    GPIO_Pin_3   // PB3 (后退)
#define BTN_FORWARD_PIN GPIO_Pin_4   // PB4 (前进)

// --- 滚轮编码器定义 (GPIOB) ---
#define ENCODER_A_PIN   GPIO_Pin_19  // PB19
#define ENCODER_B_PIN   GPIO_Pin_18  // PB18

// 鼠标输入状态结构体
typedef struct {
    uint8_t Left;
    uint8_t Right;
    uint8_t Middle;
    uint8_t Forward; // 侧键：前进
    uint8_t Back;    // 侧键：后退
    uint8_t DPI;
    int32_t WheelCount; // 滚轮累计计数值
} Mouse_State_t;

// 函数声明
void Mouse_Input_Init(void);
void Mouse_Input_Scan(Mouse_State_t *state);

#endif