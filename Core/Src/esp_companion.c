#include "esp_companion.h"
#include "stm32f4xx_ll_usart.h"
#include "temp_sensor.h"
#include "ui_manager.h"
#include "app_loader.h"
#include "FreeRTOS.h"
#include "fatfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern UART_HandleTypeDef huart4;

#define ESP_RX_DMA_BUF_SIZE  256U
#define ESP_LINE_BUF_SIZE    128U
#define ESP_TX_BUF_SIZE      128U

#define WIFI_CFG_PATH        "system/wifi.cfg"

static uint8_t  s_rx_dma_buf[ESP_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint16_t s_rx_read_ptr = 0U;

static char     s_line_buf[ESP_LINE_BUF_SIZE];
static uint16_t s_line_len = 0U;

static char     s_tx_buf[ESP_TX_BUF_SIZE] __attribute__((aligned(4)));

static volatile bool     s_is_connected  = false;
static volatile uint32_t s_last_rx_tick  = 0U;
static volatile uint32_t s_rx_packets    = 0U;
static volatile uint32_t s_tx_packets    = 0U;

static volatile int8_t   s_wifi_status   = 0; /* 0=off, 1=connected, 2=connecting, -1=err */
static char              s_wifi_ip[20]   = "0.0.0.0";
static char              s_wifi_ssid[34] = "none";
static volatile int8_t   s_wifi_rssi     = 0;

static ESP_WeatherInfo_t s_weather = {
    .valid        = false,
    .temp_deci    = 0,
    .feels_deci   = 0,
    .humidity     = 0,
    .wind_deci    = 0,
    .wmo_code     = 0,
    .day_hi_deci  = {0, 0, 0},
    .day_lo_deci  = {0, 0, 0},
    .day_wmo      = {0, 0, 0},
    .city         = "Auto",
    .updated_tick = 0U
};

static void Parse_Slash_Int16_3(const char *str, int16_t out[3])
{
    for (int i = 0; i < 3; i++)
    {
        if (str == NULL || *str == '\0') break;
        out[i] = (int16_t)atoi(str);
        const char *sl = strchr(str, '/');
        if (sl == NULL) break;
        str = sl + 1;
    }
}

static void Parse_Slash_UInt8_3(const char *str, uint8_t out[3])
{
    for (int i = 0; i < 3; i++)
    {
        if (str == NULL || *str == '\0') break;
        out[i] = (uint8_t)atoi(str);
        const char *sl = strchr(str, '/');
        if (sl == NULL) break;
        str = sl + 1;
    }
}

static void Process_Incoming_Line(const char *line)
{
    if (line == NULL || line[0] == '\0')
    {
        return;
    }

    /* 1. Remote theme control from ESP32 BOOT button: ESP32:SET_THM=<idx> */
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

    /* 2. NTP Date/Time Synchronization: ESP32:TIME,Y=...,M=...,D=...,WD=...,H=...,MIN=...,S=... */
    if (strstr(line, "TIME,") != NULL || strstr(line, "ESP32:TIME") != NULL)
    {
        s_is_connected = true;
        s_last_rx_tick = HAL_GetTick();
        s_rx_packets++;

        int y = 0, m = 0, d = 0, wd = 1, h = 0, mn = 0, s = 0;
        const char *p;
        if ((p = strstr(line, "Y=")))   y  = atoi(p + 2);
        if ((p = strstr(line, "M=")))   m  = atoi(p + 2);
        if ((p = strstr(line, "D=")))   d  = atoi(p + 2);
        if ((p = strstr(line, "WD=")))  wd = atoi(p + 3);
        if ((p = strstr(line, "H=")))   h  = atoi(p + 2);
        if ((p = strstr(line, "MIN="))) mn = atoi(p + 4);
        if ((p = strstr(line, "S=")))   s  = atoi(p + 2);

        if (y >= 2024 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
        {
            UIManager_SetDateTime((uint16_t)y, (uint8_t)m, (uint8_t)d, (uint8_t)wd,
                                  (uint8_t)h, (uint8_t)mn, (uint8_t)s);
            printf("[NTP] Synchronized RTC from Wi-Fi: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                   y, m, d, h, mn, s);
        }
        return;
    }

    /* 2.5. Outdoor Weather Telemetry: ESP32:WTH,OK=1,T=...,FL=...,HUM=...,WND=...,WMO=...,HI=a/b/c,LO=a/b/c,DW=a/b/c,CITY=... */
    if (strstr(line, "WTH,") != NULL || strstr(line, "ESP32:WTH") != NULL)
    {
        s_is_connected = true;
        s_last_rx_tick = HAL_GetTick();
        s_rx_packets++;

        const char *p;
        if ((p = strstr(line, "T=")))   s_weather.temp_deci  = (int16_t)atoi(p + 2);
        if ((p = strstr(line, "FL=")))  s_weather.feels_deci = (int16_t)atoi(p + 3);
        if ((p = strstr(line, "HUM="))) s_weather.humidity   = (uint8_t)atoi(p + 4);
        if ((p = strstr(line, "WND="))) s_weather.wind_deci  = (uint16_t)atoi(p + 4);
        if ((p = strstr(line, "WMO="))) s_weather.wmo_code   = (uint8_t)atoi(p + 4);
        if ((p = strstr(line, "HI=")))  Parse_Slash_Int16_3(p + 3, s_weather.day_hi_deci);
        if ((p = strstr(line, "LO=")))  Parse_Slash_Int16_3(p + 3, s_weather.day_lo_deci);
        if ((p = strstr(line, "DW=")))  Parse_Slash_UInt8_3(p + 3, s_weather.day_wmo);
        if ((p = strstr(line, "CITY=")))
        {
            const char *c_start = p + 5;
            size_t l = strcspn(c_start, ",\r\n");
            if (l > 0 && l < sizeof(s_weather.city))
            {
                strncpy(s_weather.city, c_start, l);
                s_weather.city[l] = '\0';
            }
        }
        s_weather.valid        = true;
        s_weather.updated_tick = HAL_GetTick();
        return;
    }

    /* 3. Heartbeat frame with Wi-Fi status: ESP32C6:HB,WIFI=...,IP=...,RSSI=...,SSID=... */
    if (strstr(line, "ESP") != NULL || strstr(line, ":HB") != NULL || strstr(line, "C6:") != NULL)
    {
        s_is_connected = true;
        s_last_rx_tick = HAL_GetTick();
        s_rx_packets++;

        const char *p = strstr(line, "WIFI=");
        if (p) s_wifi_status = (int8_t)atoi(p + 5);

        p = strstr(line, "IP=");
        if (p)
        {
            const char *ip_start = p + 3;
            size_t l = strcspn(ip_start, ",\r\n ");
            if (l > 0 && l < sizeof(s_wifi_ip))
            {
                strncpy(s_wifi_ip, ip_start, l);
                s_wifi_ip[l] = '\0';
            }
        }

        p = strstr(line, "RSSI=");
        if (p) s_wifi_rssi = (int8_t)atoi(p + 5);

        p = strstr(line, "SSID=");
        if (p)
        {
            const char *s_start = p + 5;
            size_t l = strcspn(s_start, ",\r\n ");
            if (l > 0 && l < sizeof(s_wifi_ssid))
            {
                strncpy(s_wifi_ssid, s_start, l);
                s_wifi_ssid[l] = '\0';
            }
        }
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

    /* GPIO PA0/PA1, DMA1_Stream2/4, and UART4 are configured by CubeMX in MX_UART4_Init() */
    if (huart4.Instance != NULL && huart4.hdmarx != NULL)
    {
        __HAL_UART_CLEAR_OREFLAG(&huart4);
        huart4.ErrorCode = HAL_UART_ERROR_NONE;
        huart4.RxState   = HAL_UART_STATE_READY;

        (void)HAL_UART_Receive_DMA(&huart4, s_rx_dma_buf, sizeof(s_rx_dma_buf));
    }
}

void ESPCompanion_Poll(void)
{
    if (huart4.Instance == NULL || huart4.hdmarx == NULL)
    {
        return;
    }

    /* Auto-recovery: if overrun occurred, clear flag via HAL/LL API and keep DMA RX enabled */
    if (__HAL_UART_GET_FLAG(&huart4, UART_FLAG_ORE) != RESET)
    {
        __HAL_UART_CLEAR_OREFLAG(&huart4);
        LL_USART_EnableDMAReq_RX(huart4.Instance);
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
        s_wifi_status  = 0;
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
        uint32_t min_heap_kb  = (uint32_t)(xPortGetMinimumEverFreeHeapSize() / 1024U);
        uint8_t  cur_thm = UIManager_GetThemeId();
        const char *app_name = AppLoader_IsRunning() ? AppLoader_GetAppName() : "menu";

        int len = snprintf(s_tx_buf, sizeof(s_tx_buf),
                           "STM32:HB,HEAP=%lu,MINH=%lu,TMCU=%ld,TEXT=%ld,THM=%u,APP=%.16s\n",
                           (unsigned long)free_heap_kb,
                           (unsigned long)min_heap_kb,
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

bool ESPCompanion_IsWifiConnected(void)
{
    return (s_is_connected && (s_wifi_status == 1));
}

int8_t ESPCompanion_GetWifiStatus(void)
{
    if (!s_is_connected) return 0;
    return s_wifi_status;
}

const char *ESPCompanion_GetWifiIp(void)
{
    return s_wifi_ip;
}

const char *ESPCompanion_GetWifiSsid(void)
{
    return s_wifi_ssid;
}

int8_t ESPCompanion_GetWifiRssi(void)
{
    return s_wifi_rssi;
}

bool ESPCompanion_SendWifiCredentials(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0')
    {
        return false;
    }
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "STM32:SET_WIFI,SSID=%s,PASS=%s\n",
                       ssid, (pass != NULL) ? pass : "");
    if (len <= 0) return false;

    huart4.gState = HAL_UART_STATE_READY;
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart4, (uint8_t *)buf, (uint16_t)len, 100U);
    if (st == HAL_OK)
    {
        s_tx_packets++;
        s_wifi_status = 2; /* Connecting */
        strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
        s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
        return true;
    }
    return false;
}

bool ESPCompanion_SaveWifiToSD(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') return false;

    FIL f;
    if (f_open(&f, WIFI_CFG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return false;
    }
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "SSID=%s\nPASS=%s\n", ssid, (pass != NULL) ? pass : "");
    UINT bw = 0;
    FRESULT res = f_write(&f, buf, (UINT)len, &bw);
    f_close(&f);
    return (res == FR_OK && bw == (UINT)len);
}

bool ESPCompanion_ReadWifiFromSD(char *ssid_out, size_t ssid_max, char *pass_out, size_t pass_max)
{
    if (ssid_out == NULL || ssid_max == 0) return false;
    if (pass_out && pass_max > 0) pass_out[0] = '\0';
    ssid_out[0] = '\0';

    FIL f;
    if (f_open(&f, WIFI_CFG_PATH, FA_READ) != FR_OK)
    {
        return false;
    }
    char content[256];
    UINT br = 0;
    FRESULT res = f_read(&f, content, sizeof(content) - 1, &br);
    f_close(&f);
    if (res != FR_OK || br == 0) return false;
    content[br] = '\0';

    /* Parse SSID=... and PASS=... */
    char *s_ptr = strstr(content, "SSID=");
    char *p_ptr = strstr(content, "PASS=");

    if (s_ptr != NULL)
    {
        s_ptr += 5;
        size_t l = strcspn(s_ptr, "\r\n");
        if (l >= ssid_max) l = ssid_max - 1;
        strncpy(ssid_out, s_ptr, l);
        ssid_out[l] = '\0';

        if (p_ptr != NULL && pass_out != NULL && pass_max > 0)
        {
            p_ptr += 5;
            size_t pl = strcspn(p_ptr, "\r\n");
            if (pl >= pass_max) pl = pass_max - 1;
            strncpy(pass_out, p_ptr, pl);
            pass_out[pl] = '\0';
        }
        return (ssid_out[0] != '\0');
    }

    /* Fallback: Line 1 = SSID, Line 2 = PASS */
    char *line1 = content;
    while (*line1 == ' ' || *line1 == '\r' || *line1 == '\n') line1++;
    char *eol1 = strpbrk(line1, "\r\n");
    if (eol1 == NULL)
    {
        strncpy(ssid_out, line1, ssid_max - 1);
        ssid_out[ssid_max - 1] = '\0';
        return (ssid_out[0] != '\0');
    }
    *eol1 = '\0';
    strncpy(ssid_out, line1, ssid_max - 1);
    ssid_out[ssid_max - 1] = '\0';

    char *line2 = eol1 + 1;
    while (*line2 == ' ' || *line2 == '\r' || *line2 == '\n') line2++;
    char *eol2 = strpbrk(line2, "\r\n");
    if (eol2 != NULL) *eol2 = '\0';
    if (pass_out != NULL && pass_max > 0)
    {
        strncpy(pass_out, line2, pass_max - 1);
        pass_out[pass_max - 1] = '\0';
    }
    return (ssid_out[0] != '\0');
}

bool ESPCompanion_LoadAndSendWifiFromSD(void)
{
    char ssid[34] = {0};
    char pass[64] = {0};
    if (ESPCompanion_ReadWifiFromSD(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        printf("[WIFI] Loaded from SD: SSID='%s'\r\n", ssid);
        return ESPCompanion_SendWifiCredentials(ssid, pass);
    }
    return false;
}

const ESP_WeatherInfo_t *ESPCompanion_GetWeather(void)
{
    return &s_weather;
}

bool ESPCompanion_RequestWeatherUpdate(void)
{
    static const char req[] = "STM32:REQ_WTH\n";
    huart4.gState = HAL_UART_STATE_READY;
    if (HAL_UART_Transmit(&huart4, (const uint8_t *)req, (uint16_t)(sizeof(req) - 1U), 20U) == HAL_OK)
    {
        s_tx_packets++;
        return true;
    }
    return false;
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
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        huart->RxState   = HAL_UART_STATE_READY;
        (void)HAL_UART_Receive_DMA(huart, s_rx_dma_buf, sizeof(s_rx_dma_buf));
    }
}
