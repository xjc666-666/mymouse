#include "adc.h"
#include "CH58x_common.h"

/* ============================================================
 *  电池电量 ADC（PA12 / AIN2，1/2 分压）
 * ============================================================ */

static signed short s_calib_offset = 0;
static uint8_t      s_inited       = 0;
static uint8_t      s_cached_pct   = 50;       /* 启动时给个中间值，避免广播 0% */
static uint16_t     s_cached_mv    = 3700;
static uint32_t     s_last_tick    = 0;        /* 节流：防止过于频繁地采样 */

/* 滑动平均：4 次采样 */
#define BATT_AVG_N    4
static uint16_t s_avg_buf[BATT_AVG_N] = {0};
static uint8_t  s_avg_idx = 0;
static uint8_t  s_avg_filled = 0;

/* ============================================================
 *  ADC 初始化（外部单端通道，0dB，默认 8M/4M 采样时钟）
 * ============================================================ */
void Battery_ADC_Init(void)
{
    /* PA12 浮空输入，避免内部上下拉影响 ADC 测量 */
    GPIOA_ModeCfg(BATT_ADC_GPIO_PIN, GPIO_ModeIN_Floating);

    ADC_ExtSingleChSampInit(SampleFreq_8_or_4, ADC_PGA_0);
    s_calib_offset = ADC_DataCalib_Rough();

    ADC_ChannelCfg(BATT_ADC_CHANNEL);
    /* 第一次转换通常不准，丢弃 */
    (void)ADC_ExcutSingleConver();

    s_inited = 1;

    /* 启动时立即测一次，把缓存填上真实值 */
    (void)Battery_ReadPercent();
}

/* ============================================================
 *  单次 ADC 读取（带粗校准补偿）
 * ============================================================ */
uint16_t Battery_ReadRawAdc(void)
{
    if(!s_inited) return 0;
    /* 切换通道（多通道使用时安全） */
    ADC_ChannelCfg(BATT_ADC_CHANNEL);
    int v = (int)ADC_ExcutSingleConver() + s_calib_offset;
    if(v < 0) v = 0;
    if(v > 4095) v = 4095;
    return (uint16_t)v;
}

/* ============================================================
 *  返回电池实际电压（毫伏），已补偿 1/2 分压
 *
 *  ADC 0dB 量程上限 = 1.05 V × 2 = 2.1 V（参考 SDK 公式）
 *  ADC_VoltConverSignalPGA_0dB(adc) 返回 PIN 处毫伏值。
 *  电池电压 = 2 × PIN 电压。
 * ============================================================ */
uint16_t Battery_ReadVoltage_mV(void)
{
    uint16_t adc = Battery_ReadRawAdc();
    int pin_mv = ADC_VoltConverSignalPGA_0dB(adc);
    if(pin_mv < 0) pin_mv = 0;
    /* 1/2 分压补偿 -> 实际电池电压 */
    int batt_mv = pin_mv * 2;
    if(batt_mv > 4500) batt_mv = 4500;
    return (uint16_t)batt_mv;
}

/* 把毫伏换算成 0..100 % */
static uint8_t mv_to_percent(uint16_t mv)
{
    if(mv >= BATT_FULL_MV)  return 100;
    if(mv <= BATT_EMPTY_MV) return 0;
    /* 线性插值，足够精度 */
    uint32_t p = ((uint32_t)(mv - BATT_EMPTY_MV) * 100u) / (BATT_FULL_MV - BATT_EMPTY_MV);
    if(p > 100) p = 100;
    return (uint8_t)p;
}

uint8_t Battery_ReadPercent(void)
{
    uint16_t mv = Battery_ReadVoltage_mV();

    /* 滑动平均，平滑掉单次跳动 */
    s_avg_buf[s_avg_idx] = mv;
    s_avg_idx = (s_avg_idx + 1) % BATT_AVG_N;
    if(s_avg_idx == 0) s_avg_filled = 1;

    uint8_t n = s_avg_filled ? BATT_AVG_N : (s_avg_idx == 0 ? BATT_AVG_N : s_avg_idx);
    uint32_t sum = 0;
    for(uint8_t i = 0; i < n; i++) sum += s_avg_buf[i];
    uint16_t avg_mv = (uint16_t)(sum / n);

    s_cached_mv  = avg_mv;
    s_cached_pct = mv_to_percent(avg_mv);
    return s_cached_pct;
}

uint8_t Battery_GetCachedPercent(void)
{
    return s_cached_pct;
}

/* ============================================================
 *  主循环周期调用：节流到 ~每 500ms 一次
 *  保证不会因为 ADC 过于频繁影响主循环时序
 * ============================================================ */
void Battery_PeriodicMeasure(void)
{
    static uint16_t soft_tick = 0;
    soft_tick++;
    if(soft_tick < 1500) return;        /* 主循环约 1ms 一轮，约 1.5s 一次 */
    soft_tick = 0;

    (void)Battery_ReadPercent();
    s_last_tick++;
}
