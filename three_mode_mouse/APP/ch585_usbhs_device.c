#include "ch585_usbhs_device.h"
#include "usbd_mouse.h"
#include "CH58x_common.h"

extern void USBD_Custom_Process_Cmd(uint8_t *rx_buf, uint16_t len);

volatile uint8_t  USBHS_Test_Flag;
__attribute__ ((aligned(4))) uint8_t IFTest_Buf[ 53 ] = {0}; // 略
const uint8_t    *pUSBHS_Descr;

volatile uint8_t  USBHS_SetupReqCode;
volatile uint8_t  USBHS_SetupReqType;
volatile uint16_t USBHS_SetupReqValue;
volatile uint16_t USBHS_SetupReqIndex;
volatile uint16_t USBHS_SetupReqLen;

volatile uint8_t  USBHS_DevConfig;
volatile uint8_t  USBHS_DevAddr;
volatile uint16_t USBHS_DevMaxPackLen;
volatile uint8_t  USBHS_DevSpeed;
volatile uint8_t  USBHS_DevSleepStatus;
volatile uint8_t  USBHS_DevEnumStatus;

// ? 修复为 3
volatile uint8_t  USBHS_HidIdle[ 3 ];
volatile uint8_t  USBHS_HidProtocol[ 3 ];

__attribute__ ((aligned(4))) uint8_t USBHS_EP0_Buf[ DEF_USBD_UEP0_SIZE ];     
__attribute__ ((aligned(4))) uint8_t USBHS_EP1_TX_Buf[ DEF_USB_EP1_HS_SIZE ]; 
__attribute__ ((aligned(4))) uint8_t USBHS_EP2_TX_Buf[ DEF_USB_EP2_HS_SIZE ]; 
__attribute__ ((aligned(4))) uint8_t USBHS_EP3_TX_Buf[ 64 ]; // EP3 TX
__attribute__ ((aligned(4))) uint8_t USBHS_EP3_RX_Buf[ 64 ]; // EP3 RX

volatile uint8_t  USBHS_Endp_Busy[ DEF_UEP_NUM ];
RING_BUFF_COMM  RingBuffer_Comm;
__attribute__ ((aligned(4))) uint8_t Data_Buffer[DEF_RING_BUFFER_SIZE];

void USB_TestMode_Deal( void ) { }

void USBHS_Device_Endp_Init ( void )
{
    uint8_t i = 0;
    R16_U2EP_TX_EN = RB_EP0_EN | RB_EP1_EN | RB_EP2_EN | RB_EP3_EN;
    R16_U2EP_RX_EN = RB_EP0_EN | RB_EP3_EN;

    R32_U2EP0_MAX_LEN = DEF_USBD_UEP0_SIZE;
    R32_U2EP1_MAX_LEN = DEF_USB_EP1_HS_SIZE;
    R32_U2EP2_MAX_LEN = DEF_USB_EP2_HS_SIZE;
    R32_U2EP3_MAX_LEN = 64; 

    R32_U2EP0_DMA    = (uint32_t)(uint8_t *)USBHS_EP0_Buf;
    R32_U2EP1_TX_DMA = (uint32_t)(uint8_t *)USBHS_EP1_TX_Buf;
    R32_U2EP2_TX_DMA = (uint32_t)(uint8_t *)USBHS_EP2_TX_Buf;
    R32_U2EP3_TX_DMA = (uint32_t)(uint8_t *)USBHS_EP3_TX_Buf; 
    R32_U2EP3_RX_DMA = (uint32_t)(uint8_t *)USBHS_EP3_RX_Buf; 

    R8_U2EP0_TX_CTRL = USBHS_UEP_T_RES_NAK;
    R8_U2EP0_RX_CTRL = USBHS_UEP_R_RES_ACK;
    R8_U2EP1_TX_CTRL = USBHS_UEP_T_RES_NAK;
    R8_U2EP2_TX_CTRL = USBHS_UEP_T_RES_NAK;
    R8_U2EP3_TX_CTRL = USBHS_UEP_T_RES_NAK;
    R8_U2EP3_RX_CTRL = USBHS_UEP_R_RES_ACK;

    for( i=0; i < DEF_UEP_NUM; i++ ) { USBHS_Endp_Busy[ i ] = 0; }
}

void USBHS_Device_Init ( FunctionalState sta )
{
    if( sta )
    {
        R16_CLK_SYS_CFG |= (RB_CLK_SYS_MOD & 0x40) | RB_XROM_SCLK_SEL | RB_OSC32M_SEL;
        R8_USBHS_PLL_CTRL = USBHS_PLL_EN;
        R16_PIN_CONFIG |= RB_PIN_USB2_EN;

        R8_USB2_CTRL = USBHS_UD_RST_LINK | USBHS_UD_PHY_SUSPENDM;            
        R8_USB2_INT_EN = USBHS_UDIE_BUS_RST | USBHS_UDIE_SUSPEND | USBHS_UDIE_BUS_SLEEP | USBHS_UDIE_LPM_ACT | USBHS_UDIE_TRANSFER | USBHS_UDIE_LINK_RDY;      
        USBHS_Device_Endp_Init();
        R8_USB2_BASE_MODE = USBHS_UD_SPEED_HIGH;
        R8_USB2_CTRL = USBHS_UD_DEV_EN | USBHS_UD_DMA_EN | USBHS_UD_LPM_EN | USBHS_UD_PHY_SUSPENDM;
        PFIC_EnableIRQ( USB2_DEVICE_IRQn );
    }
    else
    {
        R16_CLK_SYS_CFG &= ~((RB_CLK_SYS_MOD & 0x40) | RB_XROM_SCLK_SEL | RB_OSC32M_SEL);
        R8_USBHS_PLL_CTRL &= ~USBHS_PLL_EN;
        R32_PIN_CONFIG &= ~RB_PIN_USB2_EN;

        R8_USB2_CTRL |= USBHS_UD_RST_SIE;
        R8_USB2_CTRL &= ~USBHS_UD_RST_SIE;
        PFIC_DisableIRQ( USB2_DEVICE_IRQn );
    }
}

uint8_t USBHS_Endp_DataUp( uint8_t endp, uint8_t *pbuf, uint16_t len, uint8_t mod )
{
    uint8_t endp_en;
    if( (endp>=DEF_UEP1) && (endp<=DEF_UEP15) )
    {
        endp_en =  R16_U2EP_TX_EN;
        if( endp_en & USBHSD_UEP_TX_EN( endp ) )
        {
            if( (USBHS_Endp_Busy[ endp ] & DEF_UEP_BUSY) == 0x00 )
            {
                if( mod == DEF_UEP_DMA_LOAD ) { USBHSD_UEP_TXDMA( endp ) = (uint32_t)pbuf; }
                else if( mod == DEF_UEP_CPY_LOAD ) { memcpy( USBHSD_UEP_TXBUF(endp), pbuf, len ); }
                else { return 0; }

                USBHS_Endp_Busy[ endp ] |= DEF_UEP_BUSY;
                USBHSD_UEP_TLEN( endp ) = len;
                USBHSD_UEP_TXCTRL( endp ) = (USBHSD_UEP_TXCTRL( endp ) &= ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK;
            }
            else { return 0; }
        }
        else { return 0; }
    }
    else { return 0; }
    return 1;
}

__INTERRUPT
__HIGH_CODE
void USB2_DEVICE_IRQHandler( void )
{
    uint8_t  intflag, intst, errflag;
    uint16_t len;
    uint8_t endp_num;

    intflag = R8_USB2_INT_FG;
    intst = R8_USB2_INT_ST;

    if( intflag & USBHS_UDIF_TRANSFER )
    {
        endp_num = intst & USBHS_UDIS_EP_ID_MASK;
        if( !(intst & USBHS_UDIS_EP_DIR )) // SETUP/OUT
        {
            switch( endp_num )
            {
                case   DEF_UEP0:
                    if( R8_U2EP0_RX_CTRL & USBHS_UEP_R_SETUP_IS )
                    {
                        USBHS_SetupReqType  = pUSBHS_SetupReqPak->bRequestType;
                        USBHS_SetupReqCode  = pUSBHS_SetupReqPak->bRequest;
                        USBHS_SetupReqLen   = pUSBHS_SetupReqPak->wLength;
                        USBHS_SetupReqValue = pUSBHS_SetupReqPak->wValue;
                        USBHS_SetupReqIndex = pUSBHS_SetupReqPak->wIndex;

                        len = 0;
                        errflag = 0;
                        if ( ( USBHS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
                        {
                            if (( USBHS_SetupReqType & USB_REQ_TYP_MASK ) == USB_REQ_TYP_CLASS)
                            {
                                switch( USBHS_SetupReqCode )
                                {
                                    case HID_SET_REPORT: break;
                                    case HID_SET_IDLE:
                                        if( USBHS_SetupReqIndex < 3 ) { USBHS_HidIdle[ USBHS_SetupReqIndex ] = (uint8_t)( USBHS_SetupReqValue >> 8 ); }
                                        else { errflag = 0xFF; }
                                        break;
                                    case HID_SET_PROTOCOL:
                                        if( USBHS_SetupReqIndex < 3 ) { USBHS_HidProtocol[ USBHS_SetupReqIndex ] = (uint8_t)USBHS_SetupReqValue; }
                                        else { errflag = 0xFF; }
                                        break;
                                    case HID_GET_IDLE:
                                        if( USBHS_SetupReqIndex < 3 ) { USBHS_EP0_Buf[ 0 ] = USBHS_HidIdle[ USBHS_SetupReqIndex ]; len = 1; }
                                        else { errflag = 0xFF; }
                                        break;
                                    case HID_GET_PROTOCOL:
                                        if( USBHS_SetupReqIndex < 3 ) { USBHS_EP0_Buf[ 0 ] = USBHS_HidProtocol[ USBHS_SetupReqIndex ]; len = 1; }
                                        else { errflag = 0xFF; }
                                        break;
                                    default: errflag = 0xFF; break;
                                }
                            }
                        }
                        else
                        {
                            switch( USBHS_SetupReqCode )
                            {
                                case USB_GET_DESCRIPTOR:
                                    switch( (uint8_t)(USBHS_SetupReqValue>>8) )
                                    {
                                        case USB_DESCR_TYP_DEVICE: pUSBHS_Descr = MyDevDescr; len = DEF_USBD_DEVICE_DESC_LEN; break;
                                        case USB_DESCR_TYP_CONFIG: 
                                            pUSBHS_Descr = MyCfgDescr; 
                                            len = DEF_USBD_CONFIG_DESC_LEN; // 完美匹配 91 字节
                                            break;
                                        case USB_DESCR_TYP_STRING:
                                            switch( (uint8_t)(USBHS_SetupReqValue&0xFF) )
                                            {
                                                case DEF_STRING_DESC_LANG: pUSBHS_Descr = MyLangDescr; len = DEF_USBD_LANG_DESC_LEN; break;
                                                case DEF_STRING_DESC_MANU: pUSBHS_Descr = MyManuInfo; len = DEF_USBD_MANU_DESC_LEN; break;
                                                case DEF_STRING_DESC_PROD: pUSBHS_Descr = MyProdInfo; len = DEF_USBD_PROD_DESC_LEN; break;
                                                case DEF_STRING_DESC_SERN: pUSBHS_Descr = MySerNumInfo; len = DEF_USBD_SN_DESC_LEN; break;
                                                default: errflag = 0xFF; break;
                                            }
                                            break;
                                        case USB_DESCR_TYP_QUALIF: pUSBHS_Descr = MyQuaDesc; len = DEF_USBD_QUALFY_DESC_LEN; break;
                                        case USB_DESCR_TYP_BOS: errflag = 0xFF; break;
                                        case USB_DESCR_TYP_HID:
                                            if( USBHS_SetupReqIndex == 0x00 ) { pUSBHS_Descr = &MyCfgDescr[ 18 ]; len = 9; }
                                            else if( USBHS_SetupReqIndex == 0x01 ) { pUSBHS_Descr = &MyCfgDescr[ 43 ]; len = 9; }
                                            else if( USBHS_SetupReqIndex == 0x02 ) { pUSBHS_Descr = &MyCfgDescr[ 68 ]; len = 9; }
                                            else { errflag = 0xFF; }
                                            break;
                                        case USB_DESCR_TYP_REPORT:
                                            if( USBHS_SetupReqIndex == 0x00 ) { pUSBHS_Descr = KeyRepDesc; len = DEF_USBD_REPORT_DESC_LEN_KB; }
                                            else if( USBHS_SetupReqIndex == 0x01 ) { pUSBHS_Descr = MouseRepDesc; len = DEF_USBD_REPORT_DESC_LEN_MS; }
                                            else if( USBHS_SetupReqIndex == 0x02 ) { pUSBHS_Descr = CustomRepDesc; len = 34; }
                                            else { errflag = 0xFF; }
                                            break;
                                        default : errflag = 0xFF; break;
                                    }
                                    if( USBHS_SetupReqLen>len ) { USBHS_SetupReqLen = len; }
                                    len = (USBHS_SetupReqLen >= DEF_USBD_UEP0_SIZE) ? DEF_USBD_UEP0_SIZE : USBHS_SetupReqLen;
                                    memcpy( USBHS_EP0_Buf, pUSBHS_Descr, len );
                                    pUSBHS_Descr += len;
                                    break;
                                case USB_SET_ADDRESS: USBHS_DevAddr = (uint16_t)(USBHS_SetupReqValue&0xFF); break;
                                case USB_GET_CONFIGURATION: USBHS_EP0_Buf[0] = USBHS_DevConfig; if ( USBHS_SetupReqLen > 1 ) { USBHS_SetupReqLen = 1; } break;
                                case USB_SET_CONFIGURATION: USBHS_DevConfig = (uint8_t)(USBHS_SetupReqValue&0xFF); USBHS_DevEnumStatus = 0x01; break;
                                case USB_CLEAR_FEATURE:
                                    if( ( USBHS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                                    {
                                        if((uint8_t)(USBHS_SetupReqValue&0xFF) == 0x01) { USBHS_DevSleepStatus &= ~0x01; }
                                        else { errflag = 0xFF; }
                                    }
                                    else if ( ( USBHS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                                    {
                                        if( (uint8_t)(USBHS_SetupReqValue&0xFF) == USB_REQ_FEAT_ENDP_HALT )
                                        {
                                            switch( (uint8_t)(USBHS_SetupReqIndex&0xFF) )
                                            {
                                                case ( DEF_UEP_IN | DEF_UEP1 ): R8_U2EP1_TX_CTRL = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK; break;
                                                case ( DEF_UEP_IN | DEF_UEP2 ): R8_U2EP2_TX_CTRL = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK; break;
                                                case ( DEF_UEP_IN | DEF_UEP3 ): 
                                                case ( DEF_UEP_OUT | DEF_UEP3 ): 
                                                    R8_U2EP3_TX_CTRL = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK; 
                                                    R8_U2EP3_RX_CTRL = USBHS_UEP_R_TOG_DATA0 | USBHS_UEP_R_RES_ACK;
                                                    break;
                                                default: errflag = 0xFF; break;
                                            }
                                        }
                                        else { errflag = 0xFF; }
                                    }
                                    else { errflag = 0xFF; }
                                    break;
                                case USB_GET_INTERFACE: USBHS_EP0_Buf[0] = 0x00; if ( USBHS_SetupReqLen > 1 ) { USBHS_SetupReqLen = 1; } break;
                                case USB_SET_INTERFACE: break;
                                case USB_GET_STATUS:
                                    USBHS_EP0_Buf[ 0 ] = 0x00; USBHS_EP0_Buf[ 1 ] = 0x00;
                                    if( ( USBHS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                                    {
                                        if( USBHS_DevSleepStatus & 0x01 ) { USBHS_EP0_Buf[ 0 ] = 0x02; }
                                        else { USBHS_EP0_Buf[ 0 ] = 0x00; }
                                    }
                                    else if( ( USBHS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                                    {
                                        if( (uint8_t)( USBHS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP1 ) )
                                        {
                                            if( ( R8_U2EP1_TX_CTRL & USBHS_UEP_T_RES_MASK ) == USBHS_UEP_T_RES_STALL ) { USBHS_EP0_Buf[ 0 ] = 0x01; }
                                        }
                                        else if( (uint8_t)( USBHS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP2 ) )
                                        {
                                            if( ( R8_U2EP2_TX_CTRL & USBHS_UEP_T_RES_MASK ) == USBHS_UEP_T_RES_STALL ) { USBHS_EP0_Buf[ 0 ] = 0x01; }
                                        }
                                        else if( (uint8_t)( USBHS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP3 ) || (uint8_t)( USBHS_SetupReqIndex & 0xFF ) == ( DEF_UEP_OUT | DEF_UEP3 ) )
                                        {
                                            USBHS_EP0_Buf[ 0 ] = 0x00;
                                        }
                                        else { errflag = 0xFF; }
                                    }
                                    else { errflag = 0xFF; }
                                    if( USBHS_SetupReqLen > 2 ) { USBHS_SetupReqLen = 2; }
                                    break;
                                case USB_SET_FEATURE:
                                    if( ( USBHS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                                    {
                                        if( (uint8_t)( USBHS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_REMOTE_WAKEUP )
                                        {
                                            if( pUSBHS_Descr[ 7 ] & 0x20 ) { USBHS_DevSleepStatus |= 0x01; }
                                            else { errflag = 0xFF; }
                                        }
                                        else if( (uint8_t)(USBHS_SetupReqValue&0xFF) == 0x02 )
                                        {
                                            if( ( USBHS_SetupReqIndex == 0x0100 ) || ( USBHS_SetupReqIndex == 0x0200 ) || ( USBHS_SetupReqIndex == 0x0300 ) || ( USBHS_SetupReqIndex == 0x0400 ) )
                                            {
                                                USBHS_Test_Flag |= 0x80;
                                            }
                                        }
                                        else { errflag = 0xFF; }
                                    }
                                    else if( ( USBHS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                                    {
                                        if( (uint8_t)(USBHS_SetupReqValue&0xFF) == USB_REQ_FEAT_ENDP_HALT )
                                        {
                                            switch((uint8_t)(USBHS_SetupReqIndex&0xFF) )
                                            {
                                                case ( DEF_UEP_IN | DEF_UEP1 ): R8_U2EP1_TX_CTRL = ( R8_U2EP1_TX_CTRL & ~USBHS_UEP_T_RES_MASK ) | USBHS_UEP_T_RES_STALL; break;
                                                case ( DEF_UEP_IN | DEF_UEP2 ): R8_U2EP2_TX_CTRL = ( R8_U2EP2_TX_CTRL & ~USBHS_UEP_T_RES_MASK ) | USBHS_UEP_T_RES_STALL; break;
                                                default: errflag = 0xFF; break;
                                            }
                                        }
                                    }
                                    break;
                                default: errflag = 0xFF; break;
                            }
                        }

                        if( errflag == 0xFF )
                        {
                            R8_U2EP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_STALL;
                            R8_U2EP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_STALL;
                        }
                        else
                        {
                            if( USBHS_SetupReqType & DEF_UEP_IN )
                            {
                                len = (USBHS_SetupReqLen>DEF_USBD_UEP0_SIZE) ? DEF_USBD_UEP0_SIZE : USBHS_SetupReqLen;
                                USBHS_SetupReqLen -= len;
                                R16_U2EP0_T_LEN = len;
                                R8_U2EP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_ACK;
                            }
                            else
                            {
                                if( USBHS_SetupReqLen == 0 ) { R16_U2EP0_T_LEN  = 0; R8_U2EP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_ACK; }
                                else { R8_U2EP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK; }
                            }
                        }
                    }
                    else
                    {
                        R8_U2EP0_RX_CTRL = USBHS_UEP_R_RES_NAK; 
                        len = R16_U2EP0_RX_LEN;
                        if( USBHS_SetupReqLen == 0 ) { R16_U2EP0_T_LEN  = 0; R8_U2EP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_ACK; }
                    }
                    R8_U2EP0_RX_CTRL &= ~USBHS_UEP_R_DONE;
                   break;

                case DEF_UEP3:
                    if( R8_U2EP3_RX_CTRL & USBHS_UEP_R_DONE )
                    {
                        len = R16_U2EP3_RX_LEN; 
                        USBD_Custom_Process_Cmd(USBHS_EP3_RX_Buf, len);
                        
                        R8_U2EP3_RX_CTRL = (R8_U2EP3_RX_CTRL & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK;
                        R8_U2EP3_RX_CTRL ^= USBHS_UEP_R_TOG_DATA1;
                        R8_U2EP3_RX_CTRL &= ~USBHS_UEP_R_DONE;
                    }
                    break;

               default: errflag = 0xFF; break;
            }
        }
        else
        {
            switch ( endp_num )
            {
                case  DEF_UEP0:
                    if( USBHS_SetupReqLen == 0 ) { R8_U2EP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK; }
                    if ( ( USBHS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD ) { }
                    else
                    {
                        switch( USBHS_SetupReqCode )
                        {
                            case USB_GET_DESCRIPTOR:
                                len = USBHS_SetupReqLen >= DEF_USBD_UEP0_SIZE ? DEF_USBD_UEP0_SIZE : USBHS_SetupReqLen;
                                memcpy(USBHS_EP0_Buf, pUSBHS_Descr, len);
                                USBHS_SetupReqLen -= len; pUSBHS_Descr += len; R16_U2EP0_T_LEN = len;
                                R8_U2EP0_TX_CTRL ^= USBHS_UEP_T_TOG_DATA1;
                                R8_U2EP0_TX_CTRL = ( R8_U2EP0_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK; 
                                break;
                            case USB_SET_ADDRESS: R8_USB2_DEV_AD = USBHS_DevAddr; break;
                            default: R16_U2EP0_T_LEN = 0; break;
                        }
                    }
                    if( USBHS_Test_Flag & 0x80 ) { USB_TestMode_Deal( ); }
                    R8_U2EP0_TX_CTRL &= ~USBHS_UEP_T_DONE;
                    break;

                case DEF_UEP1:
                    R8_U2EP1_TX_CTRL = ( R8_U2EP1_TX_CTRL & ~USBHS_UEP_T_RES_MASK ) | USBHS_UEP_T_RES_NAK;
                    R8_U2EP1_TX_CTRL ^= USBHS_UEP_T_TOG_DATA1;
                    USBHS_Endp_Busy[ DEF_UEP1 ] &= ~DEF_UEP_BUSY;
                    R8_U2EP1_TX_CTRL &= ~USBHS_UEP_T_DONE;
                    break;

                case DEF_UEP2:
                    R8_U2EP2_TX_CTRL = ( R8_U2EP2_TX_CTRL & ~USBHS_UEP_T_RES_MASK ) | USBHS_UEP_T_RES_NAK;
                    R8_U2EP2_TX_CTRL ^= USBHS_UEP_T_TOG_DATA1;
                    USBHS_Endp_Busy[ DEF_UEP2 ] &= ~DEF_UEP_BUSY;
                    R8_U2EP2_TX_CTRL &= ~USBHS_UEP_T_DONE;
                    break;

                case DEF_UEP3:
                    R8_U2EP3_TX_CTRL = ( R8_U2EP3_TX_CTRL & ~USBHS_UEP_T_RES_MASK ) | USBHS_UEP_T_RES_NAK;
                    R8_U2EP3_TX_CTRL ^= USBHS_UEP_T_TOG_DATA1;
                    USBHS_Endp_Busy[ DEF_UEP3 ] &= ~DEF_UEP_BUSY;
                    R8_U2EP3_TX_CTRL &= ~USBHS_UEP_T_DONE;
                    break;

                default : break;
            }
        }
    }
    else if( intflag & USBHS_UDIF_LINK_RDY )
    {
#ifdef  SUPPORT_USB_HSI
            USB_HSI->CAL_CR |= HSI_CAL_EN | HSI_CAL_VLD;
            USB_HSI->CAL_CR &= ~HSI_CAL_RST;
#endif
            R8_USB2_INT_FG = USBHS_UDIF_LINK_RDY;
    }
    else if( intflag & USBHS_UDIF_SUSPEND )
    {
        R8_USB2_INT_FG = USBHS_UDIF_SUSPEND;
        if ( R8_USB2_MIS_ST & USBHS_UDMS_SUSPEND  )
        {
            USBHS_DevSleepStatus |= 0x02;
            if( USBHS_DevSleepStatus == 0x03 ) { MCU_Sleep_Wakeup_Operate( ); }
        }
        else { USBHS_DevSleepStatus &= ~0x02; }
    }
    else if( intflag & USBHS_UDIF_BUS_RST )
    {
        USBHS_DevConfig = 0; USBHS_DevAddr = 0; USBHS_DevSleepStatus = 0; USBHS_DevEnumStatus = 0;
        R8_USB2_DEV_AD = 0;
        USBHS_Device_Endp_Init( );
        R8_USB2_INT_FG = USBHS_UDIF_BUS_RST;
    }
    else { R8_USB2_INT_FG = intflag; }
}

void USBHS_Send_Resume(void)
{
    R8_USB2_WAKE_CTRL |= USBHS_UD_UD_REMOTE_WKUP;
}