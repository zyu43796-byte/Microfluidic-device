#ifndef APP_OLED_H
#define APP_OLED_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* OLED_RES/PA0，复位脚状态。 */
    GPIO_PinState res;

    /* OLED_DC/PA1，命令/数据选择脚状态。 */
    GPIO_PinState dc;

    /* OLED_CS/PA4，片选脚状态；空闲应为高。 */
    GPIO_PinState cs;
} AppOledPins;

/* 设置 OLED 预留脚到安全空闲态：RES=1, DC=1, CS=1。 */
void App_OledSetIdlePins(void);

/* 直接设置三个 OLED 控制脚，用于接屏前低压诊断。 */
void App_OledSetPins(GPIO_PinState res, GPIO_PinState dc, GPIO_PinState cs);

/* 产生低电平复位脉冲，low_ms 会限制在 1..200ms 范围内。 */
void App_OledResetPulse(uint32_t low_ms);

/* 读取当前 OLED 控制脚状态，供 CLI oled status 打印。 */
AppOledPins App_OledReadPins(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_OLED_H */
