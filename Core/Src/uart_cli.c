#include "uart_cli.h"
#include "uart_loader.h"
#include "app_loader.h"
#include "sdcard_fatfs.h"
#include "temp_sensor.h"
#include "usb_host_manager.h"
#include "esp_companion.h"
#include "console_abi.h"
#include "fw_update.h"
#include "stm32f4xx_ll_usart.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern UART_HandleTypeDef huart3;

#define CLI_RING_SIZE   128U
#define CLI_LINE_MAX    128U
#define CLI_CAT_LIMIT   8192U

static volatile uint8_t  s_ring[CLI_RING_SIZE];
static volatile uint16_t s_ring_head = 0U;
static volatile uint16_t s_ring_tail = 0U;

static char    s_line[CLI_LINE_MAX];
static uint8_t s_line_len = 0U;
static bool    s_last_was_cr = false;
static char    s_cwd[96] = "";

static void cli_puts(const char *s)
{
    if (s == NULL)
    {
        return;
    }
    size_t len = strlen(s);
    if (len > 0U)
    {
        (void)HAL_UART_Transmit(&huart3, (const uint8_t *)s, (uint16_t)len, 200U);
    }
}

static void cli_putc(char c)
{
    (void)HAL_UART_Transmit(&huart3, (const uint8_t *)&c, 1U, 50U);
}

static void cli_prompt(void)
{
    printf("stm32:/%s> ", s_cwd);
    fflush(stdout);
}

static bool str_eq_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb)
        {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (ls < lf)
    {
        return false;
    }
    return str_eq_ci(s + (ls - lf), suffix);
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    return p;
}

static void resolve_sd_path(const char *in, char *out, size_t max_len)
{
    char work[128];
    if (in == NULL)
    {
        snprintf(out, max_len, "%s", s_cwd);
        return;
    }

    in = skip_ws(in);
    if (*in == '/')
    {
        work[0] = '\0';
        in++;
    }
    else
    {
        snprintf(work, sizeof(work), "%s", s_cwd);
    }

    size_t i = 0U;
    while (in[i] != '\0')
    {
        while (in[i] == '/')
        {
            i++;
        }
        if (in[i] == '\0')
        {
            break;
        }
        char seg[64];
        size_t si = 0U;
        while (in[i] != '\0' && in[i] != '/' && si + 1U < sizeof(seg))
        {
            seg[si++] = in[i++];
        }
        while (in[i] != '\0' && in[i] != '/')
        {
            i++;
        }
        seg[si] = '\0';

        /* Trim trailing spaces on last segment */
        while (si > 0U && (seg[si - 1U] == ' ' || seg[si - 1U] == '\t'))
        {
            seg[--si] = '\0';
        }
        if (si == 0U || strcmp(seg, ".") == 0)
        {
            continue;
        }
        if (strcmp(seg, "..") == 0)
        {
            char *slash = strrchr(work, '/');
            if (slash != NULL)
            {
                *slash = '\0';
            }
            else
            {
                work[0] = '\0';
            }
        }
        else
        {
            size_t cur_len = strlen(work);
            if (cur_len > 0U && cur_len + 1U + si < sizeof(work))
            {
                work[cur_len] = '/';
                memcpy(work + cur_len + 1U, seg, si + 1U);
            }
            else if (cur_len == 0U && si < sizeof(work))
            {
                memcpy(work, seg, si + 1U);
            }
        }
    }

    snprintf(out, max_len, "%s", work);
}

static void cmd_help(void)
{
    cli_puts(
        "STM32 Console CLI Commands:\r\n"
        "  help | -help | /help   Show this help\r\n"
        "  pwd                    Print current SD directory\r\n"
        "  cd [dir]               Change SD directory (supports .. and /)\r\n"
        "  ls [dir]               List SD directory (default: cwd)\r\n"
        "  cat <file>             Print text file from SD (max 8 KB)\r\n"
        "  rm <file>              Delete file or empty dir on SD\r\n"
        "  run <app>              Run .bin/.c app (e.g. 'run tetris' or 'run /games/tetris.bin')\r\n"
        "  stop                   Stop running app/script\r\n"
        "  sysinfo                Firmware, heap, SD, USB & companion info\r\n"
        "  temp                   Read MCU, DS18B20 & outdoor temperatures\r\n"
        "  shot                   Capture 320x240 screenshot to SD\r\n"
        "  esp <status|wifi|wth|sync>  Query/sync ESP32-C6 companion\r\n"
        "  fwupdate               Receive fw_update.bin via XMODEM-CRC/1K and flash\r\n"
        "  reboot                 Reset console\r\n"
    );
}

static void cmd_cd(const char *arg)
{
    if (!SDCard_IsMounted())
    {
        cli_puts("ERR: SD card not mounted\r\n");
        return;
    }

    const char *p = skip_ws(arg);
    if (*p == '\0')
    {
        s_cwd[0] = '\0';
        printf("/\r\n");
        return;
    }

    char next[96];
    resolve_sd_path(p, next, sizeof(next));
    if (next[0] == '\0')
    {
        s_cwd[0] = '\0';
        printf("/\r\n");
        return;
    }

    DIR dir;
    FRESULT res = f_opendir(&dir, next);
    if (res != FR_OK)
    {
        printf("ERR: no such directory '/%s' (FR=%d)\r\n", next, (int)res);
        return;
    }
    (void)f_closedir(&dir);
    snprintf(s_cwd, sizeof(s_cwd), "%s", next);
    printf("/%s\r\n", s_cwd);
}

static void cmd_ls(const char *arg)
{
    if (!SDCard_IsMounted())
    {
        cli_puts("ERR: SD card not mounted\r\n");
        return;
    }

    char path[96];
    resolve_sd_path(arg, path, sizeof(path));

    DIR dir;
    FILINFO fno;
    FRESULT res = f_opendir(&dir, path);
    if (res != FR_OK)
    {
        printf("ERR: cannot open '/%s' (FR=%d)\r\n", path, (int)res);
        return;
    }

    printf("Listing /%s:\r\n", path);
    uint32_t count = 0U;
    for (;;)
    {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == '\0')
        {
            break;
        }
        if ((fno.fattrib & AM_DIR) != 0U)
        {
            printf("  <DIR>       %s\r\n", fno.fname);
        }
        else
        {
            printf("  %9lu B %s\r\n", (unsigned long)fno.fsize, fno.fname);
        }
        count++;
    }
    (void)f_closedir(&dir);
    printf("Total: %lu item(s)\r\n", (unsigned long)count);
}

static void cmd_cat(const char *arg)
{
    if (!SDCard_IsMounted())
    {
        cli_puts("ERR: SD card not mounted\r\n");
        return;
    }

    char path[96];
    resolve_sd_path(arg, path, sizeof(path));
    if (path[0] == '\0')
    {
        cli_puts("Usage: cat <file>\r\n");
        return;
    }

    static FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
    {
        printf("ERR: cannot open '/%s'\r\n", path);
        return;
    }

    uint32_t total = 0U;
    char buf[129];
    UINT br = 0U;
    while (total < CLI_CAT_LIMIT)
    {
        UINT chunk = (UINT)((CLI_CAT_LIMIT - total < 128U) ? (CLI_CAT_LIMIT - total) : 128U);
        if (f_read(&f, buf, chunk, &br) != FR_OK || br == 0U)
        {
            break;
        }
        for (UINT i = 0U; i < br; i++)
        {
            char c = buf[i];
            if (c == '\n')
            {
                cli_puts("\r\n");
            }
            else if (c == '\r')
            {
                /* normalize CR+LF via LF above */
            }
            else if ((uint8_t)c >= 0x20U && (uint8_t)c <= 0x7EU)
            {
                cli_putc(c);
            }
            else if (c == '\t')
            {
                cli_puts("    ");
            }
            else
            {
                cli_putc('.');
            }
        }
        total += br;
    }
    FSIZE_t fsz = f_size(&f);
    (void)f_close(&f);
    cli_puts("\r\n");
    if (fsz > CLI_CAT_LIMIT)
    {
        printf("[truncated at %u / %lu bytes]\r\n", (unsigned int)CLI_CAT_LIMIT, (unsigned long)fsz);
    }
}

static void cmd_rm(const char *arg)
{
    if (!SDCard_IsMounted())
    {
        cli_puts("ERR: SD card not mounted\r\n");
        return;
    }

    char path[96];
    resolve_sd_path(arg, path, sizeof(path));
    if (path[0] == '\0')
    {
        cli_puts("Usage: rm <file>\r\n");
        return;
    }

    FRESULT res = f_unlink(path);
    if (res == FR_OK)
    {
        printf("Removed '/%s'\r\n", path);
    }
    else
    {
        printf("ERR: f_unlink('/%s') failed (FR=%d)\r\n", path, (int)res);
    }
}

static void cmd_run(const char *arg)
{
    if (AppLoader_IsRunning())
    {
        cli_puts("ERR: app already running (use 'stop' first)\r\n");
        return;
    }
    if (!SDCard_IsMounted())
    {
        cli_puts("ERR: SD card not mounted\r\n");
        return;
    }

    const char *p = skip_ws(arg);
    if (*p == '\0')
    {
        cli_puts("Usage: run <app_name_or_path>\r\n");
        return;
    }

    char target[96];
    FILINFO fno;
    if (strchr(p, '/') != NULL || ends_with_ci(p, ".bin") || ends_with_ci(p, ".c"))
    {
        resolve_sd_path(p, target, sizeof(target));
        if (strchr(p, '/') == NULL && f_stat(target, &fno) != FR_OK)
        {
            snprintf(target, sizeof(target), "games/%s", p);
        }
    }
    else
    {
        char cand[96];
        bool found = false;
        if (s_cwd[0] != '\0')
        {
            snprintf(cand, sizeof(cand), "%s/%s.bin", s_cwd, p);
            if (f_stat(cand, &fno) == FR_OK)
            {
                snprintf(target, sizeof(target), "%s", cand);
                found = true;
            }
            else
            {
                snprintf(cand, sizeof(cand), "%s/%s.c", s_cwd, p);
                if (f_stat(cand, &fno) == FR_OK)
                {
                    snprintf(target, sizeof(target), "%s", cand);
                    found = true;
                }
            }
        }
        if (!found)
        {
            snprintf(target, sizeof(target), "games/%s.bin", p);
            if (f_stat(target, &fno) != FR_OK)
            {
                snprintf(target, sizeof(target), "games/%s.c", p);
            }
        }
    }

    bool ok = ends_with_ci(target, ".c")
            ? AppLoader_LoadAndRunScript(target)
            : AppLoader_LoadAndRun(target);
    if (ok)
    {
        printf("Launched '/%s'\r\n", target);
    }
    else
    {
        printf("ERR: failed '/%s': %s\r\n", target, AppLoader_GetLastError());
    }
}

static void cmd_stop(void)
{
    if (!AppLoader_IsRunning())
    {
        cli_puts("No app running.\r\n");
        return;
    }
    AppLoader_RequestExit();
    cli_puts("Stop requested.\r\n");
}

static void cmd_sysinfo(void)
{
    uint32_t total_mb = 0U, free_mb = 0U;
    bool sd_ok = SDCard_IsMounted() && SDCard_GetFreeSpace(&total_mb, &free_mb);
    uint32_t up_s = HAL_GetTick() / 1000UL;

    printf("FW:       v%s (SDK API v%u)\r\n", CONSOLE_FW_VERSION_STR, (unsigned int)SDK_API_VERSION);
    printf("Uptime:   %lu s (%lu:%02lu:%02lu)\r\n",
           (unsigned long)up_s,
           (unsigned long)(up_s / 3600UL),
           (unsigned long)((up_s / 60UL) % 60UL),
           (unsigned long)(up_s % 60UL));
    printf("Heap:     %lu B free (min %lu B)\r\n",
           (unsigned long)xPortGetFreeHeapSize(),
           (unsigned long)xPortGetMinimumEverFreeHeapSize());
    if (sd_ok)
    {
        printf("SD Card:  Mounted (%lu / %lu MB free)\r\n",
               (unsigned long)free_mb, (unsigned long)total_mb);
    }
    else
    {
        printf("SD Card:  %s\r\n", SDCard_IsMounted() ? "Mounted" : "Not mounted");
    }
    printf("USB HID:  KBD=%s  MOUSE=%s  PAD=%s\r\n",
           USBHostManager_IsKeyboardReady() ? "ON" : "OFF",
           USBHostManager_IsMouseReady()    ? "ON" : "OFF",
           USBHostManager_IsGamepadReady()  ? "ON" : "OFF");
    printf("ESP32-C6: %s", ESPCompanion_IsConnected() ? "Online" : "Offline");
    if (ESPCompanion_IsWifiConnected())
    {
        printf(" (Wi-Fi: %s, %s, %d dBm)",
               ESPCompanion_GetWifiSsid(), ESPCompanion_GetWifiIp(), (int)ESPCompanion_GetWifiRssi());
    }
    printf("\r\nApp:      %s\r\n",
           AppLoader_IsRunning() ? AppLoader_GetAppName() : "idle (menu)");
}

static void cmd_temp(void)
{
    int32_t mcu_dc = TempSensor_ReadMcuDeciC();
    int32_t ext_dc = 0;
    bool ext_ok = TempSensor_ReadExtDeciC(&ext_dc);
    const ESP_WeatherInfo_t *wth = ESPCompanion_GetWeather();

    printf("MCU Die:  %s%ld.%ld C%s\r\n",
           (mcu_dc < 0) ? "-" : "+", (long)(abs(mcu_dc) / 10), (long)(abs(mcu_dc) % 10),
           TempSensor_IsOverheated() ? " [HOT!]" : "");
    if (ext_ok)
    {
        printf("DS18B20:  %s%ld.%ld C\r\n",
               (ext_dc < 0) ? "-" : "+", (long)(abs(ext_dc) / 10), (long)(abs(ext_dc) % 10));
    }
    else
    {
        cli_puts("DS18B20:  N/A\r\n");
    }
    if (ESPCompanion_IsConnected() && wth != NULL && wth->valid)
    {
        printf("Outdoor:  %s%d.%d C (%s)\r\n",
               (wth->temp_deci < 0) ? "-" : "+", abs(wth->temp_deci) / 10, abs(wth->temp_deci) % 10,
               wth->city);
    }
}

static void cmd_shot(void)
{
    char path[64] = {0};
    if (Screenshot_CaptureToSD(path, sizeof(path)))
    {
        printf("Saved: %s\r\n", path);
    }
    else
    {
        cli_puts("ERR: screenshot failed (check SD card)\r\n");
    }
}

static void cmd_esp(const char *arg)
{
    const char *sub = skip_ws(arg);
    if (sub[0] == '\0' || str_eq_ci(sub, "status"))
    {
        printf("Link: %s  RX=%lu  TX=%lu  Wi-Fi=%s\r\n",
               ESPCompanion_IsConnected() ? "Online" : "Offline",
               (unsigned long)ESPCompanion_GetRxPackets(),
               (unsigned long)ESPCompanion_GetTxPackets(),
               ESPCompanion_IsWifiConnected() ? "Connected" : "Disconnected");
    }
    else if (str_eq_ci(sub, "wifi"))
    {
        printf("SSID: %s  IP: %s  RSSI: %d dBm  Status: %d\r\n",
               ESPCompanion_GetWifiSsid(),
               ESPCompanion_GetWifiIp(),
               (int)ESPCompanion_GetWifiRssi(),
               (int)ESPCompanion_GetWifiStatus());
    }
    else if (str_eq_ci(sub, "wth"))
    {
        const ESP_WeatherInfo_t *wth = ESPCompanion_GetWeather();
        if (wth != NULL && wth->valid)
        {
            printf("City: %s  Temp: %s%d.%d C (Feels %s%d.%d C)  RH: %u%%  Wind: %u.%u km/h  WMO: %u\r\n",
                   wth->city,
                   (wth->temp_deci < 0) ? "-" : "+", abs(wth->temp_deci) / 10, abs(wth->temp_deci) % 10,
                   (wth->feels_deci < 0) ? "-" : "+", abs(wth->feels_deci) / 10, abs(wth->feels_deci) % 10,
                   (unsigned int)wth->humidity,
                   (unsigned int)(wth->wind_deci / 10U), (unsigned int)(wth->wind_deci % 10U),
                   (unsigned int)wth->wmo_code);
        }
        else
        {
            cli_puts("Weather: not available\r\n");
        }
    }
    else if (str_eq_ci(sub, "sync"))
    {
        bool ok = ESPCompanion_RequestWeatherUpdate();
        cli_puts(ok ? "Weather sync requested.\r\n" : "ERR: ESP32-C6 offline\r\n");
    }
    else
    {
        cli_puts("Usage: esp <status|wifi|wth|sync>\r\n");
    }
}

static uint16_t cli_crc16(const uint8_t *buf, uint32_t len)
{
    uint16_t crc = 0U;
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= (uint16_t)buf[i] << 8;
        for (uint8_t b = 0U; b < 8U; b++)
        {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t cli_crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= buf[i];
        for (uint8_t b = 0U; b < 8U; b++)
        {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return ~crc;
}

static bool cli_uart3_getc(uint8_t *out, uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms)
    {
        if (LL_USART_IsActiveFlag_ORE(USART3)) LL_USART_ClearFlag_ORE(USART3);
        if (LL_USART_IsActiveFlag_RXNE(USART3))
        {
            *out = LL_USART_ReceiveData8(USART3);
            return true;
        }
    }
    return false;
}

static void cmd_fwupdate(void)
{
    if (!SDCard_IsMounted())
    {
        cli_puts("ERR: SD card not mounted (hold KEY0 on boot for SD-less Rescue DFU)\r\n");
        return;
    }

    cli_puts("Ready for xmodem-1k fw_update.bin (30s timeout)...\r\n");
    osDelay(20);

    (void)f_mkdir("system");
    static FIL f;
    if (f_open(&f, FW_UPDATE_SD_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        cli_puts("ERR: cannot open SD:/system/fw_update.bin\r\n");
        return;
    }

    HAL_NVIC_DisableIRQ(USART3_IRQn);
    while (LL_USART_IsActiveFlag_RXNE(USART3)) (void)LL_USART_ReceiveData8(USART3);

    uint8_t first = 0U;
    bool started = false;
    for (uint32_t tries = 0U; tries < 30U; tries++)
    {
        cli_putc('C');
        if (cli_uart3_getc(&first, 1000U))
        {
            started = true;
            break;
        }
    }

    bool ok = started;
    uint8_t expected_blk = 1U;
    uint32_t total_rx = 0U;
    static uint8_t pkt[1024U + 4U];

    while (ok)
    {
        if (first == 0x04U) /* EOT */
        {
            cli_putc(0x06U);
            break;
        }
        if (first == 0x18U) /* CAN */
        {
            ok = false;
            break;
        }
        uint32_t blk_sz = (first == 0x02U) ? 1024U : ((first == 0x01U) ? 128U : 0U);
        if (blk_sz == 0U)
        {
            ok = false;
            break;
        }

        bool got_pkt = true;
        for (uint32_t i = 0U; i < blk_sz + 4U; i++)
        {
            if (!cli_uart3_getc(&pkt[i], 1500U))
            {
                got_pkt = false;
                break;
            }
        }
        if (!got_pkt || (uint8_t)(pkt[0] + pkt[1]) != 0xFFU ||
            cli_crc16(&pkt[2], blk_sz) != (((uint16_t)pkt[blk_sz + 2U] << 8) | pkt[blk_sz + 3U]))
        {
            cli_putc(0x15U); /* NAK */
            if (!cli_uart3_getc(&first, 2000U)) ok = false;
            continue;
        }

        if (pkt[0] == expected_blk)
        {
            UINT bw = 0U;
            if (f_write(&f, &pkt[2], (UINT)blk_sz, &bw) != FR_OK || bw != blk_sz)
            {
                cli_putc(0x18U);
                ok = false;
                break;
            }
            total_rx += blk_sz;
            expected_blk++;
        }
        cli_putc(0x06U); /* ACK */
        if (!cli_uart3_getc(&first, 3000U)) ok = false;
    }

    (void)f_close(&f);
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXNE);
    HAL_NVIC_EnableIRQ(USART3_IRQn);

    if (!ok || total_rx < sizeof(FwUpdate_Header_t))
    {
        (void)f_unlink(FW_UPDATE_SD_PATH);
        cli_puts("\r\nERR: XMODEM transfer failed.\r\n");
        return;
    }

    /* Verify 4WFU header & CRC-32 before rebooting */
    if (f_open(&f, FW_UPDATE_SD_PATH, FA_READ) != FR_OK)
    {
        cli_puts("\r\nERR: cannot re-open fw_update.bin\r\n");
        return;
    }
    FwUpdate_Header_t hdr;
    UINT br = 0U;
    if (f_read(&f, &hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr) ||
        hdr.magic != FW_UPDATE_MAGIC || hdr.header_size != FW_UPDATE_HDR_SIZE ||
        hdr.image_size == 0U || hdr.image_size > FW_UPDATE_MAX_SIZE ||
        hdr.target_addr != FW_UPDATE_APP_ADDR)
    {
        (void)f_close(&f);
        (void)f_unlink(FW_UPDATE_SD_PATH);
        cli_puts("\r\nERR: invalid 4WFU header!\r\n");
        return;
    }

    uint32_t crc = 0U;
    uint32_t rem = hdr.image_size;
    while (rem > 0U)
    {
        UINT chunk = (rem > 1024U) ? 1024U : (UINT)rem;
        if (f_read(&f, pkt, chunk, &br) != FR_OK || br != chunk) break;
        crc = cli_crc32_update(crc, pkt, br);
        rem -= br;
    }
    (void)f_close(&f);

    if (rem != 0U || crc != hdr.crc32)
    {
        (void)f_unlink(FW_UPDATE_SD_PATH);
        printf("\r\nERR: CRC mismatch (got 0x%08lX, exp 0x%08lX)\r\n",
               (unsigned long)crc, (unsigned long)hdr.crc32);
        return;
    }

    printf("\r\nVerified %lu B (v%s). Rebooting into Bootloader...\r\n",
           (unsigned long)hdr.image_size, hdr.version);
    osDelay(60);
    NVIC_SystemReset();
}

static void cli_exec(char *line)
{
    const char *p = skip_ws(line);
    if (*p == '\0')
    {
        return;
    }

    char cmd[24];
    size_t ci = 0U;
    while (*p != '\0' && *p != ' ' && *p != '\t' && ci + 1U < sizeof(cmd))
    {
        cmd[ci++] = *p++;
    }
    cmd[ci] = '\0';
    const char *arg = skip_ws(p);

    if (str_eq_ci(cmd, "help") || str_eq_ci(cmd, "-help") || str_eq_ci(cmd, "/help") || strcmp(cmd, "?") == 0)
    {
        cmd_help();
    }
    else if (str_eq_ci(cmd, "pwd"))
    {
        printf("/%s\r\n", s_cwd);
    }
    else if (str_eq_ci(cmd, "cd"))
    {
        cmd_cd(arg);
    }
    else if (str_eq_ci(cmd, "ls"))
    {
        cmd_ls(arg);
    }
    else if (str_eq_ci(cmd, "cat"))
    {
        cmd_cat(arg);
    }
    else if (str_eq_ci(cmd, "rm"))
    {
        cmd_rm(arg);
    }
    else if (str_eq_ci(cmd, "run"))
    {
        cmd_run(arg);
    }
    else if (str_eq_ci(cmd, "stop"))
    {
        cmd_stop();
    }
    else if (str_eq_ci(cmd, "sysinfo"))
    {
        cmd_sysinfo();
    }
    else if (str_eq_ci(cmd, "temp"))
    {
        cmd_temp();
    }
    else if (str_eq_ci(cmd, "shot"))
    {
        cmd_shot();
    }
    else if (str_eq_ci(cmd, "esp"))
    {
        cmd_esp(arg);
    }
    else if (str_eq_ci(cmd, "fwupdate"))
    {
        cmd_fwupdate();
    }
    else if (str_eq_ci(cmd, "reboot"))
    {
        cli_puts("Rebooting...\r\n");
        osDelay(50);
        NVIC_SystemReset();
    }
    else
    {
        printf("Unknown command '%s'. Type 'help'.\r\n", cmd);
    }
}

void UARTCli_Init(void)
{
    UARTCli_Reset();
    cli_puts("\r\n[CLI] Ready on USART3. Type 'help' for commands.\r\n");
    cli_prompt();
}

void UARTCli_OnRxByte(uint8_t b)
{
    if (UARTLoader_IsSessionActive())
    {
        return;
    }
    uint16_t next = (uint16_t)((s_ring_head + 1U) % CLI_RING_SIZE);
    if (next != s_ring_tail)
    {
        s_ring[s_ring_head] = b;
        s_ring_head = next;
    }
}

void UARTCli_Reset(void)
{
    s_ring_tail   = s_ring_head;
    s_line_len    = 0U;
    s_last_was_cr = false;
}

void UARTCli_Poll(void)
{
    if (UARTLoader_IsSessionActive())
    {
        return;
    }

    while (s_ring_tail != s_ring_head)
    {
        uint8_t b = s_ring[s_ring_tail];
        s_ring_tail = (uint16_t)((s_ring_tail + 1U) % CLI_RING_SIZE);

        if (b == '\r' || b == '\n')
        {
            if (b == '\n' && s_last_was_cr)
            {
                s_last_was_cr = false;
                continue;
            }
            s_last_was_cr = (b == '\r');
            cli_puts("\r\n");
            s_line[s_line_len] = '\0';
            if (s_line_len > 0U && strcmp(s_line, "UPLD!") != 0)
            {
                cli_exec(s_line);
            }
            s_line_len = 0U;
            cli_prompt();
        }
        else
        {
            s_last_was_cr = false;
            if (b == 0x08U || b == 0x7FU)
            {
                if (s_line_len > 0U)
                {
                    s_line_len--;
                    cli_puts("\b \b");
                }
            }
            else if (b == '\t')
            {
                if ((uint16_t)s_line_len + 1U < CLI_LINE_MAX)
                {
                    s_line[s_line_len++] = ' ';
                    cli_putc(' ');
                }
            }
            else if (b >= 0x20U && b <= 0x7EU)
            {
                if ((uint16_t)s_line_len + 1U < CLI_LINE_MAX)
                {
                    s_line[s_line_len++] = (char)b;
                    cli_putc((char)b);
                }
            }
        }
    }
}
