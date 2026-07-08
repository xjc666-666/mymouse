#ifndef __USBD_KEYBOARD_PATCH_H
#define __USBD_KEYBOARD_PATCH_H

#include "CH58x_common.h"

void USBD_Keyboard_Send(uint8_t modifier, uint8_t key_code);
void USBD_Keyboard_Release(void);

#endif
