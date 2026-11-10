#include "uart_loader.h"
#include "app_loader.h"
#include "memory_map.h"
#include "ili9341_simple.h"
#include "lv_port_console.h"
#include "sdcard_fatfs.h"
#include "beeper.h"
#include "status_led.h"
#include "os_tasks.h"
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart3;
extern IWDG_HandleTypeDef hiwdg;
extern DMA_HandleTypeDef  hdma_memtomem_dma2_stream0;

/* Staging buffer in top 256 KB of External SRAM (0x680C0000..0x680FFFFF).
 * Does not overlap with AppLoader_ClearSandboxes() (first 64 KB at 0x68080000)
 * or ScriptEngine source/token buffers (0x68080000..0x6809FFFF). */
#define UART_STAGE_BASE         (MM_EXT_APP_BASE + (256UL * 1024UL))
#define UART_STAGE_MAX_SIZE     (256UL * 1024UL)

/* Framed protocol constants (tools/uart_upload.py) */
#define UPLD_FLAG_SAVE_SD       (1U << 0)
#define UPLD_FLAG_RUN_RAM       (1U << 1)
#define UPLD_FLAG_DELETE_SD     (1U << 2)
#define UPLD_FLAG_LIST_SD       (1U << 3)
#define UPLD_FLAG_PULL_SD       (1U << 4)
#define UPLD_FLAG_SCREENSHOT    (1U << 5)
#define UPLD_FLAG_MORE          (1U << 7) /* Keep session open for next command in batch */
#define UPLD_CHUNK_SIZE         1024U

/* Standard XMODEM constants */
#define XMODEM_SOH              0x01U /* 128-byte block */
#define XMODEM_STX              0x02U /* 1024-byte block */
#define XMODEM_EOT              0x04U /* End of transmission */
#define XMODEM_ACK              0x06U
#define XMODEM_NAK              0x15U
#define XMODEM_CAN              0x18U
#define XMODEM_CRC_POLL         0x43U /* 'C' */

/* RGB565 UI colors */
#define COL_BG                  0x0862U
#define COL_PANEL               0x10A4U
#define COL_CYAN                0x07FFU
#define COL_GREEN               0x07E0U
#define COL_YELLOW              0xFFE0U
#define COL_AMBER               0xFD20U
#define COL_RED                 0xF800U
#define COL_WHITE               0xFFFFU

static const uint8_t s_wake_seq[6] = { 'U', 'P', 'L', 'D', '!', '\n' };
static volatile uint8_t s_wake_idx           = 0U;
static volatile bool    s_auto_trigger       = false;
static volatile bool    s_in_session         = false;
static volatile bool    s_screenshot_pending = false;

/* --------------------------------------------------------------------------
 * Low-level polled UART3 helpers (used while USART3_IRQn is disabled)
 * -------------------------------------------------------------------------- */

static void uart_clear_errors(void)
{
    volatile uint32_t sr = USART3->SR;
    if ((sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) != 0U)
    {
        volatile uint32_t dr = USART3->DR;
        (void)dr;
    }
}

static void uart_putc(uint8_t b)
{
    while ((USART3->SR & USART_SR_TXE) == 0U) {}
    USART3->DR = b;
    while ((USART3->SR & USART_SR_TC) == 0U) {}
}

static void uart_puts_raw(const char *s)
{
    while (*s != '\0')
    {
        uart_putc((uint8_t)*s++);
    }
}

static bool uart_getc_timeout(uint8_t *out, uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    for (;;)
    {
        uint32_t sr = USART3->SR;
        if ((sr & USART_SR_RXNE) != 0U)
        {
            *out = (uint8_t)(USART3->DR & 0xFFU);
            return true;
        }
        if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0U)
        {
            volatile uint32_t dummy = USART3->DR;
            (void)dummy;
        }
        if ((HAL_GetTick() - t0) >= timeout_ms)
        {
            return false;
        }
    }
}

static bool uart_read_exact(uint8_t *dst, uint32_t len, uint32_t timeout_ms)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        if (!uart_getc_timeout(&dst[i], timeout_ms))
        {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * CRC-16-CCITT (XMODEM) and CRC-32 (IEEE 802.3)
 * -------------------------------------------------------------------------- */

static uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0x0000U;
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (uint8_t b = 0U; b < 8U; b++)
        {
            crc = ((crc & 0x8000U) != 0U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t crc32_ieee(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t k = 0U; k < 8U; k++)
        {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* --------------------------------------------------------------------------
 * Minimal direct-LCD status rendering during transfer
 * -------------------------------------------------------------------------- */

static void ui_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= 320U || y >= 240U || w == 0U || h == 0U) return;
    if (x + w > 320U) w = 320U - x;
    if (y + h > 240U) h = 240U - y;

    lcdSetWindow(x, y, x + w - 1U, y + h - 1U);
    uint32_t total = (uint32_t)w * (uint32_t)h;
    if (total >= 256U)
    {
        lcdFillConstantDMA(&hdma_memtomem_dma2_stream0, color, total);
    }
    else
    {
        while (total--) LCD_DataWrite(color);
    }
}

static void ui_text(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale)
{
    App_DrawTextParams_t p = {
        .x        = x,
        .y        = y,
        .str      = str,
        .fg_color = fg,
        .bg_color = bg,
        .scale    = scale
    };
    AppLoader_DrawText_Privileged(&p);
}

static void ui_draw_frame(const char *status_line, const char *file_line)
{
    ui_fill_rect(0, 0, 320, 240, COL_BG);
    ui_fill_rect(0, 0, 320, 28, COL_PANEL);
    ui_text(12, 7, "USART3 UPLOADER (115200 8N1)", COL_CYAN, COL_PANEL, 1);

    ui_text(16, 44, "PROTOCOL:", COL_WHITE, COL_BG, 1);
    ui_text(86, 44, "UPLD FRAMED / XMODEM-CRC", COL_GREEN, COL_BG, 1);

    ui_text(16, 68, "STATUS:", COL_WHITE, COL_BG, 1);
    ui_text(86, 68, status_line, COL_YELLOW, COL_BG, 1);

    ui_text(16, 92, "TARGET:", COL_WHITE, COL_BG, 1);
    ui_text(86, 92, file_line, COL_CYAN, COL_BG, 1);

    /* Progress bar outline */
    ui_fill_rect(16, 128, 288, 20, COL_PANEL);

    ui_fill_rect(0, 212, 320, 28, COL_PANEL);
    ui_text(12, 220, "PC: python tools/uart_upload.py <file> --run", COL_AMBER, COL_PANEL, 1);
}

static void ui_update_progress(uint32_t received, uint32_t total)
{
    char buf[40];
    if (total > 0U)
    {
        uint32_t pct = (received * 100U) / total;
        if (pct > 100U) pct = 100U;
        uint16_t bar_w = (uint16_t)((received * 284UL) / total);
        if (bar_w > 284U) bar_w = 284U;
        if (bar_w > 0U)
        {
            ui_fill_rect(18, 130, bar_w, 16, COL_CYAN);
        }
        snprintf(buf, sizeof(buf), "%lu / %lu BYTES (%lu%%)   ",
                 (unsigned long)received, (unsigned long)total, (unsigned long)pct);
    }
    else
    {
        uint16_t pulse_w = (uint16_t)(((received / 128U) * 12U) % 284U);
        ui_fill_rect(18, 130, 284, 16, COL_PANEL);
        if (pulse_w > 0U)
        {
            ui_fill_rect(18, 130, pulse_w, 16, COL_GREEN);
        }
        snprintf(buf, sizeof(buf), "XMODEM RX: %lu BYTES   ", (unsigned long)received);
    }
    ui_text(16, 160, buf, COL_WHITE, COL_BG, 1);
}

static bool has_suffix_ci(const char *name, const char *suf)
{
    size_t nl = strlen(name);
    size_t sl = strlen(suf);
    if (sl > nl) return false;
    const char *p = name + (nl - sl);
    for (size_t i = 0U; i < sl; i++)
    {
        char a = p[i];
        char b = suf[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return false;
    }
    return true;
}

static bool save_buffer_to_sd(const char *rel_name, const uint8_t *data, uint32_t size, char *out_path, size_t out_max)
{
    if (!SDCard_IsMounted())
    {
        return false;
    }

    /* Sanitize path: if no directory separator is given, place under "games/" */
    const char *clean = rel_name;
    while (*clean == '/' || *clean == '\\') clean++;

    if (strchr(clean, '/') == NULL && strchr(clean, '\\') == NULL)
    {
        (void)f_mkdir("games");
        snprintf(out_path, out_max, "games/%.48s", clean);
    }
    else
    {
        snprintf(out_path, out_max, "%.60s", clean);
    }

    static FIL out_file;
    UINT bw = 0;
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);
    FRESULT res = f_open(&out_file, out_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK)
    {
        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return false;
    }

    res = f_write(&out_file, data, (UINT)size, &bw);
    (void)f_close(&out_file);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);

    return (res == FR_OK && bw == size);
}

static void uart_write_exact(const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        uart_putc(src[i]);
    }
}

static void uart_write_u32_le(uint32_t v)
{
    uint8_t b[4] = {
        (uint8_t)(v & 0xFFU),
        (uint8_t)((v >> 8) & 0xFFU),
        (uint8_t)((v >> 16) & 0xFFU),
        (uint8_t)((v >> 24) & 0xFFU)
    };
    uart_write_exact(b, 4U);
}

/* Send data in 128-byte CRC-protected packets with automatic retry on NAK */
static bool uart_send_chunked(const uint8_t *src, uint32_t total_len)
{
    uint32_t sent = 0U;
    while (sent < total_len)
    {
        (void)HAL_IWDG_Refresh(&hiwdg);
        uint32_t chunk = total_len - sent;
        if (chunk > 128U) chunk = 128U;

        uint32_t pkt_crc = crc32_ieee(&src[sent], chunk);
        uint8_t  retries = 0U;
        for (;;)
        {
            uint8_t hdr[2] = { (uint8_t)(chunk & 0xFFU), (uint8_t)((chunk >> 8) & 0xFFU) };
            uart_write_exact(hdr, 2U);
            uart_write_u32_le(pkt_crc);
            uart_write_exact(&src[sent], chunk);

            uint8_t ack = 0U;
            if (!uart_getc_timeout(&ack, 3000U))
            {
                return false;
            }
            if (ack == 'A')
            {
                break;
            }
            if (++retries > 8U)
            {
                return false;
            }
        }
        sent += chunk;
    }
    return true;
}

static uint32_t format_sd_listing(const char *path, char *out, uint32_t max_len)
{
    uint32_t pos = 0U;
    if (!SDCard_IsMounted())
    {
        int n = snprintf(out, max_len, "[SD Card not mounted]\n");
        return (n > 0) ? (uint32_t)n : 0U;
    }

    int h = snprintf(out + pos, max_len - pos, "SD Card Mounted (0:/)\n");
    if (h > 0) pos += (uint32_t)h;

    static DIR dir;
    static DIR subdir;
    static FILINFO fno;
    static char sub_path[96];

    const char *root = (path != NULL && path[0] != '\0') ? path : "/";
    if (f_opendir(&dir, root) != FR_OK)
    {
        int n = snprintf(out + pos, max_len - pos, "[Cannot open directory: %s]\n", root);
        if (n > 0) pos += (uint32_t)n;
        return pos;
    }

    bool is_root = (strcmp(root, "/") == 0 || strcmp(root, "") == 0);
    for (;;)
    {
        (void)HAL_IWDG_Refresh(&hiwdg);
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
        if (fno.fname[0] == '.') continue;
        if (pos + 128U >= max_len) break;

        if ((fno.fattrib & AM_DIR) != 0U)
        {
            int n = snprintf(out + pos, max_len - pos, "[DIR]  %s/\n", fno.fname);
            if (n > 0) pos += (uint32_t)n;

            /* If listing root, also expand one level inside each directory */
            if (is_root)
            {
                snprintf(sub_path, sizeof(sub_path), "%.90s", fno.fname);
                if (f_opendir(&subdir, sub_path) == FR_OK)
                {
                    for (;;)
                    {
                        (void)HAL_IWDG_Refresh(&hiwdg);
                        if (f_readdir(&subdir, &fno) != FR_OK || fno.fname[0] == '\0') break;
                        if (fno.fname[0] == '.') continue;
                        if (pos + 128U >= max_len) break;

                        if ((fno.fattrib & AM_DIR) != 0U)
                        {
                            int sn = snprintf(out + pos, max_len - pos, "  [DIR]  %s/%s/\n", sub_path, fno.fname);
                            if (sn > 0) pos += (uint32_t)sn;
                        }
                        else
                        {
                            int sn = snprintf(out + pos, max_len - pos, "  %8lu B  %s/%s\n",
                                              (unsigned long)fno.fsize, sub_path, fno.fname);
                            if (sn > 0) pos += (uint32_t)sn;
                        }
                    }
                    (void)f_closedir(&subdir);
                }
            }
        }
        else
        {
            int n = snprintf(out + pos, max_len - pos, "%8lu B  %s\n",
                             (unsigned long)fno.fsize, fno.fname);
            if (n > 0) pos += (uint32_t)n;
        }
    }
    (void)f_closedir(&dir);
    return pos;
}

/* --------------------------------------------------------------------------
 * 320x240 16-bit RGB565 BMP Screenshot Capture (153,654 bytes)
 * -------------------------------------------------------------------------- */

static uint32_t build_screenshot_bmp16(uint8_t *dst)
{
    const uint32_t w         = 320U;
    const uint32_t h         = 240U;
    const uint32_t row_bytes = w * 2U;
    const uint32_t img_size  = row_bytes * h;
    const uint32_t file_size = 54U + img_size;

    memset(dst, 0, 54U);
    dst[0] = 'B';
    dst[1] = 'M';
    dst[2] = (uint8_t)(file_size & 0xFFU);
    dst[3] = (uint8_t)((file_size >> 8) & 0xFFU);
    dst[4] = (uint8_t)((file_size >> 16) & 0xFFU);
    dst[5] = (uint8_t)((file_size >> 24) & 0xFFU);
    dst[10] = 54U;
    dst[14] = 40U;
    dst[18] = (uint8_t)(w & 0xFFU);
    dst[19] = (uint8_t)((w >> 8) & 0xFFU);
    dst[22] = (uint8_t)(h & 0xFFU);
    dst[23] = (uint8_t)((h >> 8) & 0xFFU);
    dst[26] = 1U;
    dst[28] = 16U; /* 16 bpp RGB565 (compatible with Load_BMP_To_Buffer) */
    dst[34] = (uint8_t)(img_size & 0xFFU);
    dst[35] = (uint8_t)((img_size >> 8) & 0xFFU);
    dst[36] = (uint8_t)((img_size >> 16) & 0xFFU);
    dst[37] = (uint8_t)((img_size >> 24) & 0xFFU);

    /* Read bottom-up rows directly from ILI9341 GRAM into the BMP payload */
    uint16_t *pixels = (uint16_t *)(dst + 54U);
    for (uint32_t y = 0U; y < h; y++)
    {
        if ((y & 31U) == 0U)
        {
            (void)HAL_IWDG_Refresh(&hiwdg);
        }
        uint32_t bmp_row = (h - 1U) - y;
        lcdReadPixelsRGB565(0U, (uint16_t)y, (uint16_t)w, 1U, &pixels[bmp_row * w]);
    }

    return file_size;
}

static bool write_next_screenshot_sd(const uint8_t *bmp_data, uint32_t bmp_size, char *out_path, uint32_t out_max)
{
    if (!SDCard_IsMounted())
    {
        return false;
    }

    (void)f_mkdir("screenshots");

    static FILINFO fno;
    char candidate[48];
    uint32_t chosen = 999U;
    for (uint32_t i = 1U; i <= 999U; i++)
    {
        snprintf(candidate, sizeof(candidate), "screenshots/shot_%03lu.bmp", (unsigned long)i);
        if (f_stat(candidate, &fno) != FR_OK || fno.fsize == 0U)
        {
            chosen = i;
            break;
        }
    }
    snprintf(candidate, sizeof(candidate), "screenshots/shot_%03lu.bmp", (unsigned long)chosen);

    static FIL f;
    UINT bw = 0U;
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);
    FRESULT res = f_open(&f, candidate, FA_WRITE | FA_CREATE_ALWAYS);
    if (res == FR_OK)
    {
        res = f_write(&f, bmp_data, (UINT)bmp_size, &bw);
        (void)f_close(&f);
    }
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);

    if (res == FR_OK && bw == bmp_size)
    {
        if (out_path != NULL && out_max > 0U)
        {
            snprintf(out_path, out_max, "%s", candidate);
        }
        return true;
    }
    return false;
}

void Screenshot_Request(void)
{
    s_screenshot_pending = true;
}

bool Screenshot_TakeRequest(void)
{
    if (!s_screenshot_pending)
    {
        return false;
    }
    s_screenshot_pending = false;
    return true;
}

bool Screenshot_CaptureToSD(char *out_path, uint32_t out_max)
{
    LVGL_Port_WaitFlushIdle();
    uint8_t *stage    = (uint8_t *)UART_STAGE_BASE;
    uint32_t bmp_size = build_screenshot_bmp16(stage);
    bool ok           = write_next_screenshot_sd(stage, bmp_size, out_path, out_max);

    if (ok)
    {
        Beeper_Beep(35);
    }
    else
    {
        Beeper_ErrorAlert();
    }
    return ok;
}

/* --------------------------------------------------------------------------
 * Protocol 1: Fast Framed Upload ("UPLD" header from tools/uart_upload.py)
 *   Already consumed 'U'; expects 'P','L','D', flags(1B), name_len(1B),
 *   size(4B LE), crc32(4B LE), name[name_len], then 1 KB chunks.
 * -------------------------------------------------------------------------- */

static bool receive_framed_upld(uint8_t *stage, uint32_t *out_size, char *out_name, size_t name_max, uint8_t *out_flags)
{
    uint8_t hdr[13];
    if (!uart_read_exact(hdr, 13U, 2000U))
    {
        return false;
    }
    if (hdr[0] != 'P' || hdr[1] != 'L' || hdr[2] != 'D')
    {
        return false;
    }

    uint8_t  flags    = hdr[3];
    uint8_t  name_len = hdr[4];
    uint32_t size     = (uint32_t)hdr[5] | ((uint32_t)hdr[6] << 8) |
                        ((uint32_t)hdr[7] << 16) | ((uint32_t)hdr[8] << 24);
    uint32_t exp_crc  = (uint32_t)hdr[9] | ((uint32_t)hdr[10] << 8) |
                        ((uint32_t)hdr[11] << 16) | ((uint32_t)hdr[12] << 24);

    if (name_len >= name_max)
    {
        uart_putc('E');
        return false;
    }
    if (name_len > 0U)
    {
        if (!uart_read_exact((uint8_t *)out_name, name_len, 2000U))
        {
            uart_putc('E');
            return false;
        }
    }
    out_name[name_len] = '\0';

    /* ---- Command A: List SD Card Tree / Directory ---- */
    if ((flags & UPLD_FLAG_LIST_SD) != 0U)
    {
        ui_text(86, 68, "SENDING SD DIRECTORY TREE..", COL_GREEN, COL_BG, 1);
        ui_text(86, 92, "                              ", COL_CYAN, COL_BG, 1);
        ui_text(86, 92, (name_len > 0U) ? out_name : "/", COL_CYAN, COL_BG, 1);

        uint32_t list_len = format_sd_listing(out_name, (char *)stage, 32768U);
        uint32_t crc      = crc32_ieee(stage, list_len);

        uart_putc('A');
        uart_write_u32_le(list_len);
        uart_write_u32_le(crc);
        if (!uart_send_chunked(stage, list_len))
        {
            return false;
        }

        *out_size  = 0U;
        *out_flags = flags;
        return true;
    }

    /* ---- Command B: Pull / Download a file from SD Card to PC ---- */
    if ((flags & UPLD_FLAG_PULL_SD) != 0U)
    {
        const char *clean = out_name;
        while (*clean == '/' || *clean == '\\') clean++;

        static FIL pull_file;
        UINT br = 0U;
        if (!SDCard_IsMounted() || f_open(&pull_file, clean, FA_READ) != FR_OK)
        {
            uart_putc('E');
            return false;
        }
        uint32_t fsz = (uint32_t)f_size(&pull_file);
        if (fsz > UART_STAGE_MAX_SIZE || f_read(&pull_file, stage, (UINT)fsz, &br) != FR_OK || br != fsz)
        {
            (void)f_close(&pull_file);
            uart_putc('E');
            return false;
        }
        (void)f_close(&pull_file);

        ui_text(86, 68, "UPLOADING FILE TO PC...    ", COL_GREEN, COL_BG, 1);
        ui_text(86, 92, "                              ", COL_CYAN, COL_BG, 1);
        ui_text(86, 92, clean, COL_CYAN, COL_BG, 1);

        uint32_t crc = crc32_ieee(stage, fsz);
        uart_putc('A');
        uart_write_u32_le(fsz);
        uart_write_u32_le(crc);
        if (!uart_send_chunked(stage, fsz))
        {
            return false;
        }

        *out_size  = 0U;
        *out_flags = flags;
        return true;
    }

    /* ---- Command B2: Capture & Stream Screenshot (and optionally save to SD) ---- */
    if ((flags & UPLD_FLAG_SCREENSHOT) != 0U)
    {
        /* Capture the untouched 320x240 GRAM before drawing the uploader UI */
        uint32_t shot_size = build_screenshot_bmp16(stage);
        ui_draw_frame("SENDING SCREENSHOT TO PC...", "320x240 RGB565 BMP");

        char saved_path[64] = {0};
        if ((flags & UPLD_FLAG_SAVE_SD) != 0U && SDCard_IsMounted())
        {
            if (name_len > 0U)
            {
                (void)save_buffer_to_sd(out_name, stage, shot_size, saved_path, sizeof(saved_path));
            }
            else
            {
                (void)write_next_screenshot_sd(stage, shot_size, saved_path, sizeof(saved_path));
            }
        }

        if (saved_path[0] != '\0')
        {
            ui_text(86, 92, "                              ", COL_CYAN, COL_BG, 1);
            ui_text(86, 92, saved_path, COL_CYAN, COL_BG, 1);
        }

        uint32_t crc = crc32_ieee(stage, shot_size);
        uart_putc('A');
        uart_write_u32_le(shot_size);
        uart_write_u32_le(crc);
        if (!uart_send_chunked(stage, shot_size))
        {
            return false;
        }

        *out_size  = 0U;
        *out_flags = flags;
        return true;
    }

    ui_draw_frame("AUTO-TRIGGERED BY PC...", (name_len > 0U) ? out_name : "SD:/games/<auto>");

    /* ---- Command C: Delete file on SD Card ---- */
    if ((flags & UPLD_FLAG_DELETE_SD) != 0U)
    {
        if (name_len == 0U)
        {
            uart_putc('E');
            return false;
        }
        uart_putc('A');

        char del_path[80];
        const char *clean = out_name;
        while (*clean == '/' || *clean == '\\') clean++;
        if (strchr(clean, '/') == NULL && strchr(clean, '\\') == NULL)
        {
            snprintf(del_path, sizeof(del_path), "games/%.48s", clean);
        }
        else
        {
            snprintf(del_path, sizeof(del_path), "%.60s", clean);
        }

        FRESULT fres = f_unlink(del_path);
        if (fres == FR_OK || fres == FR_NO_FILE)
        {
            ui_text(86, 68, "DELETED FROM SD CARD!      ", COL_GREEN, COL_BG, 1);
            ui_text(86, 92, "                              ", COL_CYAN, COL_BG, 1);
            ui_text(86, 92, del_path, COL_CYAN, COL_BG, 1);
            OSTasks_RequestDirList("/games");
            uart_putc('K');
            *out_size  = 0U;
            *out_flags = flags;
            return true;
        }
        uart_putc('E');
        return false;
    }

    if (name_len == 0U || size == 0U || size > UART_STAGE_MAX_SIZE)
    {
        uart_putc('E');
        return false;
    }

    ui_text(86, 68, "RECEIVING FRAMED STREAM...", COL_GREEN, COL_BG, 1);
    ui_text(86, 92, "                              ", COL_CYAN, COL_BG, 1);
    ui_text(86, 92, out_name, COL_CYAN, COL_BG, 1);

    /* Acknowledge header -> sender begins streaming 1 KB chunks */
    uart_putc('A');

    uint32_t received = 0U;
    while (received < size)
    {
        (void)HAL_IWDG_Refresh(&hiwdg);
        uint32_t chunk = size - received;
        if (chunk > UPLD_CHUNK_SIZE) chunk = UPLD_CHUNK_SIZE;

        if (!uart_read_exact(&stage[received], chunk, 3000U))
        {
            uart_putc('E');
            return false;
        }
        received += chunk;
        uart_putc('A');
        ui_update_progress(received, size);
    }

    uint32_t calc_crc = crc32_ieee(stage, size);
    if (calc_crc != exp_crc)
    {
        uart_putc('C'); /* CRC mismatch */
        ui_text(86, 68, "CRC32 MISMATCH!            ", COL_RED, COL_BG, 1);
        return false;
    }

    if ((flags & UPLD_FLAG_SAVE_SD) != 0U && SDCard_IsMounted())
    {
        char saved_path[80] = {0};
        if (save_buffer_to_sd(out_name, stage, size, saved_path, sizeof(saved_path)))
        {
            ui_text(86, 68, "SAVED TO SD CARD!          ", COL_GREEN, COL_BG, 1);
            ui_text(86, 92, "                              ", COL_CYAN, COL_BG, 1);
            ui_text(86, 92, saved_path, COL_CYAN, COL_BG, 1);
        }
        else
        {
            ui_text(86, 68, "SD WRITE WARN (RAM OK)     ", COL_AMBER, COL_BG, 1);
        }
    }

    uart_putc('K'); /* Complete, verified & saved to SD */
    *out_size  = size;
    *out_flags = flags;
    return true;
}

/* --------------------------------------------------------------------------
 * Protocol 2: Standard XMODEM-CRC / XMODEM-1K
 *   first_byte is SOH (0x01) or STX (0x02).
 * -------------------------------------------------------------------------- */

static bool receive_xmodem(uint8_t first_byte, uint8_t *stage, uint32_t *out_size, char *out_name, size_t name_max)
{
    uint32_t offset   = 0U;
    uint8_t  exp_blk  = 1U;
    uint8_t  cmd      = first_byte;
    uint32_t retries  = 0U;

    ui_text(86, 68, "RECEIVING XMODEM-CRC...    ", COL_GREEN, COL_BG, 1);

    for (;;)
    {
        (void)HAL_IWDG_Refresh(&hiwdg);

        if (cmd == XMODEM_EOT)
        {
            uart_putc(XMODEM_ACK);
            break;
        }
        if (cmd == XMODEM_CAN)
        {
            return false;
        }

        uint32_t blk_sz = (cmd == XMODEM_STX) ? 1024U : ((cmd == XMODEM_SOH) ? 128U : 0U);
        if (blk_sz == 0U || (offset + blk_sz) > UART_STAGE_MAX_SIZE)
        {
            uart_putc(XMODEM_CAN);
            return false;
        }

        uint8_t seq[2];
        uint8_t crc_bytes[2];
        bool ok = uart_read_exact(seq, 2U, 2000U) &&
                  uart_read_exact(&stage[offset], blk_sz, 3000U) &&
                  uart_read_exact(crc_bytes, 2U, 2000U);

        if (ok && ((uint8_t)(seq[0] + seq[1]) == 0xFFU))
        {
            uint16_t rx_crc   = (uint16_t)(((uint16_t)crc_bytes[0] << 8) | crc_bytes[1]);
            uint16_t calc_crc = crc16_ccitt(&stage[offset], blk_sz);

            if (rx_crc == calc_crc)
            {
                if (seq[0] == exp_blk)
                {
                    offset += blk_sz;
                    exp_blk++;
                    ui_update_progress(offset, 0U);
                }
                /* Duplicate previous block is also ACKed without advancing offset */
                uart_putc(XMODEM_ACK);
                retries = 0U;
            }
            else
            {
                uart_putc(XMODEM_NAK);
                if (++retries > 10U) return false;
            }
        }
        else
        {
            uart_putc(XMODEM_NAK);
            if (++retries > 10U) return false;
        }

        if (!uart_getc_timeout(&cmd, 4000U))
        {
            uart_putc(XMODEM_NAK);
            if (++retries > 10U) return false;
            if (!uart_getc_timeout(&cmd, 4000U)) return false;
        }
    }

    if (offset == 0U)
    {
        return false;
    }

    /* Inspect payload: if it has SDK_AppHeader_t ("CAPP"), extract exact image_size and app name */
    const SDK_AppHeader_t *hdr = (const SDK_AppHeader_t *)stage;
    if (offset >= sizeof(SDK_AppHeader_t) && hdr->magic == SDK_APP_MAGIC &&
        hdr->image_size >= sizeof(SDK_AppHeader_t) && hdr->image_size <= offset)
    {
        *out_size = hdr->image_size;
        char safe_name[SDK_APP_NAME_LEN + 1U];
        uint32_t n = 0U;
        for (uint32_t i = 0U; i < SDK_APP_NAME_LEN && hdr->name[i] != '\0'; i++)
        {
            char c = hdr->name[i];
            bool valid = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-');
            safe_name[n++] = valid ? c : '_';
        }
        safe_name[n] = '\0';
        if (n == 0U) strcpy(safe_name, "uart_app");
        snprintf(out_name, name_max, "%s.bin", safe_name);
    }
    else
    {
        /* Text / C-script: strip trailing XMODEM 0x1A (SUB) and 0x00 padding */
        while (offset > 0U && (stage[offset - 1U] == 0x1AU || stage[offset - 1U] == 0x00U))
        {
            offset--;
        }
        *out_size = offset;
        snprintf(out_name, name_max, "uart_app.c");
    }

    return (*out_size > 0U);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void UARTLoader_Init(void)
{
    s_wake_idx     = 0U;
    s_auto_trigger = false;
    s_in_session   = false;
    uart_clear_errors();
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXNE);
}

void UARTLoader_OnRxByte(uint8_t b)
{
    if (s_in_session)
    {
        return;
    }

    if (b == s_wake_seq[s_wake_idx])
    {
        s_wake_idx++;
        if (s_wake_idx >= sizeof(s_wake_seq))
        {
            s_wake_idx     = 0U;
            s_auto_trigger = true;
            if (AppLoader_IsRunning())
            {
                AppLoader_RequestExit();
            }
        }
    }
    else
    {
        s_wake_idx = (b == s_wake_seq[0]) ? 1U : 0U;
    }
}

bool UARTLoader_TakeAutoTrigger(void)
{
    if (!s_auto_trigger)
    {
        return false;
    }
    s_auto_trigger = false;
    return true;
}

bool UARTLoader_IsSessionActive(void)
{
    return s_in_session;
}

bool UARTLoader_RunSession(bool auto_triggered)
{
    if (AppLoader_IsRunning())
    {
        return false;
    }

    s_in_session   = true;
    s_auto_trigger = false;
    s_wake_idx     = 0U;

    LVGL_Port_WaitFlushIdle();

    /* Suspend USART3 IRQ during high-speed polled transfer */
    __HAL_UART_DISABLE_IT(&huart3, UART_IT_RXNE);
    HAL_NVIC_DisableIRQ(USART3_IRQn);
    uart_clear_errors();

    uint8_t *stage = (uint8_t *)UART_STAGE_BASE;
    if (!auto_triggered)
    {
        ui_draw_frame("WAITING FOR PC (ESC: CANCEL)", "SD:/games/<auto>");
    }
    Beeper_Beep(20);

    /* Wait until user releases any held button before checking cancel */
    while ((AppLoader_ReadInputMask_Privileged() & APP_KEY_EXIT) != 0U)
    {
        (void)HAL_IWDG_Refresh(&hiwdg);
    }

    /* Notify tools/uart_upload.py that the console is ready */
    uart_puts_raw("RDY\n");

    uint32_t rx_size   = 0U;
    uint8_t  flags     = UPLD_FLAG_SAVE_SD | UPLD_FLAG_RUN_RAM;
    char     fname[64] = {0};
    bool     ok        = false;
    bool     cancelled = false;

    uint32_t start_wait = HAL_GetTick();
    uint32_t last_poll  = 0U;
    uint32_t wait_limit = auto_triggered ? 6000U : 30000U;

    while ((HAL_GetTick() - start_wait) < wait_limit)
    {
        (void)HAL_IWDG_Refresh(&hiwdg);

        if ((AppLoader_ReadInputMask_Privileged() & APP_KEY_EXIT) != 0U)
        {
            cancelled = true;
            break;
        }

        /* Emit XMODEM-CRC 'C' poll every 1000 ms for standard terminal clients */
        if (!auto_triggered && (HAL_GetTick() - last_poll) >= 1000U)
        {
            last_poll = HAL_GetTick();
            uart_putc(XMODEM_CRC_POLL);
        }

        uint8_t first = 0U;
        if (uart_getc_timeout(&first, 50U))
        {
            if (first == 'U')
            {
                ok = receive_framed_upld(stage, &rx_size, fname, sizeof(fname), &flags);
                if (!ok)
                {
                    break;
                }

                /* If MORE flag is set, stay in the loop for the next command in the batch */
                if ((flags & UPLD_FLAG_MORE) != 0U)
                {
                    start_wait = HAL_GetTick();
                    wait_limit = 5000U;
                    continue;
                }
                break;
            }
            else if (first == XMODEM_SOH || first == XMODEM_STX)
            {
                ok = receive_xmodem(first, stage, &rx_size, fname, sizeof(fname));
                if (ok && rx_size > 0U && SDCard_IsMounted())
                {
                    char saved_path[80] = {0};
                    if (save_buffer_to_sd(fname, stage, rx_size, saved_path, sizeof(saved_path)))
                    {
                        OSTasks_RequestDirList("/games");
                    }
                }
                break;
            }
        }
    }

    /* Restore background USART3 RX interrupt for future auto-triggers */
    uart_clear_errors();
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXNE);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    s_in_session = false;

    if (ok && SDCard_IsMounted())
    {
        OSTasks_RequestDirList("/games");
    }

    if (ok && (flags & (UPLD_FLAG_DELETE_SD | UPLD_FLAG_LIST_SD | UPLD_FLAG_PULL_SD | UPLD_FLAG_SCREENSHOT)) != 0U)
    {
        Beeper_Beep(25);
        return true;
    }

    if (!ok || rx_size == 0U)
    {
        if (!cancelled)
        {
            ui_text(86, 68, "TRANSFER FAILED / TIMEOUT  ", COL_RED, COL_BG, 1);
            Beeper_ErrorAlert();
            uint32_t t = HAL_GetTick();
            while ((HAL_GetTick() - t) < 900U) { (void)HAL_IWDG_Refresh(&hiwdg); }
        }
        return false;
    }

    Beeper_Beep(35);

    /* Launch immediately in Sandbox if requested and file is .bin or .c */
    if ((flags & UPLD_FLAG_RUN_RAM) != 0U)
    {
        if (has_suffix_ci(fname, ".bin") ||
            (rx_size >= sizeof(SDK_AppHeader_t) && ((const SDK_AppHeader_t *)stage)->magic == SDK_APP_MAGIC))
        {
            bool launched = AppLoader_LoadAndRunImage(stage, rx_size);
            memset(stage, 0, rx_size);
            return launched;
        }
        else if (has_suffix_ci(fname, ".c"))
        {
            bool launched = AppLoader_LoadAndRunScriptText(fname, (const char *)stage, rx_size);
            memset(stage, 0, rx_size);
            return launched;
        }
    }

    memset(stage, 0, rx_size);
    return true;
}
