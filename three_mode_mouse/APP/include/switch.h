#ifndef __SWITCH_H
#define __SWITCH_H

#include "CH58x_common.h"

/* ============================================================
 * 三模切换：两位拨码开关 (低电平 = 按下)
 *   PB7   PA7    模式
 *   高    高     BLE
 *   低    高     有线 (Wired USB)
 *   高    低     2.4G RF
 *   低    低     非法（保留 / 未连接）
 *
 * 软件需开启上拉，避免悬空时误判。
 * ============================================================ */
#define MODE_PIN_PB     GPIO_Pin_7   /* PB7 */
#define MODE_PIN_PA     GPIO_Pin_7   /* PA7 */

typedef enum {
    MODE_WIRED = 0,
    MODE_BLE,
    MODE_24G,
    MODE_UNKNOWN
} Mouse_Mode_t;

void Mode_Switch_Init(void);
Mouse_Mode_t Mode_Switch_Read(void);

#endif
