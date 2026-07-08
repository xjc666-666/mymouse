#include "rf_2g4.h"
#include "g24_config.h"
#include "CH58x_common.h"

volatile uint8_t Has_New_RF_Data = 0;
Dongle_Data_t Mouse_RF_Data;

const uint8_t Hop_Channels[4] = {10, 39, 60, 78};
volatile uint8_t current_hop_index = 0;

__attribute__((aligned(4))) uint8_t rx_buf[12];

void RF_2G4StatusCallBack(uint8_t sta, uint8_t crc, uint8_t *rxBuf);

static uint8_t cfg_checksum(const uint8_t *p)
{
    uint8_t sum = 0;
    for(uint8_t i = 1; i < 7; i++) sum += p[i];
    return sum;
}

static void rf_apply_channel(void)
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

void RF_2G4StatusCallBack(uint8_t sta, uint8_t crc, uint8_t *rxBuf)
{
    if(sta == RX_MODE_RX_DATA) {
        if(crc == 0 && rxBuf[2] == 0xAA) {
            uint8_t chk = (rxBuf[3] + rxBuf[4] + rxBuf[5] +
                           rxBuf[6] + rxBuf[7] + rxBuf[8]) & 0xFF;
            if(chk == rxBuf[9]) {
                Mouse_RF_Data.buttons = rxBuf[3];
                Mouse_RF_Data.x = (int16_t)(rxBuf[4] | (rxBuf[5] << 8));
                Mouse_RF_Data.y = (int16_t)(rxBuf[6] | (rxBuf[7] << 8));
                Mouse_RF_Data.wheel = (int8_t)rxBuf[8];
                Has_New_RF_Data = 1;
            }
        } else if(crc == 0 && rxBuf[2] == 0xAC) {
            if(cfg_checksum(&rxBuf[2]) == rxBuf[10]) {
                if(rxBuf[3] == G24_CFG_VERSION) {
                    uint8_t index = rxBuf[4];
                    uint8_t poll_interval = rxBuf[5];
                    uint8_t flags = rxBuf[6];
                    uint8_t key = rxBuf[7];
                    if(index >= G24_MACRO_BTN_NUM) index = 0xFF;
                    G24_Config_Apply_From_RF(poll_interval, index,
                                             (flags & 0x80) ? 1 : 0,
                                             flags & 0x0F,
                                             key);
                }
            }
        }

        RF_Rx(rx_buf, 0, 0, 250);
    } else {
        current_hop_index = (current_hop_index + 1) % 4;
        RF_Shut();
        rf_apply_channel();
        RF_Rx(rx_buf, 0, 0, 200);
    }
}

void G24_Dongle_Init(void)
{
    rf_apply_channel();
    RF_Rx(rx_buf, 0, 0, 250);
}
