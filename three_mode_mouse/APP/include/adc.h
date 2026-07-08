#ifndef __ADC_H
#define __ADC_H

#include "CH58x_common.h"

/* ============================================================
 * 电池电量 ADC 驱动
 *
 * 硬件:
 *   PA12 -> AIN2，连接 锂电池 1/2 分压（电池正极经 R1/R2 等阻值
 *   分压器到 PA12，再到 GND），所以 ADC 测到的是 V_BAT/2。
 *
 * 锂电池电压区间假设:
 *   满电 ~ 4.2V  (PA12 测到 2.10V)
 *   空电 ~ 3.3V  (PA12 测到 1.65V)
 *
 * 输出:
 *   uint8_t 0..100 表示电量百分比
 *   uint16_t mV (电池实际毫伏)
 *
 * 接口为线程安全的（仅在主循环中读取）。
 * ============================================================ */

#define BATT_ADC_CHANNEL    CH_EXTIN_2     /* PA12 -> AIN2 */
#define BATT_ADC_GPIO_PIN   GPIO_Pin_12

/* 锂电池阈值（毫伏） */
#define BATT_FULL_MV        4200
#define BATT_EMPTY_MV       3300

void    Battery_ADC_Init(void);              /* 初始化 ADC + 校准 */
uint16_t Battery_ReadRawAdc(void);           /* 单次 ADC 转换原始值 */
uint16_t Battery_ReadVoltage_mV(void);       /* 返回电池实际电压 mV（含分压补偿） */
uint8_t Battery_ReadPercent(void);           /* 返回电池电量 0..100 % */
uint8_t Battery_GetCachedPercent(void);      /* 获取上一次缓存的百分比（不触发 ADC） */
void    Battery_PeriodicMeasure(void);       /* 主循环周期调用，限频后台测量 */

#endif
