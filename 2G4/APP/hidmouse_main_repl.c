#include "CONFIG.h"
#include "HAL.h"
#include "rf_2g4.h"
#include "usbd_mouse.h"
#include "ch585_usbhs_device.h"
#include "g24_bridge.h"
#include "g24_config.h"

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

void Dongle_Main_Loop(void)
{
    while(1) {
        G24_Bridge_Process();
    }
}

int main(void)
{
#if(defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(SYSCLK_FREQ);

    R8_USB_CTRL = 0x00;
    mDelaymS(20);

    G24_Config_Load();
    USBHS_Device_Init(ENABLE);

    CH58x_BLEInit();
    HAL_Init();
    RF_RoleInit();

    G24_Dongle_Init();
    Dongle_Main_Loop();
}
