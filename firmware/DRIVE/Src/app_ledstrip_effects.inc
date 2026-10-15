/* Included only by app_ledstrip.c, reuses its single proven PWM update path. */
#include "app_ledstrip_effects.h"

static LedFxState s_effect;
static HAL_StatusTypeDef s_effect_result = HAL_OK;
static LedStripPwmPixel s_effect_frame[APP_LEDSTRIP_MAX_LEDS];

void App_LedStripEffectCancel(void)
{
    LedFxStop(&s_effect); /* freeze displayed pixels; Clear() sends the black frame */
}

HAL_StatusTypeDef App_LedStripEffectStart(uint8_t effect, uint8_t count,
                                        uint8_t brightness, uint16_t interval_ms, uint8_t demo,
                                        uint8_t use_default_brightness)
{
    if (__get_IPSR() != 0U || __get_PRIMASK() != 0U || s_busy) return HAL_BUSY;
    if (use_default_brightness > 1U) return HAL_ERROR;
    if (!LedFxStart(&s_effect, effect, count, brightness, interval_ms, demo, HAL_GetTick()))
        return HAL_ERROR;
    if (use_default_brightness) LedFxUseDefaultBrightness(&s_effect);
    s_effect_result = HAL_OK;
    return HAL_OK;
}

void App_LedStripEffectService(void)
{
    if (__get_IPSR() != 0U || __get_PRIMASK() != 0U || !s_effect.running) return;
    if (!LedFxTick(&s_effect, HAL_GetTick(), s_effect_frame)) return;

    HAL_StatusTypeDef result = ledstrip_begin(APP_LEDSTRIP_MAX_LEDS);
    if (result == HAL_OK)
    {
        memcpy(s_pixels, s_effect_frame, sizeof(s_pixels));
        /* rgb reports the first pixel for this multicolor frame. */
        result = ledstrip_update(APP_LEDSTRIP_MAX_LEDS,
                                 s_pixels[0].red, s_pixels[0].green, s_pixels[0].blue);
    }
    s_effect_result = result;
    if (result != HAL_OK) App_LedStripEffectCancel(); /* never retry endlessly */
}

void App_LedStripGetEffectStatus(AppLedStripEffectStatus *status)
{
    if (!status) return;
    status->effect = s_effect.effect;
    status->running = s_effect.running;
    status->demo = s_effect.demo;
    status->count = s_effect.count;
    status->brightness = s_effect.brightness;
    status->interval_ms = s_effect.interval_ms;
    status->frame = s_effect.frame;
    status->result = s_effect_result;
}

const char *App_LedStripEffectName(uint8_t effect)
{
    switch (effect)
    {
    case 1U: return "rainbow";
    case 2U: return "meteor";
    case 3U: return "chase";
    case 4U: return "stack";
    case 5U: return "sparkle";
    case 6U: return "collide";
    default: return "none";
    }
}
