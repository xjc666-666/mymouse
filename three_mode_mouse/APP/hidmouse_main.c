#include "CONFIG.h"
#include "HAL.h"
#include "hiddev.h"
#include "hidmouse.h"
#include <string.h>

#include "PAW3395.h"
#include "Mouse_Input.h"
#include "switch.h"
#include "usbd_mouse.h"
#include "rf_2g4.h"
#include "User_LED.h"
#include "mks142.h"
#include "adc.h"
#include "oled_ui.h"

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

/* =========================================================
 *  DPI 指示灯（双 LED 二进制显示档位）
 * ========================================================= */
void Update_DPI_LED(uint8_t index)
{
    if (index & 0x01) { User_LED_On(LED_1); } else { User_LED_Off(LED_1); }
    if (index & 0x02) { User_LED_On(LED_2); } else { User_LED_Off(LED_2); }
}

/* 全局：当前 DPI 档位、DPI 数组（与 g_cfg 同步） */
uint16_t dpi_levels[DPI_STAGE_NUM] = {6400, 3200, 1600, 800};
uint8_t  dpi_index = 2;
uint8_t  last_dpi_btn_state = 0;

/* 全局宏映射表（与 g_cfg.macros 同步） */
Button_Macro_t Macro_Table[MACRO_BTN_NUM] = {0};

/* =========================================================
 *  Flash 配置块（单块存储完整配置）
 * ========================================================= */
#define MOUSE_CFG_FLASH_ADDR    0x00     /* DataFlash 起始 */

Mouse_Config_t g_cfg;

static void cfg_apply_factory_defaults(void)
{
    g_cfg.magic         = MOUSE_CFG_MAGIC;
    g_cfg.version       = MOUSE_CFG_VERSION;
    g_cfg.dpi_index     = 2;
    g_cfg.poll_interval = POLL_INTERVAL_1000HZ;
    g_cfg.reserved0     = 0;
    g_cfg.dpi_levels[0] = 6400;
    g_cfg.dpi_levels[1] = 3200;
    g_cfg.dpi_levels[2] = 1600;
    g_cfg.dpi_levels[3] = 800;
    for(int i = 0; i < MACRO_BTN_NUM; i++) {
        g_cfg.macros[i].is_macro = 0;
        g_cfg.macros[i].modifier = 0;
        g_cfg.macros[i].key_code = 0;
    }
    memset(g_cfg.pad, 0, sizeof(g_cfg.pad));
}

static void cfg_sync_runtime_from_struct(void)
{
    if(g_cfg.dpi_index >= DPI_STAGE_NUM) g_cfg.dpi_index = 2;
    dpi_index = g_cfg.dpi_index;
    for(int i = 0; i < DPI_STAGE_NUM; i++) {
        uint16_t v = g_cfg.dpi_levels[i];
        if(v < 50)            v = 800;
        if(v > DPI_MAX_VALUE) v = DPI_MAX_VALUE;
        dpi_levels[i] = v;
    }
    for(int i = 0; i < MACRO_BTN_NUM; i++) {
        Macro_Table[i] = g_cfg.macros[i];
        /* 防御：is_macro=1 但 modifier 和 key_code 都为 0 视为无效，
         * 否则会导致按下后没有任何 HID 输出（"哑键"）。*/
        if(Macro_Table[i].is_macro &&
           Macro_Table[i].modifier == 0 && Macro_Table[i].key_code == 0) {
            Macro_Table[i].is_macro = 0;
            g_cfg.macros[i].is_macro = 0;
        }
    }
}

static void cfg_apply_ble_runtime_defaults(void)
{
    static const uint16_t ble_dpi_defaults[DPI_STAGE_NUM] = {6400, 3200, 1600, 800};

    dpi_index = 2;
    for(int i = 0; i < DPI_STAGE_NUM; i++) {
        dpi_levels[i] = ble_dpi_defaults[i];
    }
    for(int i = 0; i < MACRO_BTN_NUM; i++) {
        Macro_Table[i].is_macro = 0;
        Macro_Table[i].modifier = 0;
        Macro_Table[i].key_code = 0;
    }
}

void Mouse_Config_Save(void)
{
    EEPROM_ERASE(MOUSE_CFG_FLASH_ADDR, EEPROM_BLOCK_SIZE);
    EEPROM_WRITE(MOUSE_CFG_FLASH_ADDR, &g_cfg, sizeof(g_cfg));
}

void Mouse_Config_Load(void)
{
    Mouse_Config_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    EEPROM_READ(MOUSE_CFG_FLASH_ADDR, &tmp, sizeof(tmp));
    if(tmp.magic == MOUSE_CFG_MAGIC && tmp.version == MOUSE_CFG_VERSION) {
        g_cfg = tmp;
    } else {
        cfg_apply_factory_defaults();
        Mouse_Config_Save();
    }
    cfg_sync_runtime_from_struct();
}

/* 修改配置描述符里的 bInterval；下次枚举时 Host 会读取到新的间隔 */
void Mouse_Apply_Poll_Rate(uint8_t bInterval)
{
    extern uint8_t MyCfgDescr[];
    MyCfgDescr[CFG_BINTERVAL_OFFSET_EP1] = bInterval;
    MyCfgDescr[CFG_BINTERVAL_OFFSET_EP2] = bInterval;
}

Mouse_Mode_t sys_current_mode;
Mouse_State_t myMouse;
PAW3395_Data_t sensorData;
extern volatile uint8_t Raw_Btn_Mask;
void G24_Config_Sync_Request(void);
void G24_Config_Sync_Service(void);

/* =========================================================
 *  2.4G 模式主循环
 * ========================================================= */
void G24_Main_Loop(void)
{
    uint8_t idle_count = 0;

    G24_Config_Sync_Request();

    while(1)
    {
        if (Mode_Switch_Read() != MODE_24G) {
            SYS_ResetExecute();
        }

        G24_Config_Sync_Service();

        PAW3395_ReadMotion(&sensorData);
        Mouse_Input_Scan(&myMouse);

        if(myMouse.DPI == 1 && last_dpi_btn_state == 0) {
            dpi_index = (dpi_index + 1) % DPI_STAGE_NUM;
            g_cfg.dpi_index = dpi_index;
            PAW3395_SetDPI(dpi_levels[dpi_index]);
            Update_DPI_LED(dpi_index);
            OLED_UI_ForceRedraw();
            Mouse_Config_Save();
        }
        last_dpi_btn_state = myMouse.DPI;

        if (!sensorData.isMotion) { sensorData.deltaX = 0; sensorData.deltaY = 0; }

        uint8_t btn = Raw_Btn_Mask;

        if (sensorData.isMotion || btn != 0 || myMouse.WheelCount != 0)
        {
            G24_Mouse_Send(btn, (int16_t)sensorData.deltaX, (int16_t)sensorData.deltaY, (int8_t)myMouse.WheelCount);
            myMouse.WheelCount = 0;
            idle_count = 0;
        }
        else
        {
            if (idle_count < 2) {
                G24_Mouse_Send(0, 0, 0, 0);
                idle_count++;
            }
        }

        /* 2.4G 模式：MKS142 + 电池仍然采集，OLED 暂时关闭以排查问题 */
        MKS142_Process();
        Battery_PeriodicMeasure();
        OLED_UI_Tick();

        TMOS_SystemProcess();
    }
}

/* =========================================================
 *  BLE 模式主循环
 * ========================================================= */
__HIGH_CODE
__attribute__((noinline))
void BLE_Main_Loop(void)
{
    while(1)
    {
        if (Mode_Switch_Read() != MODE_BLE) {
            SYS_ResetExecute();
        }
        TMOS_SystemProcess();
        /* OLED UI 暂时关闭以排查问题 */
        MKS142_Process();
    }
}

/* =========================================================
 *  USB 有线模式主循环
 *  - 标准 HID 鼠标包通过 EP2 发送
 *  - 自定义按键 / 移动状态通过 EP3 异步广播给上位机用于可视化
 *  - 状态广播限频 ~50Hz，避免抢占常规 HID 通道
 *  - 心电 / 血氧广播限频 ~25Hz（每收到新 ECG 样本就发一包）
 * ========================================================= */
void Wired_Main_Loop(void)
{
    uint8_t idle_count = 0;
    uint16_t state_tick = 0;
    uint8_t  last_btn_mask = 0xFF;
    int16_t  acc_dx = 0, acc_dy = 0;
    int8_t   acc_wheel = 0;
    uint16_t bio_keepalive_tick = 0;
    uint32_t bio_ecg_cursor = 0;

    while(1)
    {
        if (Mode_Switch_Read() != MODE_WIRED) {
            SYS_ResetExecute();
        }

        PAW3395_ReadMotion(&sensorData);
        Mouse_Input_Scan(&myMouse);

        if(myMouse.DPI == 1 && last_dpi_btn_state == 0) {
            dpi_index = (dpi_index + 1) % DPI_STAGE_NUM;
            g_cfg.dpi_index = dpi_index;
            PAW3395_SetDPI(dpi_levels[dpi_index]);
            Update_DPI_LED(dpi_index);
            OLED_UI_ForceRedraw();
            Mouse_Config_Save();
        }
        last_dpi_btn_state = myMouse.DPI;

        if (!sensorData.isMotion) { sensorData.deltaX = 0; sensorData.deltaY = 0; }

        /* 在常规鼠标包发送前先抓走 wheel，留给可视化通道用 */
        int8_t wheel_now = (int8_t)myMouse.WheelCount;
        acc_dx    += sensorData.deltaX;
        acc_dy    += sensorData.deltaY;
        acc_wheel += wheel_now;

        if (sensorData.isMotion || myMouse.Left || myMouse.Right || myMouse.Middle ||
            myMouse.Forward || myMouse.Back || wheel_now != 0)
        {
            USBD_Mouse_Send(&sensorData, &myMouse);
            idle_count = 0;
        }
        else
        {
            if (idle_count < 2) {
                USBD_Mouse_Send(&sensorData, &myMouse);
                idle_count++;
            }
        }
        myMouse.WheelCount = 0;

        uint8_t btn_mask = 0;
        if(myMouse.Left)    btn_mask |= 0x01;
        if(myMouse.Right)   btn_mask |= 0x02;
        if(myMouse.Middle)  btn_mask |= 0x04;
        if(myMouse.Back)    btn_mask |= 0x08;
        if(myMouse.Forward) btn_mask |= 0x10;

        state_tick++;
        if(state_tick >= 20 || btn_mask != last_btn_mask)
        {
            USBD_Custom_Send_State(btn_mask, acc_dx, acc_dy, acc_wheel);
            last_btn_mask = btn_mask;
            acc_dx = 0; acc_dy = 0; acc_wheel = 0;
            state_tick = 0;
        }

        /* === MKS-142 心电 / 血氧 ===
         *  模块每 1.28s 上传 1 帧（64 个 ECG 采样，约 50 Hz）。
         *  这里在每次主循环里最多拉走 1 个采样发给上位机，避免在同一次循环
         *  里背靠背写共享 USB Tx 缓冲（会覆盖未发出的前一包），主循环 1 kHz
         *  足够 ~64ms 内把一帧的 64 个样本全部送达。
         *  即使没有新 ECG，每 ~1s 也发一次 keepalive，
         *  以便上位机及时刷新心率/血氧/状态/电池字段。
         */
        MKS142_Process();
        bio_keepalive_tick++;

        int8_t ecg_one;
        int    ecg_n = MKS142_PullEcgSince(&bio_ecg_cursor, &ecg_one, 1);
        if(ecg_n > 0) {
            uint8_t hr  = MKS142_GetHR();
            uint8_t spo = MKS142_GetSpO2();
            uint8_t st  = MKS142_GetStatus();
            uint8_t bp  = Battery_GetCachedPercent();
            USBD_Custom_Send_Bio((int16_t)ecg_one, hr, spo, st, bp);
            bio_keepalive_tick = 0;
        } else if(bio_keepalive_tick >= 1000) {
            uint8_t hr  = MKS142_GetHR();
            uint8_t spo = MKS142_GetSpO2();
            uint8_t st  = MKS142_GetStatus();
            uint8_t bp  = Battery_GetCachedPercent();
            USBD_Custom_Send_Bio(MKS142_PeekEcg(), hr, spo, st, bp);
            bio_keepalive_tick = 0;
        }

        /* === 电池 ADC 后台测量（限频内部完成）=== */
        Battery_PeriodicMeasure();

        /* === OLED UI 暂时关闭以排查 MKS142 数据问题 === */
        OLED_UI_Tick();

        /* 处理来自上位机命令的延迟操作（Flash 写、传感器 DPI 写等） */
        Mouse_Cmd_Service();
    }
}

/* =========================================================
 *  入口
 * ========================================================= */
int main(void)
{
#if(defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(SYSCLK_FREQ);

    Mode_Switch_Init();
    mDelaymS(20);

    Mouse_Input_Init();
    PAW3395_Init();
    User_LED_Init();

    /* 电池电量 ADC（PA12 / AIN2，1/2 分压） */
    Battery_ADC_Init();

    /* 心电血氧模块（UART3 重映射 PB20/PB21，复位通过软件命令） */
    /* OLED UI（0.66" 64x48，SPI1 + DC/RES on PA0..PA3） */
    /* OLED_UI_Init(); */   /* 暂时关闭以排查 MKS142 数据问题 */

    /* 从 Flash 加载完整配置（DPI 档位、回报率、宏） */
    Mouse_Config_Load();

    /* 应用上位机偏好的回报率到 USB 配置描述符 */
    Mouse_Apply_Poll_Rate(g_cfg.poll_interval);

    /* 初始化传感器 DPI 与档位灯 */
    PAW3395_SetDPI(dpi_levels[dpi_index]);
    Update_DPI_LED(dpi_index);
    OLED_UI_Init();

    while(1)
    {
        sys_current_mode = Mode_Switch_Read();
        if (sys_current_mode == MODE_WIRED || sys_current_mode == MODE_BLE || sys_current_mode == MODE_24G) break;
        mDelaymS(1);
    }

    if (sys_current_mode == MODE_WIRED)
    {
        R8_USB_CTRL = 0x00;
        mDelaymS(1);
        USBD_Mouse_Init();
        MKS142_Init();
        OLED_UI_ForceRedraw();
        Wired_Main_Loop();
    }
    else if (sys_current_mode == MODE_BLE)
    {
        cfg_apply_ble_runtime_defaults();
        PAW3395_SetDPI(dpi_levels[dpi_index]);
        Update_DPI_LED(dpi_index);

        CH58x_BLEInit();
        HAL_Init();
        GAPRole_PeripheralInit();
        HidDev_Init();
        HidEmu_Init();
        MKS142_Init();
        OLED_UI_ForceRedraw();
        BLE_Main_Loop();
    }
    else if (sys_current_mode == MODE_24G)
    {
        CH58x_BLEInit();
        HAL_Init();
        RF_RoleInit();

        G24_Mouse_Init();
        MKS142_Init();
        OLED_UI_ForceRedraw();

        G24_Main_Loop();
    }
}
