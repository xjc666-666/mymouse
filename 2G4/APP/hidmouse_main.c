#include "CONFIG.h"
#include "HAL.h"
#include "rf_2g4.h"
#include "usbd_mouse.h"
#include "ch585_usbhs_device.h" 

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

// =========================================================
// 接收器 (Dongle) 极速中转循环 (已剔除吞包 Bug)
// =========================================================
void Dongle_Main_Loop()
{
    while(1)
    {
        // 只要射频中断接到了新数据包，就扔进 USB 高速缓存
        if (Has_New_RF_Data)
        {
            Has_New_RF_Data = 0; // 清空标志位
            
            USBD_Mouse_Send_Raw(Mouse_RF_Data.buttons, 
                                Mouse_RF_Data.x, 
                                Mouse_RF_Data.y, 
                                Mouse_RF_Data.wheel);
        }
        
        // 注意：这里去掉了 else 分支！
        // 鼠标没动作时，USB 端点会自动 NAK (拒绝电脑)，完美节省带宽。

        // 必须执行：维持射频跳频基带的心跳
        TMOS_SystemProcess();
    }
}

// ... main 函数与之前保持一致 ...
int main(void)
{
#if(defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(SYSCLK_FREQ);

    R8_USB_CTRL = 0x00; 
    mDelaymS(20); 

    USBHS_Device_Init(ENABLE); // 加上了 ENABLE 参数

    CH58x_BLEInit();
    HAL_Init();
    RF_RoleInit();
    
    G24_Dongle_Init();
    
    Dongle_Main_Loop();
}