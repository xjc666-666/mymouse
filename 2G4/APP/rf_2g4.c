#include "rf_2g4.h"
#include "CH58x_common.h"

volatile uint8_t Has_New_RF_Data = 0;
Dongle_Data_t    Mouse_RF_Data;

const uint8_t Hop_Channels[4] = {10, 39, 60, 78}; 
volatile uint8_t current_hop_index = 0;

__attribute__((aligned(4))) uint8_t rx_buf[12];

// ? Dongle 端回调：死死咬住，超时才搜！
void RF_2G4StatusCallBack(uint8_t sta, uint8_t crc, uint8_t *rxBuf)
{
    if (sta == RX_MODE_RX_DATA) 
    {
        if (crc == 0 && rxBuf[2] == 0xAA) 
        {
            uint8_t chk = (rxBuf[3] + rxBuf[4] + rxBuf[5] + rxBuf[6] + rxBuf[7] + rxBuf[8]) & 0xFF;
            if (chk == rxBuf[9]) 
            {
                Mouse_RF_Data.buttons = rxBuf[3];
                Mouse_RF_Data.x       = (int16_t)(rxBuf[4] | (rxBuf[5] << 8));
                Mouse_RF_Data.y       = (int16_t)(rxBuf[6] | (rxBuf[7] << 8));
                Mouse_RF_Data.wheel   = (int8_t)rxBuf[8];
                
                Has_New_RF_Data = 1;
            }
        }
        // ? 只要收到了包，说明这个频道通畅，咬死它！
        // 第 4 个参数 400 代表 4ms 超时。只要鼠标 4 毫秒内哪怕发来一个空包，就不会跳频！
        RF_Rx(rx_buf, 0, 0, 250); 
    }
    else 
    {
        // 如果 4ms 都没听到鼠标声音 (超时)，或者受到干扰出错，立刻切频道扫描！
        current_hop_index = (current_hop_index + 1) % 4;
        
        RF_Shut();
        rfConfig_t rf_Config;
        tmos_memset(&rf_Config, 0, sizeof(rfConfig_t));
        rf_Config.accessAddress = 0x71764129; 
        rf_Config.CRCInit = 0x555555;
        rf_Config.Channel = Hop_Channels[current_hop_index]; 
        rf_Config.Frequency = 2400000 + Hop_Channels[current_hop_index] * 1000;
        rf_Config.LLEMode = LLE_MODE_AUTO; 
        rf_Config.rfStatusCB = RF_2G4StatusCallBack;
        RF_Config(&rf_Config);
        
        // 扫描模式下，每个频道只听 2ms，提升全盘搜索速度
        RF_Rx(rx_buf, 0, 0, 200);
    }
}

void G24_Dongle_Init(void)
{
    rfConfig_t rf_Config;
    tmos_memset(&rf_Config, 0, sizeof(rfConfig_t));
    
    rf_Config.accessAddress = 0x71764129; 
    rf_Config.CRCInit = 0x555555;
    rf_Config.Channel = Hop_Channels[current_hop_index]; 
    rf_Config.Frequency = 2400000 + Hop_Channels[current_hop_index] * 1000;
    rf_Config.LLEMode = LLE_MODE_AUTO; 
    rf_Config.rfStatusCB = RF_2G4StatusCallBack;
    
    RF_Config(&rf_Config);
    
    // 初始化启动 4ms 监听
    RF_Rx(rx_buf, 0, 0, 250);
}