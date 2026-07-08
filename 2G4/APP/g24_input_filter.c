#include "g24_input_filter.h"
#include "g24_config.h"
#include "usbd_keyboard_patch.h"

static uint8_t prev_raw[G24_MACRO_BTN_NUM] = {0};

static uint8_t btn_mask_for_index(uint8_t index)
{
    switch(index) {
        case 0: return 0x01;
        case 1: return 0x02;
        case 2: return 0x04;
        case 3: return 0x10;
        case 4: return 0x08;
        default: return 0;
    }
}

uint8_t G24_Filter_Mouse_Buttons(uint8_t raw_buttons)
{
    uint8_t filtered = raw_buttons;

    for(uint8_t i = 0; i < G24_MACRO_BTN_NUM; i++) {
        uint8_t mask = btn_mask_for_index(i);
        uint8_t down = (raw_buttons & mask) ? 1 : 0;
        G24_Button_Macro_t *m = &Macro_Table[i];

        if(m->is_macro && (m->modifier != 0 || m->key_code != 0)) {
            filtered &= (uint8_t)~mask;
            if(down && !prev_raw[i]) {
                USBD_Keyboard_Send(m->modifier, m->key_code);
            } else if(!down && prev_raw[i]) {
                USBD_Keyboard_Release();
            }
        }

        prev_raw[i] = down;
    }

    return filtered;
}
