#ifndef __OLED_H
#define __OLED_H

#include "CH58x_common.h"

/* ============================================================
 *  0.66" OLED  (SSD1306 控制器, 64 x 48 可见像素)
 *
 *  硬件 (SPI1, 4-wire):
 *    SCK1 - PA0
 *    MOSI1- PA1
 *    RES  - PA2
 *    DC   - PA3
 *    CS   - 接 GND（直接选中）
 *
 *  说明:
 *    SSD1306 的 GDDRAM 仍是 128x64，但 0.66 屏只在中间一块 64x48
 *    可见，所以我们写显存时要做 X 偏移 +32，并只用 page 1..6（中间
 *    6 行）。本驱动通过帧缓冲在 RAM 里画好整屏后再批量推送，
 *    避免逐字节命令切换 DC 引脚 -> 大幅降低主循环开销。
 *
 *  使用方式:
 *    OLED_Init();
 *    OLED_FB_Clear();
 *    OLED_DrawString(...);
 *    OLED_Refresh();          // 把帧缓冲推到屏上
 * ============================================================ */

#define OLED_WIDTH      64
#define OLED_HEIGHT     48
#define OLED_PAGES      (OLED_HEIGHT / 8)   /* = 6 */
#define OLED_X_OFFSET   32                  /* 0.66" 屏的列偏移 */

/* === 引脚控制宏 === */
#define OLED_RES_Clr()  GPIOA_ResetBits(GPIO_Pin_2)
#define OLED_RES_Set()  GPIOA_SetBits(GPIO_Pin_2)
#define OLED_DC_Clr()   GPIOA_ResetBits(GPIO_Pin_3)
#define OLED_DC_Set()   GPIOA_SetBits(GPIO_Pin_3)

/* === 基础接口 === */
void OLED_Init(void);
void OLED_Refresh(void);              /* 把帧缓冲整屏发送 */
void OLED_RefreshDirty(void);         /* 仅刷新脏页（推荐主循环周期调用） */
void OLED_RefreshDirtyStep(uint8_t max_pages);
uint8_t OLED_HasDirty(void);
void OLED_FB_Clear(void);
void OLED_SetContrast(uint8_t val);

/* === 帧缓冲绘图 (左上角原点 0,0；x: 0..63, y: 0..47) === */
void OLED_DrawPixel(int x, int y, uint8_t on);
void OLED_DrawHLine(int x, int y, int w, uint8_t on);
void OLED_DrawVLine(int x, int y, int h, uint8_t on);
void OLED_DrawRect(int x, int y, int w, int h, uint8_t on);
void OLED_FillRect(int x, int y, int w, int h, uint8_t on);

/* === 文字渲染 === */
void OLED_DrawChar6x8(int x, int y, char ch);                   /* 6x8 ASCII */
void OLED_DrawString6x8(int x, int y, const char *s);
void OLED_DrawChar12x16(int x, int y, char ch);                 /* 12x16 ASCII (限定 0-9 . : %) */
void OLED_DrawString12x16(int x, int y, const char *s);

/* === 进度条（外框 + 内填充）=== */
void OLED_DrawProgress(int x, int y, int w, int h, uint8_t pct);

/* === 折线 / 波形（用于 ECG）===
 *  把 buf 中 N 个采样点画到矩形 (x, y, w, h) 内，
 *  自动按 buf_min..buf_max 缩放 Y 轴。
 */
void OLED_DrawWave(int x, int y, int w, int h,
                   const int16_t *buf, int n,
                   int16_t v_min, int16_t v_max);

#endif
