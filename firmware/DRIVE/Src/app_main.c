#include "main.h"
#include "adc.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

#include "app_usart.h"
#include "app_ledstrip.h"
#include "bottom.h"
#include "hv507_api.h"

void SystemClock_Config(void);

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    /* 外设初始化顺序尽量保持固定，避免后续接手的人因为随意调整顺序引入问题。 */
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_USART3_UART_Init();
    MX_SPI3_Init();
    MX_TIM6_Init();
    MX_USART1_UART_Init();
    MX_ADC1_Init();
    MX_ADC2_Init();
    MX_I2C1_Init();
    /* Dedicated PWM driver: PB15 stays AF3, with zero duty between frames. */
    if (App_LedStripInit() != HAL_OK)
    {
        Error_Handler();
    }
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_USART2_UART_Init();
    MX_UART4_Init();

    /* 应用层初始化顺序：
     * 1. 先起输入与 ADC 侧
     * 2. 再起高压执行侧
     * 3. 最后起串口 CLI / 协议 / 安全状态机
     */
    Bottom_Init();
    HV507_Init(&hv507);
    App_Init();

    while (1)
    {
        /* 主循环里三个核心服务分别负责：
         * - Bottom_Service：输入与反馈刷新
         * - HV507_Service：阵列逻辑延迟提交
         * - App_Service：CLI、协议、安全状态机
         */
        Bottom_Service();
        HV507_Service(&hv507);
        App_Service();
        /* At most one LED frame, paced by HAL tick; no animation delay loop. */
        App_LedStripEffectService();
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)
    {
        /* TIM3 是当前系统里最关键的节拍：
         * - HV507 AC 翻转
         * - 同步采样触发
         */
        HV507_OnTimerTick(&hv507);
        Bottom_OnSyncTick();
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 8;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 4;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
    {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
