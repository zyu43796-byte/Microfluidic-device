#include "app_usart_internal.h"

#include "app_ledstrip.h"
#include "app_oled.h"
#include "bottom.h"
#include "hv507_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 模块：上位机 CLI 命令解析
 *
 * 主要职责：
 * 1. 把 USART1 收到的文本命令分发到 sys/gpio/adc/heater/hv/array/ledstrip 等模块。
 * 2. 对危险动作统一做 arm 和联锁检查，入口是 App_CanExecuteDangerous()。
 * 3. 打印人可读状态，同时支持部分 --json 输出，方便上位机脚本解析。
 *
 * 新增命令的位置：
 * - help 文本：cli_print_help()
 * - 命令解析：App_ProcessCliLine()
 * - 状态输出：cli_emit_xxx_status()
 *
 * 维护提醒：
 * 新增或修改命令后，请同步更新《项目使用手册.md》和《代码结构导航.html》。
 */

/* 打印 CLI 帮助文本。
 * 维护提醒：新增命令时必须同步改这里，否则现场测试人员不知道入口。
 */
static void cli_print_help(void)
{
    /* 当前帮助信息尽量和 BOARD3_TEST_INTERFACE.md 保持一致。 */
    App_CliWriteLine("Commands:");
    App_CliWriteLine("  sys [--json]");
    App_CliWriteLine("  sys arm on [sec]|arm off");
    App_CliWriteLine("  sys verify-rs232 ttl|rs232|clear");
    App_CliWriteLine("  sys clear-faults");
    App_CliWriteLine("  gpio read");
    App_CliWriteLine("  adc read");
    App_CliWriteLine("  heater ping|status|version|ntc <1..3>|cur <1..3>|pwr <1..3>|set <ch> <duty>|stop");
    App_CliWriteLine("  hv read|set <volts>|mode off|dc|ac|freq <hz>|phase <us>|acdrive pwm|gpio");
    App_CliWriteLine("  array read|clear|set <16 hex bytes>");
    App_CliWriteLine("  array all on|off");
    App_CliWriteLine("  array one|add|off <electrode 1..60 or 65..124>");
    App_CliWriteLine("  array pins");
    App_CliWriteLine("  ledstrip status|off [count]|fill <count> <r> <g> <b> [ms]");
    App_CliWriteLine("  ledstrip effect <1..6|rainbow|meteor|chase|stack|sparkle|collide> [count=30] [brightness] [interval_ms=30]");
    App_CliWriteLine("    default brightness: meteor=112, others=80; explicit brightness overrides demo too");
    App_CliWriteLine("  ledstrip demo [count] [brightness] [interval_ms]|effect stop (freeze); off stops and clears");
    App_CliWriteLine("  ledstrip color <count> <red|green|blue|white|yellow|cyan|magenta> <brightness 0..255> [ms]");
    App_CliWriteLine("  ledstrip one <count> <idx 1..count> <r> <g> <b> [ms]|pixel <count> <idx> <color> <brightness> [ms]");
    App_CliWriteLine("  ledstrip order [grb|brg|rgb|gbr|rbg|bgr]");
    App_CliWriteLine("  LED count=1..30 RGB=0..255; optional ms only accepts 0 (stay on); use off to clear");
    App_CliWriteLine("  magnet on|off");
    App_CliWriteLine("  oled status|idle|reset [ms]|pins <res 0|1> <dc 0|1> <cs 0|1>  (reserved display pins)");
    App_CliWriteLine("  aux status|send <text>     (display H6 USART2)");
    App_CliWriteLine("  fluo status|addr|raw <10 hex bytes>|led on|off|status|read|measure/simple <ms>");
    App_CliWriteLine("  fluo send <text>           (legacy text UART4)");
    App_CliWriteLine("  all_stop");
}

/* 输出系统总状态。
 * json=1 时给上位机脚本解析；json=0 时给人工串口调试阅读。
 */
static void cli_emit_sys_status(uint8_t json)
{
    /* sys 状态是当前系统总览接口，后续新增全局状态建议优先往这里加。 */
    char faults[96];
    const BottomInputState *inputs = Bottom_GetInputState();

    App_FaultFlagsToString(g_app_state.fault_flags, faults, sizeof(faults));

    if (json)
    {
        App_CliPrintf(
            "{\"ok\":true,\"cmd\":\"sys\",\"uptime_ms\":%lu,\"armed\":%s,"
            "\"rs232_verified\":%s,\"faults\":\"%s\",\"heater_online\":%s,"
            "\"hv_mode\":\"%s\",\"hv_setpoint_v\":%u,\"ac_freq_hz\":%u,"
            "\"biochip_check\":%s,\"pos_sw1\":%s,"
            "\"last_stop_reason\":\"%s\"}\r\n",
            (unsigned long)HAL_GetTick(),
            App_BoolStr(g_app_state.armed),
            App_BoolStr(g_app_state.heater_rs232_verified),
            faults,
            App_BoolStr(g_heater_link.online),
            HV507_ModeName(hv507.mode),
            hv507.setpoint_volts,
            hv507.ac_frequency_hz,
            App_BoolStr(inputs->biochip_check_active),
            App_BoolStr(inputs->pos_sw1_active),
            g_app_state.last_stop_reason);
    }
    else
    {
        App_CliPrintf("uptime=%lums armed=%s rs232_verified=%s\r\n",
                      (unsigned long)HAL_GetTick(),
                      g_app_state.armed ? "yes" : "no",
                      g_app_state.heater_rs232_verified ? "yes" : "no");
        App_CliPrintf("faults=%s heater_online=%s\r\n",
                      faults,
                      g_heater_link.online ? "yes" : "no");
        App_CliPrintf("hv_mode=%s hv_set=%uV ac=%uHz\r\n",
                      HV507_ModeName(hv507.mode),
                      hv507.setpoint_volts,
                      hv507.ac_frequency_hz);
        App_CliPrintf("chip=%s pos=%s last_stop=%s\r\n",
                      inputs->biochip_check_active ? "ok" : "open",
                      inputs->pos_sw1_active ? "ok" : "open",
                      g_app_state.last_stop_reason);
    }
}

/* 输出底板联锁 GPIO 状态。
 * 主要用于测试 biochip_check、pos_sw1 和 interlock 是否闭合。
 */
static void cli_emit_gpio_status(uint8_t json)
{
    const BottomInputState *inputs = Bottom_GetInputState();

    if (json)
    {
        App_CliPrintf(
            "{\"ok\":true,\"cmd\":\"gpio\",\"biochip_check\":%s,\"pos_sw1\":%s,\"interlock_closed\":%s,\"biochip_interlock_bypassed\":%s}\r\n",
            App_BoolStr(inputs->biochip_check_active),
            App_BoolStr(inputs->pos_sw1_active),
            App_BoolStr(Bottom_IsInterlockClosed()),
            App_BoolStr(0U));
    }
    else
    {
        App_CliPrintf(
            "biochip_check=%s pos_sw1=%s interlock_closed=%s biochip_interlock_bypassed=%s\r\n",
            inputs->biochip_check_active ? "active" : "inactive",
            inputs->pos_sw1_active ? "active" : "inactive",
            Bottom_IsInterlockClosed() ? "yes" : "no",
            "no");
    }
}

/* 输出 ADC0/ADC2 反馈状态。
 * 注意这里打印的是 MCU ADC 引脚电压，不是直接的高压真实值。
 */
static void cli_emit_adc_status(uint8_t json)
{
    char faults[96];

    if (!Bottom_IsSynchronizedSamplingActive())
    {
        Bottom_RequestImmediateSampling();
    }

    App_FaultFlagsToString(g_app_state.fault_flags, faults, sizeof(faults));

    if (json)
    {
        App_CliPrintf(
            "{\"ok\":true,\"cmd\":\"adc\",\"array_raw\":%u,\"array_v\":%.4f,"
            "\"voltage_raw\":%u,\"voltage_v\":%.4f,\"sync_active\":%s,"
            "\"sync_overruns\":%lu,\"faults\":\"%s\"}\r\n",
            Bottom_GetArrayFeedback()->raw,
            Bottom_GetArrayFeedback()->volts,
            Bottom_GetVoltageFeedback()->raw,
            Bottom_GetVoltageFeedback()->volts,
            App_BoolStr(Bottom_IsSynchronizedSamplingActive()),
            (unsigned long)Bottom_GetSyncOverrunCount(),
            faults);
    }
    else
    {
        App_CliPrintf(
            "array=%u(%.4fV) voltage=%u(%.4fV) sync=%s overruns=%lu faults=%s\r\n",
            Bottom_GetArrayFeedback()->raw,
            Bottom_GetArrayFeedback()->volts,
            Bottom_GetVoltageFeedback()->raw,
            Bottom_GetVoltageFeedback()->volts,
            Bottom_IsSynchronizedSamplingActive() ? "on" : "off",
            (unsigned long)Bottom_GetSyncOverrunCount(),
            faults);
    }
}

/* 输出 2 号板加热链路缓存状态。
 * 数据来自最近一次 2 号板返回，不会在此函数里主动发串口查询。
 */
static void cli_emit_heater_status(uint8_t json)
{
    /* heater status 读的是本地缓存。
     * 如果要尽量新鲜的数据，应先给板2发 READ_STATUS。
     */
    if (json)
    {
        App_CliPrintf(
            "{\"ok\":true,\"cmd\":\"heater\",\"online\":%s,\"version\":\"%s\","
            "\"ntc\":[%.2f,%.2f,%.2f],\"current\":[%.3f,%.3f,%.3f],"
            "\"power\":[%.3f,%.3f,%.3f],\"duty\":[%u,%u,%u],"
            "\"crc_errors\":%lu,\"transport_errors\":%lu,"
            "\"last_response\":\"%s\"}\r\n",
            App_BoolStr(g_heater_link.online),
            g_heater_link.version,
            g_heater_link.ntc_c[0], g_heater_link.ntc_c[1], g_heater_link.ntc_c[2],
            g_heater_link.current_a[0], g_heater_link.current_a[1], g_heater_link.current_a[2],
            g_heater_link.power_w[0], g_heater_link.power_w[1], g_heater_link.power_w[2],
            g_heater_link.duty_pct[0], g_heater_link.duty_pct[1], g_heater_link.duty_pct[2],
            (unsigned long)g_heater_link.crc_error_count,
            (unsigned long)g_heater_link.transport_error_count,
            g_heater_link.last_response);
    }
    else
    {
        App_CliPrintf(
            "heater online=%s ver=%s ntc=[%.2f %.2f %.2f] cur=[%.3f %.3f %.3f] pwr=[%.3f %.3f %.3f] duty=[%u %u %u] crc=%lu transport=%lu last=\"%s\"\r\n",
            g_heater_link.online ? "yes" : "no",
            g_heater_link.version,
            g_heater_link.ntc_c[0], g_heater_link.ntc_c[1], g_heater_link.ntc_c[2],
            g_heater_link.current_a[0], g_heater_link.current_a[1], g_heater_link.current_a[2],
            g_heater_link.power_w[0], g_heater_link.power_w[1], g_heater_link.power_w[2],
            g_heater_link.duty_pct[0], g_heater_link.duty_pct[1], g_heater_link.duty_pct[2],
            (unsigned long)g_heater_link.crc_error_count,
            (unsigned long)g_heater_link.transport_error_count,
            g_heater_link.last_response);
    }
}

/* 输出高压/HV507 当前状态。
 * 包含模式、电压设定、AC 频率、位序配置和 SPI 诊断配置。
 */
static void cli_emit_hv_status(uint8_t json)
{
    if (json)
    {
        App_CliPrintf(
            "{\"ok\":true,\"cmd\":\"hv\",\"mode\":\"%s\",\"setpoint_v\":%u,"
            "\"ac_freq_hz\":%u,\"ac_phase_us\":%u,\"ac_drive\":\"%s\",\"wiper\":%u,\"logic_dirty\":%s}\r\n",
            HV507_ModeName(hv507.mode),
            hv507.setpoint_volts,
            hv507.ac_frequency_hz,
            hv507.ac_phase_delay_us,
            hv507.ac_drive_pwm ? "pwm" : "gpio",
            hv507.last_wiper_value,
            App_BoolStr(hv507.logic_dirty));
    }
    else
    {
        App_CliPrintf(
            "hv mode=%s setpoint=%uV ac_freq=%uHz ac_phase=%uus ac_drive=%s wiper=%u logic_dirty=%s\r\n",
            HV507_ModeName(hv507.mode),
            hv507.setpoint_volts,
            hv507.ac_frequency_hz,
            hv507.ac_phase_delay_us,
            hv507.ac_drive_pwm ? "pwm" : "gpio",
            hv507.last_wiper_value,
            hv507.logic_dirty ? "yes" : "no");
    }
}

/* 输出 HV507 阵列逻辑缓存和最近一次 SPI 发送内容。 */
static void cli_emit_array_status(uint8_t json)
{
    if (json)
    {
        App_CliPrintf(
            "{\"ok\":true,\"cmd\":\"array\",\"logic\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]}\r\n",
            hv507.logic[0], hv507.logic[1], hv507.logic[2], hv507.logic[3],
            hv507.logic[4], hv507.logic[5], hv507.logic[6], hv507.logic[7],
            hv507.logic[8], hv507.logic[9], hv507.logic[10], hv507.logic[11],
            hv507.logic[12], hv507.logic[13], hv507.logic[14], hv507.logic[15]);
    }
    else
    {
        App_CliPrintf(
            "logic=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
            hv507.logic[0], hv507.logic[1], hv507.logic[2], hv507.logic[3],
            hv507.logic[4], hv507.logic[5], hv507.logic[6], hv507.logic[7],
            hv507.logic[8], hv507.logic[9], hv507.logic[10], hv507.logic[11],
            hv507.logic[12], hv507.logic[13], hv507.logic[14], hv507.logic[15]);
        App_CliPrintf(
            "tx=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
            hv507.last_tx[0], hv507.last_tx[1], hv507.last_tx[2], hv507.last_tx[3],
            hv507.last_tx[4], hv507.last_tx[5], hv507.last_tx[6], hv507.last_tx[7],
            hv507.last_tx[8], hv507.last_tx[9], hv507.last_tx[10], hv507.last_tx[11],
            hv507.last_tx[12], hv507.last_tx[13], hv507.last_tx[14], hv507.last_tx[15]);
    }
}

/* 输出 HV507 关键控制脚当前读数。
 * 用于确认 AC/BL/POL/LE 实际电平是否符合命令预期。
 */
static void cli_emit_array_pins(uint8_t json)
{
    uint8_t ac = (HAL_GPIO_ReadPin(PORTA, AC) == GPIO_PIN_SET) ? 1U : 0U;
    uint8_t bl = (HAL_GPIO_ReadPin(PORTE, BL) == GPIO_PIN_SET) ? 1U : 0U;
    uint8_t pol = (HAL_GPIO_ReadPin(PORTE, POL) == GPIO_PIN_SET) ? 1U : 0U;
    uint8_t le = (HAL_GPIO_ReadPin(PORTE, LE) == GPIO_PIN_SET) ? 1U : 0U;

    if (json)
    {
        App_CliPrintf(
            "{\"ok\":true,\"cmd\":\"array_pins\",\"ac\":%u,\"bl\":%u,\"pol\":%u,\"le\":%u,\"mode\":\"%s\","
            "\"byte_rev\":%u,\"msb\":%u,\"invert\":%u,\"dc_pol\":%u}\r\n",
            ac, bl, pol, le, HV507_ModeName(hv507.mode),
            hv507.shift_byte_reverse,
            hv507.shift_msb_first,
            hv507.logic_invert,
            hv507.dc_pol_high);
    }
    else
    {
        App_CliPrintf("array pins ac=%u bl=%u pol=%u le=%u mode=%s byte_rev=%u msb=%u invert=%u dc_pol=%u\r\n",
                      ac, bl, pol, le, HV507_ModeName(hv507.mode),
                      hv507.shift_byte_reverse,
                      hv507.shift_msb_first,
                      hv507.logic_invert,
                      hv507.dc_pol_high);
    }
}

/* 把“电极编号”转换成 logic[byte] + bit mask。
 * id：用户看到的真实电极编号，只接受 1..60 和 65..124。
 * byte_index/bit_mask：输出给 array one/add/off 使用。
 */
static uint8_t cli_array_id_to_bit(uint16_t id, uint8_t *byte_index, uint8_t *bit_mask)
{
    uint16_t zero_based;

    if ((byte_index == NULL) || (bit_mask == NULL))
    {
        return 0U;
    }

    /* 这里沿用旧 DMF_HOST 的 data_transfer() 编号：
     * 真实电极是 1..60 和 65..124。
     * 61..64、125..128 是 HV507 原始移位槽位，没有接到真实阵列电极。
     */
    if (!(((id >= 1U) && (id <= 60U)) || ((id >= 65U) && (id <= 124U))))
    {
        return 0U;
    }

    zero_based = (uint16_t)(id - 1U);
    *byte_index = (uint8_t)(zero_based / 8U);
    *bit_mask = (uint8_t)(1U << (zero_based % 8U));
    return 1U;
}

/* 把 0..127 的 HV507 原始 bit 位转换成 logic[byte] + bit mask。
 * array bit 命令使用它，适合绕过真实电极编号直接排查移位寄存器位序。
 */
static uint8_t cli_array_raw_to_bit(uint16_t raw, uint8_t *byte_index, uint8_t *bit_mask)
{
    if ((raw >= 128U) || (byte_index == NULL) || (bit_mask == NULL))
    {
        return 0U;
    }

    *byte_index = (uint8_t)(raw / 8U);
    *bit_mask = (uint8_t)(1U << (raw % 8U));
    return 1U;
}

/* 输出 USART2 辅助/显示口最近接收到的一行文本。 */
static void cli_emit_aux_status(uint8_t json)
{
    if (json)
    {
        App_CliPrintf("{\"ok\":true,\"cmd\":\"aux\",\"port\":\"display_h6\",\"last_line\":\"%s\"}\r\n", g_app_aux_last_line);
    }
    else
    {
        App_CliPrintf("aux port=display_h6 last_line=\"%s\"\r\n", g_app_aux_last_line);
    }
}

/* 输出荧光模块 UART4 通信状态和最近一帧解析结果。 */
static void cli_emit_fluo_status(uint8_t json)
{
    if (json)
    {
        App_CliPrintf(
            "{\"ok\":true,\"cmd\":\"fluo\",\"port\":\"uart4_pc10_pc11\","
            "\"rx_count\":%lu,\"tx_count\":%lu,\"crc_errors\":%lu,"
            "\"sync_drops\":%lu,\"last_rx\":\"%s\",\"last_frame_valid\":%s,"
            "\"rfu\":%lu,\"sample_valid\":%s,\"led\":%u,\"address\":%u}\r\n",
            (unsigned long)g_fluo_link.rx_count,
            (unsigned long)g_fluo_link.tx_count,
            (unsigned long)g_fluo_link.crc_error_count,
            (unsigned long)g_fluo_link.sync_drop_count,
            g_app_fluo_last_line,
            App_BoolStr(g_fluo_link.last_frame_valid),
            (unsigned long)g_fluo_link.last_rfu,
            App_BoolStr(g_fluo_link.last_sample_valid),
            g_fluo_link.last_led_state,
            g_fluo_link.last_address);
    }
    else
    {
        App_CliPrintf(
            "fluo port=uart4_pc10_pc11 rx=%lu tx=%lu crc=%lu sync_drop=%lu last_rx=\"%s\" valid=%s rfu=%lu sample_valid=%s led=%u address=%u\r\n",
            (unsigned long)g_fluo_link.rx_count,
            (unsigned long)g_fluo_link.tx_count,
            (unsigned long)g_fluo_link.crc_error_count,
            (unsigned long)g_fluo_link.sync_drop_count,
            g_app_fluo_last_line,
            g_fluo_link.last_frame_valid ? "yes" : "no",
            (unsigned long)g_fluo_link.last_rfu,
            g_fluo_link.last_sample_valid ? "yes" : "no",
            g_fluo_link.last_led_state,
            g_fluo_link.last_address);
    }
}

/* 把串口输入行拆成 token。
 * 同时识别 --json 参数，让各个命令分支不用重复处理输出格式。
 */
static uint8_t cli_tokenize(char *line, char *tokens[], uint8_t max_tokens, uint8_t *json)
{
    /* 命令参数和 --json 统一在这里拆分，避免每个命令分支自己重复处理。 */
    uint8_t count = 0U;
    char *cursor = strtok(line, " ");

    *json = 0U;
    while ((cursor != NULL) && (count < max_tokens))
    {
        if (App_TokenEq(cursor, "--json"))
        {
            *json = 1U;
        }
        else
        {
            tokens[count++] = cursor;
        }
        cursor = strtok(NULL, " ");
    }

    return count;
}

/* 把 ledstrip color 命令里的颜色名转换成 RGB 数值。
 * brightness 是用户输入的统一亮度，函数根据颜色名分配到 r/g/b 通道。
 */
static uint8_t cli_ledstrip_parse_color(const char *name, uint8_t brightness, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;

    if ((name == NULL) || (red == NULL) || (green == NULL) || (blue == NULL))
    {
        return 0U;
    }

    if (App_TokenEq(name, "red") || App_TokenEq(name, "r"))
    {
        r = brightness;
    }
    else if (App_TokenEq(name, "green") || App_TokenEq(name, "g"))
    {
        g = brightness;
    }
    else if (App_TokenEq(name, "blue") || App_TokenEq(name, "b"))
    {
        b = brightness;
    }
    else if (App_TokenEq(name, "white") || App_TokenEq(name, "w"))
    {
        r = brightness;
        g = brightness;
        b = brightness;
    }
    else if (App_TokenEq(name, "yellow"))
    {
        r = brightness;
        g = brightness;
    }
    else if (App_TokenEq(name, "cyan"))
    {
        g = brightness;
        b = brightness;
    }
    else if (App_TokenEq(name, "magenta"))
    {
        r = brightness;
        b = brightness;
    }
    else
    {
        return 0U;
    }

    *red = r;
    *green = g;
    *blue = b;
    return 1U;
}

/* 构造并发送一帧荧光模块命令。
 * CLI 层只关心地址/命令/数据字段，CRC 和帧头由 App_FluoBuildFrame() 统一处理。
 */
static void cli_fluo_send_frame(uint8_t address,
                                uint8_t command,
                                uint8_t subcommand,
                                uint8_t data_l,
                                uint8_t data_lm,
                                uint8_t data_hm,
                                uint8_t data_h)
{
    uint8_t frame[APP_FLUO_FRAME_LEN];

    App_FluoBuildFrame(frame, address, command, subcommand, data_l, data_lm, data_hm, data_h);
    App_FluoSendFrame(frame);
}

/* 解析 fluo raw 命令中的单字节十六进制参数。 */
static uint8_t cli_fluo_parse_hex_byte(const char *text, uint8_t *value)
{
    char *endptr;
    unsigned long parsed;

    if ((text == NULL) || (value == NULL))
    {
        return 0U;
    }

    parsed = strtoul(text, &endptr, 16);
    if ((*endptr != '\0') || (parsed > 0xFFUL))
    {
        return 0U;
    }

    *value = (uint8_t)parsed;
    return 1U;
}

/* CLI 总分发入口。
 * 调用链：USART1 中断收满一行 -> App_Service() -> App_ProcessCliLine()。
 * 本函数负责按 tokens[0] 分派到 sys/gpio/adc/heater/hv/array/ledstrip 等命令块。
 */
#include "app_cli_ledstrip.inc"

void App_ProcessCliLine(UartLineBuffer *buffer)
{
    /* CLI 是当前 3 号板板级联调的主入口，
     * 后续建议新增子命令，不建议再平行维护旧二进制协议。
     */
    char work[APP_UART_LINE_MAX];
    char *tokens[24];
    uint8_t argc;
    uint8_t json;

    if (buffer->overflow)
    {
        App_CliWriteLine("error: cli line overflow");
        App_UartLineReset(buffer);
        return;
    }

    (void)snprintf(work, sizeof(work), "%s", buffer->line);
    argc = cli_tokenize(work, tokens, 24U, &json);
    App_UartLineReset(buffer);

    if (argc == 0U)
    {
        return;
    }

    if (App_TokenEq(tokens[0], "help") || App_TokenEq(tokens[0], "?"))
    {
        cli_print_help();
        return;
    }

    if (App_TokenEq(tokens[0], "sys"))
    {
        /* sys 是系统总控命令：
         * - 裸 sys / status 用于查看系统总览
         * - ping 用于确认 CLI 在线
         * - arm 控制危险动作授权窗口
         * - verify-rs232 决定是否允许和 2 号板加热链路交互
         */
        if ((argc == 1U) || App_TokenEq(tokens[1], "status"))
        {
            cli_emit_sys_status(json);
            return;
        }

        if (App_TokenEq(tokens[1], "ping"))
        {
            if (json)
            {
                App_CliWriteLine("{\"ok\":true,\"cmd\":\"sys\",\"pong\":true}");
            }
            else
            {
                App_CliWriteLine("pong");
            }
            return;
        }

        if (App_TokenEq(tokens[1], "arm"))
        {
            /* arm on 是高压/加热/磁铁等危险动作的临时授权。
             * 默认 30 秒，最大 300 秒，超时由 App_Service() 自动 all_stop。
             */
            if ((argc >= 3U) && App_TokenEq(tokens[2], "on"))
            {
                unsigned long arm_seconds = 30UL;

                if (argc >= 4U)
                {
                    arm_seconds = strtoul(tokens[3], NULL, 10);
                    if (arm_seconds == 0UL)
                    {
                        arm_seconds = 30UL;
                    }
                    else if (arm_seconds > 300UL)
                    {
                        arm_seconds = 300UL;
                    }
                }

                g_app_state.armed = 1U;
                g_app_state.arm_deadline_ms = HAL_GetTick() + (uint32_t)(arm_seconds * 1000UL);
            }
            else if ((argc >= 3U) && App_TokenEq(tokens[2], "off"))
            {
                App_AllStop("arm off");
            }

            cli_emit_sys_status(json);
            return;
        }

        if (App_TokenEq(tokens[1], "clear-faults"))
        {
            g_app_state.fault_flags = APP_FAULT_NONE;
            if (json)
            {
                App_CliWriteLine("{\"ok\":true,\"cmd\":\"sys\",\"cleared\":true}");
            }
            else
            {
                App_CliWriteLine("fault flags cleared");
            }
            return;
        }

        if (App_TokenEq(tokens[1], "verify-rs232"))
        {
            /* 2 号板链路在硬件上可能被误接成 RS232 电平。
             * 只有现场确认是 TTL 后，才允许 heater 命令真正下发。
             */
            if ((argc >= 3U) && App_TokenEq(tokens[2], "ttl"))
            {
                g_app_state.heater_rs232_verified = 1U;
                g_app_state.fault_flags &= ~APP_FAULT_RS232_NOT_VERIFIED;
            }
            else if ((argc >= 3U) && App_TokenEq(tokens[2], "clear"))
            {
                g_app_state.heater_rs232_verified = 0U;
            }
            else if ((argc >= 3U) && App_TokenEq(tokens[2], "rs232"))
            {
                g_app_state.heater_rs232_verified = 0U;
                App_ReportFault(APP_FAULT_RS232_NOT_VERIFIED, "measured as rs232");
                App_AllStop("rs232 level detected");
            }

            cli_emit_sys_status(json);
            return;
        }

        App_CliWriteLine("error: unknown sys subcommand");
        return;
    }

    if (App_TokenEq(tokens[0], "gpio"))
    {
        cli_emit_gpio_status(json);
        return;
    }

    if (App_TokenEq(tokens[0], "adc"))
    {
        cli_emit_adc_status(json);
        return;
    }

    if (App_TokenEq(tokens[0], "heater"))
    {
        /* heater 命令最终通过 USART3 发给 2 号板。
         * 读状态类命令只要求 verify-rs232 ttl；
         * 设置非 0 duty 属于危险动作，还要经过 arm 和联锁门控。
         */
        const char *reason = NULL;

        if ((argc == 1U) || App_TokenEq(tokens[1], "status"))
        {
            if (g_app_state.heater_rs232_verified && (g_heater_link.pending == 0U))
            {
                App_HeaterSendBody("READ_STATUS");
            }
            cli_emit_heater_status(json);
            return;
        }

        if (App_TokenEq(tokens[1], "ping"))
        {
            if (!g_app_state.heater_rs232_verified)
            {
                App_CliWriteLine("error: verify-rs232 ttl before using heater link");
                return;
            }
            App_HeaterSendBody("PING");
            App_CliWriteLine("heater ping sent");
            return;
        }

        if (App_TokenEq(tokens[1], "version"))
        {
            if (!g_app_state.heater_rs232_verified)
            {
                App_CliWriteLine("error: verify-rs232 ttl before using heater link");
                return;
            }
            App_HeaterSendBody("GET_VER");
            cli_emit_heater_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "ntc"))
        {
            unsigned long channel = strtoul(tokens[2], NULL, 10);

            if (!g_app_state.heater_rs232_verified)
            {
                App_CliWriteLine("error: verify-rs232 ttl before using heater link");
                return;
            }
            /* 板 2 的 NTC1/NTC2 与加热 1/2 路物理位置相反。
             * CLI 保持“heater ntc 1 看 heater set 1 对应温度”的用户语义。
             */
            if (channel == 1UL)
            {
                channel = 2UL;
            }
            else if (channel == 2UL)
            {
                channel = 1UL;
            }
            App_HeaterSendCommandf("READ_NTC %lu", channel);
            cli_emit_heater_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "cur"))
        {
            if (!g_app_state.heater_rs232_verified)
            {
                App_CliWriteLine("error: verify-rs232 ttl before using heater link");
                return;
            }
            App_HeaterSendCommandf("READ_CUR %lu", strtoul(tokens[2], NULL, 10));
            cli_emit_heater_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "pwr"))
        {
            if (!g_app_state.heater_rs232_verified)
            {
                App_CliWriteLine("error: verify-rs232 ttl before using heater link");
                return;
            }
            App_HeaterSendCommandf("READ_PWR %lu", strtoul(tokens[2], NULL, 10));
            cli_emit_heater_status(json);
            return;
        }

        if ((argc >= 4U) && App_TokenEq(tokens[1], "set"))
        {
            /* duty=0 可以用于关加热，允许只通过 RS232 电平验证；
             * duty>0 会真正加热，必须经过 App_CanExecuteDangerous()。
             */
            unsigned long channel = strtoul(tokens[2], NULL, 10);
            unsigned long duty = strtoul(tokens[3], NULL, 10);

            if (duty == 0UL)
            {
                if (!g_app_state.heater_rs232_verified)
                {
                    App_CliWriteLine("error: verify-rs232 ttl before using heater link");
                    return;
                }
            }
            else if (!App_CanExecuteDangerous(1U, &reason))
            {
                App_CliPrintf("error: %s\r\n", reason);
                return;
            }

            App_HeaterSendCommandf("SET_HEATER %lu %lu", channel, duty);
            if ((channel >= 1UL) && (channel <= 3UL) && (duty <= 100UL))
            {
                g_heater_link.duty_pct[channel - 1UL] = (uint8_t)duty;
            }
            App_CliWriteLine("heater set command sent");
            return;
        }

        if (App_TokenEq(tokens[1], "stop"))
        {
            if (!g_app_state.heater_rs232_verified)
            {
                App_CliWriteLine("error: verify-rs232 ttl before using heater link");
                return;
            }
            App_HeaterSendBody("STOP_ALL");
            App_CliWriteLine("heater stop sent");
            return;
        }

        App_CliWriteLine("error: unknown heater subcommand");
        return;
    }

    if (App_TokenEq(tokens[0], "hv"))
    {
        /* hv 命令控制 MCP41050 高压设定和 OFF/DC/AC 模式。
         * 除 read 外，默认都视为危险动作；mode off 例外，因为它是降风险动作。
         */
        const char *reason = NULL;

        /* hv 除 read 外都属于危险命令，默认需要过统一门控。 */
        if ((argc == 1U) || App_TokenEq(tokens[1], "read"))
        {
            cli_emit_hv_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "set"))
        {
            if (!App_CanExecuteDangerous(0U, &reason))
            {
                App_CliPrintf("error: %s\r\n", reason);
                return;
            }
            HV507_SetVoltage(&hv507, (uint16_t)strtoul(tokens[2], NULL, 10));
            cli_emit_hv_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "mode"))
        {
            /* AC/DC 都会打开高压相关输出，所以需要 arm + 联锁；
             * off 是安全动作，不要求 arm。
             */
            if (App_TokenEq(tokens[2], "off"))
            {
                HV507_SetMode(&hv507, HV507_MODE_OFF);
                cli_emit_hv_status(json);
                return;
            }

            if (!App_CanExecuteDangerous(0U, &reason))
            {
                App_CliPrintf("error: %s\r\n", reason);
                return;
            }

            if (App_TokenEq(tokens[2], "dc"))
            {
                HV507_SetMode(&hv507, HV507_MODE_DC);
            }
            else if (App_TokenEq(tokens[2], "ac"))
            {
                HV507_SetMode(&hv507, HV507_MODE_AC);
            }
            else
            {
                App_CliWriteLine("error: expected off|dc|ac");
                return;
            }
            cli_emit_hv_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "freq"))
        {
            HV507_SetAcFrequency(&hv507, (uint16_t)strtoul(tokens[2], NULL, 10));
            cli_emit_hv_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "phase"))
        {
            HV507_SetAcPhaseDelayUs(&hv507, (uint16_t)strtoul(tokens[2], NULL, 10));
            cli_emit_hv_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "acdrive"))
        {
            if (App_TokenEq(tokens[2], "pwm"))
            {
                HV507_SetAcDrivePwm(&hv507, 1U);
            }
            else if (App_TokenEq(tokens[2], "gpio"))
            {
                HV507_SetAcDrivePwm(&hv507, 0U);
            }
            else
            {
                App_CliWriteLine("error: expected pwm|gpio");
                return;
            }
            cli_emit_hv_status(json);
            return;
        }

        App_CliWriteLine("error: unknown hv subcommand");
        return;
    }

    if (App_TokenEq(tokens[0], "array"))
    {
        /* array 命令控制 HV507 阵列逻辑。
         * 正常路径：array one/all/set -> HV507_SetLogicBytes() -> HV507_Service()。
         * 正式版只保留正常位图控制与只读状态；实验发送路径已归档。
         */
        const char *reason = NULL;
        uint8_t pattern[16];
        uint8_t i;

        if ((argc == 1U) || App_TokenEq(tokens[1], "read"))
        {
            cli_emit_array_status(json);
            return;
        }

        if (App_TokenEq(tokens[1], "pins"))
        {
            cli_emit_array_pins(json);
            return;
        }

        if (App_TokenEq(tokens[1], "clear"))
        {
            /* 清空阵列缓存并立即提交。
             * 这是结束阵列测试时最常用的恢复命令之一。
             */
            HV507_ClearLogic(&hv507);
            HV507_Service(&hv507);
            cli_emit_array_status(json);
            return;
        }

        if (App_TokenEq(tokens[1], "set"))
        {
            /* array set 固定要求 16 字节十六进制逻辑数据。 */
            if (!App_CanExecuteDangerous(0U, &reason))
            {
                App_CliPrintf("error: %s\r\n", reason);
                return;
            }

            if (argc != 18U)
            {
                App_CliWriteLine("error: array set expects 16 hex bytes");
                return;
            }

            for (i = 0U; i < 16U; ++i)
            {
                pattern[i] = (uint8_t)strtoul(tokens[i + 2U], NULL, 16);
            }
            HV507_SetLogicBytes(&hv507, pattern, 16U);
            HV507_Service(&hv507);
            cli_emit_array_status(json);
            return;
        }

        if (App_TokenEq(tokens[1], "all"))
        {
            /* 整个 16 字节逻辑全开/全关。
             * 这个命令会影响所有 HV507 原始输出，测试时要确认高压和联锁状态。
             */
            if (!App_CanExecuteDangerous(0U, &reason))
            {
                App_CliPrintf("error: %s\r\n", reason);
                return;
            }

            if (argc != 3U)
            {
                App_CliWriteLine("error: array all expects on|off");
                return;
            }

            if (App_TokenEq(tokens[2], "on"))
            {
                memset(pattern, 0xFF, sizeof(pattern));
            }
            else if (App_TokenEq(tokens[2], "off"))
            {
                memset(pattern, 0x00, sizeof(pattern));
            }
            else
            {
                App_CliWriteLine("error: array all expects on|off");
                return;
            }

            HV507_SetLogicBytes(&hv507, pattern, 16U);
            HV507_Service(&hv507);
            cli_emit_array_status(json);
            return;
        }

        if (App_TokenEq(tokens[1], "one") ||
            App_TokenEq(tokens[1], "add") ||
            App_TokenEq(tokens[1], "off"))
        {
            /* 电极级常用入口：
             * one 清空后只开一个电极；add 在当前缓存上追加；off 从当前缓存里关掉一个。
             */
            uint16_t id;
            uint8_t byte_index;
            uint8_t bit_mask;

            if (!App_CanExecuteDangerous(0U, &reason))
            {
                App_CliPrintf("error: %s\r\n", reason);
                return;
            }

            if (argc != 3U)
            {
                App_CliWriteLine("error: array one|add|off expects electrode id 1..60 or 65..124");
                return;
            }

            id = (uint16_t)strtoul(tokens[2], NULL, 10);
            if (!cli_array_id_to_bit(id, &byte_index, &bit_mask))
            {
                App_CliWriteLine("error: electrode id must be 1..60 or 65..124");
                return;
            }

            if (App_TokenEq(tokens[1], "one"))
            {
                memset(hv507.logic, 0, sizeof(hv507.logic));
                hv507.logic[byte_index] = bit_mask;
            }
            else if (App_TokenEq(tokens[1], "add"))
            {
                hv507.logic[byte_index] |= bit_mask;
            }
            else
            {
                hv507.logic[byte_index] &= (uint8_t)~bit_mask;
            }

            hv507.logic_dirty = 1U;
            HV507_Service(&hv507);
            cli_emit_array_status(json);
            return;
        }

        App_CliWriteLine("error: unknown array subcommand");
        return;
    }

    if (App_TokenEq(tokens[0], "ledstrip"))
    {
        cli_process_ledstrip(argc, tokens);
        return;
    }

    if (App_TokenEq(tokens[0], "oled"))
    {
        /* OLED 当前只是预留脚管理，不初始化屏幕。
         * 后续接显示屏时，应优先在 app_oled.c 扩展，不要把 PA4 再分给其他模块。
         */
        AppOledPins pins;

        if ((argc == 1U) || App_TokenEq(tokens[1], "status"))
        {
            /* 应用/交接常用：只读当前 OLED 预留脚电平，不会主动驱动屏幕协议。 */
            pins = App_OledReadPins();
            if (json)
            {
                App_CliPrintf("{\"ok\":true,\"cmd\":\"oled\",\"reserved\":true,"
                              "\"res\":%u,\"dc\":%u,\"cs\":%u,"
                              "\"res_pin\":\"PA0\",\"dc_pin\":\"PA1\",\"cs_pin\":\"PA4\"}\r\n",
                              (pins.res == GPIO_PIN_SET) ? 1U : 0U,
                              (pins.dc == GPIO_PIN_SET) ? 1U : 0U,
                              (pins.cs == GPIO_PIN_SET) ? 1U : 0U);
            }
            else
            {
                App_CliPrintf("oled reserved pins: RES=PA0(%u) DC=PA1(%u) CS=PA4(%u)\r\n",
                              (pins.res == GPIO_PIN_SET) ? 1U : 0U,
                              (pins.dc == GPIO_PIN_SET) ? 1U : 0U,
                              (pins.cs == GPIO_PIN_SET) ? 1U : 0U);
            }
            return;
        }

        if (App_TokenEq(tokens[1], "idle"))
        {
            /* 应用常用：把 OLED 控制脚恢复到安全空闲态，适合接屏前后做默认状态。 */
            App_OledSetIdlePins();
            App_CliWriteLine(json ?
                             "{\"ok\":true,\"cmd\":\"oled\",\"state\":\"idle\"}" :
                             "oled idle RES=1 DC=1 CS=1");
            return;
        }

        if (App_TokenEq(tokens[1], "reset"))
        {
            /* 应用常用：显示屏复位脉冲。
             * 当前还没有完整 OLED 初始化，所以这里只验证 RES 脚动作。
             */
            uint32_t low_ms = 10UL;

            if (argc >= 3U)
            {
                low_ms = strtoul(tokens[2], NULL, 10);
            }
            if (low_ms > 200UL)
            {
                App_CliWriteLine("error: oled reset ms range is 0..200");
                return;
            }
            if (low_ms == 0UL)
            {
                low_ms = 10UL;
            }

            App_OledResetPulse(low_ms);
            if (json)
            {
                App_CliPrintf("{\"ok\":true,\"cmd\":\"oled\",\"reset_ms\":%lu}\r\n", (unsigned long)low_ms);
            }
            else
            {
                App_CliPrintf("oled reset %lums\r\n", (unsigned long)low_ms);
            }
            return;
        }

        if (App_TokenEq(tokens[1], "pins"))
        {
            /* 测试诊断：直接指定 RES/DC/CS 电平。
             * 用于万用表或示波器确认 PA0/PA1/PA4 低压连通性，不是显示业务绘图接口。
             */
            unsigned long res;
            unsigned long dc;
            unsigned long cs;

            if (argc != 5U)
            {
                App_CliWriteLine("error: oled pins expects <res 0|1> <dc 0|1> <cs 0|1>");
                return;
            }

            res = strtoul(tokens[2], NULL, 10);
            dc = strtoul(tokens[3], NULL, 10);
            cs = strtoul(tokens[4], NULL, 10);
            if ((res > 1UL) || (dc > 1UL) || (cs > 1UL))
            {
                App_CliWriteLine("error: oled pins values must be 0 or 1");
                return;
            }

            App_OledSetPins(res ? GPIO_PIN_SET : GPIO_PIN_RESET,
                            dc ? GPIO_PIN_SET : GPIO_PIN_RESET,
                            cs ? GPIO_PIN_SET : GPIO_PIN_RESET);
            App_CliPrintf("oled pins RES=%lu DC=%lu CS=%lu\r\n", res, dc, cs);
            return;
        }

        App_CliWriteLine("error: expected oled status|idle|reset [ms]|pins <res 0|1> <dc 0|1> <cs 0|1>");
        return;
    }

    if (App_TokenEq(tokens[0], "magnet"))
    {
        /* magnet 控制 PC12 -> 继电器 -> 两线推杆。
         * on 必须通过 arm 和联锁；off 切回默认极性，不等于断电。
         */

        /* 检查磁杆 GPIO 功能是否可用。 */
        if (!g_app_magnet_available)
        {
            App_CliWriteLine("magnet control not mapped in current firmware");
            return;
        }

        if ((argc >= 2U) && App_TokenEq(tokens[1], "off"))
        {
            /* 应用常用：关闭继电器驱动，让推杆回到默认极性状态。 */
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);
            if (json)
            {
                App_CliWriteLine("{\"ok\":true,\"cmd\":\"magnet\",\"state\":\"off\"}");
            }
            else
            {
                App_CliWriteLine("magnet off");
            }
            return;
        }

        const char *reason = NULL;
        if (!App_CanExecuteDangerous(0U, &reason))
        {
            App_CliPrintf("error: %s\r\n", reason);
            return;
        }

        if ((argc >= 2U) && App_TokenEq(tokens[1], "on"))
        {
            /* 应用常用：打开继电器驱动，让两线推杆切换到另一极性状态。 */
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_SET);
            if (json)
            {
                App_CliWriteLine("{\"ok\":true,\"cmd\":\"magnet\",\"state\":\"on\"}");
            }
            else
            {
                App_CliWriteLine("magnet on");
            }
            return;
        }

        App_CliWriteLine("error: expected magnet on|off");
        return;
    }

    if (App_TokenEq(tokens[0], "aux"))
    {
        /* USART2 辅助口当前主要作为预留/显示调试链路。
         * status 是常用只读入口；send 是测试诊断入口。
         */
        if ((argc == 1U) || App_TokenEq(tokens[1], "status"))
        {
            cli_emit_aux_status(json);
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "send"))
        {
            /* 测试诊断：向 USART2 原样发文本，验证 H6/显示辅助口收发。 */
            char out_buffer[160];
            uint8_t i;

            out_buffer[0] = '\0';
            for (i = 2U; i < argc; ++i)
            {
                (void)snprintf(out_buffer + strlen(out_buffer),
                               sizeof(out_buffer) - strlen(out_buffer),
                               "%s%s",
                               (i > 2U) ? " " : "",
                               tokens[i]);
            }
            (void)snprintf(out_buffer + strlen(out_buffer), sizeof(out_buffer) - strlen(out_buffer), "\r\n");
            App_UartSend(&huart2, out_buffer);
            App_CliWriteLine("aux message sent");
            return;
        }

        App_CliWriteLine("error: unknown aux subcommand");
        return;
    }

    if (App_TokenEq(tokens[0], "fluo"))
    {
        /* 荧光模块命令：
         * 常用应用入口：status/addr/read/measure/led。
         * 测试诊断入口：raw/send，用于直接发原始帧或旧文本命令。
         */
        if ((argc == 1U) || App_TokenEq(tokens[1], "status"))
        {
            cli_emit_fluo_status(json);
            return;
        }

        if (App_TokenEq(tokens[1], "addr"))
        {
            /* 应用常用：查询荧光模块地址。 */
            cli_fluo_send_frame(0xFFU, 0x1BU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U);
            App_CliWriteLine("fluo addr command sent");
            return;
        }

        if (App_TokenEq(tokens[1], "read"))
        {
            /* 应用常用：读取最近一次荧光数据。 */
            cli_fluo_send_frame(0x01U, 0x19U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U);
            App_CliWriteLine("fluo read command sent");
            return;
        }

        if ((argc >= 3U) &&
            (App_TokenEq(tokens[1], "measure") ||
             App_TokenEq(tokens[1], "simple") ||
             App_TokenEq(tokens[1], "auto")))
        {
            /* 应用常用：触发一次荧光测量。
             * excite_ms 限制到 200ms，避免误输入导致长时间激发。
             */
            unsigned long excite_ms = strtoul(tokens[2], NULL, 10);
            if (excite_ms > 200UL)
            {
                App_CliWriteLine("error: fluo measure/simple max is 200ms");
                return;
            }
            cli_fluo_send_frame(0x01U, 0x1AU, 0x00U, (uint8_t)excite_ms, 0x00U, 0x00U, 0x00U);
            App_CliWriteLine("fluo measure/simple command sent");
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "led"))
        {
            /* 应用常用：控制/查询荧光模块自带 LED。 */
            if (App_TokenEq(tokens[2], "on"))
            {
                cli_fluo_send_frame(0x01U, 0x20U, 0x80U, 0x0AU, 0x01U, 0x00U, 0x00U);
                App_CliWriteLine("fluo led on command sent");
                return;
            }
            if (App_TokenEq(tokens[2], "off"))
            {
                cli_fluo_send_frame(0x01U, 0x20U, 0x80U, 0x0BU, 0x01U, 0x00U, 0x00U);
                App_CliWriteLine("fluo led off command sent");
                return;
            }
            if (App_TokenEq(tokens[2], "status"))
            {
                cli_fluo_send_frame(0x01U, 0x20U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U);
                App_CliWriteLine("fluo led status command sent");
                return;
            }
            App_CliWriteLine("error: expected fluo led on|off|status");
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "raw"))
        {
            /* 测试诊断：直接发送 10 字节原始帧。
             * 只有在需要绕开 CLI 封装、复现实验帧时使用。
             */
            uint8_t frame[APP_FLUO_FRAME_LEN];
            uint8_t i;

            if (argc != 12U)
            {
                App_CliWriteLine("error: fluo raw expects 10 hex bytes");
                return;
            }

            for (i = 0U; i < APP_FLUO_FRAME_LEN; ++i)
            {
                if (!cli_fluo_parse_hex_byte(tokens[i + 2U], &frame[i]))
                {
                    App_CliWriteLine("error: invalid hex byte");
                    return;
                }
            }
            App_FluoSendFrame(frame);
            App_CliWriteLine("fluo raw frame sent");
            return;
        }

        if ((argc >= 3U) && App_TokenEq(tokens[1], "send"))
        {
            /* 测试诊断：旧文本模式，保留给早期荧光串口调试。 */
            char out_buffer[160];
            uint8_t i;

            out_buffer[0] = '\0';
            for (i = 2U; i < argc; ++i)
            {
                (void)snprintf(out_buffer + strlen(out_buffer),
                               sizeof(out_buffer) - strlen(out_buffer),
                               "%s%s",
                               (i > 2U) ? " " : "",
                               tokens[i]);
            }
            (void)snprintf(out_buffer + strlen(out_buffer), sizeof(out_buffer) - strlen(out_buffer), "\r\n");
            App_UartSend(&huart4, out_buffer);
            App_CliWriteLine("fluo message sent");
            return;
        }

        App_CliWriteLine("error: unknown fluo subcommand");
        return;
    }

    if (App_TokenEq(tokens[0], "all_stop"))
    {
        App_AllStop("cli all_stop");
        if (json)
        {
            App_CliWriteLine("{\"ok\":true,\"cmd\":\"all_stop\"}");
        }
        else
        {
            App_CliWriteLine("all_stop executed");
        }
        return;
    }

    App_CliWriteLine("error: unknown command, use help");
}
