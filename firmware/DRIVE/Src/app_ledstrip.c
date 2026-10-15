#include "app_ledstrip.h"
#include <string.h>

/*
 * WS2812B: set pixel colors -> encode GRB -> update PWM/DMA.
 * Reference project uses F103 PB7/TIM4_CH2. This board is F407 PB15/TIM8_CH3N.
 * TIM8_UP -> DMA2 Stream1 Channel7 -> CCR3, 16-bit normal-mode transfers.
 * PB15 is configured once; no SPI, GPIO bit-banging or repeated black frames.
 */
#define PWM_DMA_ERRORS (DMA_LISR_TEIF1 | DMA_LISR_DMEIF1 | DMA_LISR_FEIF1)
#define PWM_DMA_FLAGS  (PWM_DMA_ERRORS | DMA_LISR_TCIF1 | DMA_LISR_HTIF1)
#define PWM_DMA_CLEAR  (DMA_LIFCR_CTCIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CFEIF1)
#define PWM_MODE       (TIM_CCMR2_OC3M | TIM_CCMR2_OC3PE) /* PWM2 */
#define PWM_FORCE_LOW  (TIM_CCMR2_OC3M_2 | TIM_CCMR2_OC3M_0 | TIM_CCMR2_OC3PE)

AppLedStripState g_app_ledstrip;
static AppLedStripPwmDiagnostics s_diag;
static AppLedStripOrder s_order = APP_LEDSTRIP_ORDER_GRB;
static LedStripPwmPixel s_pixels[APP_LEDSTRIP_MAX_LEDS];
/* Must stay in DMA-accessible SRAM, not CCM. Verify address in linker map. */
static uint16_t s_frame[LEDSTRIP_PWM_CAPACITY];
static uint8_t s_initialized;
static uint8_t s_busy;
static uint32_t s_last_start;

static uint32_t ledstrip_timer_clock(void)
{
    uint32_t clock = HAL_RCC_GetPCLK2Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE2) != 0U) clock *= 2U;
    return clock;
}

static HAL_StatusTypeDef ledstrip_dma_stop(void)
{
    const uint32_t start = HAL_GetTick();
    DMA2_Stream1->CR &= ~DMA_SxCR_EN;
    while ((DMA2_Stream1->CR & DMA_SxCR_EN) != 0U)
        if ((HAL_GetTick() - start) > 5U) return HAL_TIMEOUT;
    return HAL_OK;
}

/* CH3+CH3N form an internal complementary pair; only PB15/CH3N is muxed.
 * Force OC3REF high to make CH3N low before changing counter/preload state.
 * No pin mux changes and no timer resets occur between frames.
 */
static HAL_StatusTypeDef ledstrip_idle_low(void)
{
    TIM8->DIER = 0U;
    TIM8->CCMR2 = PWM_FORCE_LOW;
    TIM8->CR1 &= ~TIM_CR1_CEN;
    HAL_StatusTypeDef result = ledstrip_dma_stop();
    if (result != HAL_OK) return result; /* remain forced LOW, do not reuse buffer */
    TIM8->CCR3 = 0U;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->SR = 0U;
    TIM8->CCMR2 = PWM_MODE;
    TIM8->CR1 |= TIM_CR1_CEN; /* zero duty continuously, including between commands */
    DMA2->LIFCR = PWM_DMA_CLEAR;
    return HAL_OK;
}

HAL_StatusTypeDef App_LedStripInit(void)
{
    GPIO_InitTypeDef gpio = {0};
    if (s_initialized) return HAL_OK;
    if (__get_IPSR() != 0U || __get_PRIMASK() != 0U) return HAL_BUSY;
    s_diag.timer_hz = ledstrip_timer_clock();
    if (s_diag.timer_hz != LEDSTRIP_PWM_TIMER_HZ)
    {
        s_diag.result = HAL_ERROR;
        return HAL_ERROR;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIOB->BSRR = (uint32_t)GPIO_PIN_15 << 16U;
    gpio.Pin = GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    __HAL_RCC_TIM8_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    /* TIM8 and DMA2 Stream1 are dedicated to this driver. */
    __HAL_RCC_TIM8_FORCE_RESET();
    __HAL_RCC_TIM8_RELEASE_RESET();
    if (ledstrip_dma_stop() != HAL_OK)
    {
        s_diag.result = HAL_TIMEOUT;
        return HAL_TIMEOUT;
    }
    TIM8->PSC = 0U;
    TIM8->ARR = LEDSTRIP_PWM_PERIOD - 1U;
    TIM8->RCR = 0U;
    TIM8->CR1 = TIM_CR1_ARPE;
    TIM8->CR2 = 0U;
    TIM8->CCMR2 = PWM_MODE;
    TIM8->CCER = TIM_CCER_CC3E | TIM_CCER_CC3NE;
    TIM8->CCR3 = 0U;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->SR = 0U;
    TIM8->BDTR = TIM_BDTR_OSSI | TIM_BDTR_OSSR | TIM_BDTR_MOE;
    TIM8->CR1 |= TIM_CR1_CEN;

    DMA2_Stream1->CR = DMA_CHANNEL_7 | DMA_SxCR_DIR_0 | DMA_SxCR_MINC |
                      DMA_SxCR_PSIZE_0 | DMA_SxCR_MSIZE_0 | DMA_SxCR_PL_1;
    DMA2_Stream1->FCR = 0U;
    DMA2_Stream1->PAR = (uint32_t)&TIM8->CCR3;
    DMA2->LIFCR = PWM_DMA_CLEAR;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Alternate = GPIO_AF3_TIM8;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_Delay(2U); /* startup reset, does not reconfigure SysTick */
    s_initialized = 1U;
    s_diag.result = HAL_OK;
    return HAL_OK;
}

/* Claim the buffer BEFORE changing any pixel data. No callers run in IRQs. */
static HAL_StatusTypeDef ledstrip_begin(uint8_t count)
{
    if (count == 0U || count > APP_LEDSTRIP_MAX_LEDS) return HAL_ERROR;
    if (__get_IPSR() != 0U || __get_PRIMASK() != 0U || s_busy) return HAL_BUSY;
    HAL_StatusTypeDef result = App_LedStripInit();
    if (result != HAL_OK) return result;
    s_busy = 1U;
    memset(s_pixels, 0, sizeof(s_pixels));
    return HAL_OK;
}

static HAL_StatusTypeDef ledstrip_update(uint8_t count, uint8_t red, uint8_t green, uint8_t blue)
{
    const uint32_t now = HAL_GetTick();
    s_diag.interval_ms = s_diag.sequence ? now - s_last_start : 0U;
    s_last_start = now;
    ++s_diag.sequence;
    s_diag.dma_flags = 0U;
    s_diag.remaining = 0U;
    s_diag.elapsed_ms = 0U;
    s_diag.after_hash = 0U;
    s_diag.timer_hz = ledstrip_timer_clock();
    s_diag.slots = (uint16_t)LedStripPwmBuildFrame(s_frame, LEDSTRIP_PWM_CAPACITY,
                                                s_pixels, count, (uint32_t)s_order);
    s_diag.frame_hash = LedStripPwmFrameHash(s_frame, s_diag.slots);
    HAL_StatusTypeDef result = HAL_ERROR;
    if (s_diag.slots == 0U || s_diag.timer_hz != LEDSTRIP_PWM_TIMER_HZ)
        goto finish;

    result = ledstrip_idle_low();
    if (result != HAL_OK) goto finish;
    /* Stop at known zero duty, reset counter and preload together. */
    TIM8->CR1 &= ~TIM_CR1_CEN;
    TIM8->CCR3 = 0U;
    TIM8->EGR = TIM_EGR_UG; /* UDE remains disabled during software update */
    TIM8->SR = 0U;
    DMA2_Stream1->M0AR = (uint32_t)s_frame;
    DMA2_Stream1->NDTR = s_diag.slots;
    DMA2->LIFCR = PWM_DMA_CLEAR;
    __DMB();
    DMA2_Stream1->CR |= DMA_SxCR_EN;
    TIM8->DIER = TIM_DIER_UDE;
    TIM8->CR1 |= TIM_CR1_CEN;

    for (;;)
    {
        const uint32_t flags = DMA2->LISR & PWM_DMA_FLAGS;
        if (flags & PWM_DMA_ERRORS) { result = HAL_ERROR; break; }
        if (flags & DMA_LISR_TCIF1) { result = HAL_OK; break; }
        if ((HAL_GetTick() - now) > 20U) { result = HAL_TIMEOUT; break; }
    }
    s_diag.dma_flags = DMA2->LISR & PWM_DMA_FLAGS;
    s_diag.remaining = (uint16_t)DMA2_Stream1->NDTR;
finish:
    /* 802 trailing zeros include two preload stages: >=1ms LOW before DMA TC.
     * Error cleanup also forces LOW, even if a partial data frame was sent.
     */
    {
        HAL_StatusTypeDef stop = ledstrip_idle_low();
        if (stop != HAL_OK) result = stop;
    }
    s_diag.after_hash = LedStripPwmFrameHash(s_frame, s_diag.slots);
    if (s_diag.after_hash != s_diag.frame_hash) result = HAL_ERROR;
    s_diag.elapsed_ms = HAL_GetTick() - now;
    s_diag.result = result;
    if (result == HAL_OK)
    {
        ++s_diag.completed;
        g_app_ledstrip.count = count;
        g_app_ledstrip.red = red;
        g_app_ledstrip.green = green;
        g_app_ledstrip.blue = blue;
    }
    /* If DMA failed to stop, retain ownership: never overwrite its active buffer. */
    s_busy = (DMA2_Stream1->CR & DMA_SxCR_EN) ? 1U : 0U;
    return result;
}

HAL_StatusTypeDef App_LedStripFill(uint8_t count, uint8_t red, uint8_t green, uint8_t blue)
{
    HAL_StatusTypeDef result = ledstrip_begin(count);
    if (result != HAL_OK) return result;
    App_LedStripEffectCancel();
    for (uint8_t i = 0U; i < count; ++i)
    {
        s_pixels[i].red = red;
        s_pixels[i].green = green;
        s_pixels[i].blue = blue;
    }
    return ledstrip_update(count, red, green, blue);
}

HAL_StatusTypeDef App_LedStripClear(uint8_t count)
{
    return App_LedStripFill(count, 0U, 0U, 0U);
}

HAL_StatusTypeDef App_LedStripOne(uint8_t count, uint8_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (index >= count) return HAL_ERROR;
    HAL_StatusTypeDef result = ledstrip_begin(count);
    if (result != HAL_OK) return result;
    App_LedStripEffectCancel();
    s_pixels[index].red = red;
    s_pixels[index].green = green;
    s_pixels[index].blue = blue;
    return ledstrip_update(count, red, green, blue);
}

void App_LedStripGetPwmDiagnostics(AppLedStripPwmDiagnostics *diagnostics)
{
    if (diagnostics) *diagnostics = s_diag;
}

void App_LedStripSetOrder(AppLedStripOrder order)
{
    if ((unsigned)order <= (unsigned)APP_LEDSTRIP_ORDER_BGR && !s_busy) s_order = order;
}

AppLedStripOrder App_LedStripGetOrder(void) { return s_order; }

#include "app_ledstrip_effects.inc"

const char *App_LedStripOrderName(AppLedStripOrder order)
{
    switch (order)
    {
    case APP_LEDSTRIP_ORDER_BRG: return "brg";
    case APP_LEDSTRIP_ORDER_RGB: return "rgb";
    case APP_LEDSTRIP_ORDER_GBR: return "gbr";
    case APP_LEDSTRIP_ORDER_RBG: return "rbg";
    case APP_LEDSTRIP_ORDER_BGR: return "bgr";
    default: return "grb";
    }
}
