#ifndef __HV507_API_H__
#define __HV507_API_H__

#include "main.h"

/* HV507 控制脚定义。
 * 这些宏只描述管脚，不表达电气真值；真值表逻辑在 app_hv507.c 里统一处理。
 */
#define BL GPIO_PIN_13       /* PE13：HV507 /BL 消隐控制。 */
#define LE GPIO_PIN_14       /* PE14：HV507 LE 锁存控制。 */
#define AC GPIO_PIN_15       /* PA15：AC 波形输入，当前硬件确认是 PA15。 */

/* MCP41050 数字电位器片选，用于高压设定。 */
#define POT1_CS_Pin GPIO_PIN_2
#define POT1_CS_GPIO_Port GPIOD

#define POL GPIO_PIN_11      /* PE11：POL 极性控制，当前硬件确认是 PE11。 */
#define PORTA GPIOA          /* 简化 PA 口写法，只在 HV507 模块使用。 */
#define PORTE GPIOE          /* 简化 PE 口写法，只在 HV507 模块使用。 */
#define SET GPIO_PIN_SET     /* 兼容旧代码的高电平别名。 */
#define RESET GPIO_PIN_RESET /* 兼容旧代码的低电平别名。 */

/* 高压输出模式：
 * OFF：关闭 HV507 输出控制脚，作为默认安全态。
 * DC ：阵列按固定极性输出，当前已通过主要阵列测试。
 * AC ：阵列/脉冲高压相关控制进入翻转模式；当前 1 号板 U7 发热，仍需谨慎验证。
 */
typedef enum
{
    HV507_MODE_OFF = 0,
    HV507_MODE_DC = 1,
    HV507_MODE_AC = 2
} HV507_OutputMode;

typedef struct
{
    /* 当前高压输出模式，对应 hv mode off|dc|ac。 */
    HV507_OutputMode mode;

    /* 阵列逻辑缓存是否需要下发。上层改 logic[] 后置 1，由 HV507_Service() 提交。 */
    uint8_t logic_dirty;

    /* 目标高压值，单位 V。HV507_SetVoltage() 会限幅到当前允许范围。 */
    uint16_t setpoint_volts;

    /* AC 翻转频率，单位 Hz。当前默认 1000Hz，不建议再用 10Hz 长时间测试。 */
    uint16_t ac_frequency_hz;

    /* HV507 的 16 字节本地逻辑缓存；array read 看到的是这里，不等于真实高压输出。 */
    uint8_t logic[16];

    /* 最近一次真正发到 SPI1/HV507 的 16 字节数据，便于排查位序/极性问题。 */
    uint8_t last_tx[16];

    /* GPIO AC 回退路径使用的当前电平状态。PWM 路径下仅作状态记录。 */
    uint8_t ac_level_high;

    /* GPIO POL 回退路径使用的当前电平状态。当前硬件 POL=PE11。 */
    uint8_t pol_level_high;

    /* MCP41050 数字电位器最近一次写入的 wiper 值，用于 hv read 状态回读。 */
    uint8_t last_wiper_value;

    /* 调试位序配置：是否反转 16 字节发送顺序。用于适配 HV507 级联方向。 */
    uint8_t shift_byte_reverse;

    /* 调试位序配置：是否把每个字节 bit 反转后发送。 */
    uint8_t shift_msb_first;

    /* 调试极性配置：是否对 logic[] 全部取反后发送。 */
    uint8_t logic_invert;

    /* DC 模式下 POL 空闲/输出极性。1 表示 PE11 输出高。 */
    uint8_t dc_pol_high;

    /* AC 驱动路径选择：1=TIM1/TIM2 硬件 PWM，0=TIM3 中断 GPIO 翻转。 */
    uint8_t ac_drive_pwm;

    /* POL 相对 AC 的延迟，单位 us。
     * 硬件要求 AC/POL 同为 1kHz，但 POL 需要比 AC 慢一段相位；这里把相位差显式化，便于示波器验证。
     */
    uint16_t ac_phase_delay_us;
} HV507_Typedef;

extern HV507_Typedef hv507;

void HV507_Init(HV507_Typedef *hv);

/* 主循环中调用：如果 logic_dirty=1，则统一下发并锁存 HV507 阵列数据。 */
void HV507_Service(HV507_Typedef *hv);

/* 设置目标高压值并写 MCP41050。参数单位 V，函数内部会限幅。 */
void HV507_SetVoltage(HV507_Typedef *hv, uint16_t volts);


/* 切换 OFF/DC/AC 输出模式。AC 当前有 U7 发热风险，测试需短时。 */
void HV507_SetMode(HV507_Typedef *hv, HV507_OutputMode mode);

/* 设置 AC 频率。会同时更新 TIM3 同步采样节拍和 PWM 参数。 */
void HV507_SetAcFrequency(HV507_Typedef *hv, uint16_t hz);

/* 设置 POL 相对 AC 的延迟，单位 us。 */
void HV507_SetAcPhaseDelayUs(HV507_Typedef *hv, uint16_t delay_us);

/* 选择 AC 驱动路径：enable=1 使用 PWM，enable=0 使用 GPIO 翻转。 */
void HV507_SetAcDrivePwm(HV507_Typedef *hv, uint8_t enable);

/* 写 16 字节阵列缓存，不立即直接打引脚；由 Service 统一提交。 */
void HV507_SetLogicBytes(HV507_Typedef *hv, const uint8_t *logic, uint8_t length);
void HV507_ClearLogic(HV507_Typedef *hv);
void HV507_EnterSafeState(HV507_Typedef *hv);

/* TIM3 周期中断入口：GPIO AC 回退模式下翻转 AC/POL，PWM 模式下只保留同步采样节拍。 */
void HV507_OnTimerTick(HV507_Typedef *hv);

const char *HV507_ModeName(HV507_OutputMode mode);

#endif
