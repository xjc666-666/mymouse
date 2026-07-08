#include "switch.h"

/* ============================================================
 *  两位拨码开关读取（PB7 + PA7，上拉，低 = 按下）
 *
 *  PB7 高 / PA7 高  -> BLE
 *  PB7 低 / PA7 高  -> WIRED
 *  PB7 高 / PA7 低  -> 24G
 *  PB7 低 / PA7 低  -> UNKNOWN（避免误进入任何模式）
 * ============================================================ */
void Mode_Switch_Init(void)
{
    GPIOB_ModeCfg(MODE_PIN_PB, GPIO_ModeIN_PU);
    GPIOA_ModeCfg(MODE_PIN_PA, GPIO_ModeIN_PU);
}

Mouse_Mode_t Mode_Switch_Read(void)
{
    uint8_t pb = (GPIOB_ReadPortPin(MODE_PIN_PB) == 0) ? 0 : 1;  /* 1 = 高 */
    uint8_t pa = (GPIOA_ReadPortPin(MODE_PIN_PA) == 0) ? 0 : 1;

    if(pb == 1 && pa == 1) return MODE_BLE;
    if(pb == 0 && pa == 1) return MODE_WIRED;
    if(pb == 1 && pa == 0) return MODE_24G;
    return MODE_UNKNOWN;
}
