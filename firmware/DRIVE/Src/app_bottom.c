#include "bottom.h"

/*
 * 模块：底板输入、联锁与 ADC 反馈
 *
 * 主要职责：
 * 1. 周期刷新 biochip_check、pos_sw1 等联锁输入。
 * 2. 采样 ADC0/ADC2，并缓存原始 ADC 值与 MCU 引脚电压。
 * 3. 在 AC 模式下由 TIM3 触发同步采样，用于观察阵列/高压反馈。
 *
 * 参数语义：
 * - raw：结构体字段名，表示 ADC 原始值，不是高压真实值。
 * - volts：MCU ADC 引脚电压，不是经过比例换算后的实际高压。
 * - 有效标志：低有效输入被拉低后置 1。
 *
 * 修改建议：
 * - 改联锁逻辑：看 Bottom_IsInterlockClosed()。
 * - 改输入扫描周期：看 Bottom_Service() 中的 10ms。
 * - 改 ADC 采样策略：看 bottom_read_adc_blocking() 和 Bottom_OnSyncTick()。
 */

/* ADC0 阵列反馈缓存。 */
BottomAdcSample g_array_feedback;
/* ADC2 高压反馈缓存。 */
BottomAdcSample g_voltage_feedback;
/* 底板联锁输入缓存。 */
BottomInputState g_bottom_inputs;

/* 是否启用 TIM3 触发的同步采样。 */
static uint8_t s_sync_sampling_enabled = 0U;
/* 同步采样忙标志，用于检测采样重入。 */
static uint8_t s_sync_sampling_busy = 0U;
/* 同步采样重入计数，非 0 增长说明采样耗时已经超过节拍。 */
static uint32_t s_sync_overrun_count = 0U;
/* 最近一次输入扫描时间，控制 10ms 扫描节奏。 */
static uint32_t s_last_input_scan_ms = 0U;

/* 阻塞读取指定 ADC 一次。 */
static uint16_t bottom_read_adc_blocking(ADC_HandleTypeDef *hadc)
{
    /* 当前 ADC 读取策略是阻塞式单次转换。
     * 这样实现简单，便于先把板级联调跑通。
     */
    uint16_t value = 0U;

    HAL_ADC_Start(hadc);
    if (HAL_ADC_PollForConversion(hadc, 1U) == HAL_OK)
    {
        value = (uint16_t)HAL_ADC_GetValue(hadc);
    }
    HAL_ADC_Stop(hadc);

    return value;
}

/* 刷新所有底板联锁输入。 */
static void bottom_refresh_inputs(void)
{
    /* 三路输入按低有效处理：
     * - biochip_check
     * - pos_sw1
     * - bioarray_check 在 1 号板上通过 R41(0R) 和 biochip_check 绑定。
     */
    g_bottom_inputs.biochip_check_active = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_15) == GPIO_PIN_RESET) ? 1U : 0U;
    g_bottom_inputs.pos_sw1_active = (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_0) == GPIO_PIN_RESET) ? 1U : 0U;
    g_bottom_inputs.bioarray_check_active = g_bottom_inputs.biochip_check_active;
    g_bottom_inputs.tick_ms = HAL_GetTick();
}

/* 用最新 ADC 原始值更新缓存结构。 */
static void bottom_update_sample(BottomAdcSample *sample, uint16_t raw)
{
    /* 当前 volts 只是 ADC 引脚电压，不是已经换算后的实际阵列/高压值。 */
    sample->raw = raw;
    sample->volts = ((float)raw * 3.3f) / 4095.0f;
    sample->tick_ms = HAL_GetTick();
    sample->sample_count++;
}

/* 初始化底板输入和 ADC 缓存。 */
void Bottom_Init(void)
{
    /* 初始化时先清空状态，再做一次立即采样，避免上电后状态全是旧值。 */
    g_array_feedback.raw = 0U;
    g_array_feedback.volts = 0.0f;
    g_array_feedback.tick_ms = 0U;
    g_array_feedback.sample_count = 0U;

    g_voltage_feedback.raw = 0U;
    g_voltage_feedback.volts = 0.0f;
    g_voltage_feedback.tick_ms = 0U;
    g_voltage_feedback.sample_count = 0U;

    s_sync_sampling_enabled = 0U;
    s_sync_sampling_busy = 0U;
    s_sync_overrun_count = 0U;
    s_last_input_scan_ms = 0U;

    bottom_refresh_inputs();
    Bottom_RequestImmediateSampling();
}

/* 底板周期服务：当前只做 10ms 输入扫描。 */
void Bottom_Service(void)
{
    uint32_t now = HAL_GetTick();

    /* 输入扫描当前按 10ms 一次刷新。 */
    if ((now - s_last_input_scan_ms) >= 10U)
    {
        bottom_refresh_inputs();
        s_last_input_scan_ms = now;
    }
}

/* 立即采样 ADC0/ADC2。CLI 的 adc read 会走这里。 */
void Bottom_RequestImmediateSampling(void)
{
    /* 非同步模式下，允许上层主动发起一次立即采样。 */
    bottom_update_sample(&g_array_feedback, bottom_read_adc_blocking(&hadc1));
    bottom_update_sample(&g_voltage_feedback, bottom_read_adc_blocking(&hadc2));
}

/* 允许 TIM3 周期触发同步采样。 */
void Bottom_StartSynchronizedSampling(void)
{
    /* 进入 AC 模式后，由 HV507 控制层启用同步采样。 */
    s_sync_sampling_enabled = 1U;
}

/* 停止 TIM3 同步采样，并清掉忙标志。 */
void Bottom_StopSynchronizedSampling(void)
{
    s_sync_sampling_enabled = 0U;
    s_sync_sampling_busy = 0U;
}

/* TIM3 同步采样入口。 */
void Bottom_OnSyncTick(void)
{
    /* 同步采样由 TIM3 节拍触发。
     * 如果上一次采样还没结束，这里会记 overrun。
     */
    if (!s_sync_sampling_enabled)
    {
        return;
    }

    if (s_sync_sampling_busy)
    {
        s_sync_overrun_count++;
        return;
    }

    s_sync_sampling_busy = 1U;
    bottom_update_sample(&g_array_feedback, bottom_read_adc_blocking(&hadc1));
    bottom_update_sample(&g_voltage_feedback, bottom_read_adc_blocking(&hadc2));
    s_sync_sampling_busy = 0U;
}

/* 返回同步采样是否启用。 */
uint8_t Bottom_IsSynchronizedSamplingActive(void)
{
    return s_sync_sampling_enabled;
}

/* 返回危险动作联锁是否闭合。 */
uint8_t Bottom_IsInterlockClosed(void)
{
    /* Always require both chip compression and the position switch. */
    return (g_bottom_inputs.biochip_check_active && g_bottom_inputs.pos_sw1_active) ? 1U : 0U;
}

/* 返回同步采样 overrun 计数。 */
uint32_t Bottom_GetSyncOverrunCount(void)
{
    return s_sync_overrun_count;
}

/* 读取阵列反馈缓存指针。 */
const BottomAdcSample *Bottom_GetArrayFeedback(void)
{
    return &g_array_feedback;
}

/* 读取高压反馈缓存指针。 */
const BottomAdcSample *Bottom_GetVoltageFeedback(void)
{
    return &g_voltage_feedback;
}

/* 读取联锁输入缓存指针。 */
const BottomInputState *Bottom_GetInputState(void)
{
    return &g_bottom_inputs;
}

/* 旧接口兼容：读取阵列 ADC 原始值。 */
uint16_t Array_Check(void)
{
    /* 旧接口兼容层：直接触发一次立即采样后返回 ADC 原始值。 */
    Bottom_RequestImmediateSampling();
    return g_array_feedback.raw;
}

/* 旧接口兼容：读取高压 ADC 原始值。 */
uint16_t Voltage_Check(void)
{
    Bottom_RequestImmediateSampling();
    return g_voltage_feedback.raw;
}

/* 旧接口兼容：读取芯片/位置联锁原始开路状态。 */
uint8_t Chip_Check(void)
{
    /* 兼容旧逻辑的原始返回方式，仍保留高电平表示“开路/未压实”的语义。 */
    uint8_t raw_chip = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_15) == GPIO_PIN_SET) ? 1U : 0U;
    uint8_t raw_pos = (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_0) == GPIO_PIN_SET) ? 1U : 0U;
    return (uint8_t)(raw_chip | raw_pos);
}
