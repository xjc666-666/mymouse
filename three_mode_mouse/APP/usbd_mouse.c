#include "usbd_mouse.h"
#include "ch585_usbhs_device.h"
#include "adc.h"
#include "mks142.h"
#include "oled_ui.h"
#include <string.h>

extern void Mouse_Config_Save(void);
extern void Mouse_Apply_Poll_Rate(uint8_t bInterval);
extern void Update_DPI_LED(uint8_t index);
extern void PAW3395_SetDPI(uint16_t dpi);

volatile uint8_t KB_LED_Cur_Status = 0;
void MCU_Sleep_Wakeup_Operate(void) { }

static uint8_t USB_Mouse_Pack[6] = {0x00};

__attribute__((aligned(4))) uint8_t KB_Report_Buf[8] = {0};
__attribute__((aligned(4))) uint8_t Custom_Tx_Buf[64] = {0};

/* =========================================================
 * 配置写入主循环延迟处理标志
 *
 * USB 中断里执行 EEPROM_ERASE 会卡住数十毫秒，导致 Host 误判
 * 设备断连。把所有 Flash / SPI（PAW3395）操作延迟到主循环里
 * 由 Mouse_Cmd_Service() 执行，从而保持中断快速返回。
 * ========================================================= */
volatile uint8_t  g_cfg_save_pending   = 0;   /* 主循环检测到 1 -> 落盘 */
volatile uint8_t  g_cfg_apply_dpi      = 0;   /* 主循环检测到 1 -> 调用 PAW3395_SetDPI */
volatile uint16_t g_cfg_pending_dpi    = 0;
volatile uint8_t  g_cfg_apply_led      = 0;   /* 主循环检测到 1 -> 更新档位灯 */
volatile uint8_t  g_cfg_pending_led    = 0;
volatile uint16_t g_cfg_renumerate_cd  = 0;   /* >0 时主循环倒计时，到 0 时执行重枚举 */

void USBD_Mouse_Init(void) {
    USBHS_Device_Init(ENABLE);
}

/* ==========================================================
 * 键盘报告：通过 EP1 发送组合键
 * ========================================================== */
void USBD_Keyboard_Send(uint8_t modifier, uint8_t key_code)
{
    if( USBHS_DevEnumStatus )
    {
        KB_Report_Buf[0] = modifier;
        KB_Report_Buf[1] = 0x00;
        KB_Report_Buf[2] = key_code;
        KB_Report_Buf[3] = 0x00;
        KB_Report_Buf[4] = 0x00;
        KB_Report_Buf[5] = 0x00;
        KB_Report_Buf[6] = 0x00;
        KB_Report_Buf[7] = 0x00;
        USBHS_Endp_DataUp(DEF_UEP1, KB_Report_Buf, 8, DEF_UEP_CPY_LOAD);
    }
}

void USBD_Keyboard_Release(void)
{
    if( USBHS_DevEnumStatus )
    {
        for(uint8_t i = 0; i < 8; i++) KB_Report_Buf[i] = 0x00;
        USBHS_Endp_DataUp(DEF_UEP1, KB_Report_Buf, 8, DEF_UEP_CPY_LOAD);
    }
}

/* ==========================================================
 * 主循环延迟服务（由 hidmouse_main.c 的有线主循环周期调用）
 * 把耗时的 Flash 操作 / 传感器寄存器写入移出中断
 * ========================================================== */
void Mouse_Cmd_Service(void)
{
    if(g_cfg_apply_dpi) {
        g_cfg_apply_dpi = 0;
        PAW3395_SetDPI(g_cfg_pending_dpi);
    }
    if(g_cfg_apply_led) {
        g_cfg_apply_led = 0;
        Update_DPI_LED(g_cfg_pending_led);
    }
    if(g_cfg_save_pending) {
        g_cfg_save_pending = 0;
        Mouse_Config_Save();
    }
    /* 重新枚举：等到倒计时归零再触发，
     * 留出时间让 ACK 响应包送达上位机、Flash 写完成、状态稳定。 */
    if(g_cfg_renumerate_cd) {
        g_cfg_renumerate_cd--;
        if(g_cfg_renumerate_cd == 0) {
            /* 软断开 USB：关掉设备使能 + PHY，让 Host 看到 D+ 拉低；
             * 等待 ~200ms 后重新使能，Host 会发起一次新的枚举，
             * 这时读取到的就是更新后的 bInterval。 */
            USBHS_Device_Init(DISABLE);
            mDelaymS(220);
            USBHS_Device_Init(ENABLE);
        }
    }
}

/* ==========================================================
 * 自定义协议：上位机经 EP3 OUT 下发 64 字节命令
 *
 * 帧格式（小端）：
 *   [0]   = 0x55                头部 1
 *   [1]   = 0xAA                头部 2
 *   [2]   = CMD                 命令字
 *   [3..] = 载荷
 *
 * CMD 列表：
 *   0x10  GET_FULL_CONFIG         读取完整配置
 *   0x11  SET_DPI_VALUES          设置 4 档 DPI（每档 2 字节，<=6400）
 *   0x12  SET_DPI_INDEX           切换当前 DPI 档位
 *   0x13  SET_POLL_RATE           设置回报率（1=1000Hz 2=500 3=250 4=125）
 *   0x14  SET_MACRO               设置某按键的宏 (idx, is_macro, modifier, key)
 *   0x15  RESET_MACRO             清除某按键的宏 (idx)
 *   0x16  SAVE_CONFIG             立即写入 Flash
 *   0x17  RESET_FACTORY           恢复出厂设置
 *   0x18  PING                    回环测试
 *
 * 回包均为 64 字节，以 0x55 0xAA <CMD> <STATUS> 开头。
 * ========================================================== */

static void cfg_make_response(uint8_t cmd, uint8_t status)
{
    memset(Custom_Tx_Buf, 0, 64);
    Custom_Tx_Buf[0] = 0x55;
    Custom_Tx_Buf[1] = 0xAA;
    Custom_Tx_Buf[2] = cmd;
    Custom_Tx_Buf[3] = status;
}

static void cfg_send_response(void)
{
    USBHS_Endp_DataUp(DEF_UEP3, Custom_Tx_Buf, 64, DEF_UEP_CPY_LOAD);
}

void USBD_Custom_Process_Cmd(uint8_t *rx_buf, uint16_t len)
{
    if(len < 4 || rx_buf[0] != 0x55 || rx_buf[1] != 0xAA) {
        return;
    }
    uint8_t cmd = rx_buf[2];

    switch(cmd)
    {
        /* -------- 0x10 读取完整配置 -------- */
        case 0x10:
        {
            cfg_make_response(0x10, 0x00);
            /* 布局：
             *   [4] dpi_index
             *   [5] poll_interval
             *   [6] version
             *   [7] btn_num
             *   [8..15] dpi_levels (4 * uint16, little endian)
             *   [16..30] macros  (5 * 3 字节)
             */
            Custom_Tx_Buf[4] = g_cfg.dpi_index;
            Custom_Tx_Buf[5] = g_cfg.poll_interval;
            Custom_Tx_Buf[6] = g_cfg.version;
            Custom_Tx_Buf[7] = MACRO_BTN_NUM;
            for(int i = 0; i < DPI_STAGE_NUM; i++) {
                Custom_Tx_Buf[8 + i * 2 + 0] = (uint8_t)(g_cfg.dpi_levels[i] & 0xFF);
                Custom_Tx_Buf[8 + i * 2 + 1] = (uint8_t)((g_cfg.dpi_levels[i] >> 8) & 0xFF);
            }
            for(int i = 0; i < MACRO_BTN_NUM; i++) {
                Custom_Tx_Buf[16 + i * 3 + 0] = g_cfg.macros[i].is_macro;
                Custom_Tx_Buf[16 + i * 3 + 1] = g_cfg.macros[i].modifier;
                Custom_Tx_Buf[16 + i * 3 + 2] = g_cfg.macros[i].key_code;
            }
            cfg_send_response();
            break;
        }

        /* -------- 0x11 设置 4 档 DPI 值 -------- */
        case 0x11:
        {
            if(len < 4 + DPI_STAGE_NUM * 2) {
                cfg_make_response(0x11, 0xE1);
                cfg_send_response();
                break;
            }
            for(int i = 0; i < DPI_STAGE_NUM; i++) {
                uint16_t v = (uint16_t)rx_buf[3 + i * 2] | ((uint16_t)rx_buf[3 + i * 2 + 1] << 8);
                if(v < 50)            v = 50;
                if(v > DPI_MAX_VALUE) v = DPI_MAX_VALUE;
                v = (v / 50) * 50;
                if(v < 50) v = 50;
                g_cfg.dpi_levels[i] = v;
                dpi_levels[i] = v;
            }
            /* 延迟到主循环执行 */
            g_cfg_pending_dpi  = dpi_levels[dpi_index];
            g_cfg_apply_dpi    = 1;
            OLED_UI_ForceRedraw();
            g_cfg_save_pending = 1;
            cfg_make_response(0x11, 0x00);
            cfg_send_response();
            break;
        }

        /* -------- 0x12 切换 DPI 档位 -------- */
        case 0x12:
        {
            uint8_t idx = rx_buf[3];
            if(idx >= DPI_STAGE_NUM) {
                cfg_make_response(0x12, 0xE1);
                cfg_send_response();
                break;
            }
            dpi_index = idx;
            g_cfg.dpi_index = idx;
            g_cfg_pending_dpi  = dpi_levels[dpi_index];
            g_cfg_pending_led  = dpi_index;
            g_cfg_apply_dpi    = 1;
            g_cfg_apply_led    = 1;
            OLED_UI_ForceRedraw();
            g_cfg_save_pending = 1;
            cfg_make_response(0x12, 0x00);
            cfg_send_response();
            break;
        }

        /* -------- 0x13 设置回报率 -------- */
        case 0x13:
        {
            uint8_t code = rx_buf[3];
            uint8_t bInterval = POLL_INTERVAL_1000HZ;
            switch(code) {
                case 1: bInterval = POLL_INTERVAL_1000HZ; break;
                case 2: bInterval = POLL_INTERVAL_500HZ;  break;
                case 3: bInterval = POLL_INTERVAL_250HZ;  break;
                case 4: bInterval = POLL_INTERVAL_125HZ;  break;
                default:
                    cfg_make_response(0x13, 0xE1);
                    cfg_send_response();
                    return;
            }
            g_cfg.poll_interval = bInterval;
            Mouse_Apply_Poll_Rate(bInterval);  /* 只改 RAM 中描述符，快速 */
            g_cfg_save_pending = 1;
            cfg_make_response(0x13, 0x00);
            cfg_send_response();
            /* 落盘并发完响应后，由主循环触发软断开/重新枚举，
             * 让 Host 立即读取到新的 bInterval，无需用户手动拔插。
             * 倒计时给 ACK 包足够时间送达上位机。 */
            g_cfg_renumerate_cd = 200;
            break;
        }

        /* -------- 0x14 设置宏 -------- */
        case 0x14:
        {
            uint8_t idx       = rx_buf[3];
            uint8_t is_macro  = rx_buf[4];
            uint8_t modifier  = rx_buf[5];
            uint8_t key_code  = rx_buf[6];
            if(idx >= MACRO_BTN_NUM) {
                cfg_make_response(0x14, 0xE1);
                cfg_send_response();
                break;
            }
            g_cfg.macros[idx].is_macro = is_macro ? 1 : 0;
            g_cfg.macros[idx].modifier = modifier;
            g_cfg.macros[idx].key_code = key_code;
            Macro_Table[idx] = g_cfg.macros[idx];
            g_cfg_save_pending = 1;
            cfg_make_response(0x14, 0x00);
            cfg_send_response();
            break;
        }

        /* -------- 0x15 清除单个按键的宏 -------- */
        case 0x15:
        {
            uint8_t idx = rx_buf[3];
            if(idx >= MACRO_BTN_NUM) {
                cfg_make_response(0x15, 0xE1);
                cfg_send_response();
                break;
            }
            g_cfg.macros[idx].is_macro = 0;
            g_cfg.macros[idx].modifier = 0;
            g_cfg.macros[idx].key_code = 0;
            Macro_Table[idx] = g_cfg.macros[idx];
            g_cfg_save_pending = 1;
            cfg_make_response(0x15, 0x00);
            cfg_send_response();
            break;
        }

        /* -------- 0x16 主动保存（多次设置后一次落盘）-------- */
        case 0x16:
        {
            g_cfg_save_pending = 1;
            cfg_make_response(0x16, 0x00);
            cfg_send_response();
            break;
        }

        /* -------- 0x17 恢复出厂 -------- */
        case 0x17:
        {
            g_cfg.magic         = MOUSE_CFG_MAGIC;
            g_cfg.version       = MOUSE_CFG_VERSION;
            g_cfg.dpi_index     = 2;
            g_cfg.poll_interval = POLL_INTERVAL_1000HZ;
            g_cfg.dpi_levels[0] = 6400;
            g_cfg.dpi_levels[1] = 3200;
            g_cfg.dpi_levels[2] = 1600;
            g_cfg.dpi_levels[3] = 800;
            for(int i = 0; i < MACRO_BTN_NUM; i++) {
                g_cfg.macros[i].is_macro = 0;
                g_cfg.macros[i].modifier = 0;
                g_cfg.macros[i].key_code = 0;
                Macro_Table[i] = g_cfg.macros[i];
            }
            for(int i = 0; i < DPI_STAGE_NUM; i++) {
                dpi_levels[i] = g_cfg.dpi_levels[i];
            }
            dpi_index = g_cfg.dpi_index;
            Mouse_Apply_Poll_Rate(g_cfg.poll_interval);
            g_cfg_pending_dpi  = dpi_levels[dpi_index];
            g_cfg_pending_led  = dpi_index;
            g_cfg_apply_dpi    = 1;
            g_cfg_apply_led    = 1;
            OLED_UI_ForceRedraw();
            g_cfg_save_pending = 1;
            cfg_make_response(0x17, 0x00);
            cfg_send_response();
            break;
        }

        /* -------- 0x18 PING 回环 -------- */
        case 0x18:
        {
            cfg_make_response(0x18, 0x00);
            Custom_Tx_Buf[4] = rx_buf[3];
            Custom_Tx_Buf[5] = rx_buf[4];
            Custom_Tx_Buf[6] = rx_buf[5];
            Custom_Tx_Buf[7] = rx_buf[6];
            cfg_send_response();
            break;
        }

        /* -------- 0x19 读取电池电量（毫伏 + 百分比） -------- */
        case 0x19:
        {
            cfg_make_response(0x19, 0x00);
            uint8_t  pct = Battery_GetCachedPercent();
            uint16_t mv  = Battery_ReadVoltage_mV();
            Custom_Tx_Buf[4] = pct;
            Custom_Tx_Buf[5] = (uint8_t)(mv & 0xFF);
            Custom_Tx_Buf[6] = (uint8_t)((mv >> 8) & 0xFF);
            cfg_send_response();
            break;
        }

        default:
            cfg_make_response(cmd, 0xEE); /* 未知命令 */
            cfg_send_response();
            break;
    }
}

/* ==========================================================
 * 实时按键 / 移动状态广播给上位机（用于可视化）
 * EP3 IN 单次发送 64 字节，结构：
 *   [0..1]   0x55 0xAA
 *   [2]      0x80 (异步通知)
 *   [3]      btn_mask     宏过滤后的按键位掩码 (供 HID 报告对照)
 *   [4..5]   dx (int16)
 *   [6..7]   dy (int16)
 *   [8]      wheel (int8)
 *   [9]      dpi_index
 *   [10..11] current_dpi (uint16)
 *   [12]     raw_btn_mask 原始 GPIO 按键位掩码 (上位机诊断用)
 *   [13]     battery_percent  电池电量 0..100
 * ========================================================== */
extern volatile uint8_t Raw_Btn_Mask;

void USBD_Custom_Send_State(uint8_t btn_mask, int16_t dx, int16_t dy, int8_t wheel)
{
    if(!USBHS_DevEnumStatus) return;
    memset(Custom_Tx_Buf, 0, 64);
    Custom_Tx_Buf[0]  = 0x55;
    Custom_Tx_Buf[1]  = 0xAA;
    Custom_Tx_Buf[2]  = 0x80;
    Custom_Tx_Buf[3]  = btn_mask;
    Custom_Tx_Buf[4]  = (uint8_t)(dx & 0xFF);
    Custom_Tx_Buf[5]  = (uint8_t)((dx >> 8) & 0xFF);
    Custom_Tx_Buf[6]  = (uint8_t)(dy & 0xFF);
    Custom_Tx_Buf[7]  = (uint8_t)((dy >> 8) & 0xFF);
    Custom_Tx_Buf[8]  = (uint8_t)wheel;
    Custom_Tx_Buf[9]  = dpi_index;
    Custom_Tx_Buf[10] = (uint8_t)(dpi_levels[dpi_index] & 0xFF);
    Custom_Tx_Buf[11] = (uint8_t)((dpi_levels[dpi_index] >> 8) & 0xFF);
    Custom_Tx_Buf[12] = Raw_Btn_Mask;
    Custom_Tx_Buf[13] = Battery_GetCachedPercent();
    USBHS_Endp_DataUp(DEF_UEP3, Custom_Tx_Buf, 64, DEF_UEP_CPY_LOAD);
}

/* ==========================================================
 * 心电 / 血氧数据广播 (异步通知 0x81)
 *   [0..1]   0x55 0xAA
 *   [2]      0x81
 *   [3..4]   ecg (int16, LE)
 *   [5]      heart_rate (BPM, 0=无效)
 *   [6]      spo2 (%, 0=无效)
 *   [7]      status (bit0 指夹到位 / bit1 电极接触)
 *   [8]      battery_percent
 *   --- 诊断字段（用于排查上位机看不到数据的根因）---
 *   [9..12]  uart_rx_total   (u32 LE) UART3 ISR 累计收到的字节数
 *   [13..16] uart_rx_overflow(u32 LE) ring 满丢弃字节数
 *   [17..20] frames_total    (u32 LE) 成功解析的 RT_PACK 帧数
 *   [21..24] frames_drop     (u32 LE) sane 校验失败次数
 *   [25..28] start_retry_n   (u32 LE) 启动看门狗重发次数
 *   其余字节为 0，便于上位机扩展
 * ========================================================== */
void USBD_Custom_Send_Bio(int16_t ecg, uint8_t hr, uint8_t spo2, uint8_t status, uint8_t batt_pct)
{
    if(!USBHS_DevEnumStatus) return;
    memset(Custom_Tx_Buf, 0, 64);
    Custom_Tx_Buf[0] = 0x55;
    Custom_Tx_Buf[1] = 0xAA;
    Custom_Tx_Buf[2] = 0x81;
    Custom_Tx_Buf[3] = (uint8_t)(ecg & 0xFF);
    Custom_Tx_Buf[4] = (uint8_t)((ecg >> 8) & 0xFF);
    Custom_Tx_Buf[5] = hr;
    Custom_Tx_Buf[6] = spo2;
    Custom_Tx_Buf[7] = status;
    Custom_Tx_Buf[8] = batt_pct;

    uint32_t v;
    v = MKS142_GetRxBytes();
    Custom_Tx_Buf[9]  = (uint8_t)(v);
    Custom_Tx_Buf[10] = (uint8_t)(v >> 8);
    Custom_Tx_Buf[11] = (uint8_t)(v >> 16);
    Custom_Tx_Buf[12] = (uint8_t)(v >> 24);
    v = MKS142_GetRxOverflow();
    Custom_Tx_Buf[13] = (uint8_t)(v);
    Custom_Tx_Buf[14] = (uint8_t)(v >> 8);
    Custom_Tx_Buf[15] = (uint8_t)(v >> 16);
    Custom_Tx_Buf[16] = (uint8_t)(v >> 24);
    v = MKS142_GetFrameTotal();
    Custom_Tx_Buf[17] = (uint8_t)(v);
    Custom_Tx_Buf[18] = (uint8_t)(v >> 8);
    Custom_Tx_Buf[19] = (uint8_t)(v >> 16);
    Custom_Tx_Buf[20] = (uint8_t)(v >> 24);
    v = MKS142_GetFrameDrop();
    Custom_Tx_Buf[21] = (uint8_t)(v);
    Custom_Tx_Buf[22] = (uint8_t)(v >> 8);
    Custom_Tx_Buf[23] = (uint8_t)(v >> 16);
    Custom_Tx_Buf[24] = (uint8_t)(v >> 24);
    v = MKS142_GetStartRetry();
    Custom_Tx_Buf[25] = (uint8_t)(v);
    Custom_Tx_Buf[26] = (uint8_t)(v >> 8);
    Custom_Tx_Buf[27] = (uint8_t)(v >> 16);
    Custom_Tx_Buf[28] = (uint8_t)(v >> 24);
    Custom_Tx_Buf[29] = MKS142_GetLSR();
    /* [30..31] R16_PIN_ALTERNATE (LE) — 用来确认 RB_PIN_UART3 (bit7) 是否真的置位 */
    uint16_t alt = MKS142_GetPinAlternate();
    Custom_Tx_Buf[30] = (uint8_t)(alt & 0xFF);
    Custom_Tx_Buf[31] = (uint8_t)((alt >> 8) & 0xFF);
    /* [32] PB20/PB21 当前电平：bit0=PB20, bit1=PB21 */
    Custom_Tx_Buf[32] = MKS142_GetPb20Pb21();

    USBHS_Endp_DataUp(DEF_UEP3, Custom_Tx_Buf, 64, DEF_UEP_CPY_LOAD);
}

/* ==========================================================
 * 高频电竞鼠标包发送（EP2 IN，6 字节）
 * ========================================================== */
void USBD_Mouse_Send(PAW3395_Data_t *sensorData, Mouse_State_t *mouseBtn)
{
    if( USBHS_DevEnumStatus )
    {
        USB_Mouse_Pack[0] = 0x00;

        if(mouseBtn->Left)    USB_Mouse_Pack[0] |= 0x01;
        if(mouseBtn->Right)   USB_Mouse_Pack[0] |= 0x02;
        if(mouseBtn->Middle)  USB_Mouse_Pack[0] |= 0x04;
        if(mouseBtn->Back)    USB_Mouse_Pack[0] |= 0x08;
        if(mouseBtn->Forward) USB_Mouse_Pack[0] |= 0x10;

        USB_Mouse_Pack[1] = (uint8_t)(sensorData->deltaX & 0xFF);
        USB_Mouse_Pack[2] = (uint8_t)((sensorData->deltaX >> 8) & 0xFF);
        USB_Mouse_Pack[3] = (uint8_t)(sensorData->deltaY & 0xFF);
        USB_Mouse_Pack[4] = (uint8_t)((sensorData->deltaY >> 8) & 0xFF);
        USB_Mouse_Pack[5] = (uint8_t)(mouseBtn->WheelCount & 0xFF);

        USBHS_Endp_DataUp(DEF_UEP2, USB_Mouse_Pack, 6, DEF_UEP_CPY_LOAD);

        mouseBtn->WheelCount = 0;
    }
}
