/**
  ******************************************************************************
  * @file    app_usart.h
  * @brief   3 号板串口应用层接口，包含 CLI、2 号板加热协议和荧光模块状态。
  ******************************************************************************
  */
#ifndef __APP_USART_H__
#define __APP_USART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stddef.h>

/* 文本串口行缓冲最大长度。
 * 256U 里的 U 表示无符号常量，避免和 uint16_t/size_t 比较时出现有符号告警。
 */
#define APP_UART_LINE_MAX             256U

/* 2 号板版本字符串缓存长度，当前协议版本号很短，24 字节足够保存结尾 '\0'。 */
#define APP_HEATER_VERSION_MAX        24U

/* 2 号板最近一次响应正文缓存长度，用于 heater status 显示。 */
#define APP_HEATER_LAST_RESPONSE_MAX  96U

/* USART2 辅助口最近一行文本缓存长度。 */
#define APP_AUX_LAST_LINE_MAX         96U

/* 荧光模块最近一帧格式化文本缓存长度。 */
#define APP_FLUO_LAST_LINE_MAX        96U

/* 荧光模块固定 10 字节帧长度：帧头、地址、命令、数据、CRC。 */
#define APP_FLUO_FRAME_LEN            10U

typedef enum
{
    /* 普通人工串口文本输出。 */
    CLI_OUTPUT_TEXT = 0,
    /* 给上位机脚本读取的 JSON 输出。 */
    CLI_OUTPUT_JSON = 1
} CliOutputMode;

enum
{
    /* 没有故障。 */
    APP_FAULT_NONE                = 0U,
    /* 2 号板加热链路超时或 pending 超时。 */
    APP_FAULT_HEATER_LINK_TIMEOUT = (1UL << 0),
    /* 2 号板返回帧 CRC 错误。 */
    APP_FAULT_HEATER_RX_CRC       = (1UL << 1),
    /* 2 号板主动报告远端故障。 */
    APP_FAULT_HEATER_REMOTE       = (1UL << 2),
    /* AC 同步 ADC 采样重入，说明采样周期/耗时不匹配。 */
    APP_FAULT_SYNC_OVERRUN        = (1UL << 3),
    /* 加热链路未确认 TTL 电平，禁止危险加热命令。 */
    APP_FAULT_RS232_NOT_VERIFIED  = (1UL << 4),
    /* 联锁未闭合，禁止高压/加热/磁铁等危险动作。 */
    APP_FAULT_INTERLOCK_OPEN      = (1UL << 5)
};

typedef struct
{
    /* 2 号板固件版本。 */
    char version[APP_HEATER_VERSION_MAX];
    /* 三路 NTC 温度，单位摄氏度。 */
    float ntc_c[3];
    /* 三路加热电流，单位 A。 */
    float current_a[3];
    /* 三路功率，单位 W。 */
    float power_w[3];
    /* 三路加热占空比，单位百分比。 */
    uint8_t duty_pct[3];
    /* 最近收到 2 号板有效响应的时间戳。 */
    uint32_t last_rx_ms;
    /* 最近向 2 号板发送命令的时间戳。 */
    uint32_t last_tx_ms;
    /* CRC 错误累计计数。 */
    uint32_t crc_error_count;
    /* 行溢出、无 CRC、串口错误等传输错误累计计数。 */
    uint32_t transport_error_count;
    /* 2 号板是否在线。 */
    uint8_t online;
    /* 是否有命令等待 2 号板响应。 */
    uint8_t pending;
    /* 最近一次有效响应正文。 */
    char last_response[APP_HEATER_LAST_RESPONSE_MAX];
} HeaterLinkState;

typedef struct
{
    /* 危险动作授权窗口是否打开。 */
    uint8_t armed;
    /* 是否已人工确认 USART3 是 TTL 电平，而不是 RS232 电平。 */
    uint8_t heater_rs232_verified;
    /* arm 自动失效时间戳。 */
    uint32_t arm_deadline_ms;
    /* 当前故障位集合。 */
    uint32_t fault_flags;
    /* 最近一次故障上报时间戳。 */
    uint32_t last_fault_tick_ms;
    /* 最近一次 all_stop 原因。 */
    char last_stop_reason[48];
} AppControlState;

typedef struct
{
    /* 最近收到的一帧荧光模块原始数据。 */
    uint8_t last_rx[APP_FLUO_FRAME_LEN];
    /* 最近发出的一帧荧光模块原始数据。 */
    uint8_t last_tx[APP_FLUO_FRAME_LEN];
    /* 收帧计数。 */
    uint32_t rx_count;
    /* 发帧计数。 */
    uint32_t tx_count;
    /* 荧光帧 CRC 错误计数。 */
    uint32_t crc_error_count;
    /* 丢弃非 0x7A 帧头字节计数。 */
    uint32_t sync_drop_count;
    /* 最近收帧时间戳。 */
    uint32_t last_rx_ms;
    /* 最近发帧时间戳。 */
    uint32_t last_tx_ms;
    /* 最近一次荧光读数原始 RFU。 */
    uint32_t last_rfu;
    /* 最近一帧 CRC 是否有效。 */
    uint8_t last_frame_valid;
    /* 最近一次样本字段是否有效。 */
    uint8_t last_sample_valid;
    /* 最近一次 LED 状态返回值。 */
    uint8_t last_led_state;
    /* 最近一次模块地址返回值。 */
    uint8_t last_address;
} FluoLinkState;

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
extern UART_HandleTypeDef huart4;

extern HeaterLinkState g_heater_link;
extern AppControlState g_app_state;
extern FluoLinkState g_fluo_link;

void MX_USART1_UART_Init(void);
void MX_USART2_UART_Init(void);
void MX_USART3_UART_Init(void);
void MX_UART4_Init(void);

void App_Init(void);
void App_Service(void);
void App_AllStop(const char *reason);
void App_ReportFault(uint32_t fault_bits, const char *reason);
const char *App_FaultFlagsToString(uint32_t fault_flags, char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* __APP_USART_H__ */
