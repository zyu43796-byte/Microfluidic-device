/* LED strip controls. Included after the common color helper. */
static uint8_t cli_ledstrip_number(const char *text, unsigned long minimum,
                                   unsigned long maximum, unsigned long *value)
{
    unsigned long parsed = 0UL;
    if (!text || !*text) return 0U;
    for (; *text; ++text)
    {
        if (*text < '0' || *text > '9') return 0U;
        unsigned long digit = (unsigned long)(*text - '0');
        if (parsed > maximum / 10UL ||
            (parsed == maximum / 10UL && digit > maximum % 10UL)) return 0U;
        parsed = parsed * 10UL + digit;
    }
    if (parsed < minimum) return 0U;
    *value = parsed;
    return 1U;
}

static void cli_ledstrip_status(void)
{
    AppLedStripPwmDiagnostics d;
    AppLedStripEffectStatus fx;
    App_LedStripGetPwmDiagnostics(&d);
    App_LedStripGetEffectStatus(&fx);
    App_CliPrintf("ledstrip build=%s path=tim8_pwm_dma pin=PB15 timer=TIM8_CH3N\r\n",
                  APP_LEDSTRIP_BUILD);
    App_CliPrintf("order=%s result=%u\r\n",
                  App_LedStripOrderName(App_LedStripGetOrder()), (unsigned)d.result);
    App_CliPrintf("max=%u last_count=%u rgb=%u,%u,%u seq=%lu completed=%lu\r\n",
                  APP_LEDSTRIP_MAX_LEDS, g_app_ledstrip.count,
                  g_app_ledstrip.red, g_app_ledstrip.green, g_app_ledstrip.blue,
                  (unsigned long)d.sequence, (unsigned long)d.completed);
    App_CliPrintf("effect=%s mode=%s active_count=%u brightness=%u interval_ms=%u frame=%lu effect_result=%u\r\n",
                  App_LedStripEffectName(fx.effect), fx.running ? (fx.demo ? "demo" : "running") : "stopped",
                  fx.count, fx.brightness, fx.interval_ms, (unsigned long)fx.frame, (unsigned)fx.result);
    if (fx.running) App_CliWriteLine("effect frame: last_count=30; rgb above is first pixel only");
}

static uint8_t cli_ledstrip_result(HAL_StatusTypeDef result)
{
    if (result == HAL_OK) return 1U;
    App_CliPrintf("error: ledstrip PWM transmit failed result=%u; use ledstrip status\r\n",
                  (unsigned)result);
    return 0U;
}

static void cli_process_ledstrip(uint8_t argc, char *tokens[])
{
    unsigned long count = APP_LEDSTRIP_MAX_LEDS, index = 1UL;
    unsigned long red = 0UL, green = 0UL, blue = 0UL, brightness = 0UL, hold = 0UL;
    uint8_t r = 0U, g = 0U, b = 0U;
    HAL_StatusTypeDef result;
    const char *cmd;
    if (argc == 1U || (argc == 2U && App_TokenEq(tokens[1], "status")))
    {
        cli_ledstrip_status();
        return;
    }
    cmd = tokens[1];
    if (App_TokenEq(cmd, "effect") || App_TokenEq(cmd, "demo"))
    {
        const uint8_t demo = App_TokenEq(cmd, "demo");
        const uint8_t first = demo ? 2U : 3U;
        const uint8_t use_default_brightness = argc <= first + 1U;
        AppLedStripEffectStatus started;
        unsigned long effect = 1UL, interval = 30UL;
        brightness = 80UL;
        if (!demo && argc == 2U)
        {
            App_CliWriteLine("effects: 1 rainbow, 2 meteor, 3 chase, 4 stack, 5 sparkle, 6 collide");
            App_CliWriteLine("ledstrip effect <name|1..6> [count=30] [brightness] [interval_ms=30]");
            App_CliWriteLine("default brightness: meteor=112, others=80; explicit brightness overrides all");
            App_CliWriteLine("ledstrip demo [count] [brightness] [interval_ms] (10 seconds each)");
            App_CliWriteLine("ledstrip effect stop = freeze; ledstrip off = stop and blackout");
            return;
        }
        if (!demo && argc == 3U && App_TokenEq(tokens[2], "stop"))
        {
            App_LedStripEffectCancel();
            App_CliWriteLine("ledstrip effect stopped (colors held; use off to blackout)");
            return;
        }
        if (argc < first || argc > first + 3U) goto effect_usage;
        if (!demo && !cli_ledstrip_number(tokens[2], 1UL, 6UL, &effect))
        {
            for (effect = 1UL; effect <= 6UL; ++effect)
                if (App_TokenEq(tokens[2], App_LedStripEffectName((uint8_t)effect))) break;
            if (effect > 6UL) goto effect_usage;
        }
        if (argc > first && !cli_ledstrip_number(tokens[first], 1UL, APP_LEDSTRIP_MAX_LEDS, &count)) goto effect_usage;
        if (argc > first + 1U && !cli_ledstrip_number(tokens[first + 1U], 0UL, 255UL, &brightness)) goto effect_usage;
        if (argc > first + 2U && !cli_ledstrip_number(tokens[first + 2U], 10UL, 1000UL, &interval)) goto effect_usage;
        result = App_LedStripEffectStart((uint8_t)effect, (uint8_t)count, (uint8_t)brightness,
                                        (uint16_t)interval, demo, use_default_brightness);
        if (result != HAL_OK)
        {
            App_CliPrintf("error: cannot start ledstrip effect result=%u\r\n", (unsigned)result);
            return;
        }
        App_LedStripGetEffectStatus(&started);
        brightness = started.brightness;
        App_CliPrintf("ledstrip effect=%s mode=%s count=%lu brightness=%lu interval_ms=%lu\r\n",
                      App_LedStripEffectName((uint8_t)effect), demo ? "demo" : "running", count, brightness, interval);
        return;
    }
    if (App_TokenEq(cmd, "order"))
    {
        if (argc == 3U)
        {
            unsigned i;
            for (i = 0U; i < 6U; ++i)
                if (App_TokenEq(tokens[2], App_LedStripOrderName((AppLedStripOrder)i))) break;
            if (i == 6U) goto usage;
            App_LedStripSetOrder((AppLedStripOrder)i);
        }
        else if (argc != 2U) goto usage;
        App_CliPrintf("ledstrip order=%s\r\n", App_LedStripOrderName(App_LedStripGetOrder()));
        return;
    }
    if (App_TokenEq(cmd, "off"))
    {
        if (argc != 2U && !(argc == 3U && cli_ledstrip_number(tokens[2], 1UL, APP_LEDSTRIP_MAX_LEDS, &count))) goto usage;
        if (cli_ledstrip_result(App_LedStripClear((uint8_t)count)))
            App_CliPrintf("ledstrip off count=%lu rgb=0,0,0\r\n", count);
        return;
    }
    if (argc < 3U || !cli_ledstrip_number(tokens[2], 1UL, APP_LEDSTRIP_MAX_LEDS, &count)) goto usage;
    if (App_TokenEq(cmd, "color"))
    {
        if (argc != 5U && argc != 6U) goto usage;
        if (!cli_ledstrip_number(tokens[4], 0UL, 255UL, &brightness)) goto usage;
        if (argc == 6U && !cli_ledstrip_number(tokens[5], 0UL, 0UL, &hold)) goto usage;
        if (!cli_ledstrip_parse_color(tokens[3], (uint8_t)brightness, &r, &g, &b)) goto usage;
        result = App_LedStripFill((uint8_t)count, r, g, b);
        if (cli_ledstrip_result(result))
            App_CliPrintf("ledstrip %s count=%lu color=%s brightness=%lu rgb=%u,%u,%u ms=%lu\r\n",
                          cmd, count, tokens[3], brightness, r, g, b, hold);
        return;
    }
    if (App_TokenEq(cmd, "fill"))
    {
        if (argc != 6U && argc != 7U) goto usage;
        if (!cli_ledstrip_number(tokens[3], 0UL, 255UL, &red) ||
            !cli_ledstrip_number(tokens[4], 0UL, 255UL, &green) ||
            !cli_ledstrip_number(tokens[5], 0UL, 255UL, &blue)) goto usage;
        if (argc == 7U && !cli_ledstrip_number(tokens[6], 0UL, 0UL, &hold)) goto usage;
        result = App_LedStripFill((uint8_t)count, (uint8_t)red, (uint8_t)green, (uint8_t)blue);
        if (cli_ledstrip_result(result))
            App_CliPrintf("ledstrip %s count=%lu rgb=%lu,%lu,%lu ms=%lu\r\n", cmd, count, red, green, blue, hold);
        return;
    }
    if (App_TokenEq(cmd, "one") || App_TokenEq(cmd, "pixel"))
    {
        uint8_t named = App_TokenEq(cmd, "pixel");
        uint8_t expected = named ? 6U : 7U;
        if (argc != expected && argc != expected + 1U) goto usage;
        if (!cli_ledstrip_number(tokens[3], 1UL, count, &index)) goto usage;
        if (named)
        {
            if (!cli_ledstrip_number(tokens[5], 0UL, 255UL, &brightness) ||
                !cli_ledstrip_parse_color(tokens[4], (uint8_t)brightness, &r, &g, &b)) goto usage;
        }
        else
        {
            if (!cli_ledstrip_number(tokens[4], 0UL, 255UL, &red) ||
                !cli_ledstrip_number(tokens[5], 0UL, 255UL, &green) ||
                !cli_ledstrip_number(tokens[6], 0UL, 255UL, &blue)) goto usage;
            r = (uint8_t)red; g = (uint8_t)green; b = (uint8_t)blue;
        }
        if (argc == expected + 1U && !cli_ledstrip_number(tokens[expected], 0UL, 0UL, &hold)) goto usage;
        result = App_LedStripOne((uint8_t)count, (uint8_t)(index - 1UL), r, g, b);
        if (cli_ledstrip_result(result))
            App_CliPrintf("ledstrip %s count=%lu idx=%lu rgb=%u,%u,%u ms=%lu\r\n", cmd, count, index, r, g, b, hold);
        return;
    }
usage:
    App_CliWriteLine("error: invalid ledstrip command/arguments; count=1..30 RGB=0..255 ms=0 only (nonblocking); use help");
    return;
effect_usage:
    App_CliWriteLine("error: ledstrip effect <1..6|rainbow|meteor|chase|stack|sparkle|collide> [count 1..30] [brightness 0..255] [interval_ms 10..1000]");
    return;
}
