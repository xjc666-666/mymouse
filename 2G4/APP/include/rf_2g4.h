#ifndef __RF_2G4_H
#define __RF_2G4_H

#include "CONFIG.h"

// 接收到的最新鼠标数据结构
typedef struct {
    uint8_t buttons;
    int16_t  x;
    int16_t  y;
    int8_t  wheel;
} Dongle_Data_t;

// 暴露给主循环的标志位和数据缓存
extern volatile uint8_t Has_New_RF_Data;
extern Dongle_Data_t    Mouse_RF_Data;

// 接收端初始化函数
extern void G24_Dongle_Init(void);/*  */

#endif