#include "rf_2g4.h"
#include "CH58x_common.h"

volatile uint8_t tx_end_flag = 1; 
volatile uint8_t missed_ack_count = 0; // ? 新增：失败计数器

// 4个跳频信道，避开 WiFi 常用频段
const uint8_t Hop_Channels[4] = {10, 39, 60, 78}; 
volatile uint8_t current_hop_index = 0;

__attribute__((aligned(4))) uint8_t tx_buf[10];

// ? 鼠标端回调：不到黄河心不死
void RF_2G4StatusCallBack(uint8_t sta, uint8_t crc, uint8_t *rxBuf)
{
    if (sta == TX_MODE_TX_FINISH) 
    {
        // 成功收到 Dongle 的 ACK！当前频道很干净！
        missed_ack_count = 0; // 连续失败清零
        tx_end_flag = 1;      // 允许主循环立刻发下一枪
    }
    else if (sta == TX_MODE_TX_FAIL || sta == TX_MODE_RX_TIMEOUT) 
    {
        missed_ack_count++;
        tx_end_flag = 1; // 即使失败，也要解开锁，让主循环继续在这个频道尝试！

        // 只有连续失败超过 20 次 (约 20 毫秒)，才判定频道彻底死亡，进行跳频
        if (missed_ack_count > 20) 
        {
            missed_ack_count = 0;
            current_hop_index = (current_hop_index + 1) % 4;
            
            RF_Shut(); // 关闭当前射频
            // 切换到新频道
            rfConfig_t rf_Config;
            tmos_memset(&rf_Config, 0, sizeof(rfConfig_t));
            rf_Config.accessAddress = 0x71764129; 
            rf_Config.CRCInit = 0x555555;
            rf_Config.Channel = Hop_Channels[current_hop_index]; 
            rf_Config.Frequency = 2400000 + Hop_Channels[current_hop_index] * 1000;
            rf_Config.LLEMode = LLE_MODE_AUTO; // 必须是 AUTO 才有 ACK
            rf_Config.rfStatusCB = RF_2G4StatusCallBack;
            RF_Config(&rf_Config);
        }
    }
}

void G24_Mouse_Init(void)
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
}

void G24_Mouse_Send(uint8_t buttons, int16_t x, int16_t y, int8_t wheel)
{
    if (tx_end_flag) 
    {
        tx_end_flag = 0;
        
        tx_buf[0] = 0xAA; 
        tx_buf[1] = buttons;
        tx_buf[2] = (uint8_t)(x & 0xFF);
        tx_buf[3] = (uint8_t)((x >> 8) & 0xFF);
        tx_buf[4] = (uint8_t)(y & 0xFF);
        tx_buf[5] = (uint8_t)((y >> 8) & 0xFF);
        tx_buf[6] = (uint8_t)wheel;
        tx_buf[7] = (tx_buf[1] + tx_buf[2] + tx_buf[3] + tx_buf[4] + tx_buf[5] + tx_buf[6]) & 0xFF; 

        RF_Tx(tx_buf, 8, 0xFF, 0xFF); // 无限超时，死等底层的这一枪打完
    }
}