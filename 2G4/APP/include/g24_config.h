#ifndef __G24_CONFIG_H
#define __G24_CONFIG_H

#include "CH58x_common.h"

#define G24_MACRO_BTN_NUM       5
#define G24_CFG_VERSION         0x03u

#define G24_POLL_INTERVAL_1000HZ    0x04
#define G24_POLL_INTERVAL_500HZ     0x05
#define G24_POLL_INTERVAL_250HZ     0x06
#define G24_POLL_INTERVAL_125HZ     0x07

#define G24_CFG_BINTERVAL_OFFSET_EP1    33
#define G24_CFG_BINTERVAL_OFFSET_EP2    58

typedef struct {
    uint8_t is_macro;
    uint8_t modifier;
    uint8_t key_code;
} G24_Button_Macro_t;

extern G24_Button_Macro_t Macro_Table[G24_MACRO_BTN_NUM];
extern volatile uint8_t g24_cfg_save_pending;
extern volatile uint8_t g24_cfg_renumerate_pending;

void G24_Config_Load(void);
void G24_Config_Save(void);
void G24_Config_Service(void);
void G24_Config_Apply_From_RF(uint8_t poll_interval, uint8_t index, uint8_t is_macro, uint8_t modifier, uint8_t key_code);
void G24_Apply_Poll_Rate(uint8_t bInterval);

#endif
