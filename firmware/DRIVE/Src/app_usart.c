#include "app_usart_internal.h"

#include "bottom.h"
#include "hv507_api.h"

#include <stdio.h>
#include <string.h>

/*
 * 模块：串口应用层总控
 *
 * 主要职责：
 * 1. 管理 USART1 CLI、USART3 加热板、USART2 预留显示/辅助口、UART4 荧光模块。
 * 2. 维护系统安全状态，例如 arm 窗口、急停原因、2 号板在线状态。
 * 3. 在主循环 App_Service() 中做协议超时、周期 ping/status、行缓冲分发。
 *
 * 修改建议：
 * - 改串口接收分发：看 App_OnUartRxCplt()。
 * - 改系统周期任务：看 App_Service()。
 * - 改急停/安全门控：看 App_AllStop() 和 App_CanExecuteDangerous()。
 */

/* USART1：本机 CLI。 */
UART_HandleTypeDef huart1;
/* USART2：预留给 OLED/辅助调试口。 */
UART_HandleTypeDef huart2;
/* USART3：2 号板加热控制链路。 */
UART_HandleTypeDef huart3;
/* UART4：荧光模块通信口。 */
UART_HandleTypeDef huart4;

/* 2 号板链路缓存状态。 */
HeaterLinkState g_heater_link;
/* 系统总状态：arm、fault 等。 */
AppControlState g_app_state;
/* 荧光模块状态缓存。 */
FluoLinkState g_fluo_link;

/* 辅助串口最近一行文本。 */
char g_app_aux_last_line[APP_AUX_LAST_LINE_MAX];
/* 荧光最近一帧原始文本。 */
char g_app_fluo_last_line[APP_FLUO_LAST_LINE_MAX];
/* 磁铁/推杆功能是否可用。 */
uint8_t g_app_magnet_available = 1U;

/* USART1 CLI 行缓冲，接收 PC 调试工具发送的命令。 */
static UartLineBuffer s_cli_rx;

/* USART3 加热板行缓冲，接收 2 号板 ASCII 协议返回。 */
static UartLineBuffer s_heater_rx;

/* USART2 辅助/预留显示口行缓冲，当前主要保留低压调试用途。 */
static UartLineBuffer s_aux_rx;

/* UART4 荧光模块二进制帧缓冲，固定收 10 字节一帧。 */
static FluoFrameBuffer s_fluo_rx;

/* 上一次向 2 号板发送 PING 的时间，离线重连探测用。 */
static uint32_t s_last_heater_ping_ms = 0U;

/* 上一次向 2 号板发送 READ_STATUS 的时间，在线状态轮询用。 */
static uint32_t s_last_heater_status_ms = 0U;

/* 上一次同步采样 overrun 计数，用于只在变化时上报。 */
static uint32_t s_last_sync_overrun_count = 0U;

/* 2 号板在线时，3 号板每 1000ms 自动读一次状态缓存。 */
#define APP_HEATER_STATUS_PERIOD_MS   1000U

/* 2 号板离线时，3 号板每 1000ms 自动发一次 PING 探测。 */
#define APP_HEATER_PING_PERIOD_MS     1000U

/* 超过 10 秒没有收到 2 号板响应，认为加热链路掉线并上报故障。 */
#define APP_HEATER_LINK_TIMEOUT_MS   10000U

/* 命令发出后 1500ms 没回包，清 pending 并记录一次传输超时。 */
#define APP_HEATER_PENDING_TIMEOUT_MS 1500U

/* 应用层初始化。
 * 先清空状态，再打开串口接收中断，避免把上电瞬间的脏数据当成完整命令。
 */
void App_Init(void)
{
    /* 串口接收中断在这里统一打开，保证应用层初始化完成后再开始收数据。 */
    memset(&g_heater_link, 0, sizeof(g_heater_link));
    memset(&g_app_state, 0, sizeof(g_app_state));
    memset(&g_fluo_link, 0, sizeof(g_fluo_link));
    memset(&s_cli_rx, 0, sizeof(s_cli_rx));
    memset(&s_heater_rx, 0, sizeof(s_heater_rx));
    memset(&s_aux_rx, 0, sizeof(s_aux_rx));
    memset(&s_fluo_rx, 0, sizeof(s_fluo_rx));
    g_app_aux_last_line[0] = '\0';
    g_app_fluo_last_line[0] = '\0';
    (void)snprintf(g_app_state.last_stop_reason, sizeof(g_app_state.last_stop_reason), "%s", "boot");

    HAL_UART_Receive_IT(&huart1, &s_cli_rx.rx_byte, 1U);
    HAL_UART_Receive_IT(&huart2, &s_aux_rx.rx_byte, 1U);
    HAL_UART_Receive_IT(&huart3, &s_heater_rx.rx_byte, 1U);
    HAL_UART_Receive_IT(&huart4, &s_fluo_rx.rx_byte, 1U);
}

/* 应用层主服务。
 * 这里统一处理 CLI、heater、aux、fluo、超时等周期性任务。
 */
void App_Service(void)
{
    /* 当前大部分协议、故障、超时和自检都在这里统一调度。 */
    uint32_t now = HAL_GetTick();

    if (s_cli_rx.ready)
    {
        App_ProcessCliLine(&s_cli_rx);
    }

    if (s_heater_rx.ready)
    {
        App_ProcessHeaterLine(&s_heater_rx);
    }

    if (s_aux_rx.ready)
    {
        App_ProcessAuxLine(&s_aux_rx);
    }

    if (s_fluo_rx.ready)
    {
        App_ProcessFluoFrame(&s_fluo_rx);
    }

    if ((g_app_state.armed != 0U) &&
        (g_app_state.arm_deadline_ms != 0U) &&
        ((int32_t)(now - g_app_state.arm_deadline_ms) >= 0))
    {
        App_AllStop("arm timeout");
    }

    if (Bottom_GetSyncOverrunCount() != s_last_sync_overrun_count)
    {
        s_last_sync_overrun_count = Bottom_GetSyncOverrunCount();
        App_ReportFault(APP_FAULT_SYNC_OVERRUN, "synchronized sampling overrun");
        if ((g_app_state.armed != 0U) || (hv507.mode != HV507_MODE_OFF))
        {
            App_AllStop("sync overrun");
        }
    }

    if (g_app_state.heater_rs232_verified)
    {
        if ((g_heater_link.pending != 0U) &&
            ((now - g_heater_link.last_tx_ms) > APP_HEATER_PENDING_TIMEOUT_MS))
        {
            g_heater_link.pending = 0U;
            g_heater_link.transport_error_count++;
            App_ReportFault(APP_FAULT_HEATER_LINK_TIMEOUT, "heater pending timeout");
        }

        if (g_heater_link.online)
        {
            if ((now - g_heater_link.last_rx_ms) > APP_HEATER_LINK_TIMEOUT_MS)
            {
                g_heater_link.online = 0U;
                g_heater_link.pending = 0U;
                App_ReportFault(APP_FAULT_HEATER_LINK_TIMEOUT, "heater timeout");

                if (hv507.mode != HV507_MODE_OFF)
                {
                    App_AllStop("heater timeout");
                }
            }
            else if ((g_heater_link.pending == 0U) &&
                     ((now - s_last_heater_status_ms) > APP_HEATER_STATUS_PERIOD_MS))
            {
                App_HeaterSendBody("READ_STATUS");
                s_last_heater_status_ms = now;
            }
        }
        else if ((g_heater_link.pending == 0U) &&
                 ((now - s_last_heater_ping_ms) > APP_HEATER_PING_PERIOD_MS))
        {
            App_HeaterSendBody("PING");
            s_last_heater_ping_ms = now;
        }
    }

}

/* 串口接收完成回调。
 * 这里只负责把单字节推入对应缓冲，真正解析留给 App_Service()。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1)
    {
        App_UartLinePush(&s_cli_rx, s_cli_rx.rx_byte);
        HAL_UART_Receive_IT(&huart1, &s_cli_rx.rx_byte, 1U);
    }
    else if (huart == &huart2)
    {
        App_UartLinePush(&s_aux_rx, s_aux_rx.rx_byte);
        HAL_UART_Receive_IT(&huart2, &s_aux_rx.rx_byte, 1U);
    }
    else if (huart == &huart3)
    {
        App_UartLinePush(&s_heater_rx, s_heater_rx.rx_byte);
        HAL_UART_Receive_IT(&huart3, &s_heater_rx.rx_byte, 1U);
    }
    else if (huart == &huart4)
    {
        App_FluoFramePush(&s_fluo_rx, s_fluo_rx.rx_byte);
        HAL_UART_Receive_IT(&huart4, &s_fluo_rx.rx_byte, 1U);
    }
}

/* 串口错误回调。
 * 主要目的是在发生 ORE/FE/NE 后重新挂起接收，尽量保持链路不断流。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1)
    {
        HAL_UART_Receive_IT(&huart1, &s_cli_rx.rx_byte, 1U);
    }
    else if (huart == &huart2)
    {
        HAL_UART_Receive_IT(&huart2, &s_aux_rx.rx_byte, 1U);
    }
    else if (huart == &huart3)
    {
        g_heater_link.transport_error_count++;
        HAL_UART_Receive_IT(&huart3, &s_heater_rx.rx_byte, 1U);
    }
    else if (huart == &huart4)
    {
        HAL_UART_Receive_IT(&huart4, &s_fluo_rx.rx_byte, 1U);
    }
}

/* USART1 初始化：CLI 控制台。 */
void MX_USART1_UART_Init(void)
{
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        Error_Handler();
    }
}

/* USART2 初始化：预留显示/辅助串口。 */
void MX_USART2_UART_Init(void)
{
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK)
    {
        Error_Handler();
    }
}

/* UART4 初始化：荧光模块通信。 */
void MX_UART4_Init(void)
{
    huart4.Instance = UART4;
    huart4.Init.BaudRate = 115200;
    huart4.Init.WordLength = UART_WORDLENGTH_8B;
    huart4.Init.StopBits = UART_STOPBITS_1;
    huart4.Init.Parity = UART_PARITY_NONE;
    huart4.Init.Mode = UART_MODE_TX_RX;
    huart4.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart4.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart4) != HAL_OK)
    {
        Error_Handler();
    }
}

/* USART3 初始化：2 号板加热控制链路。 */
void MX_USART3_UART_Init(void)
{
    huart3.Instance = USART3;
    huart3.Init.BaudRate = 115200;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart3) != HAL_OK)
    {
        Error_Handler();
    }
}

/* UART 底层时钟和 GPIO 资源初始化。 */
void HAL_UART_MspInit(UART_HandleTypeDef *uartHandle)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (uartHandle->Instance == USART1)
    {
        /* USART1：PA9/PA10，接上位机调试串口。 */
        __HAL_RCC_USART1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_10;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
        HAL_NVIC_EnableIRQ(USART1_IRQn);
    }
    else if (uartHandle->Instance == USART2)
    {
        /* USART2：PD5/PD6，预留给显示/辅助接口 H6。 */
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_GPIOD_CLK_ENABLE();

        GPIO_InitStruct.Pin = GPIO_PIN_5 | GPIO_PIN_6;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
        HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

        HAL_NVIC_SetPriority(USART2_IRQn, 2, 0);
        HAL_NVIC_EnableIRQ(USART2_IRQn);
    }
    else if (uartHandle->Instance == USART3)
    {
        /* USART3：PD8/PD9，连接 2 号板加热控制链路。 */
        __HAL_RCC_USART3_CLK_ENABLE();
        __HAL_RCC_GPIOD_CLK_ENABLE();

        GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
        HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

        HAL_NVIC_SetPriority(USART3_IRQn, 1, 0);
        HAL_NVIC_EnableIRQ(USART3_IRQn);
    }
    else if (uartHandle->Instance == UART4)
    {
        /* UART4：PC10/PC11，连接荧光检测模块。 */
        __HAL_RCC_UART4_CLK_ENABLE();
        __HAL_RCC_GPIOC_CLK_ENABLE();

        GPIO_InitStruct.Pin = GPIO_PIN_10 | GPIO_PIN_11;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF8_UART4;
        HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

        HAL_NVIC_SetPriority(UART4_IRQn, 2, 1);
        HAL_NVIC_EnableIRQ(UART4_IRQn);
    }
}

/* UART 底层资源释放。 */
void HAL_UART_MspDeInit(UART_HandleTypeDef *uartHandle)
{
    if (uartHandle->Instance == USART1)
    {
        /* 释放 CLI 串口资源，通常只在重新初始化或低功耗场景使用。 */
        __HAL_RCC_USART1_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9 | GPIO_PIN_10);
        HAL_NVIC_DisableIRQ(USART1_IRQn);
    }
    else if (uartHandle->Instance == USART2)
    {
        /* 释放显示/辅助串口资源。 */
        __HAL_RCC_USART2_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOD, GPIO_PIN_5 | GPIO_PIN_6);
        HAL_NVIC_DisableIRQ(USART2_IRQn);
    }
    else if (uartHandle->Instance == USART3)
    {
        /* 释放 2 号板加热链路串口资源。 */
        __HAL_RCC_USART3_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOD, GPIO_PIN_8 | GPIO_PIN_9);
        HAL_NVIC_DisableIRQ(USART3_IRQn);
    }
    else if (uartHandle->Instance == UART4)
    {
        /* 释放荧光模块串口资源。 */
        __HAL_RCC_UART4_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOC, GPIO_PIN_10 | GPIO_PIN_11);
        HAL_NVIC_DisableIRQ(UART4_IRQn);
    }
}
