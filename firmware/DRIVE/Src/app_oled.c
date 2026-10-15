#include "app_oled.h"

/*
 * 模块：OLED 显示屏预留控制脚
 *
 * 当前只管理 RES/DC/CS 三个 GPIO，不主动初始化屏幕，也不主动占用 SPI。
 * 后续真正接屏时，建议在本文件继续扩展 OLED 初始化、写命令、写数据等接口。
 *
 * 引脚：
 * - OLED_RES = PA0
 * - OLED_DC  = PA1
 * - OLED_CS  = PA4
 *
 * 注意：
 * PA4 已确认是 OLED_CS，不要再给灯带或其他模块复用。
 */

/* OLED_RES 对应 PA0，用于复位屏幕控制器。 */
#define APP_OLED_RES_GPIO_Port GPIOA
#define APP_OLED_RES_Pin       GPIO_PIN_0

/* OLED_DC 对应 PA1，用于区分“命令字节”和“显示数据字节”。 */
#define APP_OLED_DC_GPIO_Port  GPIOA
#define APP_OLED_DC_Pin        GPIO_PIN_1

/* OLED_CS 对应 PA4，用于片选屏幕；PA4 后续不要再复用给其他模块。 */
#define APP_OLED_CS_GPIO_Port  GPIOA
#define APP_OLED_CS_Pin        GPIO_PIN_4

/* 设置 OLED 预留脚到安全空闲态。
 * 空闲态选择 RES=1、DC=1、CS=1，避免未接屏时误选中设备。
 */
void App_OledSetIdlePins(void)
{
    /* OLED 的安全空闲态：释放复位、默认数据模式、片选拉高不选中。 */
    HAL_GPIO_WritePin(APP_OLED_RES_GPIO_Port, APP_OLED_RES_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(APP_OLED_DC_GPIO_Port, APP_OLED_DC_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(APP_OLED_CS_GPIO_Port, APP_OLED_CS_Pin, GPIO_PIN_SET);
}

/* 直接设置 OLED 三个控制脚。
 * CLI 的 oled pins 命令使用它做低压连通性测试。
 */
void App_OledSetPins(GPIO_PinState res, GPIO_PinState dc, GPIO_PinState cs)
{
    HAL_GPIO_WritePin(APP_OLED_RES_GPIO_Port, APP_OLED_RES_Pin, res);
    HAL_GPIO_WritePin(APP_OLED_DC_GPIO_Port, APP_OLED_DC_Pin, dc);
    HAL_GPIO_WritePin(APP_OLED_CS_GPIO_Port, APP_OLED_CS_Pin, cs);
}

/* 输出 OLED 复位低脉冲。
 * low_ms 会限制在 1..200ms 内，防止误输入导致长时间拉低。
 */
void App_OledResetPulse(uint32_t low_ms)
{
    if (low_ms == 0UL)
    {
        low_ms = 10UL;
    }
    if (low_ms > 200UL)
    {
        low_ms = 200UL;
    }

    HAL_GPIO_WritePin(APP_OLED_CS_GPIO_Port, APP_OLED_CS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(APP_OLED_RES_GPIO_Port, APP_OLED_RES_Pin, GPIO_PIN_RESET);
    HAL_Delay(low_ms);
    HAL_GPIO_WritePin(APP_OLED_RES_GPIO_Port, APP_OLED_RES_Pin, GPIO_PIN_SET);
    HAL_Delay(10UL);
}

/* 读取 OLED 三个控制脚当前电平，供 oled status 打印。 */
AppOledPins App_OledReadPins(void)
{
    AppOledPins pins;

    pins.res = HAL_GPIO_ReadPin(APP_OLED_RES_GPIO_Port, APP_OLED_RES_Pin);
    pins.dc = HAL_GPIO_ReadPin(APP_OLED_DC_GPIO_Port, APP_OLED_DC_Pin);
    pins.cs = HAL_GPIO_ReadPin(APP_OLED_CS_GPIO_Port, APP_OLED_CS_Pin);

    return pins;
}
