#include "g24_config.h"
#include "usb_desc_patch.h"
#include <string.h>

extern volatile uint8_t USBHS_DevEnumStatus;
extern void USBHS_Device_Init(FunctionalState sta);
extern void mDelaymS(uint16_t t);

#define G24_CFG_MAGIC   0x4D435548u
#define G24_CFG_FLASH_ADDR 0x00

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t poll_interval;
    uint8_t reserved[2];
    G24_Button_Macro_t macros[G24_MACRO_BTN_NUM];
    uint8_t pad[9];
} G24_Config_t;

G24_Config_t g24_cfg;
G24_Button_Macro_t Macro_Table[G24_MACRO_BTN_NUM];
volatile uint8_t g24_cfg_save_pending = 0;
volatile uint8_t g24_cfg_renumerate_pending = 0;
static uint16_t cfg_save_delay = 0;

static uint8_t cfg_valid_poll(uint8_t interval)
{
    return interval >= G24_POLL_INTERVAL_1000HZ && interval <= G24_POLL_INTERVAL_125HZ;
}

static void cfg_apply_defaults(void)
{
    g24_cfg.magic = G24_CFG_MAGIC;
    g24_cfg.version = G24_CFG_VERSION;
    g24_cfg.poll_interval = G24_POLL_INTERVAL_1000HZ;
    g24_cfg.reserved[0] = 0;
    g24_cfg.reserved[1] = 0;
    memset(g24_cfg.macros, 0, sizeof(g24_cfg.macros));
    memset(g24_cfg.pad, 0, sizeof(g24_cfg.pad));
}

static void cfg_sync_runtime(void)
{
    if(!cfg_valid_poll(g24_cfg.poll_interval)) {
        g24_cfg.poll_interval = G24_POLL_INTERVAL_1000HZ;
    }

    memcpy(Macro_Table, g24_cfg.macros, sizeof(Macro_Table));
    for(uint8_t i = 0; i < G24_MACRO_BTN_NUM; i++) {
        if(Macro_Table[i].is_macro &&
           Macro_Table[i].modifier == 0 && Macro_Table[i].key_code == 0) {
            Macro_Table[i].is_macro = 0;
            g24_cfg.macros[i].is_macro = 0;
        }
    }

    G24_Apply_Poll_Rate(g24_cfg.poll_interval);
}

void G24_Config_Load(void)
{
    G24_Config_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    EEPROM_READ(G24_CFG_FLASH_ADDR, &tmp, sizeof(tmp));
    if(tmp.magic == G24_CFG_MAGIC && tmp.version == G24_CFG_VERSION) {
        g24_cfg = tmp;
    } else {
        cfg_apply_defaults();
        cfg_save_delay = 500;
        g24_cfg_save_pending = 1;
    }
    cfg_sync_runtime();
}

void G24_Config_Save(void)
{
    EEPROM_ERASE(G24_CFG_FLASH_ADDR, EEPROM_BLOCK_SIZE);
    EEPROM_WRITE(G24_CFG_FLASH_ADDR, &g24_cfg, sizeof(g24_cfg));
}

void G24_Config_Service(void)
{
    if(g24_cfg_save_pending) {
        if(cfg_save_delay) {
            cfg_save_delay--;
        } else {
            g24_cfg_save_pending = 0;
            G24_Config_Save();
        }
    }

    if(g24_cfg_renumerate_pending && USBHS_DevEnumStatus) {
        g24_cfg_renumerate_pending = 0;
        if(g24_cfg_save_pending) {
            g24_cfg_save_pending = 0;
            G24_Config_Save();
        }
        USBHS_Device_Init(DISABLE);
        mDelaymS(220);
        USBHS_Device_Init(ENABLE);
    }
}

void G24_Apply_Poll_Rate(uint8_t bInterval)
{
    MyCfgDescr[CFG_BINTERVAL_OFFSET_EP1] = bInterval;
    MyCfgDescr[CFG_BINTERVAL_OFFSET_EP2] = bInterval;
}

void G24_Config_Apply_From_RF(uint8_t poll_interval, uint8_t index, uint8_t is_macro, uint8_t modifier, uint8_t key_code)
{
    uint8_t changed = 0;

    if(cfg_valid_poll(poll_interval) && g24_cfg.poll_interval != poll_interval) {
        g24_cfg.poll_interval = poll_interval;
        G24_Apply_Poll_Rate(poll_interval);
        g24_cfg_renumerate_pending = 1;
        changed = 1;
    }

    if(index < G24_MACRO_BTN_NUM) {
        is_macro = is_macro ? 1 : 0;
        modifier &= 0x0F;
        if(g24_cfg.macros[index].is_macro != is_macro ||
           g24_cfg.macros[index].modifier != modifier ||
           g24_cfg.macros[index].key_code != key_code) {
            g24_cfg.macros[index].is_macro = is_macro;
            g24_cfg.macros[index].modifier = modifier;
            g24_cfg.macros[index].key_code = key_code;
            Macro_Table[index] = g24_cfg.macros[index];
            changed = 1;
        }
    }

    if(changed) {
        cfg_save_delay = 500;
        g24_cfg_save_pending = 1;
    }
}
