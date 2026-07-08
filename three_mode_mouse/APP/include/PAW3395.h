#ifndef __PAW3395_H
#define __PAW3395_H

#include "CH58x_common.h"

// --- 硬件引脚定义 (确保与你的实际接线一致) ---
#define PAW_CS_PIN      GPIO_Pin_5   // PA5
#define PAW_SCK_PIN     GPIO_Pin_13  // PA13
#define PAW_MOSI_PIN    GPIO_Pin_14  // PA14
#define PAW_MISO_PIN    GPIO_Pin_15  // PA15
#define PAW_MOTION_PIN  GPIO_Pin_8   // PA8
#define PAW_RESET_PIN   GPIO_Pin_9   // PA9

// --- 快速片选宏 ---
#define PAW_CS_LOW()    GPIOA_ResetBits(PAW_CS_PIN)
#define PAW_CS_HIGH()   GPIOA_SetBits(PAW_CS_PIN)

// 运动数据结构体
typedef struct {
    int16_t deltaX;
    int16_t deltaY;
    uint8_t isMotion;
    uint8_t rawMotion; 
    uint8_t squal;     
} PAW3395_Data_t;

// --- 接口声明 ---
void PAW3395_Init(void);
void PAW3395_ReadMotion(PAW3395_Data_t *data);
void PAW3395_SetDPI(uint16_t CPI_Num);
uint8_t PAW3395_GetID(void);

#endif