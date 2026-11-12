#include "ui_manager.h"
#include "os_tasks.h"
#include "usb_host_manager.h"
#include "buttons_manager.h"
#include "sdcard_fatfs.h"
#include "system_post.h"
#include "AT24Cxx.h"
#include "beeper.h"
#include "fatfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ccmram.h"
#include "app_loader.h"
#include "uart_loader.h"
#include "temp_sensor.h"
#include "esp_companion.h"
#include "XPT2046_touch.h"
#include "ili9341_simple.h"
#include "lv_port_console.h"

extern RTC_HandleTypeDef  hrtc;
extern IWDG_HandleTypeDef hiwdg;
extern DMA_HandleTypeDef  hdma_memtomem_dma2_stream0;

/* EEPROM Settings Magic & Address (0x00..0x7F reserved for OS; 0x80..0xFF for SDK apps) */
#define EEPROM_CFG_ADDR      0x00U
#define EEPROM_CFG_MAGIC_V1  0xA5U
#define EEPROM_CFG_MAGIC_V2  0xA6U
#define THEME_COUNT          4U

typedef struct {
    uint8_t  magic;          /* EEPROM_CFG_MAGIC_V2 */
    uint8_t  sound_muted;    /* 0 = sound on, 1 = muted */
    uint8_t  theme_id;       /* 0..3 UI color theme */
    uint8_t  stick_deadzone; /* Analog stick deadzone (0..48, default 8) */
    uint16_t touch_min_x;    /* XPT2046 raw calibration bounds */
    uint16_t touch_max_x;
    uint16_t touch_min_y;
    uint16_t touch_max_y;
} System_Config_t;

typedef struct {
    const char *name;
    uint32_t    ws_bg;
    uint32_t    bar_bg;
    uint32_t    title_col;
} UI_Theme_t;

static const UI_Theme_t s_themes[THEME_COUNT] = {
    { "Cyber Blue",  0x1B2234U, 0x12161FU, 0x00E5FFU },
    { "Emerald",     0x0E241BU, 0x081510U, 0x00FF66U },
    { "Retro Amber", 0x261B0EU, 0x160F07U, 0xFFAE00U },
    { "Synthwave",   0x231432U, 0x130A1CU, 0xFF40FFU }
};

static uint8_t s_theme_id = 0U;

/* ==========================================================================
 * Static UI Widgets & State
 * ========================================================================== */

static UI_Screen_t current_screen = UI_SCREEN_HOME;
static int8_t      s_pending_screen = -1; /* Deferred screen switch to avoid LVGL use-after-free */

/* Top Status Bar widgets (always visible, 320x24) */
static lv_obj_t * status_bar       = NULL;
static lv_obj_t * lbl_screen_title = NULL;
static lv_obj_t * lbl_status_icons = NULL;
static lv_obj_t * lbl_clock        = NULL;

/* Main Workspace Container (below status bar, 320x216) */
static lv_obj_t * workspace_cont   = NULL;

/* File Explorer specific widgets & static buffers (zero stack usage) */
static lv_obj_t * file_list_obj     = NULL;
static lv_obj_t * s_editor_ta       = NULL;
static lv_obj_t * s_editor_kb       = NULL;
static lv_obj_t * s_editor_save_lbl = NULL;
static lv_obj_t * s_fname_status_lbl = NULL;
static lv_obj_t * s_ext_btns[6]     = {NULL};
static lv_obj_t * s_wifi_ssid_ta    = NULL;
static lv_obj_t * s_wifi_pass_ta    = NULL;
static lv_obj_t * s_wifi_stat_lbl   = NULL;
static lv_obj_t * s_home_tv         = NULL;
static lv_obj_t * s_home_tile0      = NULL;
static lv_obj_t * s_home_tile1      = NULL;
static lv_obj_t * s_home_p0_btns[5] = {NULL};
static lv_obj_t * s_home_p1_btns[5] = {NULL};
static uint8_t    s_home_active_col = 0U;
static uint32_t   s_last_wth_tick   = 0U;
static uint32_t   s_last_spd_tick   = 0U;
static int8_t     s_last_esp_state  = -2;
CCMRAM_BUF static char s_editor_path[STORAGE_MAX_PATH_LEN];
static bool       s_editor_dirty    = false;
CCMRAM_BUF static char s_action_file_path[STORAGE_MAX_PATH_LEN];
CCMRAM_BUF static char s_action_file_name[STORAGE_MAX_NAME_LEN];
CCMRAM_BUF static char s_clip_path[STORAGE_MAX_PATH_LEN];
CCMRAM_BUF static char s_clip_name[STORAGE_MAX_NAME_LEN];
static bool       s_clip_is_cut     = false;
static uint8_t    s_fname_mode      = 0U;
static uint8_t    s_selected_ext_idx = 0U;
static char       s_orig_rename_ext[12];
CCMRAM_BUF static char s_clicked_item[STORAGE_MAX_NAME_LEN];
CCMRAM_BUF static char s_next_path[STORAGE_MAX_PATH_LEN];
CCMRAM_BUF static char s_title_buf[STORAGE_MAX_PATH_LEN + 16U];
#define EXT_TEXT_PREVIEW_BUF  ((char *)MM_EXT_TEXT_PREVIEW)
#define EXT_TEXT_PREVIEW_SIZE MM_EXT_TEXT_PREVIEW_SIZE

/* Settings Screen RTC adjustment & calibration state */
static uint8_t edit_hours   = 12;
static uint8_t edit_minutes = 0;
static lv_obj_t * lbl_time_set = NULL;
static lv_obj_t * lbl_dz_val   = NULL;
static lv_obj_t * lbl_thm_val  = NULL;

/* Wallpaper selected from the image viewer, persisted on the SD card */
#define WALLPAPER_CFG_PATH  "system/wallpaper.cfg"
CCMRAM_BUF static char s_wallpaper_path[STORAGE_MAX_PATH_LEN];
CCMRAM_BUF static char s_preview_path[STORAGE_MAX_PATH_LEN];

/* Games launcher: built-in ROM game + games/<name>.bin tiles with optional games/<name>.bmp icons */
#define GAMES_MAX_TILES       16U
#define GAME_ICON_SIZE        64
#define GAME_ICON_SLOT_BYTES  ((uint32_t)GAME_ICON_SIZE * (uint32_t)GAME_ICON_SIZE * 2UL)
_Static_assert(GAMES_MAX_TILES * GAME_ICON_SLOT_BYTES <= MM_EXT_IMAGE_BUF_SIZE, "Game icons exceed the ext SRAM image buffer");

typedef struct {
    char file[STORAGE_MAX_NAME_LEN];
    bool builtin;
    bool is_script;
} Game_Entry_t;

static lv_obj_t *              games_grid_obj = NULL;
CCMRAM_BUF static Game_Entry_t s_games[GAMES_MAX_TILES];
static uint32_t                s_games_count  = 0;
static lv_img_dsc_t            s_icon_dsc[GAMES_MAX_TILES];

extern const uint8_t g_rom_arkanoid_bin[];
extern const uint8_t g_rom_arkanoid_bin_end[];

/* Forward declarations */
static void Build_Home_Screen(void);
static void Build_Files_Screen(void);
static void Build_Games_Screen(void);
static void Build_Settings_Screen(void);
static void Build_SysInfo_Screen(void);
static void Build_Wifi_Screen(void);
static void Build_Weather_Screen(void);
static void Build_Calendar_Screen(void);
static void Build_SpeedTest_Screen(void);
static void Populate_File_List_From_Cache(void);
static void Populate_Games_Grid(void);

/* ==========================================================================
 * EEPROM Settings Load / Save
 * ========================================================================== */

static void Apply_Current_Theme(void)
{
    const UI_Theme_t *t = &s_themes[s_theme_id % THEME_COUNT];
    if (status_bar != NULL)
    {
        lv_obj_set_style_bg_color(status_bar, lv_color_hex(t->bar_bg), 0);
    }
    if (lbl_screen_title != NULL)
    {
        lv_obj_set_style_text_color(lbl_screen_title, lv_color_hex(t->title_col), 0);
    }
    if (workspace_cont != NULL)
    {
        lv_obj_set_style_bg_color(workspace_cont, lv_color_hex(t->ws_bg), 0);
    }
}

static void Settings_LoadFromEEPROM(void)
{
    System_Config_t cfg = {0};
    if (AT24Cxx_Read(EEPROM_CFG_ADDR, &cfg, sizeof(cfg)) == HAL_OK)
    {
        if (cfg.magic == EEPROM_CFG_MAGIC_V2)
        {
            Beeper_SetMuted(cfg.sound_muted != 0U);
            s_theme_id = (cfg.theme_id < THEME_COUNT) ? cfg.theme_id : 0U;
            USBHostManager_SetStickDeadzone((cfg.stick_deadzone <= 48U) ? cfg.stick_deadzone : 8U);
            XPT2046_TouchSetCalibration(cfg.touch_min_x, cfg.touch_max_x, cfg.touch_min_y, cfg.touch_max_y);
            printf("[CFG] Loaded EEPROM v2: muted=%u theme=%u dz=%u touch=[%u..%u, %u..%u]\r\n",
                   cfg.sound_muted, s_theme_id, cfg.stick_deadzone,
                   cfg.touch_min_x, cfg.touch_max_x, cfg.touch_min_y, cfg.touch_max_y);
        }
        else if (cfg.magic == EEPROM_CFG_MAGIC_V1)
        {
            Beeper_SetMuted(cfg.sound_muted != 0U);
        }
    }
}

static void Settings_SaveToEEPROM(void)
{
    uint16_t min_x = 0U, max_x = 0U, min_y = 0U, max_y = 0U;
    XPT2046_TouchGetCalibration(&min_x, &max_x, &min_y, &max_y);

    System_Config_t cfg = {
        .magic          = EEPROM_CFG_MAGIC_V2,
        .sound_muted    = Beeper_IsMuted() ? 1U : 0U,
        .theme_id       = s_theme_id,
        .stick_deadzone = USBHostManager_GetStickDeadzone(),
        .touch_min_x    = min_x,
        .touch_max_x    = max_x,
        .touch_min_y    = min_y,
        .touch_max_y    = max_y
    };
    (void)AT24Cxx_Write(EEPROM_CFG_ADDR, &cfg, sizeof(cfg));
}

uint8_t UIManager_GetThemeId(void)
{
    return s_theme_id;
}

void UIManager_SetThemeId(uint8_t id)
{
    if (id < THEME_COUNT && id != s_theme_id)
    {
        s_theme_id = id;
        Apply_Current_Theme();
        Settings_SaveToEEPROM();
        if (lbl_thm_val != NULL)
        {
            lv_label_set_text_fmt(lbl_thm_val, "Theme: %s", s_themes[s_theme_id].name);
        }
    }
}

/* ==========================================================================
 * Universal 16/24/32-bit BMP Loader into External SRAM (nearest-neighbour downscale)
 * ========================================================================== */

static lv_img_dsc_t s_custom_bmp_dsc;
static FIL          s_ui_fil;
static FIL          s_ui_fil_dst;
static FILINFO      s_ui_fno;
#define s_bmp_line_buf ((uint8_t *)MM_EXT_BMP_LINE_BUF)

/* dst_fb must hold max_w * max_h RGB565 pixels */
static bool Load_BMP_To_Buffer(const char *fatfs_path, uint16_t *dst_fb, int32_t max_w, int32_t max_h,
                               lv_img_dsc_t *out_dsc)
{
    UINT br = 0;
    uint8_t hdr[54];

    if (f_open(&s_ui_fil, fatfs_path, FA_READ) != FR_OK)
    {
        printf("[IMG] Cannot open file: %s\r\n", fatfs_path);
        return false;
    }

    if (f_read(&s_ui_fil, hdr, 54, &br) != FR_OK || br < 54 || hdr[0] != 'B' || hdr[1] != 'M')
    {
        printf("[IMG] Invalid BMP header: %s\r\n", fatfs_path);
        f_close(&s_ui_fil);
        return false;
    }

    uint32_t data_offset = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) | ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    int32_t  src_w       = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) | ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int32_t  src_h       = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) | ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
    uint16_t bpp         = (uint16_t)((uint16_t)hdr[28] | ((uint16_t)hdr[29] << 8));

    bool top_down = false;
    if (src_h < 0)
    {
        src_h    = -src_h;
        top_down = true;
    }

    if (src_w <= 0 || src_w > 1440 || src_h <= 0 || src_h > 1080 || (bpp != 24 && bpp != 16 && bpp != 32))
    {
        f_close(&s_ui_fil);
        return false;
    }

    int32_t dst_w = (src_w > max_w) ? max_w : src_w;
    int32_t dst_h = (src_h > max_h) ? max_h : src_h;

    uint32_t row_bytes = ((uint32_t)src_w * (bpp / 8U) + 3U) & ~3U;

    for (int32_t dy = 0; dy < dst_h; dy++)
    {
        int32_t sy = (dy * src_h) / dst_h;
        int32_t file_row = top_down ? sy : (src_h - 1 - sy);
        uint32_t file_pos = data_offset + (uint32_t)file_row * row_bytes;

        if (f_lseek(&s_ui_fil, file_pos) != FR_OK) break;
        if (f_read(&s_ui_fil, s_bmp_line_buf, row_bytes, &br) != FR_OK) break;

        uint16_t *dst_row = &dst_fb[dy * dst_w];

        if (bpp == 24 || bpp == 32)
        {
            uint8_t step = bpp / 8U;
            for (int32_t dx = 0; dx < dst_w; dx++)
            {
                int32_t sx = (dx * src_w) / dst_w;
                uint8_t b = s_bmp_line_buf[sx * step + 0];
                uint8_t g = s_bmp_line_buf[sx * step + 1];
                uint8_t r = s_bmp_line_buf[sx * step + 2];

                lv_color_t c = lv_color_make(r, g, b);
                dst_row[dx] = c.full;
            }
        }
        else if (bpp == 16)
        {
            uint16_t *src16 = (uint16_t *)s_bmp_line_buf;
            for (int32_t dx = 0; dx < dst_w; dx++)
            {
                int32_t sx = (dx * src_w) / dst_w;
                dst_row[dx] = src16[sx];
            }
        }
    }

    f_close(&s_ui_fil);

    memset(out_dsc, 0, sizeof(lv_img_dsc_t));
    out_dsc->header.always_zero = 0;
    out_dsc->header.w           = (uint32_t)dst_w;
    out_dsc->header.h           = (uint32_t)dst_h;
    out_dsc->header.cf          = LV_IMG_CF_TRUE_COLOR;
    out_dsc->data_size          = (uint32_t)(dst_w * dst_h * 2);
    out_dsc->data               = (const uint8_t *)dst_fb;

    return true;
}

static bool Ends_With_IgnoreCase(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    if (suf_len > str_len) return false;

    const char *p = str + (str_len - suf_len);
    for (size_t i = 0; i < suf_len; i++)
    {
        char c1 = p[i];
        char c2 = suffix[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return false;
    }
    return true;
}

static bool Set_Image_From_SD(lv_obj_t *img_obj, const char *fatfs_path)
{
    const char *clean_path = (fatfs_path[0] == '/') ? (fatfs_path + 1) : fatfs_path;

    if (Ends_With_IgnoreCase(clean_path, ".bmp"))
    {
        if (Load_BMP_To_Buffer(clean_path, (uint16_t *)MM_EXT_IMAGE_BUF, 320, 216, &s_custom_bmp_dsc))
        {
            lv_img_set_src(img_obj, &s_custom_bmp_dsc);
            return true;
        }
        return false;
    }

    static char lv_path[STORAGE_MAX_PATH_LEN + 4U];
    snprintf(lv_path, sizeof(lv_path), "S:/%s", clean_path);

    lv_img_header_t header;
    if (lv_img_decoder_get_info(lv_path, &header) != LV_RES_OK)
    {
        return false;
    }

    lv_img_set_src(img_obj, lv_path);
    return true;
}

static void Wallpaper_LoadConfig(void)
{
    UINT br = 0;
    char buf[STORAGE_MAX_PATH_LEN];

    s_wallpaper_path[0] = '\0';
    if (f_open(&s_ui_fil, WALLPAPER_CFG_PATH, FA_READ) != FR_OK)
    {
        return;
    }
    if (f_read(&s_ui_fil, buf, sizeof(buf) - 1U, &br) != FR_OK)
    {
        br = 0;
    }
    f_close(&s_ui_fil);

    buf[br] = '\0';
    while (br > 0U && (buf[br - 1U] == '\r' || buf[br - 1U] == '\n' ||
                       buf[br - 1U] == ' '))
    {
        buf[--br] = '\0';
    }
    memcpy(s_wallpaper_path, buf, br + 1U);
}

static bool Wallpaper_SaveConfig(const char *path)
{
    UINT bw = 0;
    char buf[STORAGE_MAX_PATH_LEN];
    strncpy(buf, path, sizeof(buf) - 1U);
    buf[sizeof(buf) - 1U] = '\0';
    UINT len = (UINT)strlen(buf);

    if (f_open(&s_ui_fil, WALLPAPER_CFG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return false;
    }
    FRESULT res = f_write(&s_ui_fil, buf, len, &bw);
    f_close(&s_ui_fil);
    return (res == FR_OK && bw == len);
}

/* ==========================================================================
 * Helper: Read & Write Hardware RTC
 * ========================================================================== */

static void RTC_GetDateTime(uint8_t *hours, uint8_t *minutes, uint8_t *seconds)
{
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    if (HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN) == HAL_OK)
    {
        (void)HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
        if (hours)   *hours   = sTime.Hours;
        if (minutes) *minutes = sTime.Minutes;
        if (seconds) *seconds = sTime.Seconds;
    }
}

static void RTC_GetCalendarDate(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *weekday)
{
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    (void)HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    if (HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN) == HAL_OK)
    {
        uint16_t y = (uint16_t)(2000U + sDate.Year);
        if (y < 2024U) y = 2026U;
        if (year)    *year    = y;
        if (month)   *month   = (sDate.Month >= 1U && sDate.Month <= 12U) ? sDate.Month : 11U;
        if (day)     *day     = (sDate.Date  >= 1U && sDate.Date  <= 31U) ? sDate.Date  : 1U;
        if (weekday) *weekday = (sDate.WeekDay >= 1U && sDate.WeekDay <= 7U) ? sDate.WeekDay : 1U;
    }
}

static void RTC_SetTimeManual(uint8_t hours, uint8_t minutes)
{
    RTC_TimeTypeDef sTime = {0};
    sTime.Hours          = hours % 24U;
    sTime.Minutes        = minutes % 60U;
    sTime.Seconds        = 0U;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;

    HAL_PWR_EnableBkUpAccess();
    (void)HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0x32F4U);
}

void UIManager_SetDateTime(uint16_t year, uint8_t month, uint8_t date, uint8_t weekday,
                           uint8_t hours, uint8_t minutes, uint8_t seconds)
{
    RTC_TimeTypeDef sTime = {0};
    sTime.Hours          = hours % 24U;
    sTime.Minutes        = minutes % 60U;
    sTime.Seconds        = seconds % 60U;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;

    RTC_DateTypeDef sDate = {0};
    sDate.Year           = (year >= 2000U) ? (uint8_t)(year - 2000U) : (uint8_t)(year % 100U);
    sDate.Month          = (month >= 1U && month <= 12U) ? month : RTC_MONTH_JANUARY;
    sDate.Date           = (date >= 1U && date <= 31U) ? date : 1U;
    sDate.WeekDay        = (weekday >= 1U && weekday <= 7U) ? weekday : RTC_WEEKDAY_MONDAY;

    HAL_PWR_EnableBkUpAccess();
    (void)HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    (void)HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0x32F4U);
}

void UIManager_OpenWifiSetup(void)
{
    s_pending_screen = (int8_t)UI_SCREEN_WIFI;
}

bool UIManager_SetWallpaperPath(const char *rel_path)
{
    if (rel_path == NULL || rel_path[0] == '\0')
    {
        return false;
    }
    const char *clean = (rel_path[0] == '/') ? (rel_path + 1) : rel_path;
    if (!Wallpaper_SaveConfig(clean))
    {
        return false;
    }
    strncpy(s_wallpaper_path, clean, sizeof(s_wallpaper_path) - 1U);
    s_wallpaper_path[sizeof(s_wallpaper_path) - 1U] = '\0';
    if (current_screen == UI_SCREEN_HOME)
    {
        s_pending_screen = (int8_t)UI_SCREEN_HOME;
    }
    return true;
}

/* ==========================================================================
 * Global Key Event Handler (ESC / Gamepad B -> Back to Home Screen)
 * ========================================================================== */

static void global_key_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY)
    {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_ESC && current_screen != UI_SCREEN_HOME)
        {
            Beeper_Click();
            s_pending_screen = (int8_t)UI_SCREEN_HOME;
        }
    }
}

static void btn_back_home_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        Beeper_Click();
        UIManager_ShowScreen(UI_SCREEN_HOME);
    }
}

/* ==========================================================================
 * 1. Top Status Bar & Workspace Initialization
 * ========================================================================== */

void UIManager_Init(void)
{
    /* Load persisted audio/system settings from AT24C02 EEPROM */
    Settings_LoadFromEEPROM();
    Wallpaper_LoadConfig();

    lv_obj_t * scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Create Top Status Bar (320 x 24 px) */
    status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, 320, 24);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x12161F), 0);
    lv_obj_set_style_pad_all(status_bar, 2, 0);

    lbl_screen_title = lv_label_create(status_bar);
    lv_obj_set_style_text_color(lbl_screen_title, lv_color_hex(0x00E5FF), 0);
    lv_obj_align(lbl_screen_title, LV_ALIGN_LEFT_MID, 4, 0);
    lv_label_set_text(lbl_screen_title, "STM32 OS");

    lbl_status_icons = lv_label_create(status_bar);
    lv_label_set_recolor(lbl_status_icons, true);
    lv_obj_align(lbl_status_icons, LV_ALIGN_CENTER, 4, 0);
    lv_label_set_text(lbl_status_icons, "#555555 SD K M G W#");

    lbl_clock = lv_label_create(status_bar);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_clock, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_label_set_text(lbl_clock, "00:00:00");

    /* 2. Create Main Workspace Container (320 x 216 px) */
    workspace_cont = lv_obj_create(scr);
    lv_obj_set_size(workspace_cont, 320, 216);
    lv_obj_align(workspace_cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(workspace_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(workspace_cont, 0, 0);
    lv_obj_set_style_border_width(workspace_cont, 0, 0);
    lv_obj_set_style_pad_all(workspace_cont, 0, 0);
    Apply_Current_Theme();

    /* 3. Show Home Screen by default */
    UIManager_ShowScreen(UI_SCREEN_HOME);
}

void UIManager_Update(void)
{
    /* 0. Check for sandbox crashes and show a modal alert */
    AppLoader_FaultInfo_t fault;
    if (AppLoader_TakeFaultInfo(&fault))
    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Application Crashed!\n\n"
                 "PC:  0x%08lX\n"
                 "CFSR: 0x%08lX\n"
                 "Addr: 0x%08lX",
                 (unsigned long)fault.pc,
                 (unsigned long)fault.cfsr,
                 (unsigned long)fault.fault_addr);

        lv_obj_t * mbox = lv_msgbox_create(NULL, "Hardware Fault", msg, NULL, false);
        lv_obj_center(mbox);
        lv_obj_set_style_bg_color(mbox, lv_color_hex(0x400000), 0);
        lv_obj_set_style_text_color(mbox, lv_color_hex(0xFFFFFF), 0);
        /* Delete the modal backdrop after 10 seconds or on screen change */
        lv_obj_del_delayed(lv_obj_get_parent(mbox), 10000);
        Beeper_ErrorAlert();
    }

    /* Process deferred screen transitions safely outside LVGL input callbacks! */

    if (s_pending_screen >= 0)
    {
        UI_Screen_t target = (UI_Screen_t)s_pending_screen;
        s_pending_screen = -1;
        UIManager_ShowScreen(target);
    }

    static uint32_t last_status_tick = 0;

    if ((HAL_GetTick() - last_status_tick) < 250U)
    {
        return;
    }
    last_status_tick = HAL_GetTick();

    uint8_t hrs = 0, mins = 0, secs = 0;
    RTC_GetDateTime(&hrs, &mins, &secs);
    lv_label_set_text_fmt(lbl_clock, "%02u:%02u:%02u", hrs, mins, secs);

    const char *c_sd  = SDCard_IsMounted()             ? "#00FF66 SD#" : "#555555 SD#";
    const char *c_kbd = USBHostManager_IsKeyboardReady()? "#00E5FF K#"  : "#555555 K#";
    const char *c_mse = USBHostManager_IsMouseReady()   ? "#00E5FF M#"  : "#555555 M#";
    const char *c_pad = USBHostManager_IsGamepadReady() ? "#FFAE00 G#"  : "#555555 G#";

    const char *c_esp;
    if (!ESPCompanion_IsConnected())
    {
        c_esp = "#555555 W#"; /* Gray: module offline */
    }
    else if (ESPCompanion_IsWifiConnected())
    {
        c_esp = "#00FF66 W#"; /* Bright Green: connected to Wi-Fi AP + IP assigned */
    }
    else if (ESPCompanion_GetWifiStatus() == 2)
    {
        c_esp = "#00E5FF W#"; /* Cyan: connecting in progress */
    }
    else
    {
        c_esp = "#FFAE00 W#"; /* Amber: module online on UART4, but Wi-Fi not connected */
    }

    const char *c_hot = TempSensor_IsOverheated()       ? " #FF2222 HOT!#" : "";

    lv_label_set_text_fmt(lbl_status_icons, "%s %s %s %s %s%s", c_sd, c_kbd, c_mse, c_pad, c_esp, c_hot);

    if (current_screen == UI_SCREEN_WIFI && s_wifi_stat_lbl != NULL)
    {
        if (ESPCompanion_IsWifiConnected())
        {
            lv_label_set_text_fmt(s_wifi_stat_lbl, "#00FF66 Connected:# %s (%s, %ddBm)",
                                  ESPCompanion_GetWifiSsid(), ESPCompanion_GetWifiIp(), (int)ESPCompanion_GetWifiRssi());
        }
        else if (ESPCompanion_GetWifiStatus() == 2)
        {
            lv_label_set_text_fmt(s_wifi_stat_lbl, "#00E5FF Connecting to %s...#", ESPCompanion_GetWifiSsid());
        }
        else if (ESPCompanion_GetWifiStatus() == -1)
        {
            lv_label_set_text_fmt(s_wifi_stat_lbl, "#FF5555 Connection failed:# %s", ESPCompanion_GetWifiSsid());
        }
    }

    const ESP_WeatherInfo_t   *wth = ESPCompanion_GetWeather();
    const ESP_SpeedTestInfo_t *spd = ESPCompanion_GetSpeedTest();
    int8_t cur_esp_state = !ESPCompanion_IsConnected() ? 0
                         : (ESPCompanion_IsWifiConnected() ? 2 : 1);

    if ((wth->valid && wth->updated_tick != s_last_wth_tick) ||
        (cur_esp_state != s_last_esp_state))
    {
        s_last_wth_tick  = wth->updated_tick;
        s_last_esp_state = cur_esp_state;
        if (current_screen == UI_SCREEN_WEATHER)
        {
            s_pending_screen = (int8_t)UI_SCREEN_WEATHER;
        }
        else if (current_screen == UI_SCREEN_SPEEDTEST)
        {
            s_pending_screen = (int8_t)UI_SCREEN_SPEEDTEST;
        }
        else if (current_screen == UI_SCREEN_HOME && s_home_active_col == 1U)
        {
            s_pending_screen = (int8_t)UI_SCREEN_HOME;
        }
    }

    if (spd->updated_tick != s_last_spd_tick)
    {
        s_last_spd_tick = spd->updated_tick;
        if (current_screen == UI_SCREEN_SPEEDTEST)
        {
            s_pending_screen = (int8_t)UI_SCREEN_SPEEDTEST;
        }
        else if (current_screen == UI_SCREEN_HOME && s_home_active_col == 1U)
        {
            s_pending_screen = (int8_t)UI_SCREEN_HOME;
        }
    }
}

void UIManager_OnAppExit(void)
{
    if (current_screen == UI_SCREEN_FILES)
    {
        /* Keep SD Explorer or Code Editor open exactly where the user launched from */
        lv_obj_invalidate(lv_scr_act());
    }
    else
    {
        UIManager_ShowScreen(UI_SCREEN_GAMES);
        lv_obj_invalidate(lv_scr_act());
    }
}

void UIManager_ShowScreen(UI_Screen_t screen)
{
    if (workspace_cont == NULL) return;

    current_screen     = screen;
    file_list_obj      = NULL;
    s_editor_ta        = NULL;
    s_editor_kb        = NULL;
    s_editor_save_lbl  = NULL;
    s_fname_status_lbl = NULL;
    s_wifi_ssid_ta     = NULL;
    s_wifi_pass_ta     = NULL;
    s_wifi_stat_lbl    = NULL;
    s_home_tv          = NULL;
    s_home_tile0       = NULL;
    s_home_tile1       = NULL;
    memset(s_home_p0_btns, 0, sizeof(s_home_p0_btns));
    memset(s_home_p1_btns, 0, sizeof(s_home_p1_btns));
    memset(s_ext_btns, 0, sizeof(s_ext_btns));
    games_grid_obj     = NULL;
    lbl_time_set       = NULL;
    lbl_dz_val         = NULL;
    lbl_thm_val        = NULL;

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL)
    {
        lv_group_remove_all_objs(grp);
    }

    lv_obj_clean(workspace_cont);

    switch (screen)
    {
        case UI_SCREEN_HOME:
            lv_label_set_text(lbl_screen_title, s_home_active_col == 0U ? "STM32 OS" : "Widgets");
            Build_Home_Screen();
            break;

        case UI_SCREEN_FILES:
            lv_label_set_text(lbl_screen_title, "SD Explorer");
            Build_Files_Screen();
            break;

        case UI_SCREEN_GAMES:
            lv_label_set_text(lbl_screen_title, "Games");
            Build_Games_Screen();
            break;

        case UI_SCREEN_SETTINGS:
            lv_label_set_text(lbl_screen_title, "Settings");
            Build_Settings_Screen();
            break;

        case UI_SCREEN_SYSINFO:
            lv_label_set_text(lbl_screen_title, "System Info");
            Build_SysInfo_Screen();
            break;

        case UI_SCREEN_WIFI:
            lv_label_set_text(lbl_screen_title, "Wi-Fi Setup");
            Build_Wifi_Screen();
            break;

        case UI_SCREEN_WEATHER:
            lv_label_set_text(lbl_screen_title, "Weather Station");
            Build_Weather_Screen();
            break;

        case UI_SCREEN_CALENDAR:
            lv_label_set_text(lbl_screen_title, "Calendar");
            Build_Calendar_Screen();
            break;

        case UI_SCREEN_SPEEDTEST:
            lv_label_set_text(lbl_screen_title, "SpeedTest");
            Build_SpeedTest_Screen();
            break;

        default:
            Build_Home_Screen();
            break;
    }
}

/* ==========================================================================
 * 2. Multi-Page Home Screen Builder (Swipe Tile 0: Apps <-> Tile 1: Widgets)
 * ========================================================================== */

static const char *Weather_WmoDescription(uint8_t wmo)
{
    if (wmo == 0U) return "Clear Sky";
    if (wmo == 1U) return "Mainly Clear";
    if (wmo == 2U) return "Partly Cloudy";
    if (wmo == 3U) return "Overcast";
    if (wmo == 45U || wmo == 48U) return "Foggy";
    if (wmo >= 51U && wmo <= 57U) return "Drizzle";
    if (wmo >= 61U && wmo <= 67U) return "Rain";
    if (wmo >= 71U && wmo <= 77U) return "Snow";
    if (wmo >= 80U && wmo <= 82U) return "Rain Showers";
    if (wmo >= 85U && wmo <= 86U) return "Snow Showers";
    if (wmo >= 95U) return "Thunderstorm";
    return "Cloudy";
}

static const char *Weather_WmoIconFile(uint8_t wmo)
{
    if (wmo == 0U || wmo == 1U) return "assets/weather/sun.bmp";
    if (wmo == 2U || wmo == 3U) return "assets/weather/cloud.bmp";
    if (wmo == 45U || wmo == 48U) return "assets/weather/fog.bmp";
    if (wmo >= 51U && wmo <= 67U) return "assets/weather/rain.bmp";
    if (wmo >= 71U && wmo <= 86U) return "assets/weather/snow.bmp";
    if (wmo >= 95U) return "assets/weather/storm.bmp";
    return "assets/weather/cloud.bmp";
}

static void Create_Weather_Graphic(lv_obj_t *parent, int16_t x, int16_t y, uint8_t wmo, bool allow_sd_bmp)
{
    if (allow_sd_bmp && SDCard_IsMounted())
    {
        const char *bmp_path = Weather_WmoIconFile(wmo);
        if (f_stat(bmp_path, &s_ui_fno) == FR_OK)
        {
            lv_obj_t *img = lv_img_create(parent);
            if (Set_Image_From_SD(img, bmp_path))
            {
                lv_obj_set_pos(img, x, y);
                return;
            }
            lv_obj_del(img);
        }
    }

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 64, 56);
    lv_obj_set_pos(box, x, y);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    /* Sun disc for clear / partly cloudy */
    if (wmo <= 2U)
    {
        lv_obj_t *sun = lv_obj_create(box);
        int16_t sz = (wmo <= 1U) ? 38 : 28;
        lv_obj_set_size(sun, sz, sz);
        lv_obj_set_pos(sun, (wmo <= 1U) ? 13 : 6, (wmo <= 1U) ? 9 : 4);
        lv_obj_clear_flag(sun, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sun, lv_color_hex(0xFFD600), 0);
        lv_obj_set_style_border_width(sun, 3, 0);
        lv_obj_set_style_border_color(sun, lv_color_hex(0xFF9100), 0);
    }

    /* Cloud body for cloudy / fog / rain / snow / storm */
    if (wmo >= 2U)
    {
        uint32_t c_hex = (wmo >= 95U) ? 0x546E7AU : ((wmo >= 51U) ? 0x90A4AEU : 0xECEFF1U);

        lv_obj_t *c1 = lv_obj_create(box);
        lv_obj_set_size(c1, 24, 24);
        lv_obj_set_pos(c1, 12, 14);
        lv_obj_clear_flag(c1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(c1, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(c1, lv_color_hex(c_hex), 0);
        lv_obj_set_style_border_width(c1, 0, 0);

        lv_obj_t *c2 = lv_obj_create(box);
        lv_obj_set_size(c2, 20, 20);
        lv_obj_set_pos(c2, 28, 17);
        lv_obj_clear_flag(c2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(c2, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(c2, lv_color_hex(c_hex), 0);
        lv_obj_set_style_border_width(c2, 0, 0);

        lv_obj_t *base = lv_obj_create(box);
        lv_obj_set_size(base, 44, 16);
        lv_obj_set_pos(base, 10, 24);
        lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(base, 8, 0);
        lv_obj_set_style_bg_color(base, lv_color_hex(c_hex), 0);
        lv_obj_set_style_border_width(base, 0, 0);
    }

    /* Raindrops / Snowflakes / Fog bars / Lightning */
    if ((wmo >= 51U && wmo <= 67U) || (wmo >= 80U && wmo <= 82U))
    {
        for (int i = 0; i < 3; i++)
        {
            lv_obj_t *drop = lv_obj_create(box);
            lv_obj_set_size(drop, 4, 10);
            lv_obj_set_pos(drop, (int16_t)(16 + i * 13), 43);
            lv_obj_clear_flag(drop, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(drop, 2, 0);
            lv_obj_set_style_bg_color(drop, lv_color_hex(0x00E5FF), 0);
            lv_obj_set_style_border_width(drop, 0, 0);
        }
    }
    else if ((wmo >= 71U && wmo <= 77U) || (wmo >= 85U && wmo <= 86U))
    {
        for (int i = 0; i < 3; i++)
        {
            lv_obj_t *flk = lv_obj_create(box);
            lv_obj_set_size(flk, 6, 6);
            lv_obj_set_pos(flk, (int16_t)(15 + i * 13), 44);
            lv_obj_clear_flag(flk, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(flk, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(flk, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(flk, 0, 0);
        }
    }
    else if (wmo >= 95U)
    {
        lv_obj_t *bolt = lv_label_create(box);
        lv_obj_set_style_text_color(bolt, lv_color_hex(0xFFD600), 0);
        lv_obj_set_pos(bolt, 24, 38);
        lv_label_set_text(bolt, LV_SYMBOL_CHARGE);
    }
    else if (wmo == 45U || wmo == 48U)
    {
        for (int i = 0; i < 2; i++)
        {
            lv_obj_t *fog = lv_obj_create(box);
            lv_obj_set_size(fog, 38, 3);
            lv_obj_set_pos(fog, 13, (int16_t)(43 + i * 6));
            lv_obj_clear_flag(fog, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(fog, lv_color_hex(0xB0BEC5), 0);
            lv_obj_set_style_border_width(fog, 0, 0);
        }
    }
}

static void Sync_Home_Group_To_Active_Page(void)
{
    lv_group_t * grp = lv_group_get_default();
    if (grp == NULL)
    {
        return;
    }

    lv_group_remove_all_objs(grp);

    if (s_home_active_col == 0U)
    {
        for (uint8_t i = 0U; i < 5U; i++)
        {
            if (s_home_p0_btns[i] != NULL)
            {
                lv_group_add_obj(grp, s_home_p0_btns[i]);
            }
        }
        if (s_home_p0_btns[0] != NULL)
        {
            lv_group_focus_obj(s_home_p0_btns[0]);
        }
    }
    else
    {
        for (uint8_t i = 0U; i < 5U; i++)
        {
            if (s_home_p1_btns[i] != NULL)
            {
                lv_group_add_obj(grp, s_home_p1_btns[i]);
            }
        }
        if (s_home_p1_btns[0] != NULL)
        {
            lv_group_focus_obj(s_home_p1_btns[0]);
        }
    }
}

static void home_tileview_changed_cb(lv_event_t * e)
{
    lv_obj_t * tv = lv_event_get_target(e);
    lv_obj_t * act = lv_tileview_get_tile_act(tv);
    uint8_t new_col = (act == s_home_tile1) ? 1U : 0U;
    if (new_col != s_home_active_col)
    {
        s_home_active_col = new_col;
        lv_label_set_text(lbl_screen_title, s_home_active_col == 0U ? "STM32 OS" : "Widgets");
        Sync_Home_Group_To_Active_Page();
    }
}

static void home_page_switch_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC && s_home_tv != NULL && s_home_active_col == 1U)
    {
        s_home_active_col = 0U;
        Beeper_Click();
        lv_obj_set_tile_id(s_home_tv, 0, 0, LV_ANIM_ON);
        lv_label_set_text(lbl_screen_title, "STM32 OS");
        Sync_Home_Group_To_Active_Page();
        return;
    }
    if (code == LV_EVENT_CLICKED && s_home_tv != NULL)
    {
        uint8_t target_col = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
        s_home_active_col = target_col;
        Beeper_Click();
        lv_obj_set_tile_id(s_home_tv, target_col, 0, LV_ANIM_ON);
        lv_label_set_text(lbl_screen_title, target_col == 0U ? "STM32 OS" : "Widgets");
        Sync_Home_Group_To_Active_Page();
    }
}

static void home_icon_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC && s_home_tv != NULL && s_home_active_col == 1U)
    {
        s_home_active_col = 0U;
        Beeper_Click();
        lv_obj_set_tile_id(s_home_tv, 0, 0, LV_ANIM_ON);
        lv_label_set_text(lbl_screen_title, "STM32 OS");
        Sync_Home_Group_To_Active_Page();
        return;
    }
    if (code == LV_EVENT_CLICKED)
    {
        UI_Screen_t target = (UI_Screen_t)(uintptr_t)lv_event_get_user_data(e);
        Beeper_Click();
        UIManager_ShowScreen(target);
    }
}

static void reserved_tile_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC && s_home_tv != NULL && s_home_active_col == 1U)
    {
        s_home_active_col = 0U;
        Beeper_Click();
        lv_obj_set_tile_id(s_home_tv, 0, 0, LV_ANIM_ON);
        lv_label_set_text(lbl_screen_title, "STM32 OS");
        Sync_Home_Group_To_Active_Page();
        return;
    }
    if (code == LV_EVENT_CLICKED)
    {
        Beeper_Click();
    }
}

static lv_obj_t *Create_Desktop_Shortcut(lv_obj_t *parent, int16_t x, int16_t y,
                                         const char *symbol, const char *title,
                                         uint32_t accent_hex, UI_Screen_t target_screen)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 136, 82);
    lv_obj_set_pos(btn, x, y);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x252F48), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(accent_hex), 0);

    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x324164), LV_STATE_FOCUSED);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "%s\n%s", symbol, title);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, home_icon_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)target_screen);
    lv_obj_add_event_cb(btn, home_icon_event_cb, LV_EVENT_KEY, (void *)(uintptr_t)target_screen);

    return btn;
}

static bool Home_TryWallpaper(const char *path)
{
    if (path == NULL || path[0] == '\0' || f_stat(path, &s_ui_fno) != FR_OK)
    {
        return false;
    }

    lv_obj_t * bg_img = lv_img_create(workspace_cont);
    if (Set_Image_From_SD(bg_img, path))
    {
        lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
        return true;
    }
    lv_obj_del(bg_img);
    return false;
}

static void Build_Home_Screen(void)
{
    static const char * const wall_candidates[] = {
        "assets/wall.bmp",
        "wall.bmp",
        "assets/wall.png",
        "assets/wall.jpg"
    };

    if (SDCard_IsMounted() && !Home_TryWallpaper(s_wallpaper_path))
    {
        for (size_t i = 0; i < sizeof(wall_candidates) / sizeof(wall_candidates[0]); i++)
        {
            if (Home_TryWallpaper(wall_candidates[i]))
            {
                break;
            }
        }
    }

    /* Create horizontal 2-page swipeable Tileview */
    s_home_tv = lv_tileview_create(workspace_cont);
    lv_obj_set_size(s_home_tv, 320, 216);
    lv_obj_set_pos(s_home_tv, 0, 0);
    lv_obj_set_style_bg_opa(s_home_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_home_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_home_tv, home_tileview_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_home_tile0 = lv_tileview_add_tile(s_home_tv, 0, 0, LV_DIR_RIGHT);
    s_home_tile1 = lv_tileview_add_tile(s_home_tv, 1, 0, LV_DIR_LEFT);
    lv_obj_set_style_bg_opa(s_home_tile0, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_home_tile1, LV_OPA_TRANSP, 0);

    /* ---- Page 1 (Tile 0): 4 Main Launcher Shortcuts ---- */
    s_home_p0_btns[0] = Create_Desktop_Shortcut(s_home_tile0, 16,  10, LV_SYMBOL_DIRECTORY, "SD Files",    0x00E5FF, UI_SCREEN_FILES);
    s_home_p0_btns[1] = Create_Desktop_Shortcut(s_home_tile0, 168, 10, LV_SYMBOL_PLAY,      "Games",       0x00FF66, UI_SCREEN_GAMES);
    s_home_p0_btns[2] = Create_Desktop_Shortcut(s_home_tile0, 16,  102, LV_SYMBOL_SETTINGS, "Settings",    0xFFAE00, UI_SCREEN_SETTINGS);
    s_home_p0_btns[3] = Create_Desktop_Shortcut(s_home_tile0, 168, 102, LV_SYMBOL_LIST,     "System Info", 0xB388FF, UI_SCREEN_SYSINFO);

    lv_obj_t * btn_to_p2 = lv_btn_create(s_home_tile0);
    lv_obj_set_size(btn_to_p2, 136, 20);
    lv_obj_align(btn_to_p2, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(btn_to_p2, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_bg_color(btn_to_p2, lv_color_hex(0x1B2234), 0);
    lv_obj_set_style_bg_opa(btn_to_p2, LV_OPA_80, 0);
    lv_obj_set_style_border_width(btn_to_p2, 1, 0);
    lv_obj_set_style_border_color(btn_to_p2, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_color(btn_to_p2, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn_to_p2, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn_to_p2, lv_color_hex(0x324164), LV_STATE_FOCUSED);
    lv_obj_t * lbl_p2 = lv_label_create(btn_to_p2);
    lv_label_set_text(lbl_p2, "Widgets  o . " LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_p2);
    lv_obj_add_event_cb(btn_to_p2, home_page_switch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1U);
    lv_obj_add_event_cb(btn_to_p2, home_page_switch_cb, LV_EVENT_KEY, (void *)(uintptr_t)1U);
    s_home_p0_btns[4] = btn_to_p2;

    /* ---- Page 2 (Tile 1): Standardized 2x2 4-Tile Widgets Grid ---- */
    const ESP_WeatherInfo_t   *wth = ESPCompanion_GetWeather();
    const ESP_SpeedTestInfo_t *spd = ESPCompanion_GetSpeedTest();
    bool esp_online = ESPCompanion_IsConnected();
    bool wifi_ok    = ESPCompanion_IsWifiConnected();
    bool ext_ok     = false;
    int32_t ext_dc  = TempSensor_GetCachedExtDeciC(&ext_ok);

    /* Tile [0] Top-Left (16, 10): Weather Station */
    lv_obj_t * w_btn = Create_Desktop_Shortcut(s_home_tile1, 16, 10, LV_SYMBOL_EYE_OPEN, "Weather",
                                               esp_online ? 0x00E5FFU : 0xFFAE00U, UI_SCREEN_WEATHER);
    lv_obj_t * w_lbl = lv_obj_get_child(w_btn, 0);
    lv_label_set_recolor(w_lbl, true);
    if (esp_online && wth->valid)
    {
        lv_label_set_text_fmt(w_lbl,
            "#00E5FF " LV_SYMBOL_EYE_OPEN " %s#\n#FFD600 %s%d.%d C#\n#FFFFFF %s#",
            wth->city,
            (wth->temp_deci < 0) ? "-" : "+", abs(wth->temp_deci) / 10, abs(wth->temp_deci) % 10,
            Weather_WmoDescription(wth->wmo_code));
    }
    else if (ext_ok)
    {
        lv_label_set_text_fmt(w_lbl,
            "#FFAE00 " LV_SYMBOL_EYE_OPEN " Weather#\n#FFD600 %s%ld.%ld C#\n#90A4AE Room DS18B20#",
            (ext_dc < 0) ? "-" : "+", (long)(abs(ext_dc) / 10), (long)(abs(ext_dc) % 10));
    }
    else
    {
        lv_label_set_text_fmt(w_lbl,
            "#00E5FF " LV_SYMBOL_EYE_OPEN " Weather#\n#FFD600 --.- C#\n#90A4AE %s#",
            esp_online ? (wifi_ok ? "Syncing..." : "Wi-Fi Off") : "Offline");
    }
    s_home_p1_btns[0] = w_btn;

    /* Tile [1] Top-Right (168, 10): Calendar & Date */
    uint16_t yr = 2026U;
    uint8_t  mo = 11U, dy = 11U, wd = 3U;
    RTC_GetCalendarDate(&yr, &mo, &dy, &wd);
    static const char * const s_wd_short[8] = {
        "", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
    };
    static const char * const s_mo_short[13] = {
        "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    lv_obj_t * c_btn = Create_Desktop_Shortcut(s_home_tile1, 168, 10, LV_SYMBOL_BELL, "Calendar",
                                               0x00FF66U, UI_SCREEN_CALENDAR);
    lv_obj_t * c_lbl = lv_obj_get_child(c_btn, 0);
    lv_label_set_recolor(c_lbl, true);
    lv_label_set_text_fmt(c_lbl,
        "#00FF66 " LV_SYMBOL_BELL " Calendar#\n#FFFFFF %s %02u %s#\n#90A4AE %04u (%s)#",
        s_wd_short[(wd <= 7U) ? wd : 1U],
        (unsigned int)dy, s_mo_short[(mo <= 12U) ? mo : 1U],
        (unsigned int)yr,
        wifi_ok ? "NTP" : "RTC");
    s_home_p1_btns[1] = c_btn;

    /* Tile [2] Bottom-Left (16, 102): SpeedTest */
    lv_obj_t * s_btn = Create_Desktop_Shortcut(s_home_tile1, 16, 102, LV_SYMBOL_CHARGE, "SpeedTest",
                                               0xFFAE00U, UI_SCREEN_SPEEDTEST);
    lv_obj_t * s_lbl = lv_obj_get_child(s_btn, 0);
    lv_label_set_recolor(s_lbl, true);
    if (spd->state == 1U)
    {
        lv_label_set_text(s_lbl, "#FFAE00 " LV_SYMBOL_CHARGE " SpeedTest#\n#00E5FF Running...#\n#90A4AE Measuring#");
    }
    else if (spd->state == 2U)
    {
        lv_label_set_text_fmt(s_lbl,
            "#FFAE00 " LV_SYMBOL_CHARGE " SpeedTest#\n#00FF66 %u.%u Mbps#\n#90A4AE %ums | %uKB/s#",
            (unsigned int)(spd->dl_kbps / 1000U), (unsigned int)((spd->dl_kbps % 1000U) / 100U),
            (unsigned int)spd->ping_ms,
            (unsigned int)(spd->link_kbs_x10 / 10U));
    }
    else
    {
        lv_label_set_text_fmt(s_lbl,
            "#FFAE00 " LV_SYMBOL_CHARGE " SpeedTest#\n#FFFFFF Net & Link#\n#90A4AE %s#",
            esp_online ? "Tap to test" : "ESP32 Offline");
    }
    s_home_p1_btns[2] = s_btn;

    /* Tile [3] Bottom-Right (168, 102): Reserved Slot */
    lv_obj_t * r_btn = lv_btn_create(s_home_tile1);
    lv_obj_set_size(r_btn, 136, 82);
    lv_obj_set_pos(r_btn, 168, 102);
    lv_obj_clear_flag(r_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_radius(r_btn, 10, 0);
    lv_obj_set_style_bg_color(r_btn, lv_color_hex(0x1B2234), 0);
    lv_obj_set_style_bg_opa(r_btn, LV_OPA_60, 0);
    lv_obj_set_style_border_width(r_btn, 2, 0);
    lv_obj_set_style_border_color(r_btn, lv_color_hex(0x546E7A), 0);
    lv_obj_set_style_border_color(r_btn, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(r_btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(r_btn, lv_color_hex(0x283550), LV_STATE_FOCUSED);
    lv_obj_t * r_lbl = lv_label_create(r_btn);
    lv_label_set_recolor(r_lbl, true);
    lv_obj_set_style_text_align(r_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(r_lbl, "#90A4AE " LV_SYMBOL_PLUS " Reserved#\n#546E7A Future Slot#");
    lv_obj_center(r_lbl);
    lv_obj_add_event_cb(r_btn, reserved_tile_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(r_btn, reserved_tile_cb, LV_EVENT_KEY, NULL);
    s_home_p1_btns[3] = r_btn;

    lv_obj_t * btn_to_p1 = lv_btn_create(s_home_tile1);
    lv_obj_set_size(btn_to_p1, 136, 20);
    lv_obj_align(btn_to_p1, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(btn_to_p1, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_bg_color(btn_to_p1, lv_color_hex(0x1B2234), 0);
    lv_obj_set_style_bg_opa(btn_to_p1, LV_OPA_80, 0);
    lv_obj_set_style_border_width(btn_to_p1, 1, 0);
    lv_obj_set_style_border_color(btn_to_p1, lv_color_hex(0x00FF66), 0);
    lv_obj_set_style_border_color(btn_to_p1, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn_to_p1, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn_to_p1, lv_color_hex(0x324164), LV_STATE_FOCUSED);
    lv_obj_t * lbl_p1 = lv_label_create(btn_to_p1);
    lv_label_set_text(lbl_p1, LV_SYMBOL_LEFT " . o  Launcher");
    lv_obj_center(lbl_p1);
    lv_obj_add_event_cb(btn_to_p1, home_page_switch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)0U);
    lv_obj_add_event_cb(btn_to_p1, home_page_switch_cb, LV_EVENT_KEY, (void *)(uintptr_t)0U);
    s_home_p1_btns[4] = btn_to_p1;

    if (s_home_active_col == 1U)
    {
        lv_obj_set_tile_id(s_home_tv, 1, 0, LV_ANIM_OFF);
    }

    Sync_Home_Group_To_Active_Page();
}

/* ==========================================================================
 * 3. File Explorer Screen (+ BMP Image Viewer & TXT Text Viewer!)
 * ========================================================================== */

static void close_preview_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED ||
        (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC))
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
    }
}

static void set_wallpaper_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }
    if (code != LV_EVENT_CLICKED)
    {
        return;
    }

    lv_obj_t * lbl = lv_obj_get_child(lv_event_get_target(e), 0);
    if (Wallpaper_SaveConfig(s_preview_path))
    {
        strncpy(s_wallpaper_path, s_preview_path, sizeof(s_wallpaper_path) - 1U);
        s_wallpaper_path[sizeof(s_wallpaper_path) - 1U] = '\0';
        Beeper_Click();
        if (lbl != NULL) lv_label_set_text(lbl, LV_SYMBOL_OK " Wallpaper");
    }
    else
    {
        Beeper_ErrorAlert();
        if (lbl != NULL) lv_label_set_text(lbl, LV_SYMBOL_WARNING " Failed");
    }
}

static void Show_App_Load_Error(void)
{
    lv_obj_t * mbox = lv_msgbox_create(NULL, "Cannot start app", AppLoader_GetLastError(), NULL, false);
    lv_obj_center(mbox);
    /* With a NULL parent LVGL wraps the box in a dimmed modal backdrop: delete the backdrop */
    lv_obj_del_delayed(lv_obj_get_parent(mbox), 3000);
    Beeper_ErrorAlert();
}

static void Open_Image_Preview(const char *fatfs_path)
{
    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    file_list_obj = NULL;
    lv_obj_clean(workspace_cont);

    bool decoded = false;
    lv_obj_t * img = lv_img_create(workspace_cont);
    if (Set_Image_From_SD(img, fatfs_path))
    {
        lv_obj_center(img);
        decoded = true;
    }
    else
    {
        lv_obj_del(img);
        lv_obj_t * err_lbl = lv_label_create(workspace_cont);
        lv_obj_set_style_text_color(err_lbl, lv_color_hex(0xFF5555), 0);
        lv_obj_set_style_text_align(err_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text_fmt(err_lbl, "Cannot decode image:\n%s", fatfs_path);
        lv_obj_center(err_lbl);
    }

    lv_obj_t * btn_close = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_close, 70, 26);
    lv_obj_align(btn_close, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_t * lbl = lv_label_create(btn_close);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn_close, close_preview_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_close, close_preview_cb, LV_EVENT_KEY, NULL);
    if (grp != NULL) lv_group_add_obj(grp, btn_close);

    /* BMP is the only format the home screen can decode at 320x216 */
    if (decoded && Ends_With_IgnoreCase(fatfs_path, ".bmp"))
    {
        strncpy(s_preview_path, (fatfs_path[0] == '/') ? (fatfs_path + 1) : fatfs_path, sizeof(s_preview_path) - 1U);
        s_preview_path[sizeof(s_preview_path) - 1U] = '\0';

        lv_obj_t * btn_wall = lv_btn_create(workspace_cont);
        lv_obj_set_size(btn_wall, 110, 26);
        lv_obj_align(btn_wall, LV_ALIGN_TOP_RIGHT, -6, 6);
        lv_obj_t * lbl_wall = lv_label_create(btn_wall);
        lv_label_set_text(lbl_wall, LV_SYMBOL_IMAGE " Set Wall");
        lv_obj_center(lbl_wall);

        lv_obj_add_event_cb(btn_wall, set_wallpaper_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn_wall, set_wallpaper_cb, LV_EVENT_KEY, NULL);
        if (grp != NULL) lv_group_add_obj(grp, btn_wall);
    }
}

static void Editor_SetKeyboardVisible(bool visible)
{
    if (s_editor_ta == NULL || s_editor_kb == NULL)
    {
        return;
    }

    if (visible)
    {
        lv_obj_clear_flag(s_editor_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_editor_ta, 308, 76);
    }
    else
    {
        lv_obj_add_flag(s_editor_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_editor_ta, 308, 178);
    }

    /* Keep focus on textarea so cursor stays active when using on-screen keyboard */
    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL)
    {
        lv_group_focus_obj(s_editor_ta);
    }
    lv_obj_add_state(s_editor_ta, LV_STATE_FOCUSED);
}

static bool Editor_SaveCurrentFile(void)
{
    if (s_editor_ta == NULL || s_editor_path[0] == '\0')
    {
        return false;
    }

    const char *txt = lv_textarea_get_text(s_editor_ta);
    if (txt == NULL)
    {
        return false;
    }

    UINT bw = 0;
    UINT len = (UINT)strlen(txt);

    if (f_open(&s_ui_fil, s_editor_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        Beeper_ErrorAlert();
        if (s_editor_save_lbl != NULL)
        {
            lv_label_set_text(s_editor_save_lbl, LV_SYMBOL_WARNING " Err");
        }
        return false;
    }

    FRESULT res = f_write(&s_ui_fil, txt, len, &bw);
    (void)f_close(&s_ui_fil);

    if (res != FR_OK || bw != len)
    {
        Beeper_ErrorAlert();
        if (s_editor_save_lbl != NULL)
        {
            lv_label_set_text(s_editor_save_lbl, LV_SYMBOL_WARNING " Err");
        }
        return false;
    }

    s_editor_dirty = false;
    if (s_editor_save_lbl != NULL)
    {
        lv_label_set_text(s_editor_save_lbl, LV_SYMBOL_OK " OK");
    }
    return true;
}

static void editor_ta_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);

    if (code == LV_EVENT_SCROLL_BEGIN)
    {
        /* Disable scroll animation on the textarea so cursor movement never causes vertical bounce */
        lv_anim_t * a = (lv_anim_t *)lv_event_get_param(e);
        if (a != NULL)
        {
            a->time        = 0;
            a->start_value = a->end_value;
        }
    }
    else if (code == LV_EVENT_INSERT)
    {
        const char *ins = (const char *)lv_event_get_param(e);
        if (ins != NULL && ((uint8_t)ins[0] == LV_KEY_ESC))
        {
            lv_textarea_set_insert_replace(ta, "");
        }
    }
    else if (code == LV_EVENT_VALUE_CHANGED)
    {
        if (!s_editor_dirty)
        {
            s_editor_dirty = true;
            if (s_editor_save_lbl != NULL)
            {
                lv_label_set_text(s_editor_save_lbl, LV_SYMBOL_SAVE " Save");
            }
        }
    }
    else if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        if (s_editor_kb != NULL && !lv_obj_has_flag(s_editor_kb, LV_OBJ_FLAG_HIDDEN))
        {
            Editor_SetKeyboardVisible(false);
        }
        else
        {
            s_pending_screen = (int8_t)UI_SCREEN_FILES;
        }
    }
}

static void editor_kb_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
        Beeper_Click();
        Editor_SetKeyboardVisible(false);
    }
}

static void editor_toggle_kbd_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }
    if (code != LV_EVENT_CLICKED || s_editor_kb == NULL)
    {
        return;
    }

    Beeper_Click();
    bool currently_hidden = lv_obj_has_flag(s_editor_kb, LV_OBJ_FLAG_HIDDEN);
    Editor_SetKeyboardVisible(currently_hidden);
}

static void editor_save_btn_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }
    if (code != LV_EVENT_CLICKED)
    {
        return;
    }

    if (Editor_SaveCurrentFile())
    {
        Beeper_Beep(15);
    }
}

static void editor_run_btn_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }
    if (code != LV_EVENT_CLICKED)
    {
        return;
    }

    Beeper_Click();
    if (s_editor_dirty)
    {
        (void)Editor_SaveCurrentFile();
    }

    if (!AppLoader_LoadAndRunScript(s_editor_path))
    {
        Show_App_Load_Error();
    }
}

static void Open_Text_Preview(const char *fatfs_path)
{
    UINT br = 0;
    const char *clean_path = (fatfs_path[0] == '/') ? (fatfs_path + 1) : fatfs_path;

    strncpy(s_editor_path, clean_path, sizeof(s_editor_path) - 1U);
    s_editor_path[sizeof(s_editor_path) - 1U] = '\0';
    s_editor_dirty = false;

    memset(EXT_TEXT_PREVIEW_BUF, 0, EXT_TEXT_PREVIEW_SIZE);
    if (f_open(&s_ui_fil, clean_path, FA_READ) == FR_OK)
    {
        (void)f_read(&s_ui_fil, EXT_TEXT_PREVIEW_BUF, EXT_TEXT_PREVIEW_SIZE - 1U, &br);
        f_close(&s_ui_fil);
        /* Normalize CRLF -> LF in-place so LVGL does not draw box glyphs for '\r' */
        uint32_t w = 0U;
        for (uint32_t r = 0U; r < br; r++)
        {
            if (EXT_TEXT_PREVIEW_BUF[r] != '\r')
            {
                EXT_TEXT_PREVIEW_BUF[w++] = EXT_TEXT_PREVIEW_BUF[r];
            }
        }
        EXT_TEXT_PREVIEW_BUF[w] = '\0';
    }
    else
    {
        snprintf(EXT_TEXT_PREVIEW_BUF, EXT_TEXT_PREVIEW_SIZE, "// Cannot open %s\n", clean_path);
    }

    bool is_c_script = Ends_With_IgnoreCase(clean_path, ".c");

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    file_list_obj     = NULL;
    s_editor_ta       = NULL;
    s_editor_kb       = NULL;
    s_editor_save_lbl = NULL;
    lv_obj_clean(workspace_cont);

    /* Top toolbar: [< Back] [Filename] [Kbd] [Save] [Run (for .c)] */
    lv_obj_t * btn_close = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_close, 54, 24);
    lv_obj_set_pos(btn_close, 6, 4);
    lv_obj_t * lbl = lv_label_create(btn_close);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl);

    const char *short_name = strrchr(clean_path, '/');
    short_name = (short_name != NULL) ? (short_name + 1) : clean_path;

    lv_obj_t * title_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(s_themes[s_theme_id % THEME_COUNT].title_col), 0);
    lv_obj_set_pos(title_lbl, 64, 8);
    lv_obj_set_width(title_lbl, is_c_script ? 92 : 144);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(title_lbl, short_name);

    int16_t kbd_x  = is_c_script ? 158 : 210;
    int16_t save_x = is_c_script ? 204 : 256;

    lv_obj_t * btn_kbd = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_kbd, 42, 24);
    lv_obj_set_pos(btn_kbd, kbd_x, 4);
    lv_obj_set_style_bg_color(btn_kbd, lv_color_hex(0x37474F), 0);
    lv_obj_t * lbl_kbd = lv_label_create(btn_kbd);
    lv_label_set_text(lbl_kbd, LV_SYMBOL_KEYBOARD);
    lv_obj_center(lbl_kbd);
    lv_obj_add_event_cb(btn_kbd, editor_toggle_kbd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_kbd, editor_toggle_kbd_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * btn_save = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_save, 58, 24);
    lv_obj_set_pos(btn_save, save_x, 4);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x0277BD), 0);
    s_editor_save_lbl = lv_label_create(btn_save);
    lv_label_set_text(s_editor_save_lbl, LV_SYMBOL_SAVE " Save");
    lv_obj_center(s_editor_save_lbl);
    lv_obj_add_event_cb(btn_save, editor_save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_save, editor_save_btn_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * btn_run = NULL;
    if (is_c_script)
    {
        btn_run = lv_btn_create(workspace_cont);
        lv_obj_set_size(btn_run, 48, 24);
        lv_obj_set_pos(btn_run, 266, 4);
        lv_obj_set_style_bg_color(btn_run, lv_color_hex(0x00A859), 0);
        lv_obj_t * lbl_run = lv_label_create(btn_run);
        lv_label_set_text(lbl_run, LV_SYMBOL_PLAY " Run");
        lv_obj_center(lbl_run);
        lv_obj_add_event_cb(btn_run, editor_run_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn_run, editor_run_btn_cb, LV_EVENT_KEY, NULL);
    }

    /* Multi-line Editable Textarea */
    uint32_t accent_hex = s_themes[s_theme_id % THEME_COUNT].title_col;
    s_editor_ta = lv_textarea_create(workspace_cont);
    lv_obj_set_size(s_editor_ta, 308, 178);
    lv_obj_set_pos(s_editor_ta, 6, 32);
    lv_textarea_set_one_line(s_editor_ta, false);
    lv_obj_set_scroll_dir(s_editor_ta, LV_DIR_VER);
    lv_obj_set_style_bg_color(s_editor_ta, lv_color_hex(0x0E131D), 0);
    lv_obj_set_style_text_color(s_editor_ta, lv_color_hex(0xE8F0FE), 0);
    lv_obj_set_style_border_color(s_editor_ta, lv_color_hex(0x2A3950), 0);
    lv_obj_set_style_border_color(s_editor_ta, lv_color_hex(accent_hex), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(s_editor_ta, 6, 0);

    /* High-visibility block cursor (visible regardless of focus state) */
    lv_obj_set_style_bg_color(s_editor_ta, lv_color_hex(accent_hex), LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_editor_ta, LV_OPA_70, LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_editor_ta, lv_color_hex(0x000000), LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_editor_ta, 2, LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(s_editor_ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_editor_ta, lv_color_hex(0xFFFFFF), LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_anim_time(s_editor_ta, 450, LV_PART_CURSOR | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(s_editor_ta, lv_color_hex(accent_hex), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(s_editor_ta, LV_OPA_80, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(s_editor_ta, lv_color_hex(0x000000), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_editor_ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(s_editor_ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(s_editor_ta, lv_color_hex(0xFFFFFF), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_time(s_editor_ta, 450, LV_PART_CURSOR | LV_STATE_FOCUSED);

    /* Load text at once BEFORE setting max_length (otherwise LVGL inserts char-by-char and scrolls to bottom!) */
    lv_textarea_set_text(s_editor_ta, EXT_TEXT_PREVIEW_BUF);
    lv_textarea_set_max_length(s_editor_ta, EXT_TEXT_PREVIEW_SIZE - 1U);
    lv_textarea_set_cursor_pos(s_editor_ta, 0);
    lv_obj_scroll_to(s_editor_ta, 0, 0, LV_ANIM_OFF);
    lv_textarea_set_cursor_click_pos(s_editor_ta, true);
    lv_obj_add_event_cb(s_editor_ta, editor_ta_event_cb, LV_EVENT_ALL, NULL);

    /* On-Screen Touch Keyboard (hidden by default; toggled via [Kbd] button) */
    s_editor_kb = lv_keyboard_create(workspace_cont);
    lv_obj_set_size(s_editor_kb, 312, 102);
    lv_obj_align(s_editor_kb, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_keyboard_set_textarea(s_editor_kb, s_editor_ta);
    lv_obj_add_flag(s_editor_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_editor_kb, editor_kb_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_add_event_cb(btn_close, close_preview_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_close, close_preview_cb, LV_EVENT_KEY, NULL);

    if (grp != NULL)
    {
        lv_group_add_obj(grp, s_editor_ta);
        lv_group_add_obj(grp, btn_close);
        lv_group_add_obj(grp, btn_kbd);
        lv_group_add_obj(grp, btn_save);
        if (btn_run != NULL) lv_group_add_obj(grp, btn_run);
        lv_group_focus_obj(s_editor_ta);
    }
}

#define SUPPORTED_CREATE_EXT_COUNT 6U
static const char * const s_create_exts[SUPPORTED_CREATE_EXT_COUNT] = {
    ".c", ".txt", ".h", ".md", ".ini", ".cfg"
};

static const char * const s_supported_exts[] = {
    ".c", ".txt", ".h", ".md", ".ini", ".cfg", ".log",
    ".bin", ".bmp", ".png", ".jpg", ".sjpg"
};

typedef enum {
    FNAME_MODE_NEW_FILE = 0,
    FNAME_MODE_RENAME_FILE,
    FNAME_MODE_NEW_DIR,
    FNAME_MODE_RENAME_DIR
} Fname_DialogMode_t;

static void Open_File_Action_Menu(const char *rel_path, const char *item_name);
static void Open_File_Name_Dialog(Fname_DialogMode_t mode);
static void Open_Delete_Confirm_Dialog(void);

static bool Is_Supported_File_Ext(const char *filename)
{
    for (size_t i = 0U; i < sizeof(s_supported_exts) / sizeof(s_supported_exts[0]); i++)
    {
        if (Ends_With_IgnoreCase(filename, s_supported_exts[i]))
        {
            return true;
        }
    }
    return false;
}

static const char *Get_Current_Rel_Dir(void)
{
    if (g_dir_cache.path[0] == '\0' ||
        (g_dir_cache.path[0] == '/' && g_dir_cache.path[1] == '\0'))
    {
        return "";
    }
    return (g_dir_cache.path[0] == '/') ? (g_dir_cache.path + 1) : g_dir_cache.path;
}

static void Style_Editor_Cursor(lv_obj_t *ta, uint32_t accent_hex)
{
    lv_obj_set_style_bg_color(ta, lv_color_hex(accent_hex), LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ta, LV_OPA_70, LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ta, lv_color_hex(0x000000), LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xFFFFFF), LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_anim_time(ta, 450, LV_PART_CURSOR | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(ta, lv_color_hex(accent_hex), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(ta, LV_OPA_80, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(ta, lv_color_hex(0x000000), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xFFFFFF), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_time(ta, 450, LV_PART_CURSOR | LV_STATE_FOCUSED);
}

static void Update_Ext_Buttons_Style(void)
{
    uint32_t accent_hex = s_themes[s_theme_id % THEME_COUNT].title_col;
    for (uint8_t i = 0U; i < SUPPORTED_CREATE_EXT_COUNT; i++)
    {
        if (s_ext_btns[i] == NULL) continue;
        bool sel = (i == s_selected_ext_idx);
        lv_obj_set_style_bg_color(s_ext_btns[i], lv_color_hex(sel ? accent_hex : 0x252F48U), 0);
        lv_obj_t *lbl = lv_obj_get_child(s_ext_btns[i], 0);
        if (lbl != NULL)
        {
            lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? 0x000000U : 0xE0E0E0U), 0);
        }
    }
}

static void fname_ext_btn_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }
    if (code != LV_EVENT_CLICKED)
    {
        return;
    }

    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx < SUPPORTED_CREATE_EXT_COUNT)
    {
        Beeper_Click();
        s_selected_ext_idx = idx;
        Update_Ext_Buttons_Style();
        if (s_fname_status_lbl != NULL)
        {
            lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0x90A4AE), 0);
            lv_label_set_text_fmt(s_fname_status_lbl, "Selected extension: %s", s_create_exts[idx]);
        }
    }
}

static void fname_toggle_kbd_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }
    if (code != LV_EVENT_CLICKED || s_editor_kb == NULL || s_editor_ta == NULL)
    {
        return;
    }

    Beeper_Click();
    if (lv_obj_has_flag(s_editor_kb, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_clear_flag(s_editor_kb, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_editor_kb, LV_OBJ_FLAG_HIDDEN);
    }
    lv_group_t *grp = lv_group_get_default();
    if (grp != NULL) lv_group_focus_obj(s_editor_ta);
    lv_obj_add_state(s_editor_ta, LV_STATE_FOCUSED);
}

static void fname_confirm_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }
    if (code != LV_EVENT_CLICKED && code != LV_EVENT_READY)
    {
        return;
    }
    if (s_editor_ta == NULL)
    {
        return;
    }

    const char *raw = lv_textarea_get_text(s_editor_ta);
    while (raw != NULL && *raw == ' ') raw++;

    if (raw == NULL || raw[0] == '\0' || strcmp(raw, ".") == 0 || strcmp(raw, "..") == 0)
    {
        Beeper_ErrorAlert();
        if (s_fname_status_lbl != NULL)
        {
            lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_fname_status_lbl, "Error: Enter a valid name!");
        }
        return;
    }

    /* Reject forbidden FAT characters */
    for (const char *p = raw; *p != '\0'; p++)
    {
        char ch = *p;
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"'  || ch == '<' || ch == '>' || ch == '|')
        {
            Beeper_ErrorAlert();
            if (s_fname_status_lbl != NULL)
            {
                lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
                lv_label_set_text(s_fname_status_lbl, "Invalid char in name!");
            }
            return;
        }
    }

    const char *dir = Get_Current_Rel_Dir();

    /* Directory Creation or Directory Rename */
    if (s_fname_mode == (uint8_t)FNAME_MODE_NEW_DIR ||
        s_fname_mode == (uint8_t)FNAME_MODE_RENAME_DIR)
    {
        char folder_name[STORAGE_MAX_NAME_LEN];
        snprintf(folder_name, sizeof(folder_name), "%.48s", raw);

        if (s_fname_mode == (uint8_t)FNAME_MODE_NEW_DIR)
        {
            if (dir[0] == '\0')
            {
                snprintf(s_next_path, sizeof(s_next_path), "%s", folder_name);
            }
            else
            {
                snprintf(s_next_path, sizeof(s_next_path), "%.60s/%.48s", dir, folder_name);
            }

            if (f_stat(s_next_path, &s_ui_fno) == FR_OK)
            {
                Beeper_ErrorAlert();
                if (s_fname_status_lbl != NULL)
                {
                    lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
                    lv_label_set_text(s_fname_status_lbl, "Folder already exists!");
                }
                return;
            }

            if (f_mkdir(s_next_path) != FR_OK)
            {
                Beeper_ErrorAlert();
                if (s_fname_status_lbl != NULL)
                {
                    lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
                    lv_label_set_text(s_fname_status_lbl, "SD mkdir failed!");
                }
                return;
            }

            Beeper_Beep(15);
            s_pending_screen = (int8_t)UI_SCREEN_FILES;
            return;
        }
        else
        {
            /* Rename current folder (dir is e.g. "games/sub" or "sub") */
            char parent_rel[STORAGE_MAX_PATH_LEN];
            strncpy(parent_rel, dir, sizeof(parent_rel) - 1U);
            parent_rel[sizeof(parent_rel) - 1U] = '\0';
            char *slash = strrchr(parent_rel, '/');
            if (slash != NULL)
            {
                *slash = '\0';
                snprintf(s_next_path, sizeof(s_next_path), "%.60s/%.48s", parent_rel, folder_name);
            }
            else
            {
                snprintf(s_next_path, sizeof(s_next_path), "%s", folder_name);
            }

            if (strcmp(dir, s_next_path) == 0)
            {
                Beeper_Click();
                s_pending_screen = (int8_t)UI_SCREEN_FILES;
                return;
            }
            if (f_stat(s_next_path, &s_ui_fno) == FR_OK)
            {
                Beeper_ErrorAlert();
                if (s_fname_status_lbl != NULL)
                {
                    lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
                    lv_label_set_text(s_fname_status_lbl, "Folder already exists!");
                }
                return;
            }
            if (f_rename(dir, s_next_path) != FR_OK)
            {
                Beeper_ErrorAlert();
                if (s_fname_status_lbl != NULL)
                {
                    lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
                    lv_label_set_text(s_fname_status_lbl, "SD Rename failed!");
                }
                return;
            }

            snprintf(g_dir_cache.path, sizeof(g_dir_cache.path), "/%.120s", s_next_path);
            Beeper_Beep(15);
            s_pending_screen = (int8_t)UI_SCREEN_FILES;
            return;
        }
    }

    char final_name[STORAGE_MAX_NAME_LEN];
    const char *dot = strrchr(raw, '.');
    if (dot != NULL && dot != raw && dot[1] != '\0')
    {
        /* User explicitly typed an extension: verify it is supported */
        if (!Is_Supported_File_Ext(raw))
        {
            Beeper_ErrorAlert();
            if (s_fname_status_lbl != NULL)
            {
                lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
                lv_label_set_text(s_fname_status_lbl, "Unsupported ext! Use .c .txt .h .md .ini .cfg");
            }
            return;
        }
        snprintf(final_name, sizeof(final_name), "%.60s", raw);
    }
    else
    {
        /* Append selected extension (or original binary/image extension when renaming) */
        const char *ext = (s_selected_ext_idx < SUPPORTED_CREATE_EXT_COUNT)
                          ? s_create_exts[s_selected_ext_idx]
                          : ((s_orig_rename_ext[0] != '\0') ? s_orig_rename_ext : ".c");
        size_t stem_len = (dot == raw) ? 0U : ((dot != NULL) ? (size_t)(dot - raw) : strlen(raw));
        if (stem_len == 0U || stem_len > 48U) stem_len = (stem_len == 0U) ? 1U : 48U;
        snprintf(final_name, sizeof(final_name), "%.*s%s", (int)stem_len, raw, ext);
    }

    if (dir[0] == '\0')
    {
        snprintf(s_next_path, sizeof(s_next_path), "%s", final_name);
    }
    else
    {
        snprintf(s_next_path, sizeof(s_next_path), "%.60s/%.50s", dir, final_name);
    }

    if (s_fname_mode == (uint8_t)FNAME_MODE_RENAME_FILE)
    {
        if (strcmp(s_action_file_path, s_next_path) == 0)
        {
            Beeper_Click();
            s_pending_screen = (int8_t)UI_SCREEN_FILES;
            return;
        }
        if (f_stat(s_next_path, &s_ui_fno) == FR_OK)
        {
            Beeper_ErrorAlert();
            if (s_fname_status_lbl != NULL)
            {
                lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
                lv_label_set_text(s_fname_status_lbl, "File already exists!");
            }
            return;
        }
        if (f_rename(s_action_file_path, s_next_path) != FR_OK)
        {
            Beeper_ErrorAlert();
            if (s_fname_status_lbl != NULL)
            {
                lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
                lv_label_set_text(s_fname_status_lbl, "SD Rename failed!");
            }
            return;
        }
        if (strcmp(s_clip_path, s_action_file_path) == 0)
        {
            strncpy(s_clip_path, s_next_path, sizeof(s_clip_path) - 1U);
            strncpy(s_clip_name, final_name, sizeof(s_clip_name) - 1U);
        }
        Beeper_Beep(15);
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }

    /* Create New File */
    if (f_stat(s_next_path, &s_ui_fno) == FR_OK)
    {
        Beeper_ErrorAlert();
        if (s_fname_status_lbl != NULL)
        {
            lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_fname_status_lbl, "File already exists!");
        }
        return;
    }

    static const char s_starter_c[] =
        "// C-Script Template\n"
        "void main() {\n"
        "    int x = 150, y = 110;\n"
        "    while (1) {\n"
        "        int k = input_poll();\n"
        "        if (input_pressed(KEY_EXIT)) break;\n"
        "        if (k & KEY_LEFT)  x -= 3;\n"
        "        if (k & KEY_RIGHT) x += 3;\n"
        "        if (k & KEY_UP)    y -= 3;\n"
        "        if (k & KEY_DOWN)  y += 3;\n"
        "        clear_screen(COLOR_DARK_BG);\n"
        "        draw_text(8, 8, \"C-SCRIPT EDITOR\", COLOR_CYAN, COLOR_DARK_BG, 1);\n"
        "        fill_rect(x, y, 20, 20, COLOR_NEON_GREEN);\n"
        "        sleep_ms(16);\n"
        "    }\n"
        "}\n";

    static const char s_starter_h[] =
        "#ifndef USER_HEADER_H\n"
        "#define USER_HEADER_H\n\n"
        "// Constants & declarations\n\n"
        "#endif\n";

    UINT bw = 0;
    if (f_open(&s_ui_fil, s_next_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        Beeper_ErrorAlert();
        if (s_fname_status_lbl != NULL)
        {
            lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_fname_status_lbl, "SD Write failed!");
        }
        return;
    }

    if (Ends_With_IgnoreCase(final_name, ".c"))
    {
        (void)f_write(&s_ui_fil, s_starter_c, (UINT)(sizeof(s_starter_c) - 1U), &bw);
    }
    else if (Ends_With_IgnoreCase(final_name, ".h"))
    {
        (void)f_write(&s_ui_fil, s_starter_h, (UINT)(sizeof(s_starter_h) - 1U), &bw);
    }
    (void)f_close(&s_ui_fil);

    Beeper_Beep(15);
    Open_Text_Preview(s_next_path);
}

static void Open_File_Name_Dialog(Fname_DialogMode_t mode)
{
    s_fname_mode = (uint8_t)mode;
    s_orig_rename_ext[0] = '\0';
    bool is_dir_mode = (mode == FNAME_MODE_NEW_DIR || mode == FNAME_MODE_RENAME_DIR);
    bool is_rename   = (mode == FNAME_MODE_RENAME_FILE || mode == FNAME_MODE_RENAME_DIR);

    char default_stem[STORAGE_MAX_NAME_LEN];
    const char *dir = Get_Current_Rel_Dir();

    if (mode == FNAME_MODE_RENAME_FILE)
    {
        strncpy(default_stem, s_action_file_name, sizeof(default_stem) - 1U);
        default_stem[sizeof(default_stem) - 1U] = '\0';
        char *dot = strrchr(default_stem, '.');
        s_selected_ext_idx = SUPPORTED_CREATE_EXT_COUNT; /* Custom/original ext unless matched */
        if (dot != NULL && dot != default_stem)
        {
            strncpy(s_orig_rename_ext, dot, sizeof(s_orig_rename_ext) - 1U);
            s_orig_rename_ext[sizeof(s_orig_rename_ext) - 1U] = '\0';
            for (uint8_t i = 0U; i < SUPPORTED_CREATE_EXT_COUNT; i++)
            {
                if (Ends_With_IgnoreCase(default_stem, s_create_exts[i]))
                {
                    s_selected_ext_idx = i;
                    break;
                }
            }
            *dot = '\0';
        }
        else
        {
            s_selected_ext_idx = 0U;
        }
    }
    else if (mode == FNAME_MODE_RENAME_DIR)
    {
        const char *leaf = strrchr(dir, '/');
        leaf = (leaf != NULL) ? (leaf + 1) : dir;
        strncpy(default_stem, leaf, sizeof(default_stem) - 1U);
        default_stem[sizeof(default_stem) - 1U] = '\0';
        s_selected_ext_idx = 0U;
    }
    else if (mode == FNAME_MODE_NEW_DIR)
    {
        s_selected_ext_idx = 0U;
        for (uint32_t n = 1U; n <= 99U; n++)
        {
            snprintf(default_stem, sizeof(default_stem), "folder_%lu", (unsigned long)n);
            if (dir[0] == '\0')
            {
                snprintf(s_next_path, sizeof(s_next_path), "%s", default_stem);
            }
            else
            {
                snprintf(s_next_path, sizeof(s_next_path), "%.60s/%s", dir, default_stem);
            }
            if (f_stat(s_next_path, &s_ui_fno) != FR_OK)
            {
                break;
            }
        }
    }
    else
    {
        s_selected_ext_idx = 0U; /* .c by default */
        for (uint32_t n = 1U; n <= 99U; n++)
        {
            snprintf(default_stem, sizeof(default_stem), "script_%lu", (unsigned long)n);
            if (dir[0] == '\0')
            {
                snprintf(s_next_path, sizeof(s_next_path), "%s.c", default_stem);
            }
            else
            {
                snprintf(s_next_path, sizeof(s_next_path), "%.60s/%s.c", dir, default_stem);
            }
            if (f_stat(s_next_path, &s_ui_fno) != FR_OK)
            {
                break;
            }
        }
    }

    lv_group_t *grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    file_list_obj      = NULL;
    s_editor_ta        = NULL;
    s_editor_kb        = NULL;
    s_editor_save_lbl  = NULL;
    s_fname_status_lbl = NULL;
    memset(s_ext_btns, 0, sizeof(s_ext_btns));
    lv_obj_clean(workspace_cont);

    uint32_t accent_hex = s_themes[s_theme_id % THEME_COUNT].title_col;

    /* Top bar: [< Back] [Title] [Kbd] [Create / Rename] */
    lv_obj_t *btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 58, 24);
    lv_obj_set_pos(btn_back, 6, 4);
    lv_obj_t *lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, close_preview_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, close_preview_cb, LV_EVENT_KEY, NULL);

    const char *dlg_title = "Create New File";
    if (mode == FNAME_MODE_RENAME_FILE)     dlg_title = "Rename File";
    else if (mode == FNAME_MODE_NEW_DIR)    dlg_title = "New Folder";
    else if (mode == FNAME_MODE_RENAME_DIR) dlg_title = "Rename Folder";

    lv_obj_t *title_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(accent_hex), 0);
    lv_obj_set_pos(title_lbl, 70, 8);
    lv_label_set_text(title_lbl, dlg_title);

    lv_obj_t *btn_kbd = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_kbd, 42, 24);
    lv_obj_set_pos(btn_kbd, 198, 4);
    lv_obj_set_style_bg_color(btn_kbd, lv_color_hex(0x37474F), 0);
    lv_obj_t *lbl_kbd = lv_label_create(btn_kbd);
    lv_label_set_text(lbl_kbd, LV_SYMBOL_KEYBOARD);
    lv_obj_center(lbl_kbd);
    lv_obj_add_event_cb(btn_kbd, fname_toggle_kbd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_kbd, fname_toggle_kbd_cb, LV_EVENT_KEY, NULL);

    lv_obj_t *btn_ok = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_ok, 70, 24);
    lv_obj_set_pos(btn_ok, 244, 4);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0x00A859), 0);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, is_rename ? (LV_SYMBOL_OK " Save") : (LV_SYMBOL_OK " Create"));
    lv_obj_center(lbl_ok);
    lv_obj_add_event_cb(btn_ok, fname_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_ok, fname_confirm_cb, LV_EVENT_KEY, NULL);

    /* One-line Filename Input */
    s_editor_ta = lv_textarea_create(workspace_cont);
    lv_obj_set_size(s_editor_ta, 308, 34);
    lv_obj_set_pos(s_editor_ta, 6, 32);
    lv_textarea_set_one_line(s_editor_ta, true);
    lv_obj_set_style_bg_color(s_editor_ta, lv_color_hex(0x0E131D), 0);
    lv_obj_set_style_text_color(s_editor_ta, lv_color_hex(0xE8F0FE), 0);
    lv_obj_set_style_border_color(s_editor_ta, lv_color_hex(0x2A3950), 0);
    lv_obj_set_style_border_color(s_editor_ta, lv_color_hex(accent_hex), LV_STATE_FOCUSED);
    Style_Editor_Cursor(s_editor_ta, accent_hex);
    lv_textarea_set_text(s_editor_ta, default_stem);
    lv_textarea_set_max_length(s_editor_ta, 48U);
    lv_textarea_set_cursor_pos(s_editor_ta, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(s_editor_ta, editor_ta_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(s_editor_ta, fname_confirm_cb, LV_EVENT_READY, NULL);

    if (!is_dir_mode)
    {
        /* Supported Extension Selector Buttons (.c .txt .h .md .ini .cfg) */
        for (uint8_t i = 0U; i < SUPPORTED_CREATE_EXT_COUNT; i++)
        {
            lv_obj_t *ebtn = lv_btn_create(workspace_cont);
            lv_obj_set_size(ebtn, 48, 22);
            lv_obj_set_pos(ebtn, (int16_t)(6 + i * 51), 70);
            lv_obj_t *elbl = lv_label_create(ebtn);
            lv_label_set_text(elbl, s_create_exts[i]);
            lv_obj_center(elbl);
            lv_obj_add_event_cb(ebtn, fname_ext_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_add_event_cb(ebtn, fname_ext_btn_cb, LV_EVENT_KEY, NULL);
            s_ext_btns[i] = ebtn;
        }
        Update_Ext_Buttons_Style();
    }

    s_fname_status_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(s_fname_status_lbl, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_pos(s_fname_status_lbl, 8, is_dir_mode ? 76 : 95);
    if (is_dir_mode)
    {
        lv_label_set_text_fmt(s_fname_status_lbl, "Parent folder: %s", g_dir_cache.path);
    }
    else if (s_selected_ext_idx < SUPPORTED_CREATE_EXT_COUNT)
    {
        lv_label_set_text_fmt(s_fname_status_lbl, "Selected extension: %s", s_create_exts[s_selected_ext_idx]);
    }
    else
    {
        lv_label_set_text_fmt(s_fname_status_lbl, "Keep ext %s (or pick above)", s_orig_rename_ext);
    }

    /* On-screen keyboard (visible by default for quick touch naming; hides on OK/ESC) */
    s_editor_kb = lv_keyboard_create(workspace_cont);
    lv_obj_set_size(s_editor_kb, 312, 98);
    lv_obj_align(s_editor_kb, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_keyboard_set_textarea(s_editor_kb, s_editor_ta);
    lv_obj_add_event_cb(s_editor_kb, editor_kb_event_cb, LV_EVENT_ALL, NULL);

    if (grp != NULL)
    {
        /* Focus order left-to-right, top-to-bottom */
        lv_group_add_obj(grp, s_editor_ta);
        if (!is_dir_mode)
        {
            for (uint8_t i = 0U; i < SUPPORTED_CREATE_EXT_COUNT; i++)
            {
                lv_group_add_obj(grp, s_ext_btns[i]);
            }
        }
        lv_group_add_obj(grp, btn_back);
        lv_group_add_obj(grp, btn_kbd);
        lv_group_add_obj(grp, btn_ok);
        lv_group_focus_obj(s_editor_ta);
    }
}

static bool Copy_File_On_SD(const char *src_path, const char *dst_path)
{
    UINT br = 0U;
    UINT bw = 0U;

    if (f_open(&s_ui_fil, src_path, FA_READ) != FR_OK)
    {
        return false;
    }
    if (f_open(&s_ui_fil_dst, dst_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        (void)f_close(&s_ui_fil);
        return false;
    }

    bool ok = true;
    for (;;)
    {
        HAL_IWDG_Refresh(&hiwdg);
        if (f_read(&s_ui_fil, EXT_TEXT_PREVIEW_BUF, EXT_TEXT_PREVIEW_SIZE, &br) != FR_OK)
        {
            ok = false;
            break;
        }
        if (br == 0U)
        {
            break;
        }
        if (f_write(&s_ui_fil_dst, EXT_TEXT_PREVIEW_BUF, br, &bw) != FR_OK || bw != br)
        {
            ok = false;
            break;
        }
    }

    (void)f_close(&s_ui_fil_dst);
    (void)f_close(&s_ui_fil);
    return ok;
}

static void Paste_Clipboard_In_Current_Dir(void)
{
    if (s_clip_path[0] == '\0' || s_clip_name[0] == '\0')
    {
        return;
    }

    const char *dir = Get_Current_Rel_Dir();
    if (dir[0] == '\0')
    {
        snprintf(s_next_path, sizeof(s_next_path), "%s", s_clip_name);
    }
    else
    {
        snprintf(s_next_path, sizeof(s_next_path), "%.60s/%.50s", dir, s_clip_name);
    }

    if (s_clip_is_cut)
    {
        /* Move file */
        if (strcmp(s_clip_path, s_next_path) != 0)
        {
            if (f_stat(s_next_path, &s_ui_fno) == FR_OK)
            {
                (void)f_unlink(s_next_path);
            }
            if (f_rename(s_clip_path, s_next_path) != FR_OK)
            {
                Beeper_ErrorAlert();
                return;
            }
        }
        s_clip_path[0] = '\0';
        s_clip_name[0] = '\0';
        s_clip_is_cut  = false;
        Beeper_Beep(15);
        OSTasks_RequestDirList(g_dir_cache.path[0] != '\0' ? g_dir_cache.path : "/");
    }
    else
    {
        /* Copy file (if pasting in the same folder where src == dst, generate copy_<name>) */
        if (strcmp(s_clip_path, s_next_path) == 0)
        {
            for (uint32_t n = 1U; n <= 99U; n++)
            {
                if (dir[0] == '\0')
                {
                    snprintf(s_next_path, sizeof(s_next_path), "copy%lu_%.45s", (unsigned long)n, s_clip_name);
                }
                else
                {
                    snprintf(s_next_path, sizeof(s_next_path), "%.55s/copy%lu_%.45s", dir, (unsigned long)n, s_clip_name);
                }
                if (f_stat(s_next_path, &s_ui_fno) != FR_OK)
                {
                    break;
                }
            }
        }

        if (Copy_File_On_SD(s_clip_path, s_next_path))
        {
            Beeper_Beep(15);
            OSTasks_RequestDirList(g_dir_cache.path[0] != '\0' ? g_dir_cache.path : "/");
        }
        else
        {
            Beeper_ErrorAlert();
        }
    }
}

static void delete_confirm_btn_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        Open_File_Action_Menu(s_action_file_path, s_action_file_name);
        return;
    }
    if (code != LV_EVENT_CLICKED)
    {
        return;
    }

    uint32_t confirm = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (confirm == 1U)
    {
        if (f_unlink(s_action_file_path) == FR_OK)
        {
            if (strcmp(s_clip_path, s_action_file_path) == 0)
            {
                s_clip_path[0] = '\0';
                s_clip_name[0] = '\0';
            }
            Beeper_Beep(15);
        }
        else
        {
            Beeper_ErrorAlert();
        }
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
    }
    else
    {
        Beeper_Click();
        Open_File_Action_Menu(s_action_file_path, s_action_file_name);
    }
}

static void Open_Delete_Confirm_Dialog(void)
{
    lv_group_t *grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    file_list_obj = NULL;
    lv_obj_clean(workspace_cont);

    lv_obj_t *panel = lv_obj_create(workspace_cont);
    lv_obj_set_size(panel, 296, 168);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1A1016), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xFF4444), 0);
    lv_obj_set_style_border_width(panel, 2, 0);

    lv_obj_t *lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, 268);
    lv_label_set_text_fmt(lbl, LV_SYMBOL_WARNING " Delete file permanently?\n\n%s", s_action_file_name);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *btn_cancel = lv_btn_create(panel);
    lv_obj_set_size(btn_cancel, 120, 36);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x37474F), 0);
    lv_obj_t *lbl_c = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_c, LV_SYMBOL_CLOSE " Cancel");
    lv_obj_center(lbl_c);
    lv_obj_add_event_cb(btn_cancel, delete_confirm_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)0U);
    lv_obj_add_event_cb(btn_cancel, delete_confirm_btn_cb, LV_EVENT_KEY, NULL);

    lv_obj_t *btn_del = lv_btn_create(panel);
    lv_obj_set_size(btn_del, 120, 36);
    lv_obj_align(btn_del, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_bg_color(btn_del, lv_color_hex(0xC62828), 0);
    lv_obj_t *lbl_d = lv_label_create(btn_del);
    lv_label_set_text(lbl_d, LV_SYMBOL_TRASH " Delete");
    lv_obj_center(lbl_d);
    lv_obj_add_event_cb(btn_del, delete_confirm_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1U);
    lv_obj_add_event_cb(btn_del, delete_confirm_btn_cb, LV_EVENT_KEY, NULL);

    if (grp != NULL)
    {
        lv_group_add_obj(grp, btn_cancel);
        lv_group_add_obj(grp, btn_del);
        lv_group_focus_obj(btn_cancel);
    }
}

typedef enum {
    FILE_ACT_OPEN = 1,
    FILE_ACT_RUN_SCRIPT,
    FILE_ACT_RENAME,
    FILE_ACT_COPY,
    FILE_ACT_MOVE,
    FILE_ACT_DELETE
} File_Action_t;

static void file_action_btn_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_FILES;
        return;
    }
    if (code != LV_EVENT_CLICKED)
    {
        return;
    }

    File_Action_t act = (File_Action_t)(uintptr_t)lv_event_get_user_data(e);
    Beeper_Click();

    switch (act)
    {
        case FILE_ACT_RUN_SCRIPT:
            if (!AppLoader_LoadAndRunScript(s_action_file_path))
            {
                Show_App_Load_Error();
            }
            break;

        case FILE_ACT_OPEN:
            if (Ends_With_IgnoreCase(s_action_file_name, ".bmp") ||
                Ends_With_IgnoreCase(s_action_file_name, ".png") ||
                Ends_With_IgnoreCase(s_action_file_name, ".jpg") ||
                Ends_With_IgnoreCase(s_action_file_name, ".sjpg"))
            {
                Open_Image_Preview(s_action_file_path);
            }
            else if (Ends_With_IgnoreCase(s_action_file_name, ".bin"))
            {
                if (!AppLoader_LoadAndRun(s_action_file_path))
                {
                    Show_App_Load_Error();
                }
            }
            else
            {
                Open_Text_Preview(s_action_file_path);
            }
            break;

        case FILE_ACT_RENAME:
            Open_File_Name_Dialog(FNAME_MODE_RENAME_FILE);
            break;

        case FILE_ACT_COPY:
            strncpy(s_clip_path, s_action_file_path, sizeof(s_clip_path) - 1U);
            s_clip_path[sizeof(s_clip_path) - 1U] = '\0';
            strncpy(s_clip_name, s_action_file_name, sizeof(s_clip_name) - 1U);
            s_clip_name[sizeof(s_clip_name) - 1U] = '\0';
            s_clip_is_cut = false;
            s_pending_screen = (int8_t)UI_SCREEN_FILES;
            break;

        case FILE_ACT_MOVE:
            strncpy(s_clip_path, s_action_file_path, sizeof(s_clip_path) - 1U);
            s_clip_path[sizeof(s_clip_path) - 1U] = '\0';
            strncpy(s_clip_name, s_action_file_name, sizeof(s_clip_name) - 1U);
            s_clip_name[sizeof(s_clip_name) - 1U] = '\0';
            s_clip_is_cut = true;
            s_pending_screen = (int8_t)UI_SCREEN_FILES;
            break;

        case FILE_ACT_DELETE:
            Open_Delete_Confirm_Dialog();
            break;

        default:
            break;
    }
}

static lv_obj_t *Create_Action_Btn(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h,
                                   const char *text, uint32_t bg_hex, File_Action_t act)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, file_action_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)act);
    lv_obj_add_event_cb(btn, file_action_btn_cb, LV_EVENT_KEY, NULL);
    return btn;
}

static void Open_File_Action_Menu(const char *rel_path, const char *item_name)
{
    strncpy(s_action_file_path, rel_path, sizeof(s_action_file_path) - 1U);
    s_action_file_path[sizeof(s_action_file_path) - 1U] = '\0';
    strncpy(s_action_file_name, item_name, sizeof(s_action_file_name) - 1U);
    s_action_file_name[sizeof(s_action_file_name) - 1U] = '\0';

    uint32_t fsize = 0U;
    if (f_stat(s_action_file_path, &s_ui_fno) == FR_OK)
    {
        fsize = (uint32_t)s_ui_fno.fsize;
    }

    lv_group_t *grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    file_list_obj = NULL;
    lv_obj_clean(workspace_cont);

    /* Top bar: [< Back] + File Name & Size */
    lv_obj_t *btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 64, 26);
    lv_obj_set_pos(btn_back, 8, 6);
    lv_obj_t *lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, close_preview_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, close_preview_cb, LV_EVENT_KEY, NULL);

    lv_obj_t *info_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(info_lbl, lv_color_hex(s_themes[s_theme_id % THEME_COUNT].title_col), 0);
    lv_obj_set_pos(info_lbl, 80, 4);
    lv_obj_set_width(info_lbl, 232);
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text_fmt(info_lbl, "%s (%lu B)\n/%s", item_name, (unsigned long)fsize, s_action_file_path);

    bool is_c   = Ends_With_IgnoreCase(item_name, ".c");
    bool is_bin = Ends_With_IgnoreCase(item_name, ".bin");
    bool is_img = Ends_With_IgnoreCase(item_name, ".bmp") ||
                  Ends_With_IgnoreCase(item_name, ".png") ||
                  Ends_With_IgnoreCase(item_name, ".jpg") ||
                  Ends_With_IgnoreCase(item_name, ".sjpg");

    lv_obj_t *btn_primary1 = NULL;
    lv_obj_t *btn_primary2 = NULL;

    if (is_c)
    {
        btn_primary1 = Create_Action_Btn(workspace_cont, 8,   44, 148, 38, LV_SYMBOL_PLAY " Run Script", 0x00A859, FILE_ACT_RUN_SCRIPT);
        btn_primary2 = Create_Action_Btn(workspace_cont, 164, 44, 148, 38, LV_SYMBOL_EDIT " Edit Code",  0x0277BD, FILE_ACT_OPEN);
    }
    else if (is_bin)
    {
        btn_primary1 = Create_Action_Btn(workspace_cont, 8, 44, 304, 38, LV_SYMBOL_PLAY " Launch App", 0x00A859, FILE_ACT_OPEN);
    }
    else if (is_img)
    {
        btn_primary1 = Create_Action_Btn(workspace_cont, 8, 44, 304, 38, LV_SYMBOL_IMAGE " View Image", 0x0277BD, FILE_ACT_OPEN);
    }
    else
    {
        btn_primary1 = Create_Action_Btn(workspace_cont, 8, 44, 304, 38, LV_SYMBOL_EDIT " Edit Text", 0x0277BD, FILE_ACT_OPEN);
    }

    lv_obj_t *btn_ren  = Create_Action_Btn(workspace_cont, 8,   92,  148, 36, "Rename",                  0x455A64, FILE_ACT_RENAME);
    lv_obj_t *btn_copy = Create_Action_Btn(workspace_cont, 164, 92,  148, 36, LV_SYMBOL_COPY " Copy",    0x00695C, FILE_ACT_COPY);
    lv_obj_t *btn_move = Create_Action_Btn(workspace_cont, 8,   136, 148, 36, LV_SYMBOL_CUT " Move",     0xE65100, FILE_ACT_MOVE);
    lv_obj_t *btn_del  = Create_Action_Btn(workspace_cont, 164, 136, 148, 36, LV_SYMBOL_TRASH " Delete", 0xC62828, FILE_ACT_DELETE);

    if (grp != NULL)
    {
        /* Strict left-to-right, top-to-bottom order; focus primary action first */
        lv_group_add_obj(grp, btn_back);
        if (btn_primary1 != NULL) lv_group_add_obj(grp, btn_primary1);
        if (btn_primary2 != NULL) lv_group_add_obj(grp, btn_primary2);
        lv_group_add_obj(grp, btn_ren);
        lv_group_add_obj(grp, btn_copy);
        lv_group_add_obj(grp, btn_move);
        lv_group_add_obj(grp, btn_del);
        if (btn_primary1 != NULL) lv_group_focus_obj(btn_primary1);
    }
}

static void file_list_btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);

    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_HOME;
        return;
    }

    if (code == LV_EVENT_CLICKED && file_list_obj != NULL && obj != NULL)
    {
        const char * raw_name = lv_list_get_btn_text(file_list_obj, obj);
        if (raw_name == NULL) return;

        strncpy(s_clicked_item, raw_name, sizeof(s_clicked_item) - 1U);
        s_clicked_item[sizeof(s_clicked_item) - 1U] = '\0';

        Beeper_Click();

        if (strcmp(s_clicked_item, "[ Home Screen ]") == 0)
        {
            UIManager_ShowScreen(UI_SCREEN_HOME);
            return;
        }

        if (strcmp(s_clicked_item, "[ + New File... ]") == 0)
        {
            Open_File_Name_Dialog(FNAME_MODE_NEW_FILE);
            return;
        }

        if (strcmp(s_clicked_item, "[ + New Folder... ]") == 0)
        {
            Open_File_Name_Dialog(FNAME_MODE_NEW_DIR);
            return;
        }

        if (strcmp(s_clicked_item, "[ Rename Current Folder ]") == 0)
        {
            Open_File_Name_Dialog(FNAME_MODE_RENAME_DIR);
            return;
        }

        if (strcmp(s_clicked_item, "[ Delete Empty Folder ]") == 0)
        {
            const char *cur_rel = Get_Current_Rel_Dir();
            if (cur_rel[0] != '\0' && f_unlink(cur_rel) == FR_OK)
            {
                Beeper_Beep(15);
                strncpy(s_next_path, g_dir_cache.path, sizeof(s_next_path) - 1U);
                s_next_path[sizeof(s_next_path) - 1U] = '\0';
                char *last_slash = strrchr(s_next_path, '/');
                if (last_slash == NULL || last_slash == s_next_path)
                {
                    OSTasks_RequestDirList("/");
                }
                else
                {
                    *last_slash = '\0';
                    OSTasks_RequestDirList(s_next_path);
                }
            }
            else
            {
                Beeper_ErrorAlert();
            }
            return;
        }

        if (strncmp(s_clicked_item, "[ Paste ", 8) == 0)
        {
            Paste_Clipboard_In_Current_Dir();
            return;
        }

        if (strcmp(s_clicked_item, "[ Cancel Clipboard ]") == 0)
        {
            s_clip_path[0] = '\0';
            s_clip_name[0] = '\0';
            s_clip_is_cut  = false;
            Populate_File_List_From_Cache();
            return;
        }

        if (strcmp(s_clicked_item, ".. (Up)") == 0)
        {
            strncpy(s_next_path, g_dir_cache.path, sizeof(s_next_path) - 1U);
            s_next_path[sizeof(s_next_path) - 1U] = '\0';
            char *last_slash = strrchr(s_next_path, '/');
            if (last_slash == NULL || last_slash == s_next_path)
            {
                OSTasks_RequestDirList("/");
            }
            else
            {
                *last_slash = '\0';
                OSTasks_RequestDirList(s_next_path);
            }
            return;
        }

        for (uint32_t i = 0; i < g_dir_cache.count; i++)
        {
            if (strcmp(g_dir_cache.items[i].name, s_clicked_item) == 0)
            {
                if (g_dir_cache.items[i].is_dir)
                {
                    if (strcmp(g_dir_cache.path, "/") == 0)
                    {
                        snprintf(s_next_path, sizeof(s_next_path), "/%s", s_clicked_item);
                    }
                    else
                    {
                        snprintf(s_next_path, sizeof(s_next_path), "%.60s/%.50s", g_dir_cache.path, s_clicked_item);
                    }
                    OSTasks_RequestDirList(s_next_path);
                    return;
                }
                break;
            }
        }

        /* Build relative path for selected file and open File Action Menu */
        const char *dir = Get_Current_Rel_Dir();
        if (dir[0] == '\0')
        {
            snprintf(s_next_path, sizeof(s_next_path), "%s", s_clicked_item);
        }
        else
        {
            snprintf(s_next_path, sizeof(s_next_path), "%.60s/%.50s", dir, s_clicked_item);
        }

        Open_File_Action_Menu(s_next_path, s_clicked_item);
    }
}

static void Build_Files_Screen(void)
{
    file_list_obj = lv_list_create(workspace_cont);
    lv_obj_set_size(file_list_obj, 308, 204);
    lv_obj_align(file_list_obj, LV_ALIGN_CENTER, 0, 0);

    lv_list_add_text(file_list_obj, "Scanning SD card...");
    OSTasks_RequestDirList(g_dir_cache.path[0] != '\0' ? g_dir_cache.path : "/");
}

void UIManager_OnDirReady(void)
{
    if (current_screen == UI_SCREEN_FILES && file_list_obj != NULL)
    {
        Populate_File_List_From_Cache();
    }
    else if (current_screen == UI_SCREEN_GAMES && games_grid_obj != NULL)
    {
        Populate_Games_Grid();
    }
}

static void Populate_File_List_From_Cache(void)
{
    if (file_list_obj == NULL) return;

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    lv_obj_clean(file_list_obj);

    lv_obj_t * home_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_HOME, "[ Home Screen ]");
    lv_obj_add_event_cb(home_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(home_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
    if (grp != NULL) lv_group_add_obj(grp, home_btn);

    if (g_dir_cache.io_success)
    {
        lv_obj_t * new_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_PLUS, "[ + New File... ]");
        lv_obj_add_event_cb(new_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(new_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
        if (grp != NULL) lv_group_add_obj(grp, new_btn);

        lv_obj_t * dir_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_DIRECTORY, "[ + New Folder... ]");
        lv_obj_add_event_cb(dir_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(dir_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
        if (grp != NULL) lv_group_add_obj(grp, dir_btn);

        if (s_clip_path[0] != '\0')
        {
            char paste_label[STORAGE_MAX_NAME_LEN + 24U];
            snprintf(paste_label, sizeof(paste_label), "[ Paste (%s): %.36s ]",
                     s_clip_is_cut ? "Move" : "Copy", s_clip_name);
            lv_obj_t * paste_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_PASTE, paste_label);
            lv_obj_add_event_cb(paste_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(paste_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
            if (grp != NULL) lv_group_add_obj(grp, paste_btn);

            lv_obj_t * cancel_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_CLOSE, "[ Cancel Clipboard ]");
            lv_obj_add_event_cb(cancel_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(cancel_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
            if (grp != NULL) lv_group_add_obj(grp, cancel_btn);
        }
    }

    snprintf(s_title_buf, sizeof(s_title_buf), "Folder: %s", g_dir_cache.path);
    lv_list_add_text(file_list_obj, s_title_buf);

    if (!g_dir_cache.io_success)
    {
        lv_list_add_text(file_list_obj, "SD Card Error / Not Mounted");
        return;
    }

    if (strcmp(g_dir_cache.path, "/") != 0)
    {
        lv_obj_t * up_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_UP, ".. (Up)");
        lv_obj_add_event_cb(up_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(up_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
        if (grp != NULL) lv_group_add_obj(grp, up_btn);

        lv_obj_t * ren_dir_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_EDIT, "[ Rename Current Folder ]");
        lv_obj_add_event_cb(ren_dir_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(ren_dir_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
        if (grp != NULL) lv_group_add_obj(grp, ren_dir_btn);

        if (g_dir_cache.count == 0U)
        {
            lv_obj_t * del_dir_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_TRASH, "[ Delete Empty Folder ]");
            lv_obj_add_event_cb(del_dir_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(del_dir_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
            if (grp != NULL) lv_group_add_obj(grp, del_dir_btn);
        }
    }

    for (uint32_t i = 0; i < g_dir_cache.count; i++)
    {
        const char *icon = g_dir_cache.items[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
        lv_obj_t * btn = lv_list_add_btn(file_list_obj, icon, g_dir_cache.items[i].name);
        lv_obj_add_event_cb(btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
        if (grp != NULL) lv_group_add_obj(grp, btn);
    }
}

/* ==========================================================================
 * 4. Games Launcher (built-in ROM game + games/<name>.bin as icon tiles)
 * ========================================================================== */

static void games_tile_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_HOME;
        return;
    }
    if (code != LV_EVENT_CLICKED)
    {
        return;
    }

    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_games_count)
    {
        return;
    }

    Beeper_Click();
    bool started;
    if (s_games[idx].builtin && s_games[idx].is_script)
    {
        /* Built-in ROM C-script demo (works even without SD card) */
        static const char s_rom_script_demo[] =
            "// Built-in C-Script Demo (Starfield & Bounce)\n"
            "int sx[24];\n"
            "int sy[24];\n"
            "int spd[24];\n"
            "void draw_hud(int score, int best) {\n"
            "    fill_rect(0, 0, 320, 20, COLOR_PANEL_BG);\n"
            "    draw_text(8, 6, \"C-SCRIPT DEMO   SCORE:\", COLOR_NEON_GREEN, COLOR_PANEL_BG, 1);\n"
            "    draw_num(146, 6, score, COLOR_YELLOW, COLOR_PANEL_BG, 1, 3);\n"
            "    draw_text(192, 6, \"BEST:\", COLOR_WHITE, COLOR_PANEL_BG, 1);\n"
            "    draw_num(226, 6, best, COLOR_CYAN, COLOR_PANEL_BG, 1, 3);\n"
            "}\n"
            "void main() {\n"
            "    for (int i = 0; i < 24; i++) {\n"
            "        sx[i] = rand_range(6, 314);\n"
            "        sy[i] = rand_range(24, 212);\n"
            "        spd[i] = rand_range(1, 4);\n"
            "    }\n"
            "    int best = load_int(\"best.dat\", 0);\n"
            "    int bx = 160, by = 90, vx = 3, vy = 2, score = 0;\n"
            "    int px = 136, old_px = 136;\n"
            "    clear_screen(COLOR_DARK_BG);\n"
            "    draw_hud(score, best);\n"
            "    fill_rect(px, 220, 48, 5, COLOR_CYAN);\n"
            "    fill_rect(bx - 4, by - 4, 9, 9, COLOR_AMBER);\n"
            "    while (1) {\n"
            "        int k = input_poll();\n"
            "        if (input_pressed(KEY_EXIT)) break;\n"
            "        old_px = px;\n"
            "        if (k & KEY_LEFT)  px -= 5;\n"
            "        if (k & KEY_RIGHT) px += 5;\n"
            "        px += pad_lx() / 24;\n"
            "        px = clamp(px, 4, 268);\n"
            "        if (px != old_px) {\n"
            "            fill_rect(old_px, 220, 48, 5, COLOR_DARK_BG);\n"
            "            fill_rect(px, 220, 48, 5, COLOR_CYAN);\n"
            "        }\n"
            "        for (int i = 0; i < 24; i++) {\n"
            "            if (sx[i] < bx - 6 || sx[i] > bx + 6 || sy[i] < by - 6 || sy[i] > by + 6) {\n"
            "                draw_pixel(sx[i], sy[i], COLOR_DARK_BG);\n"
            "            }\n"
            "            sy[i] += spd[i];\n"
            "            if (sy[i] >= 214) { sy[i] = 24; sx[i] = rand_range(6, 314); }\n"
            "            if (sx[i] < bx - 6 || sx[i] > bx + 6 || sy[i] < by - 6 || sy[i] > by + 6) {\n"
            "                draw_pixel(sx[i], sy[i], (spd[i] > 2) ? COLOR_CYAN : COLOR_WHITE);\n"
            "            }\n"
            "        }\n"
            "        int old_bx = bx, old_by = by;\n"
            "        bx += vx; by += vy;\n"
            "        if (bx <= 8)   { bx = 8;   vx = -vx; beep(15); }\n"
            "        if (bx >= 311) { bx = 311; vx = -vx; beep(15); }\n"
            "        if (by <= 26)  { by = 26;  vy = -vy; beep(15); }\n"
            "        if (vy > 0 && by >= 214 && by <= 222 && bx >= px - 4 && bx <= px + 52) {\n"
            "            by = 214;\n"
            "            vy = -vy;\n"
            "            vx = clamp((bx - (px + 24)) / 5, -5, 5);\n"
            "            if (vx == 0) vx = 2;\n"
            "            score++;\n"
            "            if (score > best) {\n"
            "                best = score;\n"
            "                save_int(\"best.dat\", best);\n"
            "            }\n"
            "            draw_hud(score, best);\n"
            "            beep(30);\n"
            "        } else if (by > 232) {\n"
            "            beep(120);\n"
            "            score = 0;\n"
            "            draw_hud(score, best);\n"
            "            bx = 160; by = 80; vy = 2;\n"
            "        }\n"
            "        fill_rect(old_bx - 4, old_by - 4, 9, 9, COLOR_DARK_BG);\n"
            "        fill_rect(bx - 4, by - 4, 9, 9, COLOR_AMBER);\n"
            "        fill_rect(px, 220, 48, 5, COLOR_CYAN);\n"
            "        frame_sync(16);\n"
            "    }\n"
            "}\n";
        started = AppLoader_LoadAndRunScriptText("c_demo", s_rom_script_demo,
                                                 (uint32_t)(sizeof(s_rom_script_demo) - 1U));
    }
    else if (s_games[idx].builtin)
    {
        started = AppLoader_LoadAndRunImage(g_rom_arkanoid_bin,
                                            (uint32_t)(g_rom_arkanoid_bin_end - g_rom_arkanoid_bin));
    }
    else if (s_games[idx].is_script)
    {
        snprintf(s_next_path, sizeof(s_next_path), "games/%.60s", s_games[idx].file);
        started = AppLoader_LoadAndRunScript(s_next_path);
    }
    else
    {
        snprintf(s_next_path, sizeof(s_next_path), "games/%.60s", s_games[idx].file);
        started = AppLoader_LoadAndRun(s_next_path);
    }
    if (!started)
    {
        Show_App_Load_Error();
    }
}

/* Looks for "<base>.bmp" next to the game in the cached /games listing */
static bool Games_FindIcon(const char *app_name, char *out_path, size_t out_size)
{
    const char *dot = strrchr(app_name, '.');
    if (dot == NULL || dot == app_name)
    {
        return false;
    }
    size_t base_len = (size_t)(dot - app_name);

    for (uint32_t i = 0; i < g_dir_cache.count; i++)
    {
        const char *name = g_dir_cache.items[i].name;
        if (!g_dir_cache.items[i].is_dir && strlen(name) == base_len + 4U &&
            strncmp(name, app_name, base_len) == 0 && Ends_With_IgnoreCase(name, ".bmp"))
        {
            snprintf(out_path, out_size, "games/%.60s", name);
            return true;
        }
    }
    return false;
}

static void Games_AddTile(const char *title, const char *icon_path, uint32_t accent_hex)
{
    uint32_t idx = s_games_count - 1U;

    lv_obj_t * btn = lv_btn_create(games_grid_obj);
    lv_obj_set_size(btn, 92, 92);
    lv_obj_set_style_pad_all(btn, 2, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x252F48), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(accent_hex), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x324164), LV_STATE_FOCUSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    uint16_t *slot = (uint16_t *)(MM_EXT_IMAGE_BUF + idx * GAME_ICON_SLOT_BYTES);
    if (icon_path != NULL &&
        Load_BMP_To_Buffer(icon_path, slot, GAME_ICON_SIZE, GAME_ICON_SIZE, &s_icon_dsc[idx]))
    {
        lv_obj_t * img = lv_img_create(btn);
        lv_img_set_src(img, &s_icon_dsc[idx]);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 2);
    }
    else
    {
        /* Default icon: accent tile with the first letter of the game */
        lv_obj_t * box = lv_obj_create(btn);
        lv_obj_set_size(box, GAME_ICON_SIZE, GAME_ICON_SIZE);
        lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(accent_hex), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_70, 0);

        lv_obj_t * sym = lv_label_create(box);
        char letter = (title[0] >= 'a' && title[0] <= 'z') ? (char)(title[0] - 32) : title[0];
        lv_label_set_text_fmt(sym, LV_SYMBOL_PLAY "\n%c", letter);
        lv_obj_set_style_text_align(sym, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(sym, lv_color_hex(0x10141C), 0);
        lv_obj_center(sym);
    }

    lv_obj_t * lbl = lv_label_create(btn);
    lv_obj_set_width(lbl, 86);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, title);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_add_event_cb(btn, games_tile_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(btn, games_tile_event_cb, LV_EVENT_KEY, (void *)(uintptr_t)idx);

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_add_obj(grp, btn);
}

static void btn_uart_rx_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        Beeper_Click();
        (void)UARTLoader_RunSession(false);
        if (!AppLoader_IsRunning())
        {
            s_pending_screen = (int8_t)UI_SCREEN_GAMES;
        }
    }
}

static void Populate_Games_Grid(void)
{
    if (games_grid_obj == NULL) return;

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    lv_obj_clean(games_grid_obj);
    s_games_count = 0;

    /* 1. Built-in games (work without an SD card) */
    strncpy(s_games[0].file, "Arkanoid", sizeof(s_games[0].file) - 1U);
    s_games[0].file[sizeof(s_games[0].file) - 1U] = '\0';
    s_games[0].builtin   = true;
    s_games[0].is_script = false;
    s_games_count = 1;
    Games_AddTile("Arkanoid", NULL, 0x00FF66);

    strncpy(s_games[1].file, "C-Demo.c", sizeof(s_games[1].file) - 1U);
    s_games[1].file[sizeof(s_games[1].file) - 1U] = '\0';
    s_games[1].builtin   = true;
    s_games[1].is_script = true;
    s_games_count = 2;
    Games_AddTile("C-Demo.c", NULL, 0x00E5FF);

    /* 2. SD card games (.bin native apps and .c C-scripts) */
    static char title[STORAGE_MAX_NAME_LEN];
    static char icon_path[STORAGE_MAX_PATH_LEN];
    bool games_listed = g_dir_cache.io_success && (strcmp(g_dir_cache.path, "/games") == 0);

    for (uint32_t i = 0; games_listed && i < g_dir_cache.count && s_games_count < GAMES_MAX_TILES; i++)
    {
        const char *fname = g_dir_cache.items[i].name;
        bool is_bin = !g_dir_cache.items[i].is_dir && Ends_With_IgnoreCase(fname, ".bin");
        bool is_scr = !g_dir_cache.items[i].is_dir && Ends_With_IgnoreCase(fname, ".c");
        if (!is_bin && !is_scr)
        {
            continue;
        }

        Game_Entry_t *g = &s_games[s_games_count];
        strncpy(g->file, fname, sizeof(g->file) - 1U);
        g->file[sizeof(g->file) - 1U] = '\0';
        g->builtin   = false;
        g->is_script = is_scr;
        s_games_count++;

        strncpy(title, fname, sizeof(title) - 1U);
        title[sizeof(title) - 1U] = '\0';
        if (is_bin)
        {
            size_t len = strlen(title);
            if (len > 4U) title[len - 4U] = '\0';
        }

        bool has_icon = Games_FindIcon(fname, icon_path, sizeof(icon_path));
        Games_AddTile(title, has_icon ? icon_path : NULL, is_scr ? 0x00E5FF : 0xFFAE00);
    }

    if (!SDCard_IsMounted())
    {
        lv_obj_t * lbl = lv_label_create(games_grid_obj);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
        lv_label_set_text(lbl, "No SD card: built-in games only");
    }

    /* UART Receive tile (XMODEM-CRC / uart_upload.py over USART3) */
    lv_obj_t * uart_btn = lv_btn_create(games_grid_obj);
    lv_obj_set_size(uart_btn, 92, 92);
    lv_obj_set_style_radius(uart_btn, 10, 0);
    lv_obj_set_style_bg_color(uart_btn, lv_color_hex(0x182B3A), 0);
    lv_obj_set_style_border_width(uart_btn, 2, 0);
    lv_obj_set_style_border_color(uart_btn, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_color(uart_btn, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_t * uart_lbl = lv_label_create(uart_btn);
    lv_label_set_text(uart_lbl, LV_SYMBOL_DOWNLOAD "\nUART Rx");
    lv_obj_set_style_text_align(uart_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(uart_lbl);
    lv_obj_add_event_cb(uart_btn, btn_uart_rx_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(uart_btn, global_key_event_cb, LV_EVENT_KEY, NULL);
    if (grp != NULL) lv_group_add_obj(grp, uart_btn);

    /* Home tile for touch / mouse users (keys: ESC / gamepad B) */
    lv_obj_t * home = lv_btn_create(games_grid_obj);
    lv_obj_set_size(home, 92, 92);
    lv_obj_set_style_radius(home, 10, 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x1B2234), 0);
    lv_obj_set_style_border_width(home, 2, 0);
    lv_obj_set_style_border_color(home, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_color(home, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_t * home_lbl = lv_label_create(home);
    lv_label_set_text(home_lbl, LV_SYMBOL_HOME "\nHome");
    lv_obj_set_style_text_align(home_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(home_lbl);
    lv_obj_add_event_cb(home, btn_back_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(home, global_key_event_cb, LV_EVENT_KEY, NULL);
    if (grp != NULL) lv_group_add_obj(grp, home);

    if (grp != NULL)
    {
        lv_obj_t * first = lv_obj_get_child(games_grid_obj, 0);
        if (first != NULL) lv_group_focus_obj(first);
    }
}

static void Build_Games_Screen(void)
{
    games_grid_obj = lv_obj_create(workspace_cont);
    lv_obj_set_size(games_grid_obj, 320, 216);
    lv_obj_align(games_grid_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(games_grid_obj, 0, 0);
    lv_obj_set_style_border_width(games_grid_obj, 0, 0);
    lv_obj_set_style_bg_opa(games_grid_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(games_grid_obj, 8, 0);
    lv_obj_set_style_pad_row(games_grid_obj, 8, 0);
    lv_obj_set_style_pad_column(games_grid_obj, 8, 0);
    lv_obj_set_flex_flow(games_grid_obj, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(games_grid_obj, LV_DIR_VER);

    Populate_Games_Grid();
    OSTasks_RequestDirList("/games");
}

/* ==========================================================================
 * 5. Settings Screen (RTC, Audio, Theme, Stick Deadzone & 4-Point Touch Calibration)
 * ========================================================================== */

static void draw_calib_crosshair(uint16_t cx, uint16_t cy, uint16_t color)
{
    lcdSetWindow((uint16_t)(cx - 10U), cy, (uint16_t)(cx + 10U), cy);
    for (uint8_t i = 0U; i < 21U; i++) LCD_DataWrite(color);

    lcdSetWindow(cx, (uint16_t)(cy - 10U), cx, (uint16_t)(cy + 10U));
    for (uint8_t i = 0U; i < 21U; i++) LCD_DataWrite(color);

    lcdSetWindow((uint16_t)(cx - 2U), (uint16_t)(cy - 2U), (uint16_t)(cx + 2U), (uint16_t)(cy + 2U));
    for (uint8_t i = 0U; i < 25U; i++) LCD_DataWrite(color);
}

static bool Run_Touch_Calibration_Wizard(void)
{
    static const uint16_t pts[4][2] = {
        { 20U,  20U  }, /* 0: Top-Left     */
        { 300U, 20U  }, /* 1: Top-Right    */
        { 300U, 220U }, /* 2: Bottom-Right */
        { 20U,  220U }  /* 3: Bottom-Left  */
    };
    static const char * const pt_names[4] = {
        "STEP 1/4: TOUCH TOP-LEFT",
        "STEP 2/4: TOUCH TOP-RIGHT",
        "STEP 3/4: TOUCH BOTTOM-RIGHT",
        "STEP 4/4: TOUCH BOTTOM-LEFT"
    };

    LVGL_Port_WaitFlushIdle();
    lcdSetWindow(0U, 0U, 319U, 239U);
    lcdFillConstantDMA(&hdma_memtomem_dma2_stream0, 0x0862U, 320UL * 240UL);

    App_DrawTextParams_t txt = {
        .x = 34U, .y = 92U, .str = "XPT2046 TOUCH CALIBRATION",
        .fg_color = 0x07FFU, .bg_color = 0x0862U, .scale = 1U
    };
    AppLoader_DrawText_Privileged(&txt);

    txt.x = 46U; txt.y = 132U; txt.str = "ESC / GAMEPAD B: CANCEL";
    txt.fg_color = 0xFD20U;
    AppLoader_DrawText_Privileged(&txt);

    uint16_t rx[4] = {0};
    uint16_t ry[4] = {0};

    /* Wait until touch & exit keys are released before starting */
    while (XPT2046_TouchPressed() || (AppLoader_ReadInputMask_Privileged() & APP_KEY_EXIT) != 0U)
    {
        (void)HAL_IWDG_Refresh(&hiwdg);
        osDelay(15U);
    }

    for (uint8_t step = 0U; step < 4U; step++)
    {
        txt.x = 34U; txt.y = 112U; txt.str = "                            ";
        AppLoader_DrawText_Privileged(&txt);
        txt.str = pt_names[step];
        txt.fg_color = 0x07E0U;
        AppLoader_DrawText_Privileged(&txt);

        draw_calib_crosshair(pts[step][0], pts[step][1], 0xFFFFU);

        /* Wait for stable touch sample or cancel */
        for (;;)
        {
            (void)HAL_IWDG_Refresh(&hiwdg);
            if ((AppLoader_ReadInputMask_Privileged() & APP_KEY_EXIT) != 0U)
            {
                return false;
            }
            if (XPT2046_TouchPressed())
            {
                osDelay(35U);
                if (XPT2046_TouchGetRawCoordinates(&rx[step], &ry[step]))
                {
                    Beeper_Click();
                    break;
                }
            }
            osDelay(15U);
        }

        draw_calib_crosshair(pts[step][0], pts[step][1], 0x0862U);

        /* Wait for finger release before next corner */
        while (XPT2046_TouchPressed())
        {
            (void)HAL_IWDG_Refresh(&hiwdg);
            osDelay(15U);
        }
        osDelay(120U);
    }

    /* Compute extrapolated full-screen [0..320, 0..240] raw bounds from the 20 px inset targets */
    int32_t raw_left   = ((int32_t)rx[0] + (int32_t)rx[3]) / 2;
    int32_t raw_right  = ((int32_t)rx[1] + (int32_t)rx[2]) / 2;
    int32_t raw_top    = ((int32_t)ry[0] + (int32_t)ry[1]) / 2;
    int32_t raw_bottom = ((int32_t)ry[2] + (int32_t)ry[3]) / 2;

    if (raw_left > raw_right)
    {
        int32_t t = raw_left; raw_left = raw_right; raw_right = t;
    }
    if (raw_bottom > raw_top)
    {
        int32_t t = raw_bottom; raw_bottom = raw_top; raw_top = t;
    }

    int32_t span_x = raw_right - raw_left;   /* across 280 px */
    int32_t span_y = raw_top   - raw_bottom; /* across 200 px (Y is inverted in Landscape) */

    if (span_x < 4000 || span_y < 4000)
    {
        Beeper_ErrorAlert();
        return false;
    }

    int32_t min_x = raw_left   - (span_x * 20) / 280;
    int32_t max_x = raw_right  + (span_x * 20) / 280;
    int32_t min_y = raw_bottom - (span_y * 20) / 200;
    int32_t max_y = raw_top    + (span_y * 20) / 200;

    if (min_x < 200)   min_x = 200;
    if (max_x > 32500) max_x = 32500;
    if (min_y < 200)   min_y = 200;
    if (max_y > 32500) max_y = 32500;

    XPT2046_TouchSetCalibration((uint16_t)min_x, (uint16_t)max_x, (uint16_t)min_y, (uint16_t)max_y);
    Settings_SaveToEEPROM();
    Beeper_BootMelody();
    return true;
}

static void settings_action_btn_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    intptr_t action = (intptr_t)lv_event_get_user_data(e);
    Beeper_Click();

    if (action == 1)      edit_hours = (edit_hours + 1U) % 24U;
    else if (action == 2) edit_hours = (edit_hours == 0U) ? 23U : (edit_hours - 1U);
    else if (action == 3) edit_minutes = (edit_minutes + 1U) % 60U;
    else if (action == 4) edit_minutes = (edit_minutes == 0U) ? 59U : (edit_minutes - 1U);
    else if (action == 5)
    {
        RTC_SetTimeManual(edit_hours, edit_minutes);
        Beeper_BootMelody();
    }
    else if (action == 6) /* Cycle Deadzone: 4 -> 8 -> 12 -> 16 -> 24 -> 32 */
    {
        static const uint8_t dz_steps[] = { 4U, 8U, 12U, 16U, 24U, 32U };
        uint8_t cur = USBHostManager_GetStickDeadzone();
        uint8_t next = dz_steps[0];
        for (size_t i = 0; i < sizeof(dz_steps); i++)
        {
            if (dz_steps[i] > cur)
            {
                next = dz_steps[i];
                break;
            }
        }
        USBHostManager_SetStickDeadzone(next);
        Settings_SaveToEEPROM();
        if (lbl_dz_val != NULL)
        {
            lv_label_set_text_fmt(lbl_dz_val, "DZ: %u", (unsigned int)next);
        }
    }
    else if (action == 7) /* Cycle UI Theme */
    {
        s_theme_id = (uint8_t)((s_theme_id + 1U) % THEME_COUNT);
        Apply_Current_Theme();
        Settings_SaveToEEPROM();
        if (lbl_thm_val != NULL)
        {
            lv_label_set_text_fmt(lbl_thm_val, "Theme: %s", s_themes[s_theme_id].name);
        }
    }
    else if (action == 8) /* 4-Point Touch Calibration Wizard */
    {
        (void)Run_Touch_Calibration_Wizard();
        s_pending_screen = (int8_t)UI_SCREEN_SETTINGS;
        return;
    }
    else if (action == 9) /* Wi-Fi Setup Wizard */
    {
        UIManager_OpenWifiSetup();
        return;
    }

    if (lbl_time_set != NULL)
    {
        lv_label_set_text_fmt(lbl_time_set, "RTC: %02u:%02u", edit_hours, edit_minutes);
    }
}

static void sound_switch_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
    {
        lv_obj_t * sw = lv_event_get_target(e);
        bool sound_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
        Beeper_SetMuted(!sound_enabled);
        if (sound_enabled)
        {
            Beeper_Click();
        }
        Settings_SaveToEEPROM();
    }
}

static lv_obj_t* Create_Setting_Btn(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h,
                                    const char *txt, intptr_t action)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, settings_action_btn_cb, LV_EVENT_CLICKED, (void *)action);
    lv_obj_add_event_cb(btn, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_add_obj(grp, btn);
    return btn;
}

/* ==========================================================================
 * Wi-Fi Setup Dialog (Interactive SSID/Pass Entry + SD:/system/wifi.cfg)
 * ========================================================================== */

static void wifi_ta_focus_cb(lv_event_t * e)
{
    lv_obj_t * ta = lv_event_get_target(e);
    if (s_editor_kb != NULL && ta != NULL)
    {
        lv_keyboard_set_textarea(s_editor_kb, ta);
    }
}

static void wifi_connect_btn_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_SETTINGS;
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_CLICKED && lv_event_get_code(e) != LV_EVENT_READY)
    {
        return;
    }
    if (s_wifi_ssid_ta == NULL || s_wifi_pass_ta == NULL)
    {
        return;
    }

    const char *ssid = lv_textarea_get_text(s_wifi_ssid_ta);
    while (ssid != NULL && *ssid == ' ') ssid++;
    const char *pass = lv_textarea_get_text(s_wifi_pass_ta);

    if (ssid != NULL && ssid[0] != '\0')
    {
        (void)ESPCompanion_SaveWifiToSD(ssid, pass);
        if (ESPCompanion_SendWifiCredentials(ssid, pass))
        {
            if (s_wifi_stat_lbl != NULL)
            {
                lv_label_set_text_fmt(s_wifi_stat_lbl, "#00E5FF Connecting to %s...#", ssid);
            }
            Beeper_BootMelody();
        }
        else
        {
            if (s_wifi_stat_lbl != NULL)
            {
                lv_label_set_text(s_wifi_stat_lbl, "#FF5555 Error: ESP32 module is offline!#");
            }
            Beeper_ErrorAlert();
        }
    }
    else
    {
        if (s_wifi_stat_lbl != NULL)
        {
            lv_label_set_text(s_wifi_stat_lbl, "#FF5555 Error: SSID cannot be empty!#");
        }
        Beeper_ErrorAlert();
    }
}

static void wifi_load_sd_btn_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_SETTINGS;
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    char ssid[34] = {0};
    char pass[64] = {0};
    if (ESPCompanion_ReadWifiFromSD(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        if (s_wifi_ssid_ta != NULL) lv_textarea_set_text(s_wifi_ssid_ta, ssid);
        if (s_wifi_pass_ta != NULL) lv_textarea_set_text(s_wifi_pass_ta, pass);
        if (s_wifi_stat_lbl != NULL)
        {
            lv_label_set_text_fmt(s_wifi_stat_lbl, "#90A4AE Loaded from SD: %s#", ssid);
        }
        Beeper_Beep(15);
    }
    else
    {
        if (s_wifi_stat_lbl != NULL)
        {
            lv_label_set_text(s_wifi_stat_lbl, "#FF5555 SD:/system/wifi.cfg not found!#");
        }
        Beeper_ErrorAlert();
    }
}

static void wifi_back_btn_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED ||
        (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC))
    {
        Beeper_Click();
        s_pending_screen = (int8_t)UI_SCREEN_SETTINGS;
    }
}

static void Build_Wifi_Screen(void)
{
    uint32_t accent_hex = s_themes[s_theme_id % THEME_COUNT].title_col;

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    /* Row 0: Top Bar with Back, Title, Load SD, and Connect */
    lv_obj_t * btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 54, 24);
    lv_obj_set_pos(btn_back, 6, 4);
    lv_obj_t * lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, wifi_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, wifi_back_btn_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * title_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(accent_hex), 0);
    lv_obj_set_pos(title_lbl, 66, 8);
    lv_label_set_text(title_lbl, "Wi-Fi Setup");

    lv_obj_t * btn_sd = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_sd, 74, 24);
    lv_obj_set_pos(btn_sd, 146, 4);
    lv_obj_set_style_bg_color(btn_sd, lv_color_hex(0x37474F), 0);
    lv_obj_t * lbl_sd = lv_label_create(btn_sd);
    lv_label_set_text(lbl_sd, LV_SYMBOL_SD_CARD " Load");
    lv_obj_center(lbl_sd);
    lv_obj_add_event_cb(btn_sd, wifi_load_sd_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_sd, wifi_load_sd_btn_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * btn_conn = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_conn, 88, 24);
    lv_obj_set_pos(btn_conn, 226, 4);
    lv_obj_set_style_bg_color(btn_conn, lv_color_hex(0x00A859), 0);
    lv_obj_t * lbl_c = lv_label_create(btn_conn);
    lv_label_set_text(lbl_c, LV_SYMBOL_OK " Connect");
    lv_obj_center(lbl_c);
    lv_obj_add_event_cb(btn_conn, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_conn, wifi_connect_btn_cb, LV_EVENT_KEY, NULL);

    /* Status Banner */
    s_wifi_stat_lbl = lv_label_create(workspace_cont);
    lv_label_set_recolor(s_wifi_stat_lbl, true);
    lv_obj_set_pos(s_wifi_stat_lbl, 8, 30);
    lv_obj_set_width(s_wifi_stat_lbl, 304);
    lv_label_set_long_mode(s_wifi_stat_lbl, LV_LABEL_LONG_DOT);

    if (ESPCompanion_IsWifiConnected())
    {
        lv_label_set_text_fmt(s_wifi_stat_lbl, "#00FF66 Connected:# %s (%s, %ddBm)",
                              ESPCompanion_GetWifiSsid(), ESPCompanion_GetWifiIp(), (int)ESPCompanion_GetWifiRssi());
    }
    else if (ESPCompanion_GetWifiStatus() == 2)
    {
        lv_label_set_text_fmt(s_wifi_stat_lbl, "#00E5FF Connecting to %s...#", ESPCompanion_GetWifiSsid());
    }
    else if (ESPCompanion_GetWifiStatus() == -1)
    {
        lv_label_set_text_fmt(s_wifi_stat_lbl, "#FF5555 Connection failed:# %s", ESPCompanion_GetWifiSsid());
    }
    else
    {
        lv_label_set_text(s_wifi_stat_lbl, "#90A4AE Enter credentials or load from SD card:#");
    }

    /* Input: SSID */
    lv_obj_t * lbl_s = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(lbl_s, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_s, 8, 50);
    lv_label_set_text(lbl_s, "SSID:");

    s_wifi_ssid_ta = lv_textarea_create(workspace_cont);
    lv_obj_set_size(s_wifi_ssid_ta, 240, 26);
    lv_obj_set_pos(s_wifi_ssid_ta, 70, 46);
    lv_textarea_set_one_line(s_wifi_ssid_ta, true);
    lv_textarea_set_max_length(s_wifi_ssid_ta, 32U);
    lv_obj_set_style_pad_all(s_wifi_ssid_ta, 4, 0);
    lv_obj_set_style_bg_color(s_wifi_ssid_ta, lv_color_hex(0x0E131D), 0);
    lv_obj_set_style_text_color(s_wifi_ssid_ta, lv_color_hex(0xE8F0FE), 0);
    Style_Editor_Cursor(s_wifi_ssid_ta, accent_hex);

    /* Input: Password */
    lv_obj_t * lbl_p = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(lbl_p, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_p, 8, 78);
    lv_label_set_text(lbl_p, "Pass:");

    s_wifi_pass_ta = lv_textarea_create(workspace_cont);
    lv_obj_set_size(s_wifi_pass_ta, 240, 26);
    lv_obj_set_pos(s_wifi_pass_ta, 70, 74);
    lv_textarea_set_one_line(s_wifi_pass_ta, true);
    lv_textarea_set_max_length(s_wifi_pass_ta, 63U);
    lv_obj_set_style_pad_all(s_wifi_pass_ta, 4, 0);
    lv_obj_set_style_bg_color(s_wifi_pass_ta, lv_color_hex(0x0E131D), 0);
    lv_obj_set_style_text_color(s_wifi_pass_ta, lv_color_hex(0xE8F0FE), 0);
    Style_Editor_Cursor(s_wifi_pass_ta, accent_hex);

    /* Prepopulate from SD card or current connected SSID */
    char init_ssid[34] = {0};
    char init_pass[64] = {0};
    if (ESPCompanion_ReadWifiFromSD(init_ssid, sizeof(init_ssid), init_pass, sizeof(init_pass)))
    {
        lv_textarea_set_text(s_wifi_ssid_ta, init_ssid);
        lv_textarea_set_text(s_wifi_pass_ta, init_pass);
    }
    else if (ESPCompanion_GetWifiSsid()[0] != '\0' && strcmp(ESPCompanion_GetWifiSsid(), "none") != 0)
    {
        lv_textarea_set_text(s_wifi_ssid_ta, ESPCompanion_GetWifiSsid());
    }

    lv_obj_add_event_cb(s_wifi_ssid_ta, wifi_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_wifi_ssid_ta, wifi_connect_btn_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_wifi_pass_ta, wifi_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_wifi_pass_ta, wifi_connect_btn_cb, LV_EVENT_READY, NULL);

    /* Touch Keyboard (bottom) */
    s_editor_kb = lv_keyboard_create(workspace_cont);
    lv_obj_set_size(s_editor_kb, 312, 104);
    lv_obj_align(s_editor_kb, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_keyboard_set_textarea(s_editor_kb, s_wifi_ssid_ta);

    if (grp != NULL)
    {
        lv_group_add_obj(grp, s_wifi_ssid_ta);
        lv_group_add_obj(grp, s_wifi_pass_ta);
        lv_group_add_obj(grp, btn_back);
        lv_group_add_obj(grp, btn_sd);
        lv_group_add_obj(grp, btn_conn);
        lv_group_focus_obj(s_wifi_ssid_ta);
    }
}

static void Build_Settings_Screen(void)
{
    uint8_t sec_dummy = 0;
    RTC_GetDateTime(&edit_hours, &edit_minutes, &sec_dummy);

    /* Row 1: RTC Clock adjustment */
    lbl_time_set = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(lbl_time_set, lv_color_hex(0x00FF66), 0);
    lv_obj_set_pos(lbl_time_set, 12, 10);
    lv_label_set_text_fmt(lbl_time_set, "RTC: %02u:%02u", edit_hours, edit_minutes);

    Create_Setting_Btn(workspace_cont, 98,  6, 44, 26, "H+", 1);
    Create_Setting_Btn(workspace_cont, 146, 6, 44, 26, "H-", 2);
    Create_Setting_Btn(workspace_cont, 194, 6, 44, 26, "M+", 3);
    Create_Setting_Btn(workspace_cont, 242, 6, 66, 26, LV_SYMBOL_OK " Set", 5);

    /* Row 2: Audio Mute + Stick Deadzone */
    lv_obj_t * snd_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(snd_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(snd_lbl, 12, 42);
    lv_label_set_text(snd_lbl, LV_SYMBOL_AUDIO " Sound:");

    lv_obj_t * snd_sw = lv_switch_create(workspace_cont);
    lv_obj_set_size(snd_sw, 44, 22);
    lv_obj_set_pos(snd_sw, 88, 39);
    if (!Beeper_IsMuted())
    {
        lv_obj_add_state(snd_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(snd_sw, sound_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(snd_sw, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_add_obj(grp, snd_sw);

    lv_obj_t * btn_dz = Create_Setting_Btn(workspace_cont, 146, 36, 162, 28, "", 6);
    lbl_dz_val = lv_obj_get_child(btn_dz, 0);
    lv_label_set_text_fmt(lbl_dz_val, "Stick DZ: %u", (unsigned int)USBHostManager_GetStickDeadzone());

    /* Row 3: Theme selector */
    lv_obj_t * btn_thm = Create_Setting_Btn(workspace_cont, 12, 68, 296, 28, "", 7);
    lbl_thm_val = lv_obj_get_child(btn_thm, 0);
    lv_label_set_text_fmt(lbl_thm_val, "Theme: %s", s_themes[s_theme_id % THEME_COUNT].name);

    /* Row 4: Wi-Fi Setup / Network Connection */
    lv_obj_t * btn_wifi = Create_Setting_Btn(workspace_cont, 12, 100, 296, 28, "", 9);
    lv_obj_t * lbl_wf = lv_obj_get_child(btn_wifi, 0);
    if (ESPCompanion_IsWifiConnected())
    {
        lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x004D40), 0);
        lv_label_set_text_fmt(lbl_wf, LV_SYMBOL_WIFI " Wi-Fi: %s (%s)",
                              ESPCompanion_GetWifiSsid(), ESPCompanion_GetWifiIp());
    }
    else if (ESPCompanion_IsConnected())
    {
        lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x37474F), 0);
        lv_label_set_text(lbl_wf, LV_SYMBOL_WIFI " Wi-Fi: Setup / Connect...");
    }
    else
    {
        lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x263238), 0);
        lv_label_set_text(lbl_wf, LV_SYMBOL_WIFI " Wi-Fi: Module Offline (Setup)");
    }

    /* Row 5: 4-Point XPT2046 Touchscreen Calibration */
    uint16_t min_x = 0U, max_x = 0U, min_y = 0U, max_y = 0U;
    XPT2046_TouchGetCalibration(&min_x, &max_x, &min_y, &max_y);
    lv_obj_t * btn_cal = Create_Setting_Btn(workspace_cont, 12, 132, 296, 28, "", 8);
    lv_obj_set_style_bg_color(btn_cal, lv_color_hex(0x00695C), 0);
    lv_label_set_text_fmt(lv_obj_get_child(btn_cal, 0),
                          LV_SYMBOL_PLUS " Calibrate Touch [%u..%u]",
                          (unsigned int)min_x, (unsigned int)max_x);

    /* Row 6: Back to Home button */
    lv_obj_t * btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 136, 28);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_t * lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_HOME " Home");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, btn_back_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, global_key_event_cb, LV_EVENT_KEY, NULL);

    if (grp != NULL) lv_group_add_obj(grp, btn_back);
}

/* ==========================================================================
 * 6. System Info Screen (Hardware & Memory Status)
 * ========================================================================== */

static void btn_sram_test_cb(lv_event_t * e)
{
    (void)e;
    bool ok = POST_RunSRAMFullTest(HAL_GetTick() ^ 0xA55AA55AU);
    const POST_Report_t *rep = POST_GetReport();
    printf("[DIAG] SRAM Full Test #%lu: %s (Data:%s Addr:%s PRBS:%s, %lu ms, errs=%lu)\r\n",
           (unsigned long)rep->sram_stress_runs,
           ok ? "PASS" : "FAIL",
           rep->sram_data_bus_ok ? "OK" : "ERR",
           rep->sram_addr_bus_ok ? "OK" : "ERR",
           rep->sram_prbs_ok     ? "OK" : "ERR",
           (unsigned long)rep->sram_test_ms,
           (unsigned long)rep->sram_stress_errs);
    if (ok)
    {
        Beeper_Beep(15);
    }
    else
    {
        Beeper_ErrorAlert();
    }
    s_pending_screen = (int8_t)UI_SCREEN_SYSINFO;
}

static void btn_temp_read_cb(lv_event_t * e)
{
    (void)e;
    (void)TempSensor_ReadMcuDeciC();
    int32_t ext_dc = 0;
    if (TempSensor_ReadExtDeciC(&ext_dc))
    {
        Beeper_Beep(15);
    }
    else
    {
        Beeper_Click();
    }
    s_pending_screen = (int8_t)UI_SCREEN_SYSINFO;
}

static void Build_SysInfo_Screen(void)
{
    const POST_Report_t *rep = POST_GetReport();
    uint32_t total_mb = 0, free_mb = 0;
    (void)SDCard_GetFreeSpace(&total_mb, &free_mb);

    /* Get CPU Load and Heap status */
    uint32_t free_heap = xPortGetFreeHeapSize();
    uint32_t cpu_load = 0;

#if (configGENERATE_RUN_TIME_STATS == 1)
    static TaskStatus_t *pxTaskStatusArray = NULL;
    static uint32_t ulLastTotalRunTime = 0;
    static uint32_t ulLastIdleRunTime = 0;

    UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();
    pxTaskStatusArray = pvPortMalloc(uxArraySize * sizeof(TaskStatus_t));

    if (pxTaskStatusArray != NULL)
    {
        uint32_t ulTotalRunTime;
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &ulTotalRunTime);

        if (ulTotalRunTime > 0)
        {
            uint32_t ulIdleRunTime = 0;
            for (UBaseType_t x = 0; x < uxArraySize; x++)
            {
                if (strcmp(pxTaskStatusArray[x].pcTaskName, "IDLE") == 0)
                {
                    ulIdleRunTime = pxTaskStatusArray[x].ulRunTimeCounter;
                    break;
                }
            }

            /* Calculate delta-based (instant) CPU load */
            if (ulTotalRunTime != ulLastTotalRunTime)
            {
                uint32_t deltaTotal = ulTotalRunTime - ulLastTotalRunTime;
                uint32_t deltaIdle = ulIdleRunTime - ulLastIdleRunTime;
                cpu_load = 100 - (deltaIdle * 100 / deltaTotal);
            }

            ulLastTotalRunTime = ulTotalRunTime;
            ulLastIdleRunTime = ulIdleRunTime;
        }
        vPortFree(pxTaskStatusArray);
    }
#endif

    lv_obj_t * panel = lv_obj_create(workspace_cont);

    lv_obj_set_size(panel, 296, 158);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101520), 0);
    lv_obj_set_style_pad_all(panel, 8, 0);

    lv_obj_t * info_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(info_lbl, lv_color_hex(0xE0E0E0), 0);

    int32_t mcu_dc   = TempSensor_ReadMcuDeciC();
    bool    ext_ok   = false;
    int32_t ext_dc   = TempSensor_GetCachedExtDeciC(&ext_ok);
    char    ext_str[24];
    if (ext_ok)
    {
        int32_t mag = (ext_dc >= 0) ? ext_dc : -ext_dc;
        snprintf(ext_str, sizeof(ext_str), "%s%ld.%ld C",
                 (ext_dc < 0) ? "-" : "", (long)(mag / 10), (long)(mag % 10));
    }
    else
    {
        snprintf(ext_str, sizeof(ext_str), "N/A");
    }

    char esp_status[40];
    if (!ESPCompanion_IsConnected())
    {
        snprintf(esp_status, sizeof(esp_status), "OFFLINE");
    }
    else if (ESPCompanion_IsWifiConnected())
    {
        snprintf(esp_status, sizeof(esp_status), "WiFi: %s (%s)", ESPCompanion_GetWifiSsid(), ESPCompanion_GetWifiIp());
    }
    else if (ESPCompanion_GetWifiStatus() == 2)
    {
        snprintf(esp_status, sizeof(esp_status), "Connecting: %s", ESPCompanion_GetWifiSsid());
    }
    else
    {
        snprintf(esp_status, sizeof(esp_status), "ONLINE (No Wi-Fi)");
    }

    if (rep->sram_ok)
    {
        lv_label_set_text_fmt(info_lbl,
            "OS FW v%s | SDK API v%u (168 MHz)\n"
            "CPU: %lu%% | Heap: %lu KB | CCM: %s\n"
            "Temp MCU: %ld.%ld C | DS18B20: %s\n"
            "SRAM: %lu KB | SD: %lu/%lu MB\n"
            "ESP32-C6: %s\n"
            "W25Q: 0x%06X | Stress: %lu/%lu",
            CONSOLE_FW_VERSION_STR, (unsigned int)SDK_API_VERSION,
            (unsigned long)cpu_load, (unsigned long)(free_heap / 1024),
            rep->ccmram_ok ? "OK" : "ERR",
            (long)(mcu_dc / 10), (long)((mcu_dc >= 0 ? mcu_dc : -mcu_dc) % 10), ext_str,
            (unsigned long)rep->sram_size_kb, (unsigned long)free_mb, (unsigned long)total_mb,
            esp_status,
            (unsigned int)rep->flash_id, (unsigned long)rep->sram_stress_runs, (unsigned long)rep->sram_stress_errs);
    }
    else
    {
        lv_label_set_text_fmt(info_lbl,
            "OS FW v%s | SDK API v%u (168 MHz)\n"
            "CPU: %lu%% | Heap: %lu KB | MCU: %ld.%ld C\n"
            "SRAM ERR @0x%08lX [D:%s A:%s P:%s]\n"
            "Exp: 0x%08lX Got: 0x%08lX (%lu errs)\n"
            "SPI Flash W25Q: ID 0x%06X\n"
            "SD Card: %lu MB Free / %lu MB",
            CONSOLE_FW_VERSION_STR, (unsigned int)SDK_API_VERSION,
            (unsigned long)cpu_load, (unsigned long)(free_heap / 1024),
            (long)(mcu_dc / 10), (long)((mcu_dc >= 0 ? mcu_dc : -mcu_dc) % 10),
            (unsigned long)rep->sram_fail_addr,
            rep->sram_data_bus_ok ? "OK" : "ER",
            rep->sram_addr_bus_ok ? "OK" : "ER",
            rep->sram_prbs_ok     ? "OK" : "ER",
            (unsigned long)rep->sram_fail_exp, (unsigned long)rep->sram_fail_got,
            (unsigned long)rep->sram_stress_errs,
            (unsigned int)rep->flash_id,
            (unsigned long)free_mb, (unsigned long)total_mb);
    }

    lv_obj_t * btn_test = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_test, 94, 34);
    lv_obj_align(btn_test, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_obj_set_style_bg_color(btn_test, lv_color_hex(0x00695C), 0);
    lv_obj_t * lbl_t = lv_label_create(btn_test);
    lv_label_set_text(lbl_t, LV_SYMBOL_REFRESH " SRAM");
    lv_obj_center(lbl_t);
    lv_obj_add_event_cb(btn_test, btn_sram_test_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_test, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * btn_temp = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_temp, 94, 34);
    lv_obj_align(btn_temp, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(btn_temp, lv_color_hex(0x0277BD), 0);
    lv_obj_t * lbl_tp = lv_label_create(btn_temp);
    lv_label_set_text(lbl_tp, LV_SYMBOL_REFRESH " Temp");
    lv_obj_center(lbl_tp);
    lv_obj_add_event_cb(btn_temp, btn_temp_read_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_temp, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 94, 34);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
    lv_obj_t * lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_HOME " Home");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, btn_back_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL)
    {
        lv_group_add_obj(grp, btn_test);
        lv_group_add_obj(grp, btn_temp);
        lv_group_add_obj(grp, btn_back);
    }
}

/* ==========================================================================
 * 7. Full-Screen Weather Station & 3-Day Forecast
 * ========================================================================== */

static void btn_weather_refresh_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_home_active_col = 1U;
        s_pending_screen = (int8_t)UI_SCREEN_HOME;
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    Beeper_Click();
    int32_t ext_dc = 0;
    (void)TempSensor_ReadExtDeciC(&ext_dc);
    (void)ESPCompanion_RequestWeatherUpdate();
    s_pending_screen = (int8_t)UI_SCREEN_WEATHER;
}

static void btn_widget_back_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED ||
        (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC))
    {
        Beeper_Click();
        s_home_active_col = 1U;
        s_pending_screen = (int8_t)UI_SCREEN_HOME;
    }
}

static void Build_Weather_Screen(void)
{
    uint32_t accent_hex = s_themes[s_theme_id % THEME_COUNT].title_col;
    const ESP_WeatherInfo_t *wth = ESPCompanion_GetWeather();
    bool esp_online = ESPCompanion_IsConnected();
    bool wifi_ok    = ESPCompanion_IsWifiConnected();

    /* Top Row: [< Back] [City & Condition] [Refresh] */
    lv_obj_t * btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 60, 24);
    lv_obj_set_pos(btn_back, 6, 4);
    lv_obj_t * lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, btn_widget_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, btn_widget_back_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * lbl_city = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(lbl_city, lv_color_hex(accent_hex), 0);
    lv_obj_set_pos(lbl_city, 72, 8);
    lv_obj_set_size(lbl_city, 168, 18);
    lv_label_set_long_mode(lbl_city, LV_LABEL_LONG_DOT);
    if (esp_online && wth->valid)
    {
        lv_label_set_text_fmt(lbl_city, "%s: %s", wth->city, Weather_WmoDescription(wth->wmo_code));
    }
    else if (!esp_online)
    {
        lv_label_set_text(lbl_city, "Local Sensor (No ESP32)");
    }
    else
    {
        lv_label_set_text(lbl_city, wifi_ok ? "Syncing Open-Meteo..." : "ESP32 Wi-Fi Offline");
    }

    lv_obj_t * btn_sync = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_sync, 68, 24);
    lv_obj_set_pos(btn_sync, 246, 4);
    lv_obj_set_style_bg_color(btn_sync, lv_color_hex(esp_online ? 0x0277BD : 0x37474F), 0);
    lv_obj_t * lbl_s = lv_label_create(btn_sync);
    lv_label_set_text(lbl_s, esp_online ? (LV_SYMBOL_REFRESH " Sync") : (LV_SYMBOL_REFRESH " Sens"));
    lv_obj_center(lbl_s);
    lv_obj_add_event_cb(btn_sync, btn_weather_refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_sync, btn_weather_refresh_cb, LV_EVENT_KEY, NULL);

    /* Main Current Weather Panel */
    lv_obj_t * main_card = lv_obj_create(workspace_cont);
    lv_obj_set_size(main_card, 308, 94);
    lv_obj_set_pos(main_card, 6, 32);
    lv_obj_clear_flag(main_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(main_card, lv_color_hex(0x101826), 0);
    lv_obj_set_style_border_color(main_card, lv_color_hex(accent_hex), 0);
    lv_obj_set_style_border_width(main_card, 1, 0);
    lv_obj_set_style_pad_all(main_card, 6, 0);

    Create_Weather_Graphic(main_card, 2, 14, (esp_online && wth->valid) ? wth->wmo_code : 2U, true);

    bool    ext_ok = false;
    int32_t ext_dc = TempSensor_GetCachedExtDeciC(&ext_ok);
    char    room_str[24];
    if (ext_ok)
    {
        snprintf(room_str, sizeof(room_str), "%s%ld.%ld C",
                 (ext_dc < 0) ? "-" : "+", (long)(abs(ext_dc) / 10), (long)(abs(ext_dc) % 10));
    }
    else
    {
        snprintf(room_str, sizeof(room_str), "N/A");
    }

    /* Aligned Left Column (Captions) */
    lv_obj_t * lbl_k1 = lv_label_create(main_card);
    lv_obj_set_style_text_color(lbl_k1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_k1, 68, 4);
    lv_label_set_text(lbl_k1, "Outdoor:");

    lv_obj_t * lbl_k2 = lv_label_create(main_card);
    lv_obj_set_style_text_color(lbl_k2, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_k2, 68, 26);
    lv_label_set_text(lbl_k2, "Air/Wind:");

    /* Horizontal divider separating Internet weather from Local onboard sensor */
    lv_obj_t * sep = lv_obj_create(main_card);
    lv_obj_set_size(sep, 224, 1);
    lv_obj_set_pos(sep, 68, 49);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x2A3950), 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    lv_obj_t * lbl_k3 = lv_label_create(main_card);
    lv_obj_set_style_text_color(lbl_k3, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_k3, 68, 57);
    lv_label_set_text(lbl_k3, "In-Room:");

    /* Aligned Right Column (Values) */
    lv_obj_t * lbl_v1 = lv_label_create(main_card);
    lv_label_set_recolor(lbl_v1, true);
    lv_obj_set_style_text_color(lbl_v1, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(lbl_v1, 134, 4);

    lv_obj_t * lbl_v2 = lv_label_create(main_card);
    lv_label_set_recolor(lbl_v2, true);
    lv_obj_set_style_text_color(lbl_v2, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(lbl_v2, 134, 26);

    lv_obj_t * lbl_v3 = lv_label_create(main_card);
    lv_label_set_recolor(lbl_v3, true);
    lv_obj_set_style_text_color(lbl_v3, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(lbl_v3, 134, 57);
    lv_label_set_text_fmt(lbl_v3, "#FFAE00 %s#", room_str);

    if (esp_online && wth->valid)
    {
        lv_label_set_text_fmt(lbl_v1,
            "#FFD600 %s%d.%d C# #FFFFFF (Feels %s%d.%d)#",
            (wth->temp_deci < 0) ? "-" : "+", abs(wth->temp_deci) / 10, abs(wth->temp_deci) % 10,
            (wth->feels_deci < 0) ? "-" : "+", abs(wth->feels_deci) / 10, abs(wth->feels_deci) % 10);

        lv_label_set_text_fmt(lbl_v2,
            "#00E5FF %u%%#  |  #00FF66 %u.%u km/h#",
            (unsigned int)wth->humidity,
            (unsigned int)(wth->wind_deci / 10U), (unsigned int)(wth->wind_deci % 10U));
    }
    else if (!esp_online)
    {
        lv_label_set_text(lbl_v1, "#90A4AE Companion Offline#");
        lv_label_set_text(lbl_v2, "#90A4AE Local DS18B20 Only#");
    }
    else
    {
        lv_label_set_text(lbl_v1, wifi_ok ? "#00E5FF Fetching Meteo...#" : "#90A4AE Waiting Wi-Fi...#");
        lv_label_set_text(lbl_v2, "#90A4AE Press [Sync]#");
    }

    /* 3-Day Forecast Cards (Bottom Row) */
    static const char * const s_day_titles[3] = { "Today", "Tomorrow", "Day +2" };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t * dcard = lv_obj_create(workspace_cont);
        lv_obj_set_size(dcard, 98, 80);
        lv_obj_set_pos(dcard, (int16_t)(6 + i * 105), 130);
        lv_obj_clear_flag(dcard, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(dcard, lv_color_hex(0x162032), 0);
        lv_obj_set_style_border_color(dcard, lv_color_hex(0x2A3950), 0);
        lv_obj_set_style_border_width(dcard, 1, 0);
        lv_obj_set_style_pad_all(dcard, 4, 0);

        lv_obj_t * dlbl = lv_label_create(dcard);
        lv_label_set_recolor(dlbl, true);
        lv_obj_set_style_text_color(dlbl, lv_color_hex(0xE2E8F0), 0);
        lv_obj_set_width(dlbl, 88);
        lv_obj_set_style_text_align(dlbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(dlbl);

        if (esp_online && wth->valid)
        {
            lv_label_set_text_fmt(dlbl,
                "#00E5FF %s#\n"
                "#FFFFFF %s#\n"
                "#FF8A80 %s%d.%d# / #80D8FF %s%d.%d#",
                s_day_titles[i],
                Weather_WmoDescription(wth->day_wmo[i]),
                (wth->day_hi_deci[i] < 0) ? "-" : "+", abs(wth->day_hi_deci[i]) / 10, abs(wth->day_hi_deci[i]) % 10,
                (wth->day_lo_deci[i] < 0) ? "-" : "+", abs(wth->day_lo_deci[i]) / 10, abs(wth->day_lo_deci[i]) % 10);
        }
        else
        {
            lv_label_set_text_fmt(dlbl, "#00E5FF %s#\n#90A4AE %s#\n#90A4AE -- / --#",
                                  s_day_titles[i], esp_online ? "--" : "Offline");
        }
    }

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL)
    {
        lv_group_add_obj(grp, btn_back);
        lv_group_add_obj(grp, btn_sync);
        lv_group_focus_obj(btn_sync);
    }
}

/* ==========================================================================
 * 8. Interactive Monthly Calendar Screen
 * ========================================================================== */

static void Build_Calendar_Screen(void)
{
    uint16_t yr = 2026U;
    uint8_t  mo = 11U, dy = 11U, wd = 3U;
    RTC_GetCalendarDate(&yr, &mo, &dy, &wd);

    lv_obj_t * btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 60, 24);
    lv_obj_set_pos(btn_back, 6, 4);
    lv_obj_t * lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, btn_widget_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, btn_widget_back_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * cal = lv_calendar_create(workspace_cont);
    lv_obj_set_size(cal, 242, 206);
    lv_obj_set_pos(cal, 72, 4);
    lv_calendar_set_today_date(cal, yr, mo, dy);
    lv_calendar_set_showed_date(cal, yr, mo);
    lv_obj_t * hdr = lv_calendar_header_arrow_create(cal);
    lv_obj_add_event_cb(cal, btn_widget_back_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * side_lbl = lv_label_create(workspace_cont);
    lv_label_set_recolor(side_lbl, true);
    lv_obj_set_style_text_color(side_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(side_lbl, 6, 38);
    lv_obj_set_width(side_lbl, 62);
    lv_label_set_text_fmt(side_lbl,
        "#00FF66 Today:#\n"
        "#FFFFFF %02u.%02u#\n"
        "#00E5FF %04u#\n\n"
        "#90A4AE Source:#\n"
        "%s",
        (unsigned int)dy, (unsigned int)mo, (unsigned int)yr,
        ESPCompanion_IsWifiConnected() ? "#00E5FF Wi-Fi\nNTP#" : "#FFD600 Int.\nRTC#");

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL)
    {
        /* Keep only [< Back] and header [<] [>] month buttons in group (remove 42-cell date matrix) */
        lv_obj_t * btnm = lv_calendar_get_btnmatrix(cal);
        if (btnm != NULL)
        {
            lv_group_remove_obj(btnm);
        }
        lv_group_remove_all_objs(grp);
        lv_group_add_obj(grp, btn_back);
        if (hdr != NULL)
        {
            lv_obj_t * btn_prev = lv_obj_get_child(hdr, 0);
            lv_obj_t * btn_next = lv_obj_get_child(hdr, 2);
            if (btn_prev != NULL)
            {
                lv_obj_set_style_border_color(btn_prev, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
                lv_obj_set_style_border_width(btn_prev, 2, LV_STATE_FOCUSED);
                lv_obj_add_event_cb(btn_prev, btn_widget_back_cb, LV_EVENT_KEY, NULL);
                lv_group_add_obj(grp, btn_prev);
            }
            if (btn_next != NULL)
            {
                lv_obj_set_style_border_color(btn_next, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
                lv_obj_set_style_border_width(btn_next, 2, LV_STATE_FOCUSED);
                lv_obj_add_event_cb(btn_next, btn_widget_back_cb, LV_EVENT_KEY, NULL);
                lv_group_add_obj(grp, btn_next);
            }
        }
        lv_group_focus_obj(btn_back);
    }
}

/* ==========================================================================
 * 9. Network & Companion Link SpeedTest Screen
 * ========================================================================== */

static void btn_speedtest_run_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC)
    {
        Beeper_Click();
        s_home_active_col = 1U;
        s_pending_screen = (int8_t)UI_SCREEN_HOME;
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    if (!ESPCompanion_IsConnected())
    {
        Beeper_ErrorAlert();
        return;
    }

    Beeper_Beep(25);
    (void)ESPCompanion_StartSpeedTest();
    s_pending_screen = (int8_t)UI_SCREEN_SPEEDTEST;
}

static void Build_SpeedTest_Screen(void)
{
    uint32_t accent_hex = s_themes[s_theme_id % THEME_COUNT].title_col;
    const ESP_SpeedTestInfo_t *spd = ESPCompanion_GetSpeedTest();
    bool esp_online = ESPCompanion_IsConnected();
    bool wifi_ok    = ESPCompanion_IsWifiConnected();

    lv_obj_t * btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 60, 24);
    lv_obj_set_pos(btn_back, 6, 4);
    lv_obj_t * lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, btn_widget_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, btn_widget_back_cb, LV_EVENT_KEY, NULL);

    lv_obj_t * lbl_hdr = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(lbl_hdr, lv_color_hex(accent_hex), 0);
    lv_obj_set_pos(lbl_hdr, 72, 8);
    lv_obj_set_width(lbl_hdr, 162);
    lv_label_set_long_mode(lbl_hdr, LV_LABEL_LONG_DOT);
    if (!esp_online)
    {
        lv_label_set_text(lbl_hdr, "ESP32-C6 Offline");
    }
    else if (spd->state == 1U)
    {
        lv_label_set_text(lbl_hdr, "Benchmarking...");
    }
    else if (wifi_ok)
    {
        lv_label_set_text_fmt(lbl_hdr, "%s (%ddBm)", ESPCompanion_GetWifiSsid(), (int)ESPCompanion_GetWifiRssi());
    }
    else
    {
        lv_label_set_text(lbl_hdr, "UART6 Link Only (No Wi-Fi)");
    }

    lv_obj_t * btn_run = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_run, 76, 24);
    lv_obj_set_pos(btn_run, 238, 4);
    lv_obj_set_style_bg_color(btn_run, lv_color_hex((esp_online && spd->state != 1U) ? 0x00A859 : 0x37474F), 0);
    lv_obj_t * lbl_r = lv_label_create(btn_run);
    lv_label_set_text(lbl_r, (spd->state == 1U) ? "Busy..." : (LV_SYMBOL_PLAY " Start"));
    lv_obj_center(lbl_r);
    lv_obj_add_event_cb(btn_run, btn_speedtest_run_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_run, btn_speedtest_run_cb, LV_EVENT_KEY, NULL);

    /* Metric Cards: 1) Ping Latency, 2) Wi-Fi Download, 3) USART6 Bus Speed */
    static const char * const s_titles[3] = { "Ping", "Wi-Fi DL", "UART6 Bus" };
    static const char * const s_subs[3]   = { "HTTP RTT", "HTTP Stream", "921.6k 8N1" };
    uint32_t borders[3] = { 0x00E5FFU, 0x00FF66U, 0xFFAE00U };

    for (int i = 0; i < 3; i++)
    {
        lv_obj_t * card = lv_obj_create(workspace_cont);
        lv_obj_set_size(card, 98, 92);
        lv_obj_set_pos(card, (int16_t)(6 + i * 105), 34);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x121A2B), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(borders[i]), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_pad_all(card, 4, 0);

        /* 1. Card Title (top) */
        lv_obj_t * lbl_t = lv_label_create(card);
        lv_obj_set_style_text_color(lbl_t, lv_color_hex(borders[i]), 0);
        lv_label_set_text(lbl_t, s_titles[i]);
        lv_obj_align(lbl_t, LV_ALIGN_TOP_MID, 0, 2);

        /* 2. Measured Value (center, bright high-contrast) */
        lv_obj_t * lbl_v = lv_label_create(card);
        lv_obj_set_style_text_align(lbl_v, LV_TEXT_ALIGN_CENTER, 0);

        if (i == 0)
        {
            if (spd->ping_ms > 0U)
            {
                lv_obj_set_style_text_color(lbl_v, lv_color_hex(0xFFD600), 0);
                lv_label_set_text_fmt(lbl_v, "%u ms", (unsigned int)spd->ping_ms);
            }
            else
            {
                lv_obj_set_style_text_color(lbl_v, lv_color_hex(0x90A4AE), 0);
                lv_label_set_text(lbl_v, (spd->state == 1U) ? "..." : "-- ms");
            }
        }
        else if (i == 1)
        {
            if (spd->dl_kbps > 0U)
            {
                lv_obj_set_style_text_color(lbl_v, lv_color_hex(0xFFFFFF), 0);
                lv_label_set_text_fmt(lbl_v, "%u.%u\nMbps",
                                      (unsigned int)(spd->dl_kbps / 1000U),
                                      (unsigned int)((spd->dl_kbps % 1000U) / 100U));
            }
            else
            {
                lv_obj_set_style_text_color(lbl_v, lv_color_hex(0x90A4AE), 0);
                lv_label_set_text(lbl_v, (spd->state == 1U) ? "..." : "-- Mbps");
            }
        }
        else
        {
            if (spd->link_kbs_x10 > 0U)
            {
                lv_obj_set_style_text_color(lbl_v, lv_color_hex(0xFFFFFF), 0);
                lv_label_set_text_fmt(lbl_v, "%u.%u\nKB/s",
                                      (unsigned int)(spd->link_kbs_x10 / 10U),
                                      (unsigned int)(spd->link_kbs_x10 % 10U));
            }
            else
            {
                lv_obj_set_style_text_color(lbl_v, lv_color_hex(0x90A4AE), 0);
                lv_label_set_text(lbl_v, (spd->state == 1U) ? "..." : "-- KB/s");
            }
        }
        lv_obj_align(lbl_v, LV_ALIGN_CENTER, 0, 2);

        /* 3. Card Subtitle (bottom) */
        lv_obj_t * lbl_sub = lv_label_create(card);
        lv_obj_set_style_text_color(lbl_sub, lv_color_hex(0x90A4AE), 0);
        lv_label_set_text(lbl_sub, s_subs[i]);
        lv_obj_align(lbl_sub, LV_ALIGN_BOTTOM_MID, 0, -1);
    }

    /* Bottom Summary / Progress Bar Panel */
    lv_obj_t * sum = lv_obj_create(workspace_cont);
    lv_obj_set_size(sum, 308, 78);
    lv_obj_set_pos(sum, 6, 132);
    lv_obj_clear_flag(sum, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sum, lv_color_hex(0x101624), 0);
    lv_obj_set_style_border_color(sum, lv_color_hex(0x2A3950), 0);
    lv_obj_set_style_border_width(sum, 1, 0);
    lv_obj_set_style_pad_all(sum, 6, 0);

    lv_obj_t * bar = lv_bar_create(sum);
    lv_obj_set_size(bar, 292, 14);
    lv_obj_set_pos(bar, 0, 2);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x00FF66), LV_PART_INDICATOR);

    int32_t pct = 0;
    if (spd->state == 1U)
    {
        pct = (spd->dl_kbps > 0U) ? 75 : ((spd->ping_ms > 0U) ? 35 : 15);
    }
    else if (spd->state == 2U)
    {
        pct = (int32_t)(spd->dl_kbps / 400U); /* 40 Mbps = 100% scale on 2.4 GHz ESP32-C6 */
        if (pct < 12 && spd->link_kbs_x10 > 0U) pct = 25;
        if (pct > 100) pct = 100;
    }
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);

    lv_obj_t * slbl = lv_label_create(sum);
    lv_label_set_recolor(slbl, true);
    lv_obj_set_style_text_color(slbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(slbl, 0, 22);
    lv_obj_set_width(slbl, 292);

    if (!esp_online)
    {
        lv_label_set_text(slbl,
            "#FF5555 Companion link inactive.#\n"
            "#90A4AE Connect ESP32-C6 to P13 (PC6/PC7 @ 921600).#");
    }
    else if (spd->state == 1U)
    {
        lv_label_set_text(slbl,
            "#00E5FF Running 3-stage benchmark on ESP32-C6...#\n"
            "#90A4AE 1) HTTP Ping  2) 512KB Wi-Fi DL  3) 32KB UART6#");
    }
    else if (spd->state == 2U)
    {
        lv_label_set_text_fmt(slbl,
            "#00FF66 Benchmark Complete!# IP: #00E5FF %s#\n"
            "#90A4AE UART6 theoretical max: 90.0 KB/s (8N1 framed)#",
            ESPCompanion_GetWifiIp());
    }
    else
    {
        lv_label_set_text_fmt(slbl,
            "#FFFFFF Ready:# Press #00FF66 [Start]# to test Wi-Fi & UART6.\n"
            "#90A4AE Host: http://stm32-console.local (%s)#",
            ESPCompanion_GetWifiIp());
    }

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL)
    {
        lv_group_add_obj(grp, btn_run);
        lv_group_add_obj(grp, btn_back);
        lv_group_focus_obj(btn_run);
    }
}
