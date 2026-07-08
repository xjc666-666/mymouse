#include "usbd_keyboard_patch.h"
#include "ch585_usbhs_device.h"

static uint8_t key_report[8] = {0};

void USBD_Keyboard_Send(uint8_t modifier, uint8_t key_code)
{
    if(USBHS_DevEnumStatus) {
        key_report[0] = modifier;
        key_report[1] = 0x00;
        key_report[2] = key_code;
        key_report[3] = 0x00;
        key_report[4] = 0x00;
        key_report[5] = 0x00;
        key_report[6] = 0x00;
        key_report[7] = 0x00;
        USBHS_Endp_DataUp(DEF_UEP1, key_report, sizeof(key_report), DEF_UEP_CPY_LOAD);
    }
}

void USBD_Keyboard_Release(void)
{
    if(USBHS_DevEnumStatus) {
        for(uint8_t i = 0; i < sizeof(key_report); i++) key_report[i] = 0x00;
        USBHS_Endp_DataUp(DEF_UEP1, key_report, sizeof(key_report), DEF_UEP_CPY_LOAD);
    }
}
