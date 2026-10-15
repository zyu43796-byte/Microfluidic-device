/**
  ******************************************************************************
  * @file    app_usart_internal.h
  * @brief   3 号板串口应用内部接口。
  *
  * 说明：
  * - 这个头文件只给 3 号板串口应用内部多个 .c 文件互相调用使用
  * - 不建议被外部模块直接包含
  ******************************************************************************
  */
#ifndef __APP_USART_INTERNAL_H__
#define __APP_USART_INTERNAL_H__

#include "app_usart.h"

typedef struct
{
    /* HAL 串口中断每次接收 1 字节时使用的缓存。 */
    uint8_t rx_byte;

    /* 当前正在拼接的一行文本命令或文本协议。 */
    char line[APP_UART_LINE_MAX];

    /* line 中已经写入的有效字符数，不包含字符串结尾 '\0'。 */
    uint16_t length;

    /* 收到换行后置 1，由主循环取走并调用对应解析函数。 */
    volatile uint8_t ready;

    /* 行长度超过 APP_UART_LINE_MAX 时置 1，用于提示命令被截断。 */
    volatile uint8_t overflow;
} UartLineBuffer;

typedef struct
{
    /* 荧光 UART 中断每次接收 1 字节时使用的缓存。 */
    uint8_t rx_byte;

    /* 荧光模块固定 10 字节二进制帧缓存。 */
    uint8_t frame[APP_FLUO_FRAME_LEN];

    /* 当前已收到的帧字节位置，达到 APP_FLUO_FRAME_LEN 后认为一帧完成。 */
    uint8_t index;

    /* 完整收到一帧后置 1，由主循环调用 App_ProcessFluoFrame() 解析。 */
    volatile uint8_t ready;
} FluoFrameBuffer;

/* 最近一次辅助串口文本响应，供 sys/调试输出回看。 */
extern char g_app_aux_last_line[APP_AUX_LAST_LINE_MAX];

/* 最近一次荧光串口文本响应，兼容早期文本测试路径。 */
extern char g_app_fluo_last_line[APP_FLUO_LAST_LINE_MAX];

/* 磁铁/推杆模块是否可用；当前硬件有 PC12 控制路径，所以初始化为可用。 */
extern uint8_t g_app_magnet_available;

/* 把 0/非 0 转成 no/yes，统一串口状态输出风格。 */
const char *App_BoolStr(uint8_t value);

/* 单字符忽略大小写比较，避免 CLI 命令区分大小写。 */
int App_CharIeq(char lhs, char rhs);

/* token 忽略大小写完全匹配，用于命令字判断。 */
uint8_t App_TokenEq(const char *lhs, const char *rhs);

/* token 忽略大小写前缀匹配，用于类似 v1*、bb* 的调试命令分组。 */
uint8_t App_TokenStartsWith(const char *token, const char *prefix);

/* 清空一行串口缓存，通常在处理完命令或发生溢出后调用。 */
void App_UartLineReset(UartLineBuffer *buffer);

/* 把中断收到的 1 字节推进文本行缓存，遇到换行置 ready。 */
void App_UartLinePush(UartLineBuffer *buffer, uint8_t byte);

/* 发送字符串到指定 UART，不自动补换行。 */
void App_UartSend(UART_HandleTypeDef *huart, const char *text);

/* 发送原始二进制字节，用于荧光帧或诊断原始数据。 */
void App_UartSendBytes(UART_HandleTypeDef *huart, const uint8_t *data, size_t length);

/* printf 风格输出到 CLI 主串口。 */
void App_CliPrintf(const char *fmt, ...);

/* 输出一整行 CLI 文本，内部会补 CRLF。 */
void App_CliWriteLine(const char *text);

/* 2 号板加热文本协议使用的 CRC8。 */
uint8_t App_Crc8(const uint8_t *data, size_t length);

/* 荧光 10 字节帧使用的 CRC16。 */
uint16_t App_FluoCrc16(const uint8_t *frame);

/* 按荧光模块协议组一帧 10 字节命令。 */
void App_FluoBuildFrame(uint8_t *frame,
                        uint8_t address,
                        uint8_t command,
                        uint8_t subcommand,
                        uint8_t data_l,
                        uint8_t data_lm,
                        uint8_t data_hm,
                        uint8_t data_h);

/* 危险动作门控：检查 arm、联锁、可选 RS232 验证，失败时返回原因。 */
uint8_t App_CanExecuteDangerous(uint8_t require_rs232_verify, const char **reason);

/* 直接向 2 号板发送一段协议正文。 */
void App_HeaterSendBody(const char *body);

/* printf 风格拼接并发送 2 号板加热命令。 */
void App_HeaterSendCommandf(const char *fmt, ...);

/* 解析 2 号板加热模块返回的一行文本。 */
void App_ProcessHeaterLine(UartLineBuffer *buffer);

/* 解析辅助串口返回的一行文本。 */
void App_ProcessAuxLine(UartLineBuffer *buffer);

/* 清空荧光二进制帧接收缓存。 */
void App_FluoFrameReset(FluoFrameBuffer *buffer);

/* 把荧光 UART 收到的 1 字节推进 10 字节帧缓存。 */
void App_FluoFramePush(FluoFrameBuffer *buffer, uint8_t byte);

/* 发送一帧荧光二进制命令。 */
void App_FluoSendFrame(const uint8_t *frame);

/* 解析荧光模块完整二进制响应帧。 */
void App_ProcessFluoFrame(FluoFrameBuffer *buffer);

/* CLI 主入口：解析 PC 串口输入的一行命令。 */
void App_ProcessCliLine(UartLineBuffer *buffer);

#endif /* __APP_USART_INTERNAL_H__ */

