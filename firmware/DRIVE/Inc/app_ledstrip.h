#ifndef APP_LEDSTRIP_H
#define APP_LEDSTRIP_H
#include "main.h"
#include "app_ledstrip_pwm_frame.h"
#ifdef __cplusplus
extern "C" {
#endif

#define APP_LEDSTRIP_MAX_LEDS LEDSTRIP_PWM_MAX_LEDS
#define APP_LEDSTRIP_BUILD "pwm_fx_v4_core"

typedef struct
{
    uint8_t count, red, green, blue; /* last successfully sent command */
} AppLedStripState;

typedef enum
{
    APP_LEDSTRIP_ORDER_GRB = 0, APP_LEDSTRIP_ORDER_BRG,
    APP_LEDSTRIP_ORDER_RGB, APP_LEDSTRIP_ORDER_GBR,
    APP_LEDSTRIP_ORDER_RBG, APP_LEDSTRIP_ORDER_BGR
} AppLedStripOrder;

typedef struct
{
    uint32_t sequence, completed;
    uint32_t timer_hz, interval_ms, elapsed_ms;
    uint32_t frame_hash, after_hash, dma_flags;
    uint16_t slots, remaining;
    HAL_StatusTypeDef result;
} AppLedStripPwmDiagnostics;

extern AppLedStripState g_app_ledstrip;

typedef struct
{
    uint8_t effect, running, demo, count, brightness;
    uint16_t interval_ms;
    uint32_t frame;
    HAL_StatusTypeDef result;
} AppLedStripEffectStatus;

/* Effects 1..6, active count 1..30, interval 10..1000ms. No automatic startup.
 * use_default_brightness=1 selects 112 for meteor, 80 otherwise, also in demo.
 * When zero, the supplied brightness is respected throughout demo rotation. */
HAL_StatusTypeDef App_LedStripEffectStart(uint8_t effect, uint8_t count,
                                        uint8_t brightness, uint16_t interval_ms, uint8_t demo,
                                        uint8_t use_default_brightness);
void App_LedStripEffectService(void);
void App_LedStripEffectCancel(void); /* freeze, without changing displayed colors */
void App_LedStripGetEffectStatus(AppLedStripEffectStatus *status);
const char *App_LedStripEffectName(uint8_t effect);

/* Only PWM + DMA is supported. PB15 stays AF3/TIM8_CH3N between updates. */
HAL_StatusTypeDef App_LedStripInit(void);
HAL_StatusTypeDef App_LedStripFill(uint8_t count, uint8_t red, uint8_t green, uint8_t blue);
HAL_StatusTypeDef App_LedStripClear(uint8_t count);
HAL_StatusTypeDef App_LedStripOne(uint8_t count, uint8_t index, uint8_t red, uint8_t green, uint8_t blue);
void App_LedStripGetPwmDiagnostics(AppLedStripPwmDiagnostics *diagnostics);
void App_LedStripSetOrder(AppLedStripOrder order);
AppLedStripOrder App_LedStripGetOrder(void);
const char *App_LedStripOrderName(AppLedStripOrder order);

#ifdef __cplusplus
}
#endif
#endif
