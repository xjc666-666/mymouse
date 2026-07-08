#ifndef __USBD_MOUSE_H
#define __USBD_MOUSE_H

#include "CH58x_common.h"
#include "PAW3395.h"
#include "Mouse_Input.h"
#include "usb_desc.h"

/* =========================================================
 * 按键索引（与 Macro_Table 数组下标对应）
 * ========================================================= */
#define BTN_IDX_LEFT    0
#define BTN_IDX_RIGHT   1
#define BTN_IDX_MIDDLE  2
#define BTN_IDX_FORWARD 3   /* 侧键 1 */
#define BTN_IDX_BACK    4   /* 侧键 2 */
#define MACRO_BTN_NUM   5

/* DPI 档位数量 */
#define DPI_STAGE_NUM   4
#define DPI_MAX_VALUE   6400

/* 回报率档位（写入 USB EP bInterval 字段，高速模式：interval = 2^(b-1) * 125us） */
#define POLL_INTERVAL_1000HZ    0x04   /* 1ms  */
#define POLL_INTERVAL_500HZ     0x05   /* 2ms  */
#define POLL_INTERVAL_250HZ     0x06   /* 4ms  */
#define POLL_INTERVAL_125HZ     0x07   /* 8ms  */

/* 宏映射结构体 */
typedef struct {
    uint8_t is_macro;     /* 0: 原生鼠标按键 1: 键盘组合键 */
    uint8_t modifier;     /* HID 修饰键位掩码 */
    uint8_t key_code;     /* 普通键 HID Usage */
} Button_Macro_t;

/* 系统配置（写入 Flash 的整体结构） */
typedef struct {
    uint32_t        magic;                  /* 魔术字，用于识别有效配置 */
    uint8_t         version;                /* 配置版本 */
    uint8_t         dpi_index;              /* 当前选中档位 0..3 */
    uint8_t         poll_interval;          /* USB bInterval */
    uint8_t         reserved0;
    uint16_t        dpi_levels[DPI_STAGE_NUM];   /* 4 档自定义 DPI 值，<=6400 */
    Button_Macro_t  macros[MACRO_BTN_NUM];
    uint8_t         pad[8];                 /* 对齐补齐 */
} Mouse_Config_t;

#define MOUSE_CFG_MAGIC     0x4D435548u   /* 'MCUH' */
#define MOUSE_CFG_VERSION   0x03

/* 全局变量 */
extern volatile uint8_t KB_LED_Cur_Status;
extern Mouse_Config_t   g_cfg;
extern Button_Macro_t   Macro_Table[MACRO_BTN_NUM];
extern uint16_t         dpi_levels[DPI_STAGE_NUM];
extern uint8_t          dpi_index;

/* 接口 */
void USBD_Mouse_Init(void);
void USBD_Mouse_Send(PAW3395_Data_t *sensorData, Mouse_State_t *mouseBtn);
void USBD_Keyboard_Send(uint8_t modifier, uint8_t key_code);
void USBD_Keyboard_Release(void);
void USBD_Custom_Process_Cmd(uint8_t *rx_buf, uint16_t len);
void USBD_Custom_Send_State(uint8_t btn_mask, int16_t dx, int16_t dy, int8_t wheel);
void USBD_Custom_Send_Bio(int16_t ecg, uint8_t hr, uint8_t spo2, uint8_t status, uint8_t batt_pct);
void Mouse_Cmd_Service(void);
void MCU_Sleep_Wakeup_Operate(void);

/* 配置加载/保存（实现位于 hidmouse_main.c） */
void Mouse_Config_Load(void);
void Mouse_Config_Save(void);
void Mouse_Apply_Poll_Rate(uint8_t bInterval);

#endif
