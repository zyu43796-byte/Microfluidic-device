#ifndef APP_LEDSTRIP_PWM_FRAME_H
#define APP_LEDSTRIP_PWM_FRAME_H
#include <stdint.h>

/* Reference: 72 MHz / 90 counts, high times 30 / 59 counts.
 * F407 TIM8: 168 MHz / 210 counts, corresponding highs 70 / 138.
 */
#define LEDSTRIP_PWM_TIMER_HZ   168000000U
#define LEDSTRIP_PWM_PERIOD     210U
#define LEDSTRIP_PWM_ZERO_HIGH  70U
#define LEDSTRIP_PWM_ONE_HIGH   138U
#define LEDSTRIP_PWM_MAX_LEDS   30U
#define LEDSTRIP_PWM_HEAD_SLOTS 800U
#define LEDSTRIP_PWM_TAIL_SLOTS 802U
#define LEDSTRIP_PWM_CAPACITY   (LEDSTRIP_PWM_HEAD_SLOTS + LEDSTRIP_PWM_MAX_LEDS * 24U + LEDSTRIP_PWM_TAIL_SLOTS)

typedef struct { uint8_t red, green, blue; } LedStripPwmPixel;

#ifdef __cplusplus
#define LEDSTRIP_INLINE static constexpr
#else
#define LEDSTRIP_INLINE static inline
#endif

LEDSTRIP_INLINE uint32_t LedStripPwmFrameSlots(uint32_t count)
{
    return LEDSTRIP_PWM_HEAD_SLOTS + count * 24U + LEDSTRIP_PWM_TAIL_SLOTS;
}

LEDSTRIP_INLINE uint32_t LedStripPwmEncodeByte(uint16_t *out, uint32_t pos, uint8_t value)
{
    for (uint32_t mask = 128U; mask != 0U; mask >>= 1U)
        out[pos++] = (value & mask) ? LEDSTRIP_PWM_ONE_HIGH : LEDSTRIP_PWM_ZERO_HIGH;
    return pos;
}

/* Order numbers match AppLedStripOrder. No memory is written on invalid input. */
LEDSTRIP_INLINE uint32_t LedStripPwmBuildFrame(uint16_t *out, uint32_t capacity,
                                            const LedStripPwmPixel *pixels,
                                            uint32_t count, uint32_t order)
{
    if (!out || !pixels || count == 0U || count > LEDSTRIP_PWM_MAX_LEDS ||
        order > 5U || capacity < LedStripPwmFrameSlots(count))
        return 0U;
    const uint32_t slots = LedStripPwmFrameSlots(count);
    for (uint32_t i = 0U; i < slots; ++i) out[i] = 0U;
    uint32_t pos = LEDSTRIP_PWM_HEAD_SLOTS;
    for (uint32_t i = 0U; i < count; ++i)
    {
        const uint8_t r = pixels[i].red, g = pixels[i].green, b = pixels[i].blue;
        uint8_t first = g, second = r, third = b;
        switch (order)
        {
        case 1U: first = b; second = r; third = g; break;
        case 2U: first = r; second = g; third = b; break;
        case 3U: first = g; second = b; third = r; break;
        case 4U: first = r; second = b; third = g; break;
        case 5U: first = b; second = g; third = r; break;
        default: break;
        }
        pos = LedStripPwmEncodeByte(out, pos, first);
        pos = LedStripPwmEncodeByte(out, pos, second);
        pos = LedStripPwmEncodeByte(out, pos, third);
    }
    return slots;
}

/* FNV-1a over little-endian halfwords, for repeated-frame diagnostics. */
LEDSTRIP_INLINE uint32_t LedStripPwmFrameHash(const uint16_t *frame, uint32_t slots)
{
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0U; i < slots; ++i)
    {
        hash = (hash ^ (frame[i] & 255U)) * 16777619U;
        hash = (hash ^ (frame[i] >> 8U)) * 16777619U;
    }
    return hash;
}
#undef LEDSTRIP_INLINE
#endif
