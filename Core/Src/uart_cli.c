#include "uart_cli.h"
#include "uart_loader.h"
#include "app_loader.h"
#include "sdcard_fatfs.h"
#include "temp_sensor.h"
#include "usb_host_manager.h"
#include "esp_companion.h"
#include "console_abi.h"
#include "fw_update.h"
#include "memory_map.h"
#include "ccmram.h"
#include "lvgl.h"
#include "stm32f4xx_ll_usart.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern UART_HandleTypeDef huart3;
extern IWDG_HandleTypeDef hiwdg;

#define CLI_RING_SIZE   128U
#define CLI_LINE_MAX    128U
#define CLI_CAT_LIMIT   8192U
#define CLI_HIST_DEPTH  8U

static volatile uint8_t  s_ring[CLI_RING_SIZE];
static volatile uint16_t s_ring_head = 0U;
static volatile uint16_t s_ring_tail = 0U;

static char    s_line[CLI_LINE_MAX];
static uint8_t s_line_len    = 0U;
static bool    s_last_was_cr = false;
static uint8_t s_esc_state   = 0U; /* 0=normal, 1=got ESC, 2=got ESC [ */
static char    s_cwd[96]     = "";

CCMRAM_BUF static char s_hist[CLI_HIST_DEPTH][CLI_LINE_MAX];
static uint8_t s_hist_count = 0U;
static int8_t  s_hist_nav   = -1;

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
        "STM32 Console CLI Commands (Up/Down = history, Ctrl+L = clear):\r\n"
        "  help | -help | /help   Show this help\r\n"
        "  pwd / cd [dir]         Print or change SD directory (.. and /)\r\n"
        "  ls [dir]               List SD directory (default: cwd)\r\n"
        "  mkdir <dir>            Create directory on SD\r\n"
        "  cp <src> <dst>         Copy file on SD\r\n"
        "  mv <src> <dst>         Rename or move file/folder on SD\r\n"
        "  rm <file>              Delete file or empty dir on SD\r\n"
        "  cat <file>             Print text file from SD (max 8 KB)\r\n"
        "  rx <file>              Receive file into SD via XMODEM-CRC/1K\r\n"
        "  df | mem               Detailed RAM, LVGL pool, heap & SD usage\r\n"
        "  run <app> / stop       Launch .bin/.c app or stop running app\r\n"
        "  sysinfo / temp / shot  Diagnostics, temperatures, 320x240 screenshot\r\n"
        "  esp <status|wifi [ssid pass]|wth|sync|speedtest|hudkeep 0|1>\r\n"
        "  fwupdate               Receive fw_update.bin via XMODEM-1K and flash\r\n"
        "  clear | reboot         Clear terminal or reset console\r\n"
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

static bool split_two_args(const char *arg, char *a1, size_t max1, char *a2, size_t max2)
{
    const char *p = skip_ws(arg);
    if (*p == '\0') return false;

    size_t i = 0U;
    if (*p == '"')
    {
        p++;
        while (*p != '\0' && *p != '"' && i + 1U < max1) a1[i++] = *p++;
        if (*p == '"') p++;
    }
    else
    {
        while (*p != '\0' && *p != ' ' && *p != '\t' && i + 1U < max1) a1[i++] = *p++;
    }
    a1[i] = '\0';

    p = skip_ws(p);
    if (*p == '\0')
    {
        a2[0] = '\0';
        return false;
    }

    size_t j = 0U;
    if (*p == '"')
    {
        p++;
        while (*p != '\0' && *p != '"' && j + 1U < max2) a2[j++] = *p++;
    }
    else
    {
        while (*p != '\0' && j + 1U < max2) a2[j++] = *p++;
        while (j > 0U && (a2[j - 1U] == ' ' || a2[j - 1U] == '\t')) j--;
    }
    a2[j] = '\0';
    return (a1[0] != '\0' && a2[0] != '\0');
}

static void cmd_mkdir(const char *arg)
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
        cli_puts("Usage: mkdir <dir>\r\n");
        return;
    }
    FRESULT res = f_mkdir(path);
    if (res == FR_OK)
    {
        printf("Created '/%s'\r\n", path);
    }
    else
    {
        printf("ERR: f_mkdir('/%s') failed (FR=%d)\r\n", path, (int)res);
    }
}

static void cmd_mv(const char *arg)
{
    if (!SDCard_IsMounted())
    {
        cli_puts("ERR: SD card not mounted\r\n");
        return;
    }
    char raw_src[96], raw_dst[96], src[96], dst[96];
    if (!split_two_args(arg, raw_src, sizeof(raw_src), raw_dst, sizeof(raw_dst)))
    {
        cli_puts("Usage: mv <src> <dst>\r\n");
        return;
    }
    resolve_sd_path(raw_src, src, sizeof(src));
    resolve_sd_path(raw_dst, dst, sizeof(dst));
    FRESULT res = f_rename(src, dst);
    if (res == FR_OK)
    {
        printf("Moved '/%s' -> '/%s'\r\n", src, dst);
    }
    else
    {
        printf("ERR: f_rename failed (FR=%d)\r\n", (int)res);
    }
}

static void cmd_cp(const char *arg)
{
    if (!SDCard_IsMounted())
    {
        cli_puts("ERR: SD card not mounted\r\n");
        return;
    }
    char raw_src[96], raw_dst[96], src[96], dst[96];
    if (!split_two_args(arg, raw_src, sizeof(raw_src), raw_dst, sizeof(raw_dst)))
    {
        cli_puts("Usage: cp <src> <dst>\r\n");
        return;
    }
    resolve_sd_path(raw_src, src, sizeof(src));
    resolve_sd_path(raw_dst, dst, sizeof(dst));
    if (strcmp(src, dst) == 0)
    {
        cli_puts("ERR: source and destination are identical\r\n");
        return;
    }

    static FIL fs, fd;
    __attribute__((aligned(4))) static uint8_t cbuf[512];
    if (f_open(&fs, src, FA_READ) != FR_OK)
    {
        printf("ERR: cannot open '/%s'\r\n", src);
        return;
    }
    if (f_open(&fd, dst, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        (void)f_close(&fs);
        printf("ERR: cannot create '/%s'\r\n", dst);
        return;
    }

    uint32_t total = 0U;
    bool ok = true;
    for (;;)
    {
        UINT br = 0U, bw = 0U;
        if (f_read(&fs, cbuf, sizeof(cbuf), &br) != FR_OK) { ok = false; break; }
        if (br == 0U) break;
        if (f_write(&fd, cbuf, br, &bw) != FR_OK || bw != br) { ok = false; break; }
        total += bw;
    }
    (void)f_close(&fs);
    (void)f_close(&fd);

    if (ok)
    {
        printf("Copied '/%s' -> '/%s' (%lu B)\r\n", src, dst, (unsigned long)total);
    }
    else
    {
        (void)f_unlink(dst);
        cli_puts("ERR: copy failed\r\n");
    }
}

static void cmd_df(void)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    uint32_t lv_used = mon.total_size - mon.free_size;

    printf("Int SRAM:   64 KB OS + 64 KB App Sandbox @ 0x%08lX (%lu KB img + %lu KB stk)\r\n",
           (unsigned long)MM_APP_RAM_BASE,
           (unsigned long)(MM_APP_IMAGE_MAX_SIZE / 1024UL),
           (unsigned long)(MM_APP_STACK_SIZE / 1024UL));
    printf("LVGL Pool:  %lu / %lu B used (%u%%, max %lu B, frag %u%%) @ 0x%08lX\r\n",
           (unsigned long)lv_used, (unsigned long)mon.total_size,
           (unsigned int)mon.used_pct, (unsigned long)mon.max_used,
           (unsigned int)mon.frag_pct, (unsigned long)MM_EXT_LVGL_MEM_BASE);
    printf("RTOS Heap:  %lu B free (min-ever %lu B) [15 KB CCM + %lu KB Ext]\r\n",
           (unsigned long)xPortGetFreeHeapSize(),
           (unsigned long)xPortGetMinimumEverFreeHeapSize(),
           (unsigned long)(MM_EXT_OS_HEAP_SIZE / 1024UL));
    printf("App ExtRAM: %lu KB @ 0x%08lX\r\n",
           (unsigned long)(MM_EXT_APP_SIZE / 1024UL),
           (unsigned long)MM_EXT_APP_BASE);

    uint32_t total_mb = 0U, free_mb = 0U;
    if (SDCard_IsMounted() && SDCard_GetFreeSpace(&total_mb, &free_mb))
    {
        printf("SD Card:    %lu MB free / %lu MB total\r\n",
               (unsigned long)free_mb, (unsigned long)total_mb);
    }
    else
    {
        printf("SD Card:    %s\r\n", SDCard_IsMounted() ? "Mounted" : "Not mounted");
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
        char cand[160];
        bool found = false;
        if (s_cwd[0] != '\0')
        {
            snprintf(cand, sizeof(cand), "%.80s/%.64s.bin", s_cwd, p);
            if (f_stat(cand, &fno) == FR_OK)
            {
                snprintf(target, sizeof(target), "%.95s", cand);
                found = true;
            }
            else
            {
                snprintf(cand, sizeof(cand), "%.80s/%.64s.c", s_cwd, p);
                if (f_stat(cand, &fno) == FR_OK)
                {
                    snprintf(target, sizeof(target), "%.95s", cand);
                    found = true;
                }
            }
        }
        if (!found)
        {
            snprintf(target, sizeof(target), "games/%.80s.bin", p);
            if (f_stat(target, &fno) != FR_OK)
            {
                snprintf(target, sizeof(target), "games/%.80s.c", p);
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
    else if (str_eq_ci(sub, "wifi") || strncmp(sub, "wifi ", 5) == 0 || strncmp(sub, "WIFI ", 5) == 0)
    {
        const char *warg = skip_ws(sub + 4);
        if (*warg == '\0')
        {
            printf("SSID: %s  IP: %s  RSSI: %d dBm  Status: %d\r\n",
                   ESPCompanion_GetWifiSsid(),
                   ESPCompanion_GetWifiIp(),
                   (int)ESPCompanion_GetWifiRssi(),
                   (int)ESPCompanion_GetWifiStatus());
        }
        else
        {
            char ssid[34] = {0}, pass[64] = {0};
            if (!split_two_args(warg, ssid, sizeof(ssid), pass, sizeof(pass)))
            {
                snprintf(ssid, sizeof(ssid), "%.33s", warg);
                pass[0] = '\0';
            }
            bool sd_saved = SDCard_IsMounted() && ESPCompanion_SaveWifiToSD(ssid, pass);
            bool sent     = ESPCompanion_SendWifiCredentials(ssid, pass);
            printf("Wi-Fi SSID='%s': SD=%s, ESP32=%s\r\n",
                   ssid, sd_saved ? "saved" : "skip", sent ? "connecting" : "offline");
        }
    }
    else if (strncmp(sub, "hudkeep ", 8) == 0 || strncmp(sub, "HUDKEEP ", 8) == 0)
    {
        bool persistent = (atoi(skip_ws(sub + 8)) != 0);
        bool ok = ESPCompanion_HudSetPersistent(persistent);
        printf("HUD persistent: %s (%s)\r\n", persistent ? "ON" : "OFF", ok ? "sent" : "offline");
        if (!persistent)
        {
            (void)ESPCompanion_HudRestore();
        }
    }
    else if (str_eq_ci(sub, "speedtest") || str_eq_ci(sub, "spd"))
    {
        const ESP_SpeedTestInfo_t *spd = ESPCompanion_GetSpeedTest();
        if (ESPCompanion_StartSpeedTest())
        {
            cli_puts("SpeedTest started on ESP32-C6 (run 'esp spd' again in ~3s for results)...\r\n");
        }
        if (spd != NULL && spd->state == 2U)
        {
            printf("Last SpeedTest: Ping=%u ms  Wi-Fi=%u.%u Mbps  UART6=%u.%u KB/s\r\n",
                   (unsigned int)spd->ping_ms,
                   (unsigned int)(spd->dl_kbps / 1000U), (unsigned int)((spd->dl_kbps % 1000U) / 100U),
                   (unsigned int)(spd->link_kbs_x10 / 10U), (unsigned int)(spd->link_kbs_x10 % 10U));
        }
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
        cli_puts("Usage: esp <status|wifi [ssid pass]|wth|sync|speedtest|hudkeep 0|1>\r\n");
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
        (void)HAL_IWDG_Refresh(&hiwdg);
        if (LL_USART_IsActiveFlag_ORE(USART3)) LL_USART_ClearFlag_ORE(USART3);
        if (LL_USART_IsActiveFlag_RXNE(USART3))
        {
            *out = LL_USART_ReceiveData8(USART3);
            return true;
        }
    }
    return false;
}

static bool is_text_ext(const char *path)
{
    return ends_with_ci(path, ".c")   || ends_with_ci(path, ".h")   ||
           ends_with_ci(path, ".txt") || ends_with_ci(path, ".md")  ||
           ends_with_ci(path, ".ini") || ends_with_ci(path, ".cfg") ||
           ends_with_ci(path, ".json")|| ends_with_ci(path, ".csv");
}

static bool cli_receive_xmodem_to_file(const char *sd_path, bool strip_cpmeof, uint32_t *out_bytes)
{
    static FIL f;
    if (f_open(&f, sd_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return false;
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
    __attribute__((aligned(4))) static uint8_t pkt[1024U + 4U];

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
            cli_putc(0x15U);
            if (!cli_uart3_getc(&first, 2000U)) ok = false;
            continue;
        }

        if (pkt[0] == expected_blk)
        {
            uint32_t wr_len = blk_sz;
            if (strip_cpmeof)
            {
                while (wr_len > 0U && pkt[2U + wr_len - 1U] == 0x1AU)
                {
                    wr_len--;
                }
            }
            UINT bw = 0U;
            if (wr_len > 0U && (f_write(&f, &pkt[2], (UINT)wr_len, &bw) != FR_OK || bw != wr_len))
            {
                cli_putc(0x18U);
                ok = false;
                break;
            }
            total_rx += wr_len;
            expected_blk++;
        }
        cli_putc(0x06U);
        if (!cli_uart3_getc(&first, 3000U)) ok = false;
    }

    (void)f_close(&f);
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXNE);
    HAL_NVIC_EnableIRQ(USART3_IRQn);

    if (!ok)
    {
        (void)f_unlink(sd_path);
        return false;
    }
    if (out_bytes != NULL) *out_bytes = total_rx;
    return true;
}

static void cmd_rx(const char *arg)
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
        cli_puts("Usage: rx <dest_file>\r\n");
        return;
    }

    printf("Ready for xmodem-1k -> '/%s' (30s timeout)...\r\n", path);
    osDelay(20);

    uint32_t bytes = 0U;
    if (cli_receive_xmodem_to_file(path, is_text_ext(path), &bytes))
    {
        printf("\r\nSaved '/%s' (%lu B)\r\n", path, (unsigned long)bytes);
    }
    else
    {
        cli_puts("\r\nERR: XMODEM receive failed.\r\n");
    }
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
    uint32_t total_rx = 0U;
    if (!cli_receive_xmodem_to_file(FW_UPDATE_SD_PATH, false, &total_rx) ||
        total_rx < sizeof(FwUpdate_Header_t))
    {
        (void)f_unlink(FW_UPDATE_SD_PATH);
        cli_puts("\r\nERR: XMODEM transfer failed.\r\n");
        return;
    }

    /* Verify 4WFU header & CRC-32 before rebooting */
    static FIL f;
    __attribute__((aligned(4))) static uint8_t vbuf[1024];
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
        UINT chunk = (rem > sizeof(vbuf)) ? (UINT)sizeof(vbuf) : (UINT)rem;
        if (f_read(&f, vbuf, chunk, &br) != FR_OK || br != chunk) break;
        crc = cli_crc32_update(crc, vbuf, br);
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
    else if (str_eq_ci(cmd, "mkdir"))
    {
        cmd_mkdir(arg);
    }
    else if (str_eq_ci(cmd, "mv"))
    {
        cmd_mv(arg);
    }
    else if (str_eq_ci(cmd, "cp"))
    {
        cmd_cp(arg);
    }
    else if (str_eq_ci(cmd, "cat"))
    {
        cmd_cat(arg);
    }
    else if (str_eq_ci(cmd, "rm"))
    {
        cmd_rm(arg);
    }
    else if (str_eq_ci(cmd, "rx"))
    {
        cmd_rx(arg);
    }
    else if (str_eq_ci(cmd, "df") || str_eq_ci(cmd, "mem") || str_eq_ci(cmd, "free"))
    {
        cmd_df();
    }
    else if (str_eq_ci(cmd, "clear") || str_eq_ci(cmd, "cls"))
    {
        cli_puts("\x1b[2J\x1b[H");
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
    s_esc_state   = 0U;
    s_hist_nav    = -1;
}

static void cli_hist_push(const char *cmd_line)
{
    if (cmd_line == NULL || cmd_line[0] == '\0') return;
    if (s_hist_count > 0U && strcmp(s_hist[0], cmd_line) == 0) return;

    uint8_t top = (s_hist_count < CLI_HIST_DEPTH) ? s_hist_count : (CLI_HIST_DEPTH - 1U);
    for (uint8_t i = top; i > 0U; i--)
    {
        memcpy(s_hist[i], s_hist[i - 1U], CLI_LINE_MAX);
    }
    strncpy(s_hist[0], cmd_line, CLI_LINE_MAX - 1U);
    s_hist[0][CLI_LINE_MAX - 1U] = '\0';
    if (s_hist_count < CLI_HIST_DEPTH)
    {
        s_hist_count++;
    }
}

static void cli_replace_line(const char *new_str)
{
    while (s_line_len > 0U)
    {
        s_line_len--;
        cli_puts("\b \b");
    }
    if (new_str != NULL)
    {
        size_t n = strlen(new_str);
        if (n >= CLI_LINE_MAX) n = CLI_LINE_MAX - 1U;
        memcpy(s_line, new_str, n);
        s_line[n] = '\0';
        s_line_len = (uint8_t)n;
        cli_puts(s_line);
    }
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

        /* ANSI Escape sequence parser for Up/Down Arrow history */
        if (s_esc_state == 1U)
        {
            s_esc_state = (b == '[') ? 2U : 0U;
            continue;
        }
        else if (s_esc_state == 2U)
        {
            s_esc_state = 0U;
            if (b == 'A') /* Up Arrow */
            {
                if (s_hist_count > 0U && (s_hist_nav + 1) < (int8_t)s_hist_count)
                {
                    s_hist_nav++;
                    cli_replace_line(s_hist[(uint8_t)s_hist_nav]);
                }
            }
            else if (b == 'B') /* Down Arrow */
            {
                if (s_hist_nav > 0)
                {
                    s_hist_nav--;
                    cli_replace_line(s_hist[(uint8_t)s_hist_nav]);
                }
                else if (s_hist_nav == 0)
                {
                    s_hist_nav = -1;
                    cli_replace_line("");
                }
            }
            continue;
        }

        if (b == 0x1BU) /* ESC */
        {
            s_esc_state = 1U;
            continue;
        }
        if (b == 0x03U) /* Ctrl+C */
        {
            s_line_len    = 0U;
            s_hist_nav    = -1;
            s_last_was_cr = false;
            cli_puts("^C\r\n");
            cli_prompt();
            continue;
        }
        if (b == 0x0CU) /* Ctrl+L */
        {
            s_line[s_line_len] = '\0';
            cli_puts("\x1b[2J\x1b[H");
            cli_prompt();
            if (s_line_len > 0U) cli_puts(s_line);
            continue;
        }

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
            s_hist_nav = -1;
            if (s_line_len > 0U && strcmp(s_line, "UPLD!") != 0)
            {
                cli_hist_push(s_line);
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
