#include "esp_companion.h"
#include "temp_sensor.h"
#include "ui_manager.h"
#include "app_loader.h"
#include "FreeRTOS.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern UART_HandleTypeDef huart4;

#define ESP_RX_DMA_BUF_SIZE  256U
#define ESP_LINE_BUF_SIZE    128U
#define ESP_TX_BUF_SIZE      128U

static uint8_t  s_rx_dma_buf[ESP_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint16_t s_rx_read_ptr = 0U;

static char     s_line_buf[ESP_LINE_BUF_SIZE];
static uint16_t s_line_len = 0U;

static char     s_tx_buf[ESP_TX_BUF_SIZE] __attribute__((aligned(4)));

static volatile bool     s_is_connected = false;
static volatile uint32_t s_last_rx_tick = 0U;
static volatile uint32_t s_rx_packets  = 0U;
static volatile uint32_t s_tx_packets  = 0U;

static void Process_Incoming_Line(const char *line)
{
    if (line == NULL || line[0] == '\0')
    {
        return;
    }

    /* Remote theme control from ESP32 BOOT button: ESP32:SET_THM=<idx> */
    const char *thm_ptr = strstr(line, "THM=");
    if (thm_ptr != NULL)
    {
        int thm_val = atoi(thm_ptr + 4);
        if (thm_val >= 0 && thm_val < 4)
        {
            UIManager_SetThemeId((uint8_t)thm_val);
        }
        s_is_connected = true;
        s_last_rx_tick = HAL_GetTick();
        s_rx_packets++;
        return;
    }

    /* Heartbeat frame: ESP32C6:HB */
    if (strstr(line, "ESP") != NULL || strstr(line, ":HB") != NULL || strstr(line, "C6:") != NULL)
    {
        s_is_connected = true;
        s_last_rx_tick = HAL_GetTick();
        s_rx_packets++;
        return;
    }
}

void ESPCompanion_Init(void)
{
    s_is_connected = false;
    s_last_rx_tick = 0U;
    s_rx_packets   = 0U;
    s_tx_packets   = 0U;
    s_rx_read_ptr  = 0U;
    s_line_len     = 0U;
    memset(s_rx_dma_buf, 0, sizeof(s_rx_dma_buf));

    /* Ensure PA1 (UART4_RX) has GPIO_NOPULL as per CubeMX default */
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_UART4;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    if (huart4.Instance != NULL && huart4.hdmarx != NULL)
    {
        /* Clear any initial ORE/FE/NE flags left from boot/connecting wires */
        volatile uint32_t sr = huart4.Instance->SR;
        volatile uint32_t dr = huart4.Instance->DR;
        (void)sr;
        (void)dr;
        huart4.ErrorCode = HAL_UART_ERROR_NONE;
        huart4.RxState = HAL_UART_STATE_READY;

        (void)HAL_UART_Receive_DMA(&huart4, s_rx_dma_buf, sizeof(s_rx_dma_buf));
    }
}

void ESPCompanion_Poll(void)
{
    if (huart4.Instance == NULL || huart4.hdmarx == NULL)
    {
        return;
    }

    /* Auto-recovery: if overrun occurred, read DR and re-enable DMAR */
    if ((huart4.Instance->SR & USART_SR_ORE) != 0U)
    {
        volatile uint32_t sr = huart4.Instance->SR;
        volatile uint32_t dr = huart4.Instance->DR;
        (void)sr;
        (void)dr;
        huart4.Instance->CR3 |= USART_CR3_DMAR;
    }

    /* 1. Read newly received bytes from circular DMA buffer */
    uint16_t dma_rem = (uint16_t)__HAL_DMA_GET_COUNTER(huart4.hdmarx);
    uint16_t dma_head = (dma_rem <= sizeof(s_rx_dma_buf))
                        ? (uint16_t)(sizeof(s_rx_dma_buf) - dma_rem)
                        : 0U;
    if (dma_head >= sizeof(s_rx_dma_buf))
    {
        dma_head = 0U;
    }

    while (s_rx_read_ptr != dma_head)
    {
        /* Mask bit 7 (7-bit ASCII) to tolerate stop-bit edge slew on jumper wires */
        char c = (char)(s_rx_dma_buf[s_rx_read_ptr] & 0x7FU);
        s_rx_read_ptr = (s_rx_read_ptr + 1U) % sizeof(s_rx_dma_buf);

        if (c == '\n' || c == '\r')
        {
            if (s_line_len > 0U)
            {
                s_line_buf[s_line_len] = '\0';
                Process_Incoming_Line(s_line_buf);
                s_line_len = 0U;
            }
        }
        else if ((uint8_t)c >= 0x20U && (uint8_t)c <= 0x7EU)
        {
            if (s_line_len >= (sizeof(s_line_buf) - 1U))
            {
                s_line_len = 0U;
            }
            s_line_buf[s_line_len++] = c;
        }
    }

    /* 2. Link Timeout Check: disconnected if no heartbeat for 1500 ms */
    if (s_is_connected && ((HAL_GetTick() - s_last_rx_tick) > 1500U))
    {
        s_is_connected = false;
    }

    /* 3. Periodic Telemetry Transmission to ESP32 (every 500 ms) */
    static uint32_t last_tx_tick = 0U;
    if ((HAL_GetTick() - last_tx_tick) >= 500U)
    {
        last_tx_tick = HAL_GetTick();

        int32_t mcu_dc = TempSensor_ReadMcuDeciC();
        bool    ext_ok = false;
        int32_t ext_dc = TempSensor_GetCachedExtDeciC(&ext_ok);
        uint32_t free_heap_kb = (uint32_t)(xPortGetFreeHeapSize() / 1024U);
        uint8_t  cur_thm = UIManager_GetThemeId();
        const char *app_name = AppLoader_IsRunning() ? AppLoader_GetAppName() : "menu";

        int len = snprintf(s_tx_buf, sizeof(s_tx_buf),
                           "STM32:HB,HEAP=%lu,TMCU=%ld,TEXT=%ld,THM=%u,APP=%.16s\n",
                           (unsigned long)free_heap_kb,
                           (long)mcu_dc,
                           ext_ok ? (long)ext_dc : -999L,
                           (unsigned int)cur_thm,
                           app_name);

        if (len > 0)
        {
            huart4.gState = HAL_UART_STATE_READY;
            if (HAL_UART_Transmit(&huart4, (uint8_t *)s_tx_buf, (uint16_t)len, 10U) == HAL_OK)
            {
                s_tx_packets++;
            }
        }
    }
}

bool ESPCompanion_IsConnected(void)
{
    return s_is_connected;
}

uint32_t ESPCompanion_GetRxPackets(void)
{
    return s_rx_packets;
}

uint32_t ESPCompanion_GetTxPackets(void)
{
    return s_tx_packets;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4)
    {
        volatile uint32_t sr = huart->Instance->SR;
        volatile uint32_t dr = huart->Instance->DR;
        (void)sr;
        (void)dr;
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        huart->RxState = HAL_UART_STATE_READY;
        (void)HAL_UART_Receive_DMA(huart, s_rx_dma_buf, sizeof(s_rx_dma_buf));
    }
}
