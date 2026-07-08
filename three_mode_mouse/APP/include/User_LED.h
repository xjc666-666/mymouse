#ifndef __USER_LED_H
#define __USER_LED_H

#include "CH58x_common.h"

// --- 用户 LED 引脚宏定义 ---
#define LED1_PIN    GPIO_Pin_4   // 对应 PA4
#define LED2_PIN    GPIO_Pin_6   // 对应 PA6

// --- 方便调用的枚举或宏定义 ---
#define LED_1       1
#define LED_2       2
#define LED_ALL     3

// --- 函数声明 ---
void User_LED_Init(void);
void User_LED_On(uint8_t led_id);
void User_LED_Off(uint8_t led_id);
void User_LED_Toggle(uint8_t led_id);

#endif