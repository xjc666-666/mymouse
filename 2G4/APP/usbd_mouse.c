#include "usbd_mouse.h"
#include "ch585_usbhs_device.h" 

volatile uint8_t KB_LED_Cur_Status = 0; 
void MCU_Sleep_Wakeup_Operate(void) { }

// 电竞级鼠标数据包 (1 字节按键 + 2 字节X + 2 字节Y + 1 字节滚轮 = 6 字节)
static uint8_t USB_Mouse_Pack[6] = {0x00};

void USBD_Mouse_Init(void) {
    // 启用高速 USB 引擎
    USBHS_Device_Init(ENABLE);
    // 开启 USB2_DEVICE 的全局中断
    PFIC_EnableIRQ(USB2_DEVICE_IRQn); 
}

// -------------------------------------------------------------------------
// 极速中转发送 (向高速端点 2 写入 6 字节电竞鼠标数据)
// -------------------------------------------------------------------------
void USBD_Mouse_Send_Raw(uint8_t btn, int16_t x, int16_t y, int8_t wheel)
{
    // 如果 USB 枚举成功且与电脑连接正常
    if( USBHS_DevEnumStatus ) 
    {
        // 按照 MouseRepDesc 的定义打包 6 字节数据
        USB_Mouse_Pack[0] = btn;                          // 按键状态 (bit0=左键, bit1=右键, bit2=中键)
        USB_Mouse_Pack[1] = (uint8_t)(x & 0xFF);          // X 轴相对位移 (低 8 位)
        USB_Mouse_Pack[2] = (uint8_t)((x >> 8) & 0xFF);   // X 轴相对位移 (高 8 位)
        USB_Mouse_Pack[3] = (uint8_t)(y & 0xFF);          // Y 轴相对位移 (低 8 位)
        USB_Mouse_Pack[4] = (uint8_t)((y >> 8) & 0xFF);   // Y 轴相对位移 (高 8 位)
        USB_Mouse_Pack[5] = (uint8_t)(wheel);             // 滚轮滚动值 (8 位)

        // 调用沁恒的 USBHS 底层端点上传函数
        // 目标端点2 (DEF_UEP2), 数据地址 USB_Mouse_Pack, 长度 6 字节
        // DEF_UEP_CPY_LOAD 表示使用 CPU 直接拷贝到端点 RAM
        USBHS_Endp_DataUp(DEF_UEP2, USB_Mouse_Pack, 6, DEF_UEP_CPY_LOAD);
    }
}