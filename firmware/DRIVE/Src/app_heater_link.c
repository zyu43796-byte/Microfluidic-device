#include "app_usart_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 模块：3 号板到 2 号板加热控制链路
 *
 * 主要职责：
 * 1. 把 heater CLI 命令转换为 2 号板 ASCII 行协议：BODY*CRC\r\n。
 * 2. 校验 2 号板返回帧 CRC，并解析温度、电流、功率、duty 等 key=value 字段。
 * 3. 缓存最近一次状态，供 heater status 和 sys 输出。
 *
 * 修改建议：
 * - 改底层命令格式：看 App_HeaterSendBody()。
 * - 新增返回字段解析：看 app_heater_parse_value_token()。
 * - 改 CRC 或帧格式：同时检查 App_Crc8() 和 App_ProcessHeaterLine()。
 */

/* 向 2 号板发送一条 ASCII 协议正文。
 * body 不包含 CRC 和换行，本函数会自动封装成 BODY*CRC\r\n。
 */
void App_HeaterSendBody(const char *body)
{
    char frame[160];
    uint8_t crc = App_Crc8((const uint8_t *)body, strlen(body));

    (void)snprintf(frame, sizeof(frame), "%s*%02X\r\n", body, crc);
    HAL_UART_Transmit(&huart3, (uint8_t *)frame, (uint16_t)strlen(frame), 100U);
    g_heater_link.last_tx_ms = HAL_GetTick();
    g_heater_link.pending = 1U;
}

/* printf 风格的 heater 命令发送封装。 */
void App_HeaterSendCommandf(const char *fmt, ...)
{
    char body[120];
    va_list args;

    va_start(args, fmt);
    (void)vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    App_HeaterSendBody(body);
}

/* 解析 2 号板返回中的单个 key=value 字段。 */
static void app_heater_parse_value_token(const char *token)
{
    /* 2 号板当前返回格式采用 key=value 文本。
     * 这里统一负责把这些文本字段更新到 g_heater_link 缓存里；
     * CLI 的 heater status/sys 只读缓存，不在打印时重新解析。
     */
    if (App_TokenStartsWith(token, "version=") || App_TokenStartsWith(token, "ver="))
    {
        const char *value = strchr(token, '=');
        if (value != NULL)
        {
            value++;
            (void)snprintf(g_heater_link.version, sizeof(g_heater_link.version), "%s", value);
        }
        return;
    }

    /* 现场验证发现：2 号板返回的 NTC1/NTC2 与加热通道 1/2 的物理对应关系相反。
     * 为了让 CLI 显示的 ntc[1] 跟 heater set 1 对齐，这里只交换 NTC 的软件槽位；
     * cur/pwr/duty 已经和加热通道一致，不在这里对调。
     */
    if (App_TokenStartsWith(token, "ntc1=") || App_TokenStartsWith(token, "temp1="))
    {
        g_heater_link.ntc_c[1] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "ntc2=") || App_TokenStartsWith(token, "temp2="))
    {
        g_heater_link.ntc_c[0] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "ntc3=") || App_TokenStartsWith(token, "temp3="))
    {
        g_heater_link.ntc_c[2] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "cur1="))
    {
        g_heater_link.current_a[0] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "cur2="))
    {
        g_heater_link.current_a[1] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "cur3="))
    {
        g_heater_link.current_a[2] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "pwr1="))
    {
        g_heater_link.power_w[0] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "pwr2="))
    {
        g_heater_link.power_w[1] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "pwr3="))
    {
        g_heater_link.power_w[2] = (float)strtod(strchr(token, '=') + 1, NULL);
        return;
    }
    if (App_TokenStartsWith(token, "duty1="))
    {
        g_heater_link.duty_pct[0] = (uint8_t)strtoul(strchr(token, '=') + 1, NULL, 10);
        return;
    }
    if (App_TokenStartsWith(token, "duty2="))
    {
        g_heater_link.duty_pct[1] = (uint8_t)strtoul(strchr(token, '=') + 1, NULL, 10);
        return;
    }
    if (App_TokenStartsWith(token, "duty3="))
    {
        g_heater_link.duty_pct[2] = (uint8_t)strtoul(strchr(token, '=') + 1, NULL, 10);
        return;
    }
    if (App_TokenStartsWith(token, "faults="))
    {
        /* 2 号板主动上报 faults 非 0 时，3 号板只记录远端故障，不在这里直接急停。
         * 是否急停由上层 App_Service()/安全策略统一决定。
         */
        uint32_t remote_faults = (uint32_t)strtoul(strchr(token, '=') + 1, NULL, 0);
        if (remote_faults != 0U)
        {
            App_ReportFault(APP_FAULT_HEATER_REMOTE, token);
        }
    }
}

/* 解析已通过 CRC 校验的 2 号板正文。 */
static void app_process_heater_body(char *body)
{
    /* 2 号板返回格式约定：
     * OK VERB key=value ...
     * ERR VERB key=value ...
     * 如果没有 OK/ERR 前缀，也会尽量按 key=value 兼容解析，方便调试固件临时输出。
     */
    char *tokens[16];
    uint8_t count = 0U;
    uint8_t i;
    char *cursor = strtok(body, " ");

    while ((cursor != NULL) && (count < 16U))
    {
        tokens[count++] = cursor;
        cursor = strtok(NULL, " ");
    }

    if (count == 0U)
    {
        return;
    }

    g_heater_link.online = 1U;
    g_heater_link.pending = 0U;
    g_heater_link.last_rx_ms = HAL_GetTick();
    g_app_state.fault_flags &= ~APP_FAULT_HEATER_LINK_TIMEOUT;

    if (App_TokenEq(tokens[0], "ERR"))
    {
        App_ReportFault(APP_FAULT_HEATER_REMOTE, body);
        return;
    }

    if (App_TokenEq(tokens[0], "OK") && (count > 1U))
    {
        for (i = 1U; i < count; ++i)
        {
            if (strchr(tokens[i], '=') != NULL)
            {
                app_heater_parse_value_token(tokens[i]);
            }
        }
        return;
    }

    for (i = 0U; i < count; ++i)
    {
        app_heater_parse_value_token(tokens[i]);
    }
}

/* 处理 USART3 收到的一整行 2 号板响应。 */
void App_ProcessHeaterLine(UartLineBuffer *buffer)
{
    char body[APP_UART_LINE_MAX];
    char *separator;
    unsigned long parsed_crc;
    uint8_t expected_crc;

    if (buffer->overflow)
    {
        /* 行太长说明协议已经失步或对端输出异常，记传输错误后丢弃整行。 */
        g_heater_link.transport_error_count++;
        App_UartLineReset(buffer);
        return;
    }

    separator = strrchr(buffer->line, '*');
    if (separator == NULL)
    {
        /* 没有 '*' 就无法区分正文和 CRC，按传输错误处理。 */
        g_heater_link.transport_error_count++;
        App_UartLineReset(buffer);
        return;
    }

    *separator = '\0';
    expected_crc = App_Crc8((const uint8_t *)buffer->line, strlen(buffer->line));
    parsed_crc = strtoul(separator + 1, NULL, 16);

    if ((uint8_t)parsed_crc != expected_crc)
    {
        /* CRC 错误会记录故障，但不直接修改已有缓存，避免用坏帧污染上一次有效状态。 */
        g_heater_link.crc_error_count++;
        App_ReportFault(APP_FAULT_HEATER_RX_CRC, buffer->line);
        App_UartLineReset(buffer);
        return;
    }

    (void)snprintf(g_heater_link.last_response, sizeof(g_heater_link.last_response), "%s", buffer->line);
    (void)snprintf(body, sizeof(body), "%s", buffer->line);
    app_process_heater_body(body);
    App_UartLineReset(buffer);
}

/* 处理 USART2 辅助/显示口收到的一整行文本。 */
void App_ProcessAuxLine(UartLineBuffer *buffer)
{
    (void)snprintf(g_app_aux_last_line, APP_AUX_LAST_LINE_MAX, "%s", buffer->line);
    App_UartLineReset(buffer);
}

/* 把荧光模块 10 字节帧格式化成人类可读的十六进制字符串。 */
static void app_fluo_format_frame(char *buffer, size_t buffer_size, const uint8_t *frame)
{
    size_t used = 0U;
    uint8_t i;

    if ((buffer == NULL) || (buffer_size == 0U) || (frame == NULL))
    {
        return;
    }

    buffer[0] = '\0';
    for (i = 0U; i < APP_FLUO_FRAME_LEN; ++i)
    {
        if (used >= (buffer_size - 1U))
        {
            break;
        }
        used += (size_t)snprintf(buffer + used,
                                 buffer_size - used,
                                 "%s%02X",
                                 (i == 0U) ? "" : " ",
                                 frame[i]);
        if (used >= buffer_size)
        {
            break;
        }
    }
}

/* 清空荧光模块帧接收缓冲。 */
void App_FluoFrameReset(FluoFrameBuffer *buffer)
{
    buffer->index = 0U;
    buffer->ready = 0U;
}

/* 荧光模块逐字节接收状态机。
 * 等到收满 APP_FLUO_FRAME_LEN 后置 ready，由 App_Service() 调用解析。
 */
void App_FluoFramePush(FluoFrameBuffer *buffer, uint8_t byte)
{
    if (buffer->ready)
    {
        return;
    }

    if (buffer->index == 0U)
    {
        /* 荧光模块帧头固定 0x7A；不是帧头就丢弃，直到重新同步。 */
        if (byte != 0x7AU)
        {
            g_fluo_link.sync_drop_count++;
            return;
        }
    }

    buffer->frame[buffer->index++] = byte;
    if (buffer->index >= APP_FLUO_FRAME_LEN)
    {
        buffer->index = 0U;
        buffer->ready = 1U;
    }
}

/* 向荧光模块发送一帧原始 10 字节数据。 */
void App_FluoSendFrame(const uint8_t *frame)
{
    if (frame == NULL)
    {
        return;
    }

    App_UartSendBytes(&huart4, frame, APP_FLUO_FRAME_LEN);
    memcpy(g_fluo_link.last_tx, frame, APP_FLUO_FRAME_LEN);
    g_fluo_link.last_tx_ms = HAL_GetTick();
    g_fluo_link.tx_count++;
}

/* 解析荧光模块收到的一帧数据，并更新 g_fluo_link 缓存。 */
void App_ProcessFluoFrame(FluoFrameBuffer *buffer)
{
    uint16_t expected_crc;
    uint16_t parsed_crc;
    uint8_t *frame = buffer->frame;

    if (frame[0] != 0x7AU)
    {
        g_fluo_link.sync_drop_count++;
        App_FluoFrameReset(buffer);
        return;
    }

    expected_crc = App_FluoCrc16(frame);
    parsed_crc = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8U);

    /* 不管 CRC 是否正确，都保存最近一帧，方便现场看原始返回。 */
    memcpy(g_fluo_link.last_rx, frame, APP_FLUO_FRAME_LEN);
    app_fluo_format_frame(g_app_fluo_last_line, APP_FLUO_LAST_LINE_MAX, frame);
    g_fluo_link.last_rx_ms = HAL_GetTick();
    g_fluo_link.rx_count++;
    g_fluo_link.last_frame_valid = (expected_crc == parsed_crc) ? 1U : 0U;

    if (expected_crc != parsed_crc)
    {
        g_fluo_link.crc_error_count++;
        App_FluoFrameReset(buffer);
        return;
    }

    if ((frame[2] == 0x19U) || (frame[2] == 0x1AU))
    {
        /* 0x19/0x1A 是读数相关返回，data_l/lm/hm 拼成 RFU/sample 原始值。 */
        g_fluo_link.last_sample_valid = (frame[7] == 0U) ? 1U : 0U;
        g_fluo_link.last_rfu = ((uint32_t)frame[6] << 16U) |
                               ((uint32_t)frame[5] << 8U) |
                               (uint32_t)frame[4];
    }

    if (frame[2] == 0x20U)
    {
        /* 0x20 返回 LED 状态。 */
        g_fluo_link.last_led_state = frame[4];
    }

    if (frame[2] == 0x1BU)
    {
        /* 0x1B 返回模块地址。 */
        g_fluo_link.last_address = frame[4];
    }

    App_FluoFrameReset(buffer);
}

