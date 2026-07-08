#ifndef __USB_DESC_H
#define __USB_DESC_H

#include "CH58x_common.h"

#define DEF_USB_VID             0x1A86
// #define DEF_USB_PID          0xFE07
#define DEF_USB_PID             0x8890  // 换个新马甲，强迫电脑重新认识它！
#define DEF_IC_PRG_VER          0x01

// 核心缓冲区大小定义 (满足底层的饥渴)
#define DEF_USBD_UEP0_SIZE      64
#define DEF_USB_EP1_HS_SIZE     8     // 键盘端点大小 (最大包长)
#define DEF_USB_EP2_HS_SIZE     8     // 鼠标端点大小 (最大包长，8字节足够装下你的 6字节 16-bit 数据)
#define DEF_USBD_HS_PACK_SIZE   512   // 高速包大小

// 描述符长度硬编码 (骗过编译器的底层 sizeof 检查)
#define DEF_USBD_DEVICE_DESC_LEN    18
#define DEF_USBD_CONFIG_DESC_LEN    59
#define DEF_USBD_LANG_DESC_LEN      4
#define DEF_USBD_MANU_DESC_LEN      14
#define DEF_USBD_PROD_DESC_LEN      24
#define DEF_USBD_SN_DESC_LEN        22
#define DEF_USBD_QUALFY_DESC_LEN    10
#define DEF_USBD_REPORT_DESC_LEN_KB 62
#define DEF_USBD_REPORT_DESC_LEN_MS 64 // 电竞级鼠标报表长度为 64 (0x40)

// 描述符声明
extern const uint8_t MyDevDescr[];
extern const uint8_t MyCfgDescr[];
extern const uint8_t MyLangDescr[];
extern const uint8_t MyManuInfo[];
extern const uint8_t MyProdInfo[];
extern const uint8_t MySerNumInfo[];
extern const uint8_t KeyRepDesc[];
extern const uint8_t MouseRepDesc[];
extern const uint8_t MyQuaDesc[];

// ?? 修复项：沁恒原厂 USBHS 库必须依赖此数组进行速度降级枚举，不能删！
extern uint8_t USB_FS_OSC_DESC[];

#endif