#include "hv507_api.h"

#include "bottom.h"
#include "tim.h"
#include "spi.h"

#include <string.h>

/*
 * 模块：高压电源与 HV507 阵列控制
 *
 * 主要职责：
 * 1. 通过 SPI1 向 HV507 下发 16 字节阵列逻辑。
 * 2. 通过 SPI3 控制 MCP41050 数字电位器，设置高压目标值。
 * 3. 控制 BL/LE/POL/AC 等关键引脚，实现 OFF/DC/AC 模式切换。
 * 4. 提供大量 array 诊断命令的底层实现，便于排查位序、极性、SPI 时序。
 *
 * 关键引脚：
 * - AC  = PA15
 * - POL = PE11
 * - BL  = PE13
 * - LE  = PE14
 * - SPI1 PA5/PA7：HV507 CLK/DIO
 * - SPI3 PB3/PB5 + PD2：MCP41050
 *
 * 修改建议：
 * - 改电极编号映射：优先看 app_cli.c 里的 cli_array_id_to_bit()。
 * - HV507 位序/极性固定于 HV507_Init()，发送解释见 hv507_build_tx_buffer()。
 * - 改 AC 波形：优先看 hv507_configure_ac_pwm_timers()、hv507_start_ac_pwm()。
 * - 改电压换算：优先看 hv507_voltage_to_wiper()。
 *
 * 风险提醒：
 * AC 模式当前仍存在 1 号板 U7/AQY210EH 快速发热问题，详见《高压AC问题交接记录.md》。
 * 后续不要长时间直接执行 hv mode ac，抓波形优先使用 hv acburst <ms>。
 */

/* 全局高压控制对象。
 * 其他模块通过 extern hv507 读取/修改当前高压、阵列和 AC 状态。
 */
HV507_Typedef hv507;

/* HV507 使用 SPI1 下发阵列逻辑，这里记录当前 SPI 分频。
 * 默认 32 是早期联调中较稳妥的速度，过快可能让长线/隔离/芯片时序更难判断。
 * CLI 的 array spi 命令会改这个值，主要用于排查 SPI 时序问题。
 */
static uint16_t s_hv507_spi_divisor = 32U;

/* 记录 SPI1 时钟空闲电平。
 * 默认高电平是为了贴近早期成功版本；array clkpol 命令可切换，用于排查 HV507 采样边沿问题。
 */
static uint8_t s_hv507_spi_polarity_high = 1U;

/* AC 硬件 PWM 是否正在运行。
 * TIM1/TIM2 输出 AC/POL 时，TIM3 中断不再软件翻转引脚，需要靠这个标志跳过 GPIO 翻转。
 */
static uint8_t s_hv507_ac_pwm_running = 0U;

/* AC PWM 当前周期，单位是 1MHz 定时器计数。
 * 默认 1000 对应 1000Hz，主要给 hv507_start_ac_pwm() 计算 TIM1 初始相位用。
 */
static uint32_t s_hv507_ac_pwm_period = 1000UL;

/* 把 AC/POL 恢复为普通 GPIO 输出模式。
 * 使用场景：退出 AC、切回 DC/OFF、或从 PWM 路径退回软件 GPIO 翻转路径。
 */
static void hv507_prepare_ac_gpio_pins(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin = AC;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(PORTA, &gpio);

    gpio.Pin = POL;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(PORTE, &gpio);
}

/* 把 AC/POL 切到定时器复用功能。
 * PA15 输出 TIM2_CH1，PE11 输出 TIM1_CH2；这是当前硬件脚位，不能照搬旧版 PE12/TIM1_CH3N。
 */
static void hv507_prepare_ac_pwm_pins(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin = AC;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_TIM2;
    HAL_GPIO_Init(PORTA, &gpio);

    gpio.Pin = POL;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(PORTE, &gpio);
}

/* 按指定频率和相位差配置 AC/POL 的硬件 PWM。
 * hv->ac_frequency_hz：目标 AC 频率，已由上层限制在 10..10000Hz。
 * hv->ac_phase_delay_us：POL 相对 AC 的延迟，单位 us。
 * period：以 1MHz 定时器时基计算的周期计数。
 * pulse：50% 占空比。
 * pol_counter_start：为了让 POL 延迟 delay_us 后再进入高电平，TIM1 初值取 period-delay。
 */
static void hv507_configure_ac_pwm_timers(const HV507_Typedef *hv)
{
    uint32_t period = 1000000UL / (uint32_t)hv->ac_frequency_hz;
    uint32_t pulse;
    uint32_t delay_ticks = hv->ac_phase_delay_us;
    uint32_t pol_counter_start;

    if (period < 2UL)
    {
        period = 2UL;
    }
    pulse = period / 2UL;
    if (delay_ticks >= period)
    {
        delay_ticks = period - 1UL;
    }
    pol_counter_start = (delay_ticks == 0UL) ? 0UL : (period - delay_ticks);
    s_hv507_ac_pwm_period = period;

    __HAL_TIM_SET_AUTORELOAD(&htim2, period - 1UL);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, pulse);
    __HAL_TIM_SET_COUNTER(&htim2, 0U);
    HAL_TIM_GenerateEvent(&htim2, TIM_EVENTSOURCE_UPDATE);

    __HAL_TIM_SET_AUTORELOAD(&htim1, period - 1UL);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, pulse);
    __HAL_TIM_SET_COUNTER(&htim1, pol_counter_start);
    __HAL_TIM_SET_CAPTUREPOLARITY(&htim1, TIM_CHANNEL_2, TIM_OCPOLARITY_HIGH);
    HAL_TIM_GenerateEvent(&htim1, TIM_EVENTSOURCE_UPDATE);
}

/* 停止 AC 硬件 PWM，并把 AC/POL 引脚恢复成普通 GPIO。
 * 所有退出 AC、进入调试 bit-bang、写阵列前都会调用，避免定时器继续抢占引脚。
 */
static void hv507_stop_ac_pwm(void)
{
    HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_1);
    HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_2);
    htim1.Instance->SMCR &= ~(TIM_SMCR_SMS | TIM_SMCR_TS);
    s_hv507_ac_pwm_running = 0U;
    hv507_prepare_ac_gpio_pins();
}

/* 启动 AC 硬件 PWM。
 * 调用链：HV507_SetMode(AC) -> hv507_apply_output_idle() -> hv507_start_ac_pwm()。
 * 注意：当前 AC 仍会导致 1 号板 U7 发热，保留此路径仅用于短时诊断和示波器抓波形。
 */
static void hv507_start_ac_pwm(HV507_Typedef *hv)
{
    uint32_t delay_ticks;

    hv507_configure_ac_pwm_timers(hv);
    hv507_prepare_ac_pwm_pins();

    /*
     * 借鉴旧版“硬件定时器输出 AC/POL”的思路，但保持当前板卡脚位：
     * PA15 = TIM2_CH1，PE11 = TIM1_CH2。
     *
     * 旧版使用的是 TIM1_CH3N 互补输出，并通过 TIM1->CNT = 460 引入相位差；
     * 当前硬件 POL 已确认是 PE11，不能直接照搬 PE12/TIM1_CH3N，因此这里用
     * 现在改为更直观的“POL 同极性 PWM，但相对 AC 延迟 hv->ac_phase_delay_us us”。
     * 这样示波器上可以直接量 AC 上升沿到 POL 上升沿的时间差。
     *
     * 注意：1 号板 U7 发热问题尚未关闭，AC 只能短时诊断，不要长时间运行。
     */
    htim1.Instance->SMCR &= ~(TIM_SMCR_SMS | TIM_SMCR_TS);
    delay_ticks = hv->ac_phase_delay_us;
    if (delay_ticks >= s_hv507_ac_pwm_period)
    {
        delay_ticks = s_hv507_ac_pwm_period - 1UL;
    }
    __HAL_TIM_SET_COUNTER(&htim1,
                          (delay_ticks == 0U) ? 0U :
                          (s_hv507_ac_pwm_period - delay_ticks));
    __HAL_TIM_SET_COUNTER(&htim2, 0U);

    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
    s_hv507_ac_pwm_running = 1U;
}

/* 统一停止所有 AC 相关活动。
 * 同时停 TIM3 软件翻转和 TIM1/TIM2 硬件 PWM，避免两条路径互相叠加。
 */
static void hv507_stop_ac_activity(void)
{
    HAL_TIM_Base_Stop_IT(&htim3);
    hv507_stop_ac_pwm();
}

/* 通过 SPI1 逐字节发送 HV507 数据。
 * tx：已经按 byte_reverse/msb_first/invert 处理好的发送缓冲。
 * length：发送字节数，正常路径是 16，raw64 诊断路径是 8。
 */
static void hv507_shift_bytes(const uint8_t *tx, uint16_t length)
{
    uint16_t i;

    for (i = 0U; i < length; ++i)
    {
        /* V1 成功版本按字节发送到 SPI1。
         * 这里保持新版框架不变，但把下发方式对齐到旧成功路径，
         * 便于排除“一次发送 16 字节”和“逐字节发送”之间的时序差异。
         */
        HAL_SPI_Transmit(&hspi1, (uint8_t *)&tx[i], 1U, 10U);
    }
}


/* 把目标高压换算成 MCP41050 数字电位器 wiper。
 * volts：用户通过 hv set 输入的目标电压，先夹到 40..300V。
 * 返回值：0..255 的电位器档位。
 * 这里的公式来自当前硬件反馈/升压经验模型，后续如果实测电压偏差大，优先校准此函数。
 */
static uint8_t hv507_voltage_to_wiper(uint16_t volts)
{
    uint32_t numerator;
    uint32_t denominator;
    int32_t resistance;

    if (volts < 40U)
    {
        volts = 40U;
    }
    if (volts > 300U)
    {
        volts = 300U;
    }

    /* 经验公式分三步：
     * 1. 由目标电压反推所需等效电阻。
     * 2. 把等效电阻映射到 MCP41050 的 0..255 档位。
     * 3. 减去 3 个档位作为当前板卡实测偏移修正。
     */
    numerator = 1500UL * 1500UL;
    denominator = (uint32_t)volts;
    resistance = (int32_t)((numerator / denominator) - 6800UL);
    resistance = (resistance * 255) / 50000;
    resistance -= 3;

    if (resistance < 0)
    {
        resistance = 0;
    }
    if (resistance > 255)
    {
        resistance = 255;
    }

    return (uint8_t)resistance;
}

/* 根据 hv->mode 统一落地 AC/POL/BL/LE 与同步采样状态。
 * OFF/DC/AC 三种模式最终都从这里收口，避免多个函数各自改引脚造成残留状态。
 */
static void hv507_apply_output_idle(HV507_Typedef *hv)
{
    hv507_stop_ac_activity();

    if (hv->mode == HV507_MODE_AC)
    {
        /* AC 模式：BL 打开，启动同步采样；AC/POL 由 PWM 或 TIM3 GPIO 翻转产生。 */
        HAL_GPIO_WritePin(PORTE, BL, GPIO_PIN_SET);
        Bottom_StartSynchronizedSampling();
        if (hv->ac_drive_pwm)
        {
            hv507_start_ac_pwm(hv);
        }
        else
        {
            HAL_GPIO_WritePin(PORTA, AC, hv->ac_level_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
            HAL_GPIO_WritePin(PORTE, POL, hv->pol_level_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
        }
        __HAL_TIM_SET_COUNTER(&htim3, 0U);
        HAL_TIM_Base_Start_IT(&htim3);
    }
    else if (hv->mode == HV507_MODE_DC)
    {
        /* DC 模式：停止同步采样，AC 固定低，POL 使用已验证的 dc_pol_high。 */
        Bottom_StopSynchronizedSampling();
        HAL_GPIO_WritePin(PORTE, BL, GPIO_PIN_SET);
        HAL_GPIO_WritePin(PORTA, AC, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(PORTE, POL, hv->dc_pol_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(PORTE, LE, GPIO_PIN_RESET);
    }
    else
    {
        /* OFF 模式：关闭 BL，AC 拉低，POL 回到高电平安全默认值。 */
        Bottom_StopSynchronizedSampling();
        HAL_GPIO_WritePin(PORTE, BL, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(PORTA, AC, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(PORTE, POL, GPIO_PIN_SET);
    }
}

/* 进入 HV507 串行装载状态。
 * HV507 真值表要求移位阶段保持 LE=L、BL=H、POL=H。
 * 这里故意不跟随 DC/AC 输出极性，否则 POL=L 时会进入反相相关状态，导致写入和输出解释混在一起。
 */
static void hv507_enter_serial_load_state(HV507_Typedef *hv)
{
    (void)hv;

    HAL_GPIO_WritePin(PORTE, POL, GPIO_PIN_SET);
    HAL_GPIO_WritePin(PORTE, LE, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORTE, BL, GPIO_PIN_SET);
    return;

#if 0
    if (hv->mode == HV507_MODE_DC)
    {
        HAL_GPIO_WritePin(PORTE, POL, hv->dc_pol_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    else if (hv->mode == HV507_MODE_AC)
    {
        HAL_GPIO_WritePin(PORTE, POL, hv->pol_level_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    else
    {
        HAL_GPIO_WritePin(PORTE, POL, GPIO_PIN_SET);
    }
    HAL_GPIO_WritePin(PORTE, LE, GPIO_PIN_RESET);
    /* HV507 真值表要求串行装载阶段保持:
     *   /LE = L, /BL = H, /POL = H
     * 也就是进入“移位寄存器装载”状态，而不是落入
     * /BL = L 的“强制全开/强制全关”模式。
     */
    HAL_GPIO_WritePin(PORTE, BL, GPIO_PIN_SET);
}
#endif
}

/* 反转一个字节内的 bit 顺序。
 * array order 的 msb_first 选项会用到它，用于适配 HV507 串联方向或上位机位序定义。
 */
static uint8_t hv507_reverse8(uint8_t value)
{
    value = (uint8_t)(((value & 0x55U) << 1U) | ((value & 0xAAU) >> 1U));
    value = (uint8_t)(((value & 0x33U) << 2U) | ((value & 0xCCU) >> 2U));
    value = (uint8_t)(((value & 0x0FU) << 4U) | ((value & 0xF0U) >> 4U));
    return value;
}

/* 根据当前调试配置生成真正发给 HV507 的 16 字节。
 * 输入 hv->logic 是上层“逻辑视角”的阵列缓存；
 * 输出 tx 是考虑 byte_reverse、msb_first、invert 后的硬件发送数据。
 */
static void hv507_build_tx_buffer(const HV507_Typedef *hv, uint8_t *tx)
{
    uint8_t out_byte;
    uint8_t byte_index;
    uint8_t value;

    for (out_byte = 0U; out_byte < 16U; ++out_byte)
    {
        byte_index = hv->shift_byte_reverse ? (uint8_t)(15U - out_byte) : out_byte;
        value = hv->logic[byte_index];
        if (hv->logic_invert)
        {
            value = (uint8_t)~value;
        }
        if (hv->shift_msb_first)
        {
            value = hv507_reverse8(value);
        }
        tx[out_byte] = value;
    }
}

/* 把 hv->logic 真正提交到 HV507。
 * 调用链：HV507_Service() 发现 logic_dirty -> hv507_apply_logic()。
 * 这里会临时停止 AC，完成移位、锁存，再恢复当前 OFF/DC/AC 输出状态。
 */
static void hv507_apply_logic(HV507_Typedef *hv)
{
    uint8_t tx[16];

    hv507_stop_ac_activity();
    Bottom_StopSynchronizedSampling();

    hv507_build_tx_buffer(hv, tx);
    memcpy(hv->last_tx, tx, sizeof(hv->last_tx));

    hv507_enter_serial_load_state(hv);
    Delay_us(10U);
    hv507_shift_bytes(tx, 16U);
    Delay_us(10U);

    HAL_GPIO_WritePin(PORTE, LE, GPIO_PIN_SET);
    Delay_us(10U);
    HAL_GPIO_WritePin(PORTE, LE, GPIO_PIN_RESET);
    Delay_us(10U);
    HAL_GPIO_WritePin(PORTE, BL, GPIO_PIN_SET);
    Delay_us(10U);
    hv->logic_dirty = 0U;
    hv507_apply_output_idle(hv);
}

/* 把 hv->setpoint_volts 写入 MCP41050。
 * SPI3 命令 0x11 是写数字电位器通道的控制字；wiper 由 hv507_voltage_to_wiper() 换算。
 */
static void hv507_apply_voltage(HV507_Typedef *hv)
{
    uint8_t cmd = 0x11U;
    uint8_t wiper = hv507_voltage_to_wiper(hv->setpoint_volts);

    /* hspi3 对应数字电位器控制链路。 */
    hv->last_wiper_value = wiper;
    HAL_GPIO_WritePin(POT1_CS_GPIO_Port, POT1_CS_Pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi3, &cmd, 1U, 10U);
    HAL_SPI_Transmit(&hspi3, &wiper, 1U, 10U);
    HAL_GPIO_WritePin(POT1_CS_GPIO_Port, POT1_CS_Pin, GPIO_PIN_SET);
}

/* 初始化高压控制对象与硬件输出状态。
 * 默认值说明：
 * - setpoint=40V：最低允许设定，降低上电风险。
 * - ac_frequency=1000Hz：当前 AC 默认测试频率。
 * - ac_drive_pwm=1：默认使用硬件 PWM 路径，但 AC 仍需短时谨慎测试。
 * - dc_pol_high=1：DC 实测通过路径使用的 POL 默认状态。
 */
void HV507_Init(HV507_Typedef *hv)
{
    /* 初始化时默认进入最保守状态：
     * - 高压模式关闭
     * - 设定值回到最低允许电压
     * - 逻辑数组清空
     */
    memset(hv, 0, sizeof(*hv));
    hv->mode = HV507_MODE_OFF;
    hv->setpoint_volts = 40U;
    hv->ac_frequency_hz = 1000U;
    hv->ac_phase_delay_us = 40U;
    hv->ac_drive_pwm = 1U;
    hv->ac_level_high = 1U;
    hv->pol_level_high = 0U;
    hv->shift_byte_reverse = 0U;
    hv->shift_msb_first = 0U;
    hv->logic_invert = 0U;
    hv->dc_pol_high = 1U;

    HV507_ClearLogic(hv);
    HV507_SetAcFrequency(hv, hv->ac_frequency_hz);
    hv507_apply_voltage(hv);
    HV507_EnterSafeState(hv);
}

/* 高压模块主循环服务函数。
 * 当前只处理 logic_dirty 的延迟提交；这样 CLI 修改 logic 时不会在解析函数里直接阻塞太久。
 */
void HV507_Service(HV507_Typedef *hv)
{
    /* 当前采用“延迟提交”方式：
     * 上层改 logic 后只置 dirty 标志，真正的 SPI 下发放到主循环里做。
     */
    if (hv->logic_dirty)
    {
        hv507_apply_logic(hv);
    }
}

/* 设置目标高压。
 * volts 会被限制在 40..300V，然后立即写入 MCP41050。
 */
void HV507_SetVoltage(HV507_Typedef *hv, uint16_t volts)
{
    /* 当前允许的设定范围固定在 40V~300V。 */
    if (volts < 40U)
    {
        volts = 40U;
    }
    if (volts > 300U)
    {
        volts = 300U;
    }

    hv->setpoint_volts = volts;
    hv507_apply_voltage(hv);
}

/* 切换高压输出模式。
 * mode=OFF：关断输出；mode=DC：保持直流阵列输出；mode=AC：启动 AC/POL 翻转和同步采样。
 */
void HV507_SetMode(HV507_Typedef *hv, HV507_OutputMode mode)
{
    /* 每次切模式都把 AC/POL 状态重置到固定初值，避免从上一模式残留状态进入。 */
    hv->mode = mode;
    hv->ac_level_high = 1U;
    hv->pol_level_high = (mode == HV507_MODE_AC) ? 0U : 1U;
    if (mode == HV507_MODE_DC)
    {
        hv->dc_pol_high = 1U;
    }
    hv507_apply_output_idle(hv);
}

/* 设置 AC 频率，同时更新 TIM3 同步节拍和 TIM1/TIM2 PWM 周期。
 * hz 被限制在 10..10000Hz；过低会让高压脉冲链路导通时间过长，过高则不利于软件诊断。
 */
void HV507_SetAcFrequency(HV507_Typedef *hv, uint16_t hz)
{
    /* TIM3 在 AC 模式下承担两个职责：
     * 1. 翻转 AC/POL 输出
     * 2. 触发同步采样
     *
     * 所以改频率时，需要同步调整 ARR。
     */
    uint32_t timer_hz;
    uint32_t arr;
    uint8_t restart_needed = (hv->mode == HV507_MODE_AC) ? 1U : 0U;

    if (hz < 10U)
    {
        hz = 10U;
    }
    if (hz > 10000U)
    {
        hz = 10000U;
    }

    hv->ac_frequency_hz = hz;

    if (restart_needed)
    {
        hv507_stop_ac_activity();
    }

    timer_hz = 1000000UL;
    arr = timer_hz / ((uint32_t)hz * 2UL);
    if (arr < 2UL)
    {
        arr = 2UL;
    }

    __HAL_TIM_SET_AUTORELOAD(&htim3, (uint32_t)(arr - 1UL));
    __HAL_TIM_SET_COUNTER(&htim3, 0U);
    hv507_configure_ac_pwm_timers(hv);

    if (restart_needed)
    {
        hv507_apply_output_idle(hv);
    }
}

/* 设置 POL 相对 AC 的延迟。
 * delay_us 按当前 AC 周期限幅；1kHz 时周期为 1000us，所以允许 0..999us。
 */
void HV507_SetAcPhaseDelayUs(HV507_Typedef *hv, uint16_t delay_us)
{
    uint32_t period = 1000000UL / (uint32_t)hv->ac_frequency_hz;
    uint8_t restart_needed = (hv->mode == HV507_MODE_AC) ? 1U : 0U;

    if (period < 2UL)
    {
        period = 2UL;
    }
    if (delay_us >= period)
    {
        delay_us = (uint16_t)(period - 1UL);
    }

    hv->ac_phase_delay_us = delay_us;
    if (restart_needed)
    {
        hv507_apply_output_idle(hv);
    }
    else
    {
        hv507_configure_ac_pwm_timers(hv);
    }
}

/* 选择 AC 驱动路径。
 * enable=1：TIM1/TIM2 硬件 PWM；enable=0：TIM3 中断软件翻转 GPIO。
 */
void HV507_SetAcDrivePwm(HV507_Typedef *hv, uint8_t enable)
{
    hv->ac_drive_pwm = enable ? 1U : 0U;
    if (hv->mode == HV507_MODE_AC)
    {
        hv507_apply_output_idle(hv);
    }
}

/* 写入 16 字节阵列逻辑缓存。
 * 不会立即发送，只置 logic_dirty，等待 HV507_Service() 在主循环里提交。
 */
void HV507_SetLogicBytes(HV507_Typedef *hv, const uint8_t *logic, uint8_t length)
{
    uint8_t i;

    /* 数组固定为 16 字节，不足部分补 0。 */
    for (i = 0U; i < 16U; ++i)
    {
        hv->logic[i] = (i < length) ? logic[i] : 0U;
    }
    hv->logic_dirty = 1U;
}

/* 清空阵列逻辑缓存，相当于 array clear 的底层动作。 */
void HV507_ClearLogic(HV507_Typedef *hv)
{
    memset(hv->logic, 0, sizeof(hv->logic));
    hv->logic_dirty = 1U;
}

/* 进入高压安全态。
 * all_stop、故障、初始化都会调用，确保模式回到 OFF 且停止 AC 活动。
 */
void HV507_EnterSafeState(HV507_Typedef *hv)
{
    /* 安全态只做一件事：确保高压输出链路关闭。 */
    hv->mode = HV507_MODE_OFF;
    hv->ac_level_high = 0U;
    hv->pol_level_high = 0U;
    hv507_apply_output_idle(hv);
}

/* TIM3 周期中断回调里的 AC 软件翻转入口。
 * 只有 ac_drive_pwm=0 时真正翻转 PA15/PE11；硬件 PWM 路径下直接返回。
 */
void HV507_OnTimerTick(HV507_Typedef *hv)
{
    /* 只有 AC 模式下才允许定时翻转。 */
    if (hv->mode != HV507_MODE_AC)
    {
        return;
    }
    if (hv->ac_drive_pwm && s_hv507_ac_pwm_running)
    {
        return;
    }

    hv->ac_level_high = (uint8_t)!hv->ac_level_high;
    hv->pol_level_high = (uint8_t)!hv->ac_level_high;

    HAL_GPIO_WritePin(PORTA, AC, hv->ac_level_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PORTE, POL, hv->pol_level_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 把模式枚举转换成 CLI 打印用字符串。 */
const char *HV507_ModeName(HV507_OutputMode mode)
{
    switch (mode)
    {
        case HV507_MODE_DC:
            return "dc";
        case HV507_MODE_AC:
            return "ac";
        case HV507_MODE_OFF:
        default:
            return "off";
    }
}

