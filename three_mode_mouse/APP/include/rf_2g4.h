#ifndef __RF_2G4_H
#define __RF_2G4_H

#include "CONFIG.h"

// 暴露给外面的初始化和发送函数
extern void G24_Mouse_Init(void);
extern void G24_Mouse_Send(uint8_t buttons, int16_t x, int16_t y, int8_t wheel);

#endif