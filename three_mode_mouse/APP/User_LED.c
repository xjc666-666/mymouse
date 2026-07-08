#include "User_LED.h"

/*********************************************************************
 * @fn      User_LED_Init
 *
 * @brief   初始化用户 LED 引脚，并默认熄灭
 *
 * @return  none
 */
void User_LED_Init(void)
{
    // 1. 配置 PA4 和 PA6 为推挽输出，最大驱动能力 5mA
    GPIOA_ModeCfg(LED1_PIN | LED2_PIN, GPIO_ModeOut_PP_5mA);

    // 2. 默认熄灭 (因为是低电平亮，所以初始化时输出高电平)
    GPIOA_SetBits(LED1_PIN | LED2_PIN);
}

/*********************************************************************
 * @fn      User_LED_On
 *
 * @brief   点亮指定的 LED (拉低电平)
 *
 * @param   led_id - LED_1, LED_2, 或 LED_ALL
 *
 * @return  none
 */
void User_LED_On(uint8_t led_id)
{
    if (led_id == LED_1) {
        GPIOA_ResetBits(LED1_PIN);
    } 
    else if (led_id == LED_2) {
        GPIOA_ResetBits(LED2_PIN);
    } 
    else if (led_id == LED_ALL) {
        GPIOA_ResetBits(LED1_PIN | LED2_PIN);
    }
}

/*********************************************************************
 * @fn      User_LED_Off
 *
 * @brief   熄灭指定的 LED (拉高电平)
 *
 * @param   led_id - LED_1, LED_2, 或 LED_ALL
 *
 * @return  none
 */
void User_LED_Off(uint8_t led_id)
{
    if (led_id == LED_1) {
        GPIOA_SetBits(LED1_PIN);
    } 
    else if (led_id == LED_2) {
        GPIOA_SetBits(LED2_PIN);
    } 
    else if (led_id == LED_ALL) {
        GPIOA_SetBits(LED1_PIN | LED2_PIN);
    }
}

/*********************************************************************
 * @fn      User_LED_Toggle
 *
 * @brief   翻转指定的 LED 状态 (亮变灭，灭变亮)
 *
 * @param   led_id - LED_1, LED_2, 或 LED_ALL
 *
 * @return  none
 */
void User_LED_Toggle(uint8_t led_id)
{
    if (led_id == LED_1) {
        GPIOA_InverseBits(LED1_PIN);
    } 
    else if (led_id == LED_2) {
        GPIOA_InverseBits(LED2_PIN);
    } 
    else if (led_id == LED_ALL) {
        GPIOA_InverseBits(LED1_PIN | LED2_PIN);
    }
}