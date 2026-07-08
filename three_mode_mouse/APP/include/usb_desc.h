#ifndef __USB_DESC_H
#define __USB_DESC_H

#include "CH58x_common.h"

#define DEF_USB_VID             0x1A86
#define DEF_USB_PID             0x8894
#define DEF_IC_PRG_VER          0x01

#define DEF_USBD_UEP0_SIZE      64
#define DEF_USB_EP1_HS_SIZE     8
#define DEF_USB_EP2_HS_SIZE     8
#define DEF_USB_EP3_HS_SIZE     64
#define DEF_USBD_HS_PACK_SIZE   512

#define DEF_USBD_DEVICE_DESC_LEN    18
#define DEF_USBD_CONFIG_DESC_LEN    91
#define DEF_USBD_LANG_DESC_LEN      4
#define DEF_USBD_MANU_DESC_LEN      14
#define DEF_USBD_PROD_DESC_LEN      24
#define DEF_USBD_SN_DESC_LEN        22
#define DEF_USBD_QUALFY_DESC_LEN    10
#define DEF_USBD_REPORT_DESC_LEN_KB 62
#define DEF_USBD_REPORT_DESC_LEN_MS 64
#define DEF_USBD_REPORT_DESC_LEN_CM 34

/* 配置描述符中 EP1/EP2 的 bInterval 字段偏移，用于运行时修改回报率 */
#define CFG_BINTERVAL_OFFSET_EP1    33
#define CFG_BINTERVAL_OFFSET_EP2    58

extern const uint8_t MyDevDescr[];
extern       uint8_t MyCfgDescr[];   /* 非 const，运行时可修改 bInterval */
extern const uint8_t MyLangDescr[];
extern const uint8_t MyManuInfo[];
extern const uint8_t MyProdInfo[];
extern const uint8_t MySerNumInfo[];
extern const uint8_t KeyRepDesc[];
extern const uint8_t MouseRepDesc[];
extern const uint8_t CustomRepDesc[];
extern const uint8_t MyQuaDesc[];

#endif
