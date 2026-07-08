#include "Mouse_Input.h"
#include "usbd_mouse.h"

/* 编码器状态机查找表 */
static const int8_t enc_states[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
static uint8_t old_AB = 0;
static int8_t  enc_acc = 0;

/* 单格脉冲的颗粒度 */
#define ENCODER_PULSES_PER_STEP 2

/* 各按键的历史状态，用于宏的边沿检测 */
static uint8_t prev_state[MACRO_BTN_NUM] = {0};

/* 暴露给主循环 / 上位机调试通道的"原始 GPIO 位掩码"，与宏屏蔽无关 */
volatile uint8_t Raw_Btn_Mask = 0;

void Mouse_Input_Init(void)
{
    /* 所有按键引脚配为内部上拉输入；按下接 GND */
    GPIOB_ModeCfg(BTN_LEFT_PIN | BTN_RIGHT_PIN | BTN_MIDDLE_PIN |
                  BTN_DPI_PIN  | BTN_FORWARD_PIN | BTN_BACK_PIN, GPIO_ModeIN_PU);

    /* 滚轮编码器引脚 */
    GPIOB_ModeCfg(ENCODER_A_PIN | ENCODER_B_PIN, GPIO_ModeIN_PU);

    uint8_t A = (GPIOB_ReadPortPin(ENCODER_A_PIN) == 0) ? 0 : 1;
    uint8_t B = (GPIOB_ReadPortPin(ENCODER_B_PIN) == 0) ? 0 : 1;
    old_AB = (A << 1) | B;
}

/*
 * 重构后的扫描逻辑：
 *   1) 先把所有按键的 raw GPIO 状态原封不动填进 state；
 *   2) 再遍历宏表，只有当 macro 真正"有效"（is_macro=1 且
 *      modifier 或 key_code 非零）时，才屏蔽对应原生按键、
 *      并按边沿触发键盘组合键。
 *
 * 这样即使宏表里残留无效条目，中键 / 任何按键都不会"消失"。
 */
void Mouse_Input_Scan(Mouse_State_t *state)
{
    uint8_t raw[MACRO_BTN_NUM];
    raw[BTN_IDX_LEFT]    = (GPIOB_ReadPortPin(BTN_LEFT_PIN)    == 0) ? 1 : 0;
    raw[BTN_IDX_RIGHT]   = (GPIOB_ReadPortPin(BTN_RIGHT_PIN)   == 0) ? 1 : 0;
    raw[BTN_IDX_MIDDLE]  = (GPIOB_ReadPortPin(BTN_MIDDLE_PIN)  == 0) ? 1 : 0;
    raw[BTN_IDX_FORWARD] = (GPIOB_ReadPortPin(BTN_FORWARD_PIN) == 0) ? 1 : 0;
    raw[BTN_IDX_BACK]    = (GPIOB_ReadPortPin(BTN_BACK_PIN)    == 0) ? 1 : 0;

    state->DPI = (GPIOB_ReadPortPin(BTN_DPI_PIN) == 0) ? 1 : 0;

    /* 默认全部按原始 GPIO 透传 */
    state->Left    = raw[BTN_IDX_LEFT];
    state->Right   = raw[BTN_IDX_RIGHT];
    state->Middle  = raw[BTN_IDX_MIDDLE];
    state->Forward = raw[BTN_IDX_FORWARD];
    state->Back    = raw[BTN_IDX_BACK];

    /* 同步原始按键位掩码（独立于宏逻辑），供上位机诊断 */
    uint8_t mask = 0;
    if(raw[BTN_IDX_LEFT])    mask |= 0x01;
    if(raw[BTN_IDX_RIGHT])   mask |= 0x02;
    if(raw[BTN_IDX_MIDDLE])  mask |= 0x04;
    if(raw[BTN_IDX_BACK])    mask |= 0x08;
    if(raw[BTN_IDX_FORWARD]) mask |= 0x10;
    Raw_Btn_Mask = mask;

    /* 处理宏：只在 macro 真正有效时屏蔽原生按键 */
    for(uint8_t i = 0; i < MACRO_BTN_NUM; i++)
    {
        Button_Macro_t *m = &Macro_Table[i];
        if(m->is_macro && (m->modifier != 0 || m->key_code != 0))
        {
            if(raw[i] == 1 && prev_state[i] == 0) {
                USBD_Keyboard_Send(m->modifier, m->key_code);
            } else if(raw[i] == 0 && prev_state[i] == 1) {
                USBD_Keyboard_Release();
            }
            switch(i) {
                case BTN_IDX_LEFT:    state->Left    = 0; break;
                case BTN_IDX_RIGHT:   state->Right   = 0; break;
                case BTN_IDX_MIDDLE:  state->Middle  = 0; break;
                case BTN_IDX_FORWARD: state->Forward = 0; break;
                case BTN_IDX_BACK:    state->Back    = 0; break;
                default: break;
            }
        }
        prev_state[i] = raw[i];
    }

    /* 滚轮编码器：标准 2-bit 状态机 */
    uint8_t A = (GPIOB_ReadPortPin(ENCODER_A_PIN) == 0) ? 0 : 1;
    uint8_t B = (GPIOB_ReadPortPin(ENCODER_B_PIN) == 0) ? 0 : 1;

    old_AB <<= 2;
    old_AB |= ((A << 1) | B);

    int8_t movement = enc_states[old_AB & 0x0F];
    if(movement != 0) {
        enc_acc += movement;
    }

    if(enc_acc >= ENCODER_PULSES_PER_STEP) {
        state->WheelCount++;
        enc_acc = 0;
    } else if(enc_acc <= -ENCODER_PULSES_PER_STEP) {
        state->WheelCount--;
        enc_acc = 0;
    }
}
