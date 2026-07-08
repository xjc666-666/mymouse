#include "rf_2g4.h"
#include "usbd_mouse.h"
#include "CH58x_common.h"

volatile uint8_t tx_end_flag = 1;
volatile uint8_t missed_ack_count = 0;

const uint8_t Hop_Channels[4] = {10, 39, 60, 78};
volatile uint8_t current_hop_index = 0;

__attribute__((aligned(4))) uint8_t tx_buf[10];

void RF_2G4StatusCallBack(uint8_t sta, uint8_t crc, uint8_t *rxBuf);

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
    (void)crc;
    (void)rxBuf;

    if(sta == TX_MODE_TX_FINISH) {
        missed_ack_count = 0;
        tx_end_flag = 1;
    } else if(sta == TX_MODE_TX_FAIL || sta == TX_MODE_RX_TIMEOUT) {
        missed_ack_count++;
        tx_end_flag = 1;

        if(missed_ack_count > 20) {
            missed_ack_count = 0;
            current_hop_index = (current_hop_index + 1) % 4;
            RF_Shut();
            rf_apply_channel();
        }
    }
}

void G24_Mouse_Init(void)
{
    rf_apply_channel();
}

void G24_Mouse_Send(uint8_t buttons, int16_t x, int16_t y, int8_t wheel)
{
    if(tx_end_flag) {
        tx_end_flag = 0;

        tx_buf[0] = 0xAA;
        tx_buf[1] = buttons;
        tx_buf[2] = (uint8_t)(x & 0xFF);
        tx_buf[3] = (uint8_t)((x >> 8) & 0xFF);
        tx_buf[4] = (uint8_t)(y & 0xFF);
        tx_buf[5] = (uint8_t)((y >> 8) & 0xFF);
        tx_buf[6] = (uint8_t)wheel;
        tx_buf[7] = (tx_buf[1] + tx_buf[2] + tx_buf[3] +
                     tx_buf[4] + tx_buf[5] + tx_buf[6]) & 0xFF;

        RF_Tx(tx_buf, 8, 0xFF, 0xFF);
    }
}
