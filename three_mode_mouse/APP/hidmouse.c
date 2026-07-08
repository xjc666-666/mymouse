/******************************************************************************/
/* ͷ�ļ����� */
#include "CONFIG.h"
#include "battservice.h"
#include "devinfoservice.h"
#include "hiddev.h"
#include "hidmouse.h"
#include "hidmouseservice.h"

// ����Ӳ��������ȫ�ֱ���
#include "PAW3395.h"
#include "Mouse_Input.h"
#include "adc.h"
#include "mks142.h"
#include "oled_ui.h"

extern uint16_t dpi_levels[4];
extern uint8_t  dpi_index;
extern uint8_t  last_dpi_btn_state;

extern PAW3395_Data_t sensorData;
extern Mouse_State_t myMouse;
extern void Update_DPI_LED(uint8_t index);
/* ��������ͳһ�� Mouse_Config_Save ��䣬��֤ BLE ��ֱ��ʹ��ԭ�ӿ� */
#include "usbd_mouse.h"
extern Mouse_Config_t g_cfg;
extern void Mouse_Config_Save(void);
static inline void Save_DPI_To_Flash(uint8_t index)
{
    g_cfg.dpi_index = index;
}
/*********************************************************************
 * MACROS
 */
#define MOUSE_BUTTON_NONE                    0x00
// �ٷ���׼������������� (����, X, Y, ����)
#define HID_MOUSE_IN_RPT_LEN                 4

// �������Ӳ�������
#define START_PARAM_UPDATE_EVT_DELAY         12800
#define START_PHY_UPDATE_DELAY               1600
#define DEFAULT_HID_IDLE_TIMEOUT             60000

// ����ϵͳ�� 7.5ms (6 * 1.25) ��������������
#define DEFAULT_DESIRED_MIN_CONN_INTERVAL    6
#define DEFAULT_DESIRED_MAX_CONN_INTERVAL    6

#define DEFAULT_DESIRED_SLAVE_LATENCY        0
#define DEFAULT_DESIRED_CONN_TIMEOUT         500
#define DEFAULT_PASSCODE                     0
#define DEFAULT_PAIRING_MODE                 GAPBOND_PAIRING_MODE_WAIT_FOR_REQ
#define DEFAULT_MITM_MODE                    FALSE
#define DEFAULT_BONDING_MODE                 TRUE
#define DEFAULT_IO_CAPABILITIES              GAPBOND_IO_CAP_NO_INPUT_NO_OUTPUT
#define DEFAULT_BATT_CRITICAL_LEVEL          6

/*********************************************************************
 * GLOBAL VARIABLES
 */
static uint8_t hidEmuTaskId = INVALID_TASK_ID;
static uint16_t hidEmuConnHandle = GAP_CONNHANDLE_INIT;

static uint8_t scanRspData[] = {
    0x0A, GAP_ADTYPE_LOCAL_NAME_COMPLETE, 
    'H', 'I', 'D', ' ', 'M', 'o', 'u', 's', 'e',
    0x05, GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE,
    LO_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL), HI_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL),
    LO_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL), HI_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL),
    0x05, GAP_ADTYPE_16BIT_MORE,
    LO_UINT16(HID_SERV_UUID), HI_UINT16(HID_SERV_UUID),
    LO_UINT16(BATT_SERV_UUID), HI_UINT16(BATT_SERV_UUID),
    0x02, GAP_ADTYPE_POWER_LEVEL, 0
};

static uint8_t advertData[] = {
    0x02, GAP_ADTYPE_FLAGS, GAP_ADTYPE_FLAGS_LIMITED | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,
    0x03, GAP_ADTYPE_APPEARANCE, LO_UINT16(GAP_APPEARE_HID_MOUSE), HI_UINT16(GAP_APPEARE_HID_MOUSE)
};

static CONST uint8_t attDeviceName[GAP_DEVICE_NAME_LEN] = "HID Mouse";
static hidDevCfg_t hidEmuCfg = { DEFAULT_HID_IDLE_TIMEOUT, HID_FEATURE_FLAGS };

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void hidEmu_ProcessTMOSMsg(tmos_event_hdr_t *pMsg);
static void hidEmuSendMouseReport(uint8_t buttons, int8_t X_data, int8_t Y_data, int8_t wheel);
static uint8_t hidEmuRptCB(uint8_t id, uint8_t type, uint16_t uuid, uint8_t oper, uint16_t *pLen, uint8_t *pData);
static void hidEmuEvtCB(uint8_t evt);
static void hidEmuStateCB(gapRole_States_t newState, gapRoleEvent_t *pEvent);

static hidDevCB_t hidEmuHidCBs = { hidEmuRptCB, hidEmuEvtCB, NULL, hidEmuStateCB };

void HidEmu_Init()
{
    hidEmuTaskId = TMOS_ProcessEventRegister(HidEmu_ProcessEvent);
    uint8_t initial_advertising_enable = TRUE;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);
    GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(advertData), advertData);
    GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, sizeof(scanRspData), scanRspData);
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, (void *)attDeviceName);

    uint32_t passkey = DEFAULT_PASSCODE;
    uint8_t  pairMode = DEFAULT_PAIRING_MODE;
    uint8_t  mitm = DEFAULT_MITM_MODE;
    uint8_t  ioCap = DEFAULT_IO_CAPABILITIES;
    uint8_t  bonding = DEFAULT_BONDING_MODE;
    GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
    GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &pairMode);
    GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(uint8_t), &mitm);
    GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
    GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(uint8_t), &bonding);

    uint8_t critical = DEFAULT_BATT_CRITICAL_LEVEL;
    Batt_SetParameter(BATT_PARAM_CRITICAL_LEVEL, sizeof(uint8_t), &critical);

    Hid_AddService();
    HidDev_Register(&hidEmuCfg, &hidEmuHidCBs);

    /* 启动时立即测一次电池电量，让 Host 的首次读取就拿到真实值 */
    Batt_MeasLevel();

    tmos_set_event(hidEmuTaskId, START_DEVICE_EVT);
    tmos_start_task(hidEmuTaskId, START_UI_EVT, 12);
}

uint16_t HidEmu_ProcessEvent(uint8_t task_id, uint16_t events)
{
    if(events & SYS_EVENT_MSG) {
        uint8_t *pMsg;
        if((pMsg = tmos_msg_receive(hidEmuTaskId)) != NULL) {
            hidEmu_ProcessTMOSMsg((tmos_event_hdr_t *)pMsg);
            tmos_msg_deallocate(pMsg);
        }
        return (events ^ SYS_EVENT_MSG);
    }

    if(events & START_DEVICE_EVT) return (events ^ START_DEVICE_EVT);

    if(events & START_PARAM_UPDATE_EVT) {
        GAPRole_PeripheralConnParamUpdateReq(hidEmuConnHandle, DEFAULT_DESIRED_MIN_CONN_INTERVAL,
            DEFAULT_DESIRED_MAX_CONN_INTERVAL, DEFAULT_DESIRED_SLAVE_LATENCY, DEFAULT_DESIRED_CONN_TIMEOUT, hidEmuTaskId);
        return (events ^ START_PARAM_UPDATE_EVT);
    }

    if(events & START_PHY_UPDATE_EVT) {
        GAPRole_UpdatePHY(hidEmuConnHandle, 0, GAP_PHY_BIT_LE_2M, GAP_PHY_BIT_LE_2M, 0);
        return (events ^ START_PHY_UPDATE_EVT);
    }

    if(events & START_UI_EVT) {
        static uint16_t batt_refresh_tick = 0;

        MKS142_Process();
        Battery_PeriodicMeasure();
        OLED_UI_Tick();
        batt_refresh_tick++;
        if(batt_refresh_tick >= 250) {
            batt_refresh_tick = 0;
            Batt_MeasLevel();
        }

        tmos_start_task(hidEmuTaskId, START_UI_EVT, 12);
        return (events ^ START_UI_EVT);
    }

    // ====================================================================
    // ? TMOS ��ʱ�������� (�����Ż����ع�����ԭʼ��Ӳ�˻���ȫ��)
    // ====================================================================
    if(events & START_REPORT_EVT)
    {
        static uint8_t ble_idle_count = 0;

        PAW3395_ReadMotion(&sensorData);
        Mouse_Input_Scan(&myMouse);

        /* === 周期刷新电池电量（每 ~3s 一次，~12ms 一轮） === */
        // ? �����������ﴦ������ģʽ�µ� DPI �л���
        if(myMouse.DPI == 1 && last_dpi_btn_state == 0) {
            dpi_index = (dpi_index + 1) % 4; 
            PAW3395_SetDPI(dpi_levels[dpi_index]);
            Update_DPI_LED(dpi_index);
            OLED_UI_ForceRedraw();
            Save_DPI_To_Flash(dpi_index); // ? �������
        }
        last_dpi_btn_state = myMouse.DPI;



        // ���ǿת������0 ������ģ�����Ϊ���޸����������� Bug
        int16_t true_dx = (int16_t)sensorData.deltaX;
        int16_t true_dy = (int16_t)sensorData.deltaY;
        
        int8_t send_x = 0;
        int8_t send_y = 0;
        
        if(true_dx > 127) send_x = 127;
        else if(true_dx < -127) send_x = -127;
        else send_x = (int8_t)true_dx;

        if(true_dy > 127) send_y = 127;
        else if(true_dy < -127) send_y = -127;
        else send_y = (int8_t)true_dy;

        // ��װ���� (������)
        uint8_t btn = 0;
        if(myMouse.Left) btn |= 0x01;
        if(myMouse.Right) btn |= 0x02;
        if(myMouse.Middle) btn |= 0x04;
        if(myMouse.Back) btn |= 0x08;      // ���
        if(myMouse.Forward) btn |= 0x10;   // ���

        // ? ���Ļع飺ֻҪ btn != 0 (ֻҪ�м�����)���ͷ���䣡�������ŵ��������£�
        if(sensorData.isMotion || btn != 0 || myMouse.WheelCount != 0) {
            hidEmuSendMouseReport(btn, send_x, send_y, (int8_t)myMouse.WheelCount);
            myMouse.WheelCount = 0; 
            ble_idle_count = 0; 
        }
        else 
        {
            // ֻ�����㳹�����֣���û���ƶ�ʱ�������οհ����ճ����Ȼ��������ȥ����
            if (ble_idle_count < 2) {
                hidEmuSendMouseReport(0, 0, 0, 0); 
                ble_idle_count++;
            }
        }

        tmos_start_task(hidEmuTaskId, START_REPORT_EVT, 12);
        return (events ^ START_REPORT_EVT);
    }
    return 0;
}

static void hidEmu_ProcessTMOSMsg(tmos_event_hdr_t *pMsg) { }

static void hidEmuSendMouseReport(uint8_t buttons, int8_t X_data, int8_t Y_data, int8_t wheel)
{
    uint8_t buf[HID_MOUSE_IN_RPT_LEN];
    buf[0] = buttons; 
    buf[1] = (uint8_t)X_data;  
    buf[2] = (uint8_t)Y_data;  
    buf[3] = (uint8_t)wheel;       
    HidDev_Report(HID_RPT_ID_MOUSE_IN, HID_REPORT_TYPE_INPUT, HID_MOUSE_IN_RPT_LEN, buf);
}

static void hidEmuStateCB(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    switch(newState & GAPROLE_STATE_ADV_MASK) {
        case GAPROLE_STARTED: {
            uint8_t ownAddr[6];
            GAPRole_GetParameter(GAPROLE_BD_ADDR, ownAddr);
            GAP_ConfigDeviceAddr(ADDRTYPE_STATIC, ownAddr);
        } break;
        case GAPROLE_CONNECTED:
            if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT) {
                gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;
                hidEmuConnHandle = event->connectionHandle;
                tmos_start_task(hidEmuTaskId, START_PARAM_UPDATE_EVT, START_PARAM_UPDATE_EVT_DELAY);
            }
            break;
        case GAPROLE_WAITING:
            if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT) {
                uint8_t initial_advertising_enable = TRUE;
                GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);
            }
            break;
        default: break;
    }
}

static uint8_t hidEmuRptCB(uint8_t id, uint8_t type, uint16_t uuid, uint8_t oper, uint16_t *pLen, uint8_t *pData)
{
    uint8_t status = SUCCESS;
    if(oper == HID_DEV_OPER_WRITE) {
        status = Hid_SetParameter(id, type, uuid, *pLen, pData);
    } else if(oper == HID_DEV_OPER_READ) {
        status = Hid_GetParameter(id, type, uuid, pLen, pData);
    } else if(oper == HID_DEV_OPER_ENABLE) {
        tmos_start_task(hidEmuTaskId, START_REPORT_EVT, 12);
    } else if(oper == HID_DEV_OPER_DISABLE) {
        tmos_stop_task(hidEmuTaskId, START_REPORT_EVT);
    }
    return status;
}

static void hidEmuEvtCB(uint8_t evt) { return; }
