#ifndef __BOTTOM_H__
#define __BOTTOM_H__

#include "main.h"
#include "adc.h"

typedef struct
{
    /* ADC 原始值，12bit，范围约 0..4095。 */
    uint16_t raw;

    /* MCU ADC 引脚电压，单位 V；不是换算后的真实高压。 */
    float volts;

    /* 最近一次采样的 HAL_GetTick() 时间戳。 */
    uint32_t tick_ms;

    /* 累计采样次数，用于判断 ADC 是否仍在刷新。 */
    uint32_t sample_count;
} BottomAdcSample;

typedef struct
{
    /* 芯片/阵列压紧检测，低有效；1 表示有效/到位。 */
    uint8_t biochip_check_active;

    /* 2 号板位置开关联锁，低有效；1 表示有效/到位。 */
    uint8_t pos_sw1_active;

    /* 当前硬件上与 biochip_check 合并，保留字段只为兼容旧接口。 */
    uint8_t bioarray_check_active;

    /* 最近一次输入扫描时间戳。 */
    uint32_t tick_ms;
} BottomInputState;

extern BottomAdcSample g_array_feedback;
extern BottomAdcSample g_voltage_feedback;
extern BottomInputState g_bottom_inputs;

void Bottom_Init(void);

/* 主循环调用：定期刷新联锁输入，不做耗时 ADC 采样。 */
void Bottom_Service(void);

/* 立即阻塞采样 ADC0/ADC2，CLI 的 adc read 会走这里。 */
void Bottom_RequestImmediateSampling(void);

/* AC 模式下允许 TIM3 周期触发同步采样。 */
void Bottom_StartSynchronizedSampling(void);
void Bottom_StopSynchronizedSampling(void);

/* TIM3 周期中断入口；同步采样忙时会记录 overrun。 */
void Bottom_OnSyncTick(void);

uint8_t Bottom_IsSynchronizedSamplingActive(void);

/* 危险动作联锁总判断；临时屏蔽芯片压实时仍要求 pos_sw1 到位。 */
uint8_t Bottom_IsInterlockClosed(void);
uint32_t Bottom_GetSyncOverrunCount(void);
const BottomAdcSample *Bottom_GetArrayFeedback(void);
const BottomAdcSample *Bottom_GetVoltageFeedback(void);
const BottomInputState *Bottom_GetInputState(void);

uint16_t Array_Check(void);
uint16_t Voltage_Check(void);
uint8_t Chip_Check(void);

#endif
