#include "rf_2g4.h"
#include "usbd_mouse.h"

extern volatile uint8_t tx_end_flag;
extern uint8_t tx_buf[10];

static volatile uint8_t cfg_sync_pending = 0;
static uint8_t cfg_sync_step = 0;
static uint8_t cfg_sync_retry = 0;

static uint8_t cfg_checksum(const uint8_t *p)
{
    uint8_t sum = 0;
    for(uint8_t i = 1; i < 7; i++) sum += p[i];
    return sum;
}

void G24_Config_Sync_Request(void)
{
    cfg_sync_pending = 1;
    cfg_sync_step = 0;
    cfg_sync_retry = 0;
}

void G24_Config_Sync_Service(void)
{
    if(!cfg_sync_pending || !tx_end_flag) return;

    tx_end_flag = 0;
    tx_buf[0] = 0xAC;
    tx_buf[1] = g_cfg.version;
    tx_buf[2] = cfg_sync_step;
    tx_buf[3] = g_cfg.poll_interval;
    tx_buf[4] = 0;
    tx_buf[5] = 0;
    tx_buf[6] = 0;
    tx_buf[7] = 0;

    if(cfg_sync_step < MACRO_BTN_NUM) {
        tx_buf[4] = ((g_cfg.macros[cfg_sync_step].is_macro ? 1 : 0) << 7) |
                    (g_cfg.macros[cfg_sync_step].modifier & 0x0F);
        tx_buf[5] = g_cfg.macros[cfg_sync_step].key_code;
    }

    tx_buf[8] = cfg_checksum(tx_buf);
    tx_buf[9] = 0;
    RF_Tx(tx_buf, 10, 0xFF, 0xFF);

    if(++cfg_sync_step > MACRO_BTN_NUM) {
        cfg_sync_step = 0;
        if(++cfg_sync_retry >= 4) cfg_sync_pending = 0;
    }
}
