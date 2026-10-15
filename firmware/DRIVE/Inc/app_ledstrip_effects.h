#ifndef APP_LEDSTRIP_EFFECTS_H
#define APP_LEDSTRIP_EFFECTS_H

#include "app_ledstrip_pwm_frame.h"

/* Visual inspiration: WS2818_34.ino (rainbow, trails, accumulation, twinkles).
 * Pure integer renderer, independent of HAL/FastLED. All loops are bounded.
 * Test builds evaluate the exact same functions with C++14 static_assert.
 */
#ifdef __cplusplus
#define FX_INLINE static constexpr
#else
#define FX_INLINE static inline
#endif

#define LED_FX_COUNT 6U
#define LED_FX_DEMO_MS 10000U

typedef struct
{
    uint8_t effect, count, brightness, running, demo, auto_brightness;
    uint16_t interval_ms;
    uint32_t last_ms, demo_ms, frame;
    LedStripPwmPixel pixels[LEDSTRIP_PWM_MAX_LEDS]; /* unscaled trail/history */
} LedFxState;

FX_INLINE uint8_t LedFxScale(uint32_t value, uint32_t brightness)
{
    return (uint8_t)((value * brightness + 127U) / 255U);
}

FX_INLINE uint8_t LedFxDefaultBrightness(uint32_t effect)
{
    return effect == 2U ? 112U : 80U;
}

FX_INLINE void LedFxUseDefaultBrightness(LedFxState *s)
{
    s->auto_brightness = 1U;
    s->brightness = LedFxDefaultBrightness(s->effect);
}

/* 768-step RGB color wheel, continuous at each primary and at wrap. */
FX_INLINE LedStripPwmPixel LedFxWheel(uint32_t hue)
{
    LedStripPwmPixel p = {0, 0, 0};
    hue %= 768U;
    if (hue < 256U) { p.red = (uint8_t)(255U - hue); p.green = (uint8_t)hue; }
    else if (hue < 512U) { hue -= 256U; p.green = (uint8_t)(255U - hue); p.blue = (uint8_t)hue; }
    else { hue -= 512U; p.blue = (uint8_t)(255U - hue); p.red = (uint8_t)hue; }
    return p;
}

/* Smoothstep-shaped triangle, period 512. No floating point or libm. */
FX_INLINE uint32_t LedFxWave(uint32_t phase)
{
    uint32_t x = phase & 511U;
    if (x > 255U) x = 511U - x;
    return (x * x * (765U - 2U * x) + 32512U) / 65025U;
}

FX_INLINE void LedFxResetHistory(LedFxState *s)
{
    for (uint32_t i = 0U; i < LEDSTRIP_PWM_MAX_LEDS; ++i)
    {
        s->pixels[i].red = 0U; s->pixels[i].green = 0U; s->pixels[i].blue = 0U;
    }
    s->frame = 0U;
}

FX_INLINE uint8_t LedFxStart(LedFxState *s, uint32_t effect, uint32_t count,
                            uint32_t brightness, uint32_t interval_ms, uint32_t demo,
                            uint32_t now)
{
    if (!s || effect < 1U || effect > LED_FX_COUNT || count < 1U ||
        count > LEDSTRIP_PWM_MAX_LEDS || brightness > 255U ||
        interval_ms < 10U || interval_ms > 1000U || demo > 1U) return 0U;
    s->effect = (uint8_t)effect; s->count = (uint8_t)count;
    s->brightness = (uint8_t)brightness; s->interval_ms = (uint16_t)interval_ms;
    s->demo = (uint8_t)demo; s->running = 1U;
    s->auto_brightness = 0U; /* an explicit value, including zero, always wins */
    s->last_ms = now - interval_ms; /* next service renders immediately */
    s->demo_ms = now;
    LedFxResetHistory(s);
    return 1U;
}

FX_INLINE void LedFxStop(LedFxState *s)
{
    if (s) { s->running = 0U; s->demo = 0U; }
}

FX_INLINE void LedFxFade(LedFxState *s, uint32_t retain)
{
    for (uint32_t i = 0U; i < s->count; ++i)
    {
        s->pixels[i].red = LedFxScale(s->pixels[i].red, retain);
        s->pixels[i].green = LedFxScale(s->pixels[i].green, retain);
        s->pixels[i].blue = LedFxScale(s->pixels[i].blue, retain);
        /* Rounded scaling otherwise leaves low-level trails forever. */
        if (s->pixels[i].red < 4U) s->pixels[i].red = 0U;
        if (s->pixels[i].green < 4U) s->pixels[i].green = 0U;
        if (s->pixels[i].blue < 4U) s->pixels[i].blue = 0U;
    }
}

FX_INLINE uint32_t LedFxDistance(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

FX_INLINE uint32_t LedFxGlow(uint32_t distance, uint32_t width)
{
    return distance < width ? (width - distance) * 255U / width : 0U;
}

FX_INLINE uint8_t LedFxAdd(uint32_t a, uint32_t b)
{
    return (uint8_t)(a + b > 255U ? 255U : a + b);
}

FX_INLINE void LedFxRender(LedFxState *s, LedStripPwmPixel *out)
{
    const uint32_t f = s->frame;
    const uint32_t n = s->count;
    switch (s->effect)
    {
    case 1U: /* rainbow: full spectrum travelling along the strip */
        for (uint32_t i = 0U; i < n; ++i)
            s->pixels[i] = LedFxWheel((f % 256U) * 3U + i * 768U / n);
        break;
    case 2U: /* meteor: colored decaying tail and a pale bright head */
    {
        LedFxFade(s, 205U);
        const uint32_t head = (f / 2U) % n;
        LedStripPwmPixel p = LedFxWheel((f % 256U) * 3U);
        p.red = (uint8_t)(80U + LedFxScale(p.red, 175U));
        p.green = (uint8_t)(80U + LedFxScale(p.green, 175U));
        p.blue = (uint8_t)(80U + LedFxScale(p.blue, 175U));
        s->pixels[head] = p;
        break;
    }
    case 3U: /* three colored comets chase around the strip, half a LED/frame */
    {
        for (uint32_t i = 0U; i < n; ++i)
        {
            LedStripPwmPixel p = {0, 0, 0};
            for (uint32_t k = 0U; k < 3U; ++k)
            {
                const uint32_t head = ((f % (n * 2U)) * 128U + k * n * 256U / 3U) % (n * 256U);
                const uint32_t behind = (head + n * 256U - i * 256U) % (n * 256U);
                const uint32_t glow = LedFxGlow(behind, 1024U);
                const LedStripPwmPixel color = LedFxWheel((f % 768U) + k * 256U);
                p.red = LedFxAdd(p.red, LedFxScale(color.red, glow));
                p.green = LedFxAdd(p.green, LedFxScale(color.green, glow));
                p.blue = LedFxAdd(p.blue, LedFxScale(color.blue, glow));
            }
            s->pixels[i] = p;
        }
        break;
    }
    case 4U: /* deliver colored beads, stack at the far end, hold then fade */
    {
        uint32_t travel = 0U;
        for (uint32_t m = 1U; m <= n; ++m) travel += m < 10U ? m : 10U;
        const uint32_t cycle = f / (travel + 52U);
        const uint32_t phase = f % (travel + 52U);
        uint32_t remaining = n, step = phase, head = 0U, glow = 255U;
        if (phase < travel)
        {
            while (remaining > 0U)
            {
                const uint32_t steps = remaining < 10U ? remaining : 10U;
                if (step < steps)
                {
                    head = steps > 1U ? step * (remaining - 1U) / (steps - 1U) : 0U;
                    break;
                }
                step -= steps;
                --remaining;
            }
        }
        else remaining = 0U;
        if (phase >= travel + 20U) glow = (travel + 51U - phase) * 255U / 31U;
        for (uint32_t i = 0U; i < n; ++i)
        {
            const uint32_t moving = remaining > 0U && i == head;
            const LedStripPwmPixel color = LedFxWheel((cycle % 768U) * 97U + (moving ? remaining - 1U : i) * 59U);
            const uint32_t level = i >= remaining || moving ? glow : 0U;
            s->pixels[i].red = LedFxScale(color.red, level);
            s->pixels[i].green = LedFxScale(color.green, level);
            s->pixels[i].blue = LedFxScale(color.blue, level);
        }
        break;
    }
    case 5U: /* starlight: independently phased stars, a new palette each twinkle */
        for (uint32_t i = 0U; i < n; ++i)
        {
            const uint32_t offset = (i * 37U + i * i * 11U) % 128U;
            const uint32_t phase = (f + offset) % 128U;
            const uint32_t cycle = (f + offset) / 128U;
            const uint32_t glow = phase < 32U ? LedFxWave(phase * 16U) : 0U;
            const uint32_t hue = (i * 97U + cycle * 151U + 400U) % 768U;
            LedStripPwmPixel p = LedFxWheel(hue);
            s->pixels[i].red = LedFxScale(64U + LedFxScale(p.red, 191U), glow);
            s->pixels[i].green = LedFxScale(64U + LedFxScale(p.green, 191U), glow);
            s->pixels[i].blue = (uint8_t)(3U + LedFxScale(64U + LedFxScale(p.blue, 188U), glow));
        }
        break;
    case 6U: /* cyan/pink collision, white core, expanding rainbow shockwave */
    {
        const uint32_t phase = f % 128U;
        const uint32_t center = (n - 1U) * 128U; /* 256 units per LED */
        for (uint32_t i = 0U; i < n; ++i)
        {
            const uint32_t x = i * 256U;
            LedStripPwmPixel p = {0, 0, 0};
            if (phase < 60U)
            {
                const uint32_t head = phase * center / 59U;
                const uint32_t mirror = (n - 1U) * 256U - x;
                const uint32_t cyan = LedFxGlow(LedFxDistance(x, head), x > head ? 256U : 1024U);
                const uint32_t pink = LedFxGlow(LedFxDistance(mirror, head), mirror > head ? 256U : 1024U);
                p.red = LedFxAdd(LedFxScale(cyan, 32U), pink);
                p.green = LedFxAdd(cyan, LedFxScale(pink, 32U));
                p.blue = LedFxAdd(cyan, pink);
            }
            else if (phase < 72U)
            {
                const uint32_t glow = LedFxScale(LedFxGlow(LedFxDistance(x, center), 1024U),
                                                255U - (phase - 60U) * 12U);
                p.red = (uint8_t)glow;
                p.green = (uint8_t)glow;
                p.blue = (uint8_t)glow;
            }
            else
            {
                const uint32_t radius = phase < 112U ? (phase - 72U) * center / 39U : center;
                uint32_t glow = LedFxGlow(LedFxDistance(LedFxDistance(x, center), radius), 384U);
                if (phase >= 112U) glow = LedFxScale(glow, (127U - phase) * 255U / 16U);
                const LedStripPwmPixel color = LedFxWheel((f % 768U) * 5U + i * 21U);
                p.red = LedFxScale(color.red, glow);
                p.green = LedFxScale(color.green, glow);
                p.blue = LedFxScale(color.blue, glow);
            }
            s->pixels[i] = p;
        }
        break;
    }
    default: break;
    }

    /* Always return 30 pixels: clear a previously longer effect's tail. */
    for (uint32_t i = 0U; i < LEDSTRIP_PWM_MAX_LEDS; ++i)
    {
        out[i].red = i < n ? LedFxScale(s->pixels[i].red, s->brightness) : 0U;
        out[i].green = i < n ? LedFxScale(s->pixels[i].green, s->brightness) : 0U;
        out[i].blue = i < n ? LedFxScale(s->pixels[i].blue, s->brightness) : 0U;
    }
    ++s->frame;
}

/* One frame maximum per call; interrupts and the rest of the app keep running. */
FX_INLINE uint8_t LedFxTick(LedFxState *s, uint32_t now, LedStripPwmPixel *out)
{
    if (!s || !out || !s->running) return 0U;
    if (s->demo && (uint32_t)(now - s->demo_ms) >= LED_FX_DEMO_MS)
    {
        s->effect = (uint8_t)(s->effect % LED_FX_COUNT + 1U);
        if (s->auto_brightness) s->brightness = LedFxDefaultBrightness(s->effect);
        s->demo_ms = now;
        LedFxResetHistory(s);
        s->last_ms = now - s->interval_ms;
    }
    if ((uint32_t)(now - s->last_ms) < s->interval_ms) return 0U;
    s->last_ms = now;
    LedFxRender(s, out);
    return 1U;
}
#undef FX_INLINE
#endif
