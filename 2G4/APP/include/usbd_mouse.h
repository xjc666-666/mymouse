#ifndef __USBD_MOUSE_H
#define __USBD_MOUSE_H

#include "CH58x_common.h"
#include "usb_desc.h"  // 把长度宏传递给底层

// 欺骗底层中断的键盘灯变量
extern volatile uint8_t KB_LED_Cur_Status;

// 对外接口
void USBD_Mouse_Init(void);

// 高速中转发送函数 (直接将射频收到的裸数据发送给电脑)
// 注意：X 和 Y 变成了 16 位整数，以支持高精度电竞坐标
void USBD_Mouse_Send_Raw(uint8_t btn, int16_t x, int16_t y, int8_t wheel);

// 欺骗底层的睡眠函数
void MCU_Sleep_Wakeup_Operate(void);

#endif