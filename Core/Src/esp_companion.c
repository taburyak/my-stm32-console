#include "esp_companion.h"
#include "stm32f4xx_ll_usart.h"
#include "temp_sensor.h"
#include "ui_manager.h"
#include "app_loader.h"
#include "uart_loader.h"
#include "usb_host_manager.h"
#include "ccmram.h"
#include "beeper.h"
#include "FreeRTOS.h"
#include "fatfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern UART_HandleTypeDef huart6;
extern IWDG_HandleTypeDef hiwdg;

#define ESP_RX_DMA_BUF_SIZE  2048U
#define ESP_LINE_BUF_SIZE    1152U
#define ESP_TX_BUF_SIZE      1152U

#define WIFI_CFG_PATH        "system/wifi.cfg"

static uint8_t  s_rx_dma_buf[ESP_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint16_t s_rx_read_ptr = 0U;

CCMRAM_BUF static char s_line_buf[ESP_LINE_BUF_SIZE];
static uint16_t s_line_len = 0U;

static char     s_tx_buf[ESP_TX_BUF_SIZE] __attribute__((aligned(4)));

/* DMA-safe FatFs structures in internal SRAM (never CCMRAM) */
static FIL      s_rpc_fil __attribute__((aligned(4)));
static DIR      s_rpc_dir __attribute__((aligned(4)));
static FILINFO  s_rpc_fno __attribute__((aligned(4)));
static uint8_t  s_rpc_io_buf[1024] __attribute__((aligned(4)));

CCMRAM_BUF static char s_pending_app_run[64];
static volatile bool s_has_pending_app_run = false;
static volatile bool s_game_hud_dirty      = false;

static ESP_SpeedTestInfo_t s_speed_test = {
    .state        = 0U,
    .ping_ms      = 0U,
    .dl_kbps      = 0U,
    .link_kbs_x10 = 0U,
    .updated_tick = 0U
};

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

static void Send_Raw_Line(const char *str)
{
    if (str == NULL) return;
    size_t len = strlen(str);
    if (len == 0U) return;
    huart6.gState = HAL_UART_STATE_READY;
    if (HAL_UART_Transmit(&huart6, (const uint8_t *)str, (uint16_t)len, 100U) == HAL_OK)
    {
        s_tx_packets++;
    }
}

static uint8_t Hex_Nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0U;
}

static const char *Normalize_Rel_Path(const char *p)
{
    if (p == NULL) return "";
    while (*p == '/' || *p == ' ') p++;
    return p;
}

static uint16_t Get_Dma_Rx_Head(void)
{
    uint16_t dma_rem = (uint16_t)__HAL_DMA_GET_COUNTER(huart6.hdmarx);
    uint16_t head = (dma_rem <= sizeof(s_rx_dma_buf))
                    ? (uint16_t)(sizeof(s_rx_dma_buf) - dma_rem)
                    : 0U;
    return (head < sizeof(s_rx_dma_buf)) ? head : 0U;
}

static bool WaitForStreamAck(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms)
    {
        (void)HAL_IWDG_Refresh(&hiwdg);
        if (__HAL_UART_GET_FLAG(&huart6, UART_FLAG_ORE) != RESET)
        {
            __HAL_UART_CLEAR_OREFLAG(&huart6);
            LL_USART_EnableDMAReq_RX(huart6.Instance);
        }

        while (s_rx_read_ptr != Get_Dma_Rx_Head())
        {
            uint8_t c = s_rx_dma_buf[s_rx_read_ptr];
            s_rx_read_ptr = (s_rx_read_ptr + 1U) % sizeof(s_rx_dma_buf);
            if (c == 'K')
            {
                s_is_connected = true;
                s_last_rx_tick = HAL_GetTick();
                return true;
            }
            if (c == 'X')
            {
                s_is_connected = true;
                s_last_rx_tick = HAL_GetTick();
                return false;
            }
        }
    }
    return false;
}

static bool Handle_File_Rpc(const char *line)
{
    const char *p_cmd = NULL;

    /* 1. List Directory: ESP32:FS_LS,<path> */
    if ((p_cmd = strstr(line, "FS_LS,")) != NULL)
    {
        const char *rel = Normalize_Rel_Path(p_cmd + 6);
        (void)HAL_IWDG_Refresh(&hiwdg);

        if (f_opendir(&s_rpc_dir, rel) != FR_OK)
        {
            Send_Raw_Line("STM32:FLS_END,ERR\n");
            return true;
        }

        uint32_t count = 0U;
        while (count < 64U)
        {
            (void)HAL_IWDG_Refresh(&hiwdg);
            if (f_readdir(&s_rpc_dir, &s_rpc_fno) != FR_OK || s_rpc_fno.fname[0] == '\0')
            {
                break;
            }
            if (strcmp(s_rpc_fno.fname, ".") == 0 || strcmp(s_rpc_fno.fname, "..") == 0)
            {
                continue;
            }
            bool is_dir = ((s_rpc_fno.fattrib & AM_DIR) != 0U);
            snprintf(s_tx_buf, sizeof(s_tx_buf), "STM32:FLS_I,%c,%lu,%s\n",
                     is_dir ? 'D' : 'F',
                     (unsigned long)s_rpc_fno.fsize,
                     s_rpc_fno.fname);
            Send_Raw_Line(s_tx_buf);
            count++;
        }
        (void)f_closedir(&s_rpc_dir);
        Send_Raw_Line("STM32:FLS_END,OK\n");
        return true;
    }

    /* 1.5. Fast Full-File Binary Stream: ESP32:FS_STREAM,<path> */
    if ((p_cmd = strstr(line, "FS_STREAM,")) != NULL)
    {
        const char *rel = Normalize_Rel_Path(p_cmd + 10);
        (void)HAL_IWDG_Refresh(&hiwdg);

        if (f_open(&s_rpc_fil, rel, FA_READ) != FR_OK)
        {
            Send_Raw_Line("STM32:STRM_ERR\n");
            return true;
        }

        uint32_t total_sz = (uint32_t)f_size(&s_rpc_fil);
        snprintf(s_tx_buf, sizeof(s_tx_buf), "STM32:STRM_START,%lu\n", (unsigned long)total_sz);
        Send_Raw_Line(s_tx_buf);

        uint32_t sent = 0U;
        while (sent < total_sz)
        {
            /* Wait for ESP32 to request the next chunk ('K') or abort ('X') BEFORE transmitting binary bytes */
            if (!WaitForStreamAck(1500U))
            {
                break;
            }

            (void)HAL_IWDG_Refresh(&hiwdg);
            UINT req = (total_sz - sent > sizeof(s_rpc_io_buf))
                       ? (UINT)sizeof(s_rpc_io_buf)
                       : (UINT)(total_sz - sent);
            UINT br = 0U;
            if (f_read(&s_rpc_fil, s_rpc_io_buf, req, &br) != FR_OK || br == 0U)
            {
                break;
            }
            huart6.gState = HAL_UART_STATE_READY;
            if (HAL_UART_Transmit(&huart6, s_rpc_io_buf, (uint16_t)br, 1000U) != HAL_OK)
            {
                break;
            }
            s_tx_packets++;
            sent += br;
            s_last_rx_tick = HAL_GetTick();
        }
        (void)f_close(&s_rpc_fil);
        s_last_rx_tick = HAL_GetTick();
        return true;
    }

    /* 2. Read File Chunk: ESP32:FS_RD,<path>,<offset>,<len> */
    if ((p_cmd = strstr(line, "FS_RD,")) != NULL)
    {
        char tmp[160];
        strncpy(tmp, p_cmd + 6, sizeof(tmp) - 1U);
        tmp[sizeof(tmp) - 1U] = '\0';

        char *c1 = strchr(tmp, ',');
        if (c1 == NULL) { Send_Raw_Line("STM32:FRD_ERR\n"); return true; }
        *c1 = '\0';
        char *c2 = strchr(c1 + 1, ',');
        if (c2 == NULL) { Send_Raw_Line("STM32:FRD_ERR\n"); return true; }
        *c2 = '\0';

        const char *rel = Normalize_Rel_Path(tmp);
        uint32_t offset = (uint32_t)strtoul(c1 + 1, NULL, 10);
        uint32_t req_len = (uint32_t)strtoul(c2 + 1, NULL, 10);
        if (req_len > sizeof(s_rpc_io_buf)) req_len = sizeof(s_rpc_io_buf);

        (void)HAL_IWDG_Refresh(&hiwdg);
        if (f_open(&s_rpc_fil, rel, FA_READ) != FR_OK)
        {
            Send_Raw_Line("STM32:FRD_ERR\n");
            return true;
        }

        uint32_t total_sz = (uint32_t)f_size(&s_rpc_fil);
        UINT br = 0U;
        if (offset < total_sz)
        {
            if (f_lseek(&s_rpc_fil, offset) == FR_OK)
            {
                (void)f_read(&s_rpc_fil, s_rpc_io_buf, (UINT)req_len, &br);
            }
        }
        (void)f_close(&s_rpc_fil);

        int hdr_len = snprintf(s_tx_buf, sizeof(s_tx_buf), "STM32:FRD,%lu,%u,%lu,",
                               (unsigned long)offset, (unsigned int)br, (unsigned long)total_sz);
        if (hdr_len > 0 && (size_t)(hdr_len + (int)(br * 2U) + 2) < sizeof(s_tx_buf))
        {
            static const char hex_lut[] = "0123456789ABCDEF";
            char *dst = s_tx_buf + hdr_len;
            for (UINT i = 0U; i < br; i++)
            {
                uint8_t b = s_rpc_io_buf[i];
                *dst++ = hex_lut[(b >> 4) & 0x0FU];
                *dst++ = hex_lut[b & 0x0FU];
            }
            *dst++ = '\n';
            *dst   = '\0';
            Send_Raw_Line(s_tx_buf);
        }
        else
        {
            Send_Raw_Line("STM32:FRD_ERR\n");
        }
        return true;
    }

    /* 3. Write / Append File Chunk: ESP32:FS_WR,<path>,<W|A>,<HEX> */
    if ((p_cmd = strstr(line, "FS_WR,")) != NULL)
    {
        const char *p_path = p_cmd + 6;
        const char *c1 = strchr(p_path, ',');
        if (c1 == NULL) { Send_Raw_Line("STM32:FERR\n"); return true; }
        const char *c2 = strchr(c1 + 1, ',');
        if (c2 == NULL) { Send_Raw_Line("STM32:FERR\n"); return true; }

        char rel_buf[96];
        size_t path_len = (size_t)(c1 - p_path);
        if (path_len >= sizeof(rel_buf)) path_len = sizeof(rel_buf) - 1U;
        strncpy(rel_buf, p_path, path_len);
        rel_buf[path_len] = '\0';
        const char *rel = Normalize_Rel_Path(rel_buf);

        char mode_ch = c1[1];
        const char *hex = c2 + 1;
        size_t hex_len = strlen(hex);
        uint32_t data_len = 0U;
        while ((data_len < sizeof(s_rpc_io_buf)) && ((data_len * 2U + 1U) < hex_len))
        {
            uint8_t hi = Hex_Nibble(hex[data_len * 2U]);
            uint8_t lo = Hex_Nibble(hex[data_len * 2U + 1U]);
            s_rpc_io_buf[data_len++] = (uint8_t)((hi << 4) | lo);
        }

        (void)HAL_IWDG_Refresh(&hiwdg);
        BYTE fmode = (mode_ch == 'W') ? (FA_WRITE | FA_CREATE_ALWAYS) : (FA_WRITE | FA_OPEN_APPEND);
        if (f_open(&s_rpc_fil, rel, fmode) != FR_OK)
        {
            Send_Raw_Line("STM32:FERR\n");
            return true;
        }

        UINT bw = 0U;
        FRESULT fr = FR_OK;
        if (data_len > 0U)
        {
            fr = f_write(&s_rpc_fil, s_rpc_io_buf, (UINT)data_len, &bw);
        }
        (void)f_close(&s_rpc_fil);

        if (fr == FR_OK && bw == (UINT)data_len)
        {
            snprintf(s_tx_buf, sizeof(s_tx_buf), "STM32:FOK,%u\n", (unsigned int)bw);
            Send_Raw_Line(s_tx_buf);
        }
        else
        {
            Send_Raw_Line("STM32:FERR\n");
        }
        return true;
    }

    /* 4. Create Folder: ESP32:FS_MKDIR,<path> */
    if ((p_cmd = strstr(line, "FS_MKDIR,")) != NULL)
    {
        const char *rel = Normalize_Rel_Path(p_cmd + 9);
        (void)HAL_IWDG_Refresh(&hiwdg);
        FRESULT fr = f_mkdir(rel);
        if (fr == FR_OK || fr == FR_EXIST)
        {
            Beeper_Click();
            Send_Raw_Line("STM32:FOK,0\n");
        }
        else
        {
            Send_Raw_Line("STM32:FERR\n");
        }
        return true;
    }

    /* 5. Delete File or Empty Folder: ESP32:FS_DEL,<path> */
    if ((p_cmd = strstr(line, "FS_DEL,")) != NULL)
    {
        const char *rel = Normalize_Rel_Path(p_cmd + 7);
        (void)HAL_IWDG_Refresh(&hiwdg);
        if (f_unlink(rel) == FR_OK)
        {
            Beeper_Click();
            Send_Raw_Line("STM32:FOK,0\n");
        }
        else
        {
            Send_Raw_Line("STM32:FERR\n");
        }
        return true;
    }

    /* 6. Rename File or Folder: ESP32:FS_REN,<old>,<new> */
    if ((p_cmd = strstr(line, "FS_REN,")) != NULL)
    {
        char tmp[160];
        strncpy(tmp, p_cmd + 7, sizeof(tmp) - 1U);
        tmp[sizeof(tmp) - 1U] = '\0';

        char *c1 = strchr(tmp, ',');
        if (c1 == NULL) { Send_Raw_Line("STM32:FERR\n"); return true; }
        *c1 = '\0';

        const char *old_rel = Normalize_Rel_Path(tmp);
        const char *new_rel = Normalize_Rel_Path(c1 + 1);
        (void)HAL_IWDG_Refresh(&hiwdg);
        if (f_rename(old_rel, new_rel) == FR_OK)
        {
            Beeper_Click();
            Send_Raw_Line("STM32:FOK,0\n");
        }
        else
        {
            Send_Raw_Line("STM32:FERR\n");
        }
        return true;
    }

    /* 7. Launch App or C-Script on Console: ESP32:APP_RUN,<path> */
    if ((p_cmd = strstr(line, "APP_RUN,")) != NULL)
    {
        const char *rel = Normalize_Rel_Path(p_cmd + 8);
        if (rel[0] != '\0')
        {
            strncpy(s_pending_app_run, rel, sizeof(s_pending_app_run) - 1U);
            s_pending_app_run[sizeof(s_pending_app_run) - 1U] = '\0';
            s_has_pending_app_run = true;
            if (AppLoader_IsRunning())
            {
                AppLoader_RequestExit();
            }
            Send_Raw_Line("STM32:FOK,0\n");
        }
        else
        {
            Send_Raw_Line("STM32:FERR\n");
        }
        return true;
    }

    /* 8. Stop Running App on Console: ESP32:APP_STOP */
    if (strstr(line, "APP_STOP") != NULL)
    {
        if (AppLoader_IsRunning())
        {
            AppLoader_RequestExit();
        }
        Beeper_Click();
        Send_Raw_Line("STM32:FOK,0\n");
        return true;
    }

    /* 9. Set Console Desktop Wallpaper: ESP32:SET_WALL,<path> */
    if ((p_cmd = strstr(line, "SET_WALL,")) != NULL)
    {
        const char *rel = Normalize_Rel_Path(p_cmd + 9);
        (void)HAL_IWDG_Refresh(&hiwdg);
        if (UIManager_SetWallpaperPath(rel))
        {
            Beeper_Click();
            Send_Raw_Line("STM32:FOK,0\n");
        }
        else
        {
            Send_Raw_Line("STM32:FERR\n");
        }
        return true;
    }

    /* 10. Capture Live Console Screen to SD: ESP32:SCREENSHOT */
    if (strstr(line, "SCREENSHOT") != NULL)
    {
        char shot_path[64] = {0};
        (void)HAL_IWDG_Refresh(&hiwdg);
        if (Screenshot_CaptureToSD(shot_path, sizeof(shot_path)) && shot_path[0] != '\0')
        {
            snprintf(s_tx_buf, sizeof(s_tx_buf), "STM32:SHOT_OK,/%s\n", shot_path);
            Send_Raw_Line(s_tx_buf);
        }
        else
        {
            Send_Raw_Line("STM32:SHOT_ERR\n");
        }
        return true;
    }

    /* 11. Virtual Web Gamepad Input: ESP32:VPAD,<mask>,<lx>,<ly> */
    if ((p_cmd = strstr(line, "VPAD,")) != NULL)
    {
        const char *p = p_cmd + 5;
        uint16_t vmask = (uint16_t)strtoul(p, NULL, 10);
        int8_t lx = 0, ly = 0;
        const char *c1 = strchr(p, ',');
        if (c1 != NULL)
        {
            lx = (int8_t)atoi(c1 + 1);
            const char *c2 = strchr(c1 + 1, ',');
            if (c2 != NULL)
            {
                ly = (int8_t)atoi(c2 + 1);
            }
        }
        USBHostManager_SetVirtualPad(vmask, lx, ly);
        return true;
    }

    /* 12. USART6 Link Benchmark Burst: ESP32:SPD_BURST,<kb> */
    if ((p_cmd = strstr(line, "SPD_BURST,")) != NULL)
    {
        uint32_t kb = (uint32_t)strtoul(p_cmd + 10, NULL, 10);
        if (kb == 0U || kb > 64U) kb = 16U;
        uint32_t total_bytes = kb * 1024U;

        snprintf(s_tx_buf, sizeof(s_tx_buf), "STM32:STRM_START,%lu\n", (unsigned long)total_bytes);
        Send_Raw_Line(s_tx_buf);

        for (uint32_t i = 0U; i < sizeof(s_rpc_io_buf); i++)
        {
            s_rpc_io_buf[i] = (uint8_t)(0x30U + (i & 0x3FU));
        }

        uint32_t sent = 0U;
        while (sent < total_bytes)
        {
            if (!WaitForStreamAck(500U))
            {
                break;
            }
            (void)HAL_IWDG_Refresh(&hiwdg);
            huart6.gState = HAL_UART_STATE_READY;
            if (HAL_UART_Transmit(&huart6, s_rpc_io_buf, (uint16_t)sizeof(s_rpc_io_buf), 1000U) != HAL_OK)
            {
                break;
            }
            s_tx_packets++;
            sent += (uint32_t)sizeof(s_rpc_io_buf);
            s_last_rx_tick = HAL_GetTick();
        }
        return true;
    }

    return false;
}

static void Process_Incoming_Line(const char *line)
{
    if (line == NULL || line[0] == '\0')
    {
        return;
    }

    /* 0. Web Dashboard File & App Control RPC */
    if (Handle_File_Rpc(line))
    {
        s_is_connected = true;
        s_last_rx_tick = HAL_GetTick();
        s_rx_packets++;
        return;
    }

    /* 1. Remote theme control from ESP32 Web UI: ESP32:SET_THM=<idx> */
    const char *thm_ptr = strstr(line, "THM=");
    if (thm_ptr != NULL)
    {
        int thm_val = atoi(thm_ptr + 4);
        if (thm_val >= 0 && thm_val < 4)
        {
            UIManager_SetThemeId((uint8_t)thm_val);
            snprintf(s_tx_buf, sizeof(s_tx_buf), "STM32:FOK,%d\n", thm_val);
            Send_Raw_Line(s_tx_buf);
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

    /* 2.6. SpeedTest Progress & Result Telemetry: ESP32:SPD,ST=...,PING=...,DL=...,LINK=... */
    if (strstr(line, "SPD,") != NULL || strstr(line, "ESP32:SPD") != NULL)
    {
        s_is_connected = true;
        s_last_rx_tick = HAL_GetTick();
        s_rx_packets++;

        const char *p;
        if ((p = strstr(line, "ST=")))   s_speed_test.state        = (uint8_t)atoi(p + 3);
        if ((p = strstr(line, "PING="))) s_speed_test.ping_ms      = (uint16_t)atoi(p + 5);
        if ((p = strstr(line, "DL=")))   s_speed_test.dl_kbps      = (uint16_t)strtoul(p + 3, NULL, 10);
        if ((p = strstr(line, "LINK="))) s_speed_test.link_kbs_x10 = (uint16_t)strtoul(p + 5, NULL, 10);
        s_speed_test.updated_tick = HAL_GetTick();
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

    /* GPIO PC6/PC7, DMA2_Stream1/7, and USART6 are configured by CubeMX in MX_USART6_UART_Init() */
    if (huart6.Instance != NULL && huart6.hdmarx != NULL)
    {
        __HAL_UART_CLEAR_OREFLAG(&huart6);
        huart6.ErrorCode = HAL_UART_ERROR_NONE;
        huart6.RxState   = HAL_UART_STATE_READY;

        (void)HAL_UART_Receive_DMA(&huart6, s_rx_dma_buf, sizeof(s_rx_dma_buf));
    }
}

void ESPCompanion_Poll(void)
{
    if (huart6.Instance == NULL || huart6.hdmarx == NULL)
    {
        return;
    }

    /* Auto-recovery: if overrun occurred, clear flag via HAL/LL API and keep DMA RX enabled */
    if (__HAL_UART_GET_FLAG(&huart6, UART_FLAG_ORE) != RESET)
    {
        __HAL_UART_CLEAR_OREFLAG(&huart6);
        LL_USART_EnableDMAReq_RX(huart6.Instance);
    }

    /* 1. Read newly received bytes from circular DMA buffer (query live DMA head on every step) */
    while (s_rx_read_ptr != Get_Dma_Rx_Head())
    {
        char c = (char)s_rx_dma_buf[s_rx_read_ptr];
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

    /* 2. Link Timeout Check: disconnected if no packet/heartbeat for 5000 ms */
    if (s_is_connected && ((HAL_GetTick() - s_last_rx_tick) > 5000U))
    {
        s_is_connected = false;
        s_wifi_status  = 0;
    }

    /* 2.5. Safety net: never leave SpeedTest stuck in Running state > 12 s */
    if (s_speed_test.state == 1U && ((HAL_GetTick() - s_speed_test.updated_tick) > 12000U))
    {
        s_speed_test.state        = 2U;
        s_speed_test.updated_tick = HAL_GetTick();
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
            huart6.gState = HAL_UART_STATE_READY;
            if (HAL_UART_Transmit(&huart6, (uint8_t *)s_tx_buf, (uint16_t)len, 10U) == HAL_OK)
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

    huart6.gState = HAL_UART_STATE_READY;
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart6, (uint8_t *)buf, (uint16_t)len, 100U);
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
    huart6.gState = HAL_UART_STATE_READY;
    if (HAL_UART_Transmit(&huart6, (const uint8_t *)req, (uint16_t)(sizeof(req) - 1U), 20U) == HAL_OK)
    {
        s_tx_packets++;
        return true;
    }
    return false;
}

const ESP_SpeedTestInfo_t *ESPCompanion_GetSpeedTest(void)
{
    return &s_speed_test;
}

bool ESPCompanion_StartSpeedTest(void)
{
    if (!s_is_connected)
    {
        return false;
    }
    s_speed_test.state        = 1U;
    s_speed_test.ping_ms      = 0U;
    s_speed_test.dl_kbps      = 0U;
    s_speed_test.link_kbs_x10 = 0U;
    s_speed_test.updated_tick = HAL_GetTick();
    Send_Raw_Line("\nSTM32:REQ_SPD\n");
    return true;
}

bool ESPCompanion_HudClear(void)
{
    s_game_hud_dirty = true;
    Send_Raw_Line("STM32:GHUD_CLR\n");
    return s_is_connected;
}

bool ESPCompanion_HudSetLine(uint8_t line_idx, const char *text)
{
    if (line_idx > 5U || text == NULL)
    {
        return false;
    }
    char clean[52];
    size_t j = 0U;
    for (size_t i = 0U; text[i] != '\0' && j + 1U < sizeof(clean); i++)
    {
        char c = text[i];
        if (c >= 0x20 && c <= 0x7E)
        {
            clean[j++] = c;
        }
    }
    clean[j] = '\0';

    s_game_hud_dirty = true;
    char cmd[72];
    snprintf(cmd, sizeof(cmd), "STM32:GHUD_LN,%u,%s\n", (unsigned int)line_idx, clean);
    Send_Raw_Line(cmd);
    return s_is_connected;
}

bool ESPCompanion_SetLedRgb(uint8_t r, uint8_t g, uint8_t b)
{
    s_game_hud_dirty = true;
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "STM32:GLED,%u,%u,%u\n",
             (unsigned int)r, (unsigned int)g, (unsigned int)b);
    Send_Raw_Line(cmd);
    return s_is_connected;
}

bool ESPCompanion_HudRestore(void)
{
    if (!s_game_hud_dirty)
    {
        return true;
    }
    s_game_hud_dirty = false;
    Send_Raw_Line("STM32:GHUD_EXIT\n");
    return true;
}

bool ESPCompanion_TakePendingAppRun(char *path_out, size_t max_len)
{
    if (!s_has_pending_app_run || path_out == NULL || max_len == 0U)
    {
        return false;
    }
    s_has_pending_app_run = false;
    strncpy(path_out, s_pending_app_run, max_len - 1U);
    path_out[max_len - 1U] = '\0';
    return (path_out[0] != '\0');
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
    if (huart->Instance == USART6)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        huart->RxState   = HAL_UART_STATE_READY;
        (void)HAL_UART_Receive_DMA(huart, s_rx_dma_buf, sizeof(s_rx_dma_buf));
    }
}
