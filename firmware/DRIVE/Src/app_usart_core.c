#include "app_usart_internal.h"

#include "bottom.h"
#include "app_ledstrip.h"
#include "hv507_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * 模块：串口公共工具函数
 *
 * 主要职责：
 * 1. 提供大小写不敏感的命令比较、前缀判断、CRC8 等通用函数。
 * 2. 封装 CLI 输出，统一走 USART1。
 * 3. 提供危险动作门控判断，避免各个命令分支重复写安全检查。
 *
 * 修改建议：
 * - 改命令比较规则：看 App_TokenEq() / App_TokenStartsWith()。
 * - 改 CRC 算法：看 App_Crc8()，加热板协议会依赖它。
 * - 改危险动作允许条件：看 App_CanExecuteDangerous()。
 */

/* 把 0/1 转成 true/false 字符串，供 JSON/状态打印使用。 */
const char *App_BoolStr(uint8_t value)
{
    return value ? "true" : "false";
}

/* 单字符大小写不敏感比较。 */
int App_CharIeq(char lhs, char rhs)
{
    if ((lhs >= 'A') && (lhs <= 'Z'))
    {
        lhs = (char)(lhs - 'A' + 'a');
    }
    if ((rhs >= 'A') && (rhs <= 'Z'))
    {
        rhs = (char)(rhs - 'A' + 'a');
    }
    return lhs == rhs;
}

/* 大小写不敏感的整词比较。 */
uint8_t App_TokenEq(const char *lhs, const char *rhs)
{
    while ((*lhs != '\0') && (*rhs != '\0'))
    {
        if (!App_CharIeq(*lhs, *rhs))
        {
            return 0U;
        }
        ++lhs;
        ++rhs;
    }

    return ((*lhs == '\0') && (*rhs == '\0')) ? 1U : 0U;
}

/* 大小写不敏感的前缀比较。 */
uint8_t App_TokenStartsWith(const char *token, const char *prefix)
{
    while (*prefix != '\0')
    {
        if ((*token == '\0') || !App_CharIeq(*token, *prefix))
        {
            return 0U;
        }
        ++token;
        ++prefix;
    }

    return 1U;
}

/* 清空一行串口缓存。 */
void App_UartLineReset(UartLineBuffer *buffer)
{
    buffer->length = 0U;
    buffer->ready = 0U;
    buffer->overflow = 0U;
    buffer->line[0] = '\0';
}

/* 串口逐字节喂入行缓存。
 * 收到 \r 或 \n 时标记 ready，供主循环统一处理完整命令。
 */
void App_UartLinePush(UartLineBuffer *buffer, uint8_t byte)
{
    if ((byte == '\r') || (byte == '\n'))
    {
        if (buffer->length > 0U)
        {
            buffer->line[buffer->length] = '\0';
            buffer->ready = 1U;
        }
        return;
    }

    if (buffer->ready)
    {
        return;
    }

    if (buffer->length >= (APP_UART_LINE_MAX - 1U))
    {
        buffer->overflow = 1U;
        return;
    }

    buffer->line[buffer->length++] = (char)byte;
    buffer->line[buffer->length] = '\0';
}

/* 阻塞发送字符串。
 * 板级联调阶段优先要“日志稳定可见”，所以这里刻意不用中断/DMA。
 */
void App_UartSend(UART_HandleTypeDef *huart, const char *text)
{
    size_t remaining;
    const uint8_t *cursor;

    if (text == 0)
    {
        return;
    }

    remaining = strlen(text);
    cursor = (const uint8_t *)text;

    while (remaining > 0U)
    {
        uint16_t chunk = (remaining > 32U) ? 32U : (uint16_t)remaining;

        if (HAL_UART_Transmit(huart, (uint8_t *)cursor, chunk, 100U) != HAL_OK)
        {
            break;
        }

        cursor += chunk;
        remaining -= chunk;
    }

    return;
}

/* 阻塞发送原始字节数组。 */
void App_UartSendBytes(UART_HandleTypeDef *huart, const uint8_t *data, size_t length)
{
    size_t remaining;
    const uint8_t *cursor;

    if ((data == NULL) || (length == 0U))
    {
        return;
    }

    remaining = length;
    cursor = data;

    while (remaining > 0U)
    {
        uint16_t chunk = (remaining > 32U) ? 32U : (uint16_t)remaining;

        if (HAL_UART_Transmit(huart, (uint8_t *)cursor, chunk, 100U) != HAL_OK)
        {
            break;
        }

        cursor += chunk;
        remaining -= chunk;
    }
}

/* 统一从 USART1 打印 CLI 文本。 */
void App_CliPrintf(const char *fmt, ...)
{
    char buffer[256];
    va_list args;

    va_start(args, fmt);
    (void)vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    App_UartSend(&huart1, buffer);
}

/* 打印一行 CLI 文本并自动补 CRLF。 */
void App_CliWriteLine(const char *text)
{
    App_CliPrintf("%s\r\n", text);
}

/* 2 号板 ASCII 协议使用的 CRC8。 */
uint8_t App_Crc8(const uint8_t *data, size_t length)
{
    /* 与 2 号板当前 ASCII 协议保持一致：
     * poly=0x07, init=0x00, 输入为 '*' 之前的所有 ASCII 字节。
     */
    uint8_t crc = 0U;
    size_t i;
    uint8_t bit;

    for (i = 0U; i < length; ++i)
    {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((crc & 0x80U) != 0U)
            {
                crc = (uint8_t)((crc << 1U) ^ 0x07U);
            }
            else
            {
                crc <<= 1U;
            }
        }
    }

    return crc;
}

/* 荧光模块帧使用的 CRC16。
 * 只计算 frame[3]..frame[5] 三个协议字段，这是当前荧光模块手册/实测对应的算法。
 * 返回值额外 +1，保持和模块返回帧一致。
 */
uint16_t App_FluoCrc16(const uint8_t *frame)
{
    uint8_t i;
    uint8_t j;
    uint8_t b_tmp;
    uint16_t crc = 0xFFFFU;

    if (frame == NULL)
    {
        return 0U;
    }

    for (i = 1U; i < 4U; ++i)
    {
        b_tmp = frame[i + 2U];
        for (j = 0U; j < 8U; ++j)
        {
            if (((crc ^ b_tmp) & 0x0001U) != 0U)
            {
                crc >>= 1U;
                crc ^= 0x1021U;
            }
            else
            {
                crc >>= 1U;
            }
            b_tmp >>= 1U;
        }
    }

    return (uint16_t)(crc + 1U);
}

/* 构造荧光模块标准 10 字节帧。
 * 帧格式：0x7A + address + command + subcommand + 4 字节数据 + 2 字节 CRC。
 */
void App_FluoBuildFrame(uint8_t *frame,
                        uint8_t address,
                        uint8_t command,
                        uint8_t subcommand,
                        uint8_t data_l,
                        uint8_t data_lm,
                        uint8_t data_hm,
                        uint8_t data_h)
{
    uint16_t crc;

    if (frame == NULL)
    {
        return;
    }

    frame[0] = 0x7AU;
    frame[1] = address;
    frame[2] = command;
    frame[3] = subcommand;
    frame[4] = data_l;
    frame[5] = data_lm;
    frame[6] = data_hm;
    frame[7] = data_h;
    crc = App_FluoCrc16(frame);
    frame[8] = (uint8_t)(crc & 0xFFU);
    frame[9] = (uint8_t)((crc >> 8U) & 0xFFU);
}

/* 把 fault_bits 展成可读字符串。
 * sys 使用这段文本给人看；JSON 输出也沿用同一份 fault 字段，避免两套解释不一致。
 */
const char *App_FaultFlagsToString(uint32_t fault_flags, char *buffer, size_t buffer_size)
{
    size_t used = 0U;

    if (buffer_size == 0U)
    {
        return "";
    }

    buffer[0] = '\0';

    if (fault_flags == APP_FAULT_NONE)
    {
        (void)snprintf(buffer, buffer_size, "none");
        return buffer;
    }

    if ((fault_flags & APP_FAULT_HEATER_LINK_TIMEOUT) != 0U)
    {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%sheater_link_timeout", (used > 0U) ? "|" : "");
    }
    if ((fault_flags & APP_FAULT_HEATER_RX_CRC) != 0U)
    {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%sheater_crc", (used > 0U) ? "|" : "");
    }
    if ((fault_flags & APP_FAULT_HEATER_REMOTE) != 0U)
    {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%sheater_remote", (used > 0U) ? "|" : "");
    }
    if ((fault_flags & APP_FAULT_SYNC_OVERRUN) != 0U)
    {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%ssync_overrun", (used > 0U) ? "|" : "");
    }
    if ((fault_flags & APP_FAULT_RS232_NOT_VERIFIED) != 0U)
    {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%srs232_not_verified", (used > 0U) ? "|" : "");
    }
    if ((fault_flags & APP_FAULT_INTERLOCK_OPEN) != 0U)
    {
        (void)snprintf(buffer + used, buffer_size - used, "%sinterlock_open", (used > 0U) ? "|" : "");
    }

    return buffer;
}

/* 记录故障位并刷新最后故障时间。
 * reason 目前只保留给后续扩展日志系统；现阶段故障原因主要通过 fault_bits 表达。
 */
void App_ReportFault(uint32_t fault_bits, const char *reason)
{
    (void)reason;
    g_app_state.fault_flags |= fault_bits;
    g_app_state.last_fault_tick_ms = HAL_GetTick();
}

/* 全局急停出口。
 * 高压、阵列、磁铁和 2 号板链路都应该在这里统一回到安全态。
 */
void App_AllStop(const char *reason)
{
    /* all_stop 是系统统一的安全出口：
     * - 高压立即进入安全态
     * - 如果已确认板2链路是 TTL，则给板2发送 STOP_ALL
     * - arm 状态清零
     */
    HV507_EnterSafeState(&hv507);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);

    if (g_app_state.heater_rs232_verified)
    {
        App_HeaterSendBody("STOP_ALL");
    }

    g_app_state.armed = 0U;
    g_app_state.arm_deadline_ms = 0U;
    (void)snprintf(g_app_state.last_stop_reason, sizeof(g_app_state.last_stop_reason), "%s", reason);
    /* Stop decorative animation after the safety-critical outputs above. */
    App_LedStripEffectCancel();
    (void)App_LedStripClear(APP_LEDSTRIP_MAX_LEDS);
}

/* 危险动作门控。
 * 只有 arm 有效、联锁闭合、必要时 2 号板串口已验证为 TTL，才允许继续执行高压/加热相关动作。
 */
uint8_t App_CanExecuteDangerous(uint8_t require_rs232_verify, const char **reason)
{
    /* 所有危险动作都统一走这个门控函数，避免各个命令分支自己复制判断逻辑。 */
    if (!g_app_state.armed)
    {
        *reason = "system is not armed";
        return 0U;
    }

    if (!Bottom_IsInterlockClosed())
    {
        App_ReportFault(APP_FAULT_INTERLOCK_OPEN, "interlock open");
        *reason = "interlock is open";
        return 0U;
    }

    if (require_rs232_verify && !g_app_state.heater_rs232_verified)
    {
        App_ReportFault(APP_FAULT_RS232_NOT_VERIFIED, "heater link not verified as ttl");
        *reason = "verify TXD3/RXD3 as TTL before enabling heater link";
        return 0U;
    }

    *reason = "";
    return 1U;
}

