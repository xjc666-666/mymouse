#ifndef __OLED_UI_H
#define __OLED_UI_H

#include "CH58x_common.h"

/* ============================================================
 *  OLED UI 控制层
 *
 *  在主循环周期性调用 OLED_UI_Tick()。内部做了节流（默认 ~10Hz
 *  刷新），并且只推送脏页，最大限度避免占用 USB 主循环时间。
 *
 *  数据源：直接通过 extern 拿全局的 g_cfg / Battery / MKS142。
 * ============================================================ */
void OLED_UI_Init(void);
void OLED_UI_Tick(void);

/* 可选：强制立刻全屏重绘（如模式切换时使用） */
void OLED_UI_ForceRedraw(void);

#endif
