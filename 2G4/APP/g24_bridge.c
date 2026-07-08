#include "rf_2g4.h"
#include "usbd_mouse.h"
#include "g24_config.h"
#include "g24_input_filter.h"

void G24_Bridge_Process(void)
{
    if(Has_New_RF_Data) {
        Has_New_RF_Data = 0;
        uint8_t buttons = G24_Filter_Mouse_Buttons(Mouse_RF_Data.buttons);
        USBD_Mouse_Send_Raw(buttons, Mouse_RF_Data.x, Mouse_RF_Data.y, Mouse_RF_Data.wheel);
    }

    G24_Config_Service();
    TMOS_SystemProcess();
}
