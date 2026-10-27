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
#include <string.h>
#include "ccmram.h"
#include "app_loader.h"

extern RTC_HandleTypeDef hrtc;

/* EEPROM Settings Magic & Address */
#define EEPROM_CFG_ADDR   0x00U
#define EEPROM_CFG_MAGIC  0xA5U

typedef struct {
    uint8_t magic;
    uint8_t sound_muted;
} System_Config_t;

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
static lv_obj_t * file_list_obj    = NULL;
static char s_clicked_item[STORAGE_MAX_NAME_LEN];
static char s_next_path[STORAGE_MAX_PATH_LEN];
static char s_title_buf[STORAGE_MAX_PATH_LEN + 16U];
#define EXT_TEXT_PREVIEW_BUF  ((char *)MM_EXT_TEXT_PREVIEW)
#define EXT_TEXT_PREVIEW_SIZE MM_EXT_TEXT_PREVIEW_SIZE

/* Settings Screen RTC adjustment state */
static uint8_t edit_hours   = 12;
static uint8_t edit_minutes = 0;
static lv_obj_t * lbl_time_set = NULL;

/* Wallpaper selected from the image viewer, persisted on the SD card */
#define WALLPAPER_CFG_PATH  "system/wallpaper.cfg"
static char s_wallpaper_path[STORAGE_MAX_PATH_LEN];
static char s_preview_path[STORAGE_MAX_PATH_LEN];

/* Games launcher: built-in ROM game + games/<name>.bin tiles with optional games/<name>.bmp icons */
#define GAMES_MAX_TILES       16U
#define GAME_ICON_SIZE        64
#define GAME_ICON_SLOT_BYTES  ((uint32_t)GAME_ICON_SIZE * (uint32_t)GAME_ICON_SIZE * 2UL)
_Static_assert(GAMES_MAX_TILES * GAME_ICON_SLOT_BYTES <= MM_EXT_IMAGE_BUF_SIZE, "Game icons exceed the ext SRAM image buffer");

typedef struct {
    char file[STORAGE_MAX_NAME_LEN];
    bool builtin;
} Game_Entry_t;

static lv_obj_t *    games_grid_obj = NULL;
static Game_Entry_t  s_games[GAMES_MAX_TILES];
static uint32_t      s_games_count  = 0;
static lv_img_dsc_t  s_icon_dsc[GAMES_MAX_TILES];

extern const uint8_t g_rom_arkanoid_bin[];
extern const uint8_t g_rom_arkanoid_bin_end[];

/* Forward declarations */
static void Build_Home_Screen(void);
static void Build_Files_Screen(void);
static void Build_Games_Screen(void);
static void Build_Settings_Screen(void);
static void Build_SysInfo_Screen(void);
static void Populate_File_List_From_Cache(void);
static void Populate_Games_Grid(void);

/* ==========================================================================
 * EEPROM Settings Load / Save
 * ========================================================================== */

static void Settings_LoadFromEEPROM(void)
{
    System_Config_t cfg = {0};
    if (AT24Cxx_Read(EEPROM_CFG_ADDR, &cfg, sizeof(cfg)) == HAL_OK)
    {
        if (cfg.magic == EEPROM_CFG_MAGIC)
        {
            Beeper_SetMuted(cfg.sound_muted != 0U);
            printf("[CFG] Loaded from EEPROM: Sound Muted = %u\r\n", cfg.sound_muted);
        }
    }
}

static void Settings_SaveToEEPROM(void)
{
    System_Config_t cfg = {
        .magic       = EEPROM_CFG_MAGIC,
        .sound_muted = Beeper_IsMuted() ? 1U : 0U
    };
    (void)AT24Cxx_Write(EEPROM_CFG_ADDR, &cfg, sizeof(cfg));
}

/* ==========================================================================
 * Universal 16/24/32-bit BMP Loader into External SRAM (nearest-neighbour downscale)
 * ========================================================================== */

static lv_img_dsc_t s_custom_bmp_dsc;
#define s_bmp_line_buf ((uint8_t *)MM_EXT_BMP_LINE_BUF)

/* dst_fb must hold max_w * max_h RGB565 pixels */
static bool Load_BMP_To_Buffer(const char *fatfs_path, uint16_t *dst_fb, int32_t max_w, int32_t max_h,
                               lv_img_dsc_t *out_dsc)
{
    static FIL bmp_file;
    UINT br = 0;
    uint8_t hdr[54];

    if (f_open(&bmp_file, fatfs_path, FA_READ) != FR_OK)
    {
        printf("[IMG] Cannot open file: %s\r\n", fatfs_path);
        return false;
    }

    if (f_read(&bmp_file, hdr, 54, &br) != FR_OK || br < 54 || hdr[0] != 'B' || hdr[1] != 'M')
    {
        printf("[IMG] Invalid BMP header: %s\r\n", fatfs_path);
        f_close(&bmp_file);
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
        f_close(&bmp_file);
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

        if (f_lseek(&bmp_file, file_pos) != FR_OK) break;
        if (f_read(&bmp_file, s_bmp_line_buf, row_bytes, &br) != FR_OK) break;

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

    f_close(&bmp_file);

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
    static FIL cfg_file;
    UINT br = 0;

    s_wallpaper_path[0] = '\0';
    if (f_open(&cfg_file, WALLPAPER_CFG_PATH, FA_READ) != FR_OK)
    {
        return;
    }
    if (f_read(&cfg_file, s_wallpaper_path, sizeof(s_wallpaper_path) - 1U, &br) != FR_OK)
    {
        br = 0;
    }
    f_close(&cfg_file);

    s_wallpaper_path[br] = '\0';
    while (br > 0U && (s_wallpaper_path[br - 1U] == '\r' || s_wallpaper_path[br - 1U] == '\n' ||
                       s_wallpaper_path[br - 1U] == ' '))
    {
        s_wallpaper_path[--br] = '\0';
    }
}

static bool Wallpaper_SaveConfig(const char *path)
{
    static FIL cfg_file;
    UINT bw = 0;
    UINT len = (UINT)strlen(path);

    if (f_open(&cfg_file, WALLPAPER_CFG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return false;
    }
    FRESULT res = f_write(&cfg_file, path, len, &bw);
    f_close(&cfg_file);
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
    lv_obj_align(lbl_status_icons, LV_ALIGN_CENTER, 10, 0);
    lv_label_set_text(lbl_status_icons, "#555555 SD K M G#");

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
    lv_obj_set_style_bg_color(workspace_cont, lv_color_hex(0x1B2234), 0);

    /* 3. Show Home Screen by default */
    UIManager_ShowScreen(UI_SCREEN_HOME);
}

void UIManager_Update(void)
{
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

    lv_label_set_text_fmt(lbl_status_icons, "%s %s %s %s", c_sd, c_kbd, c_mse, c_pad);
}

void UIManager_ShowScreen(UI_Screen_t screen)
{
    if (workspace_cont == NULL) return;

    current_screen = screen;
    file_list_obj  = NULL;
    games_grid_obj = NULL;
    lbl_time_set   = NULL;

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL)
    {
        lv_group_remove_all_objs(grp);
    }

    lv_obj_clean(workspace_cont);

    switch (screen)
    {
        case UI_SCREEN_HOME:
            lv_label_set_text(lbl_screen_title, "STM32 OS");
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

        default:
            Build_Home_Screen();
            break;
    }
}

/* ==========================================================================
 * 2. Home Screen Builder
 * ========================================================================== */

static void home_icon_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        UI_Screen_t target = (UI_Screen_t)(uintptr_t)lv_event_get_user_data(e);
        Beeper_Click();
        UIManager_ShowScreen(target);
    }
}

static void Create_Desktop_Shortcut(lv_obj_t *parent, int16_t x, int16_t y,
                                    const char *symbol, const char *title,
                                    uint32_t accent_hex, UI_Screen_t target_screen)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 136, 84);
    lv_obj_set_pos(btn, x, y);
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

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL)
    {
        lv_group_add_obj(grp, btn);
    }
}

static bool Home_TryWallpaper(const char *path)
{
    static FILINFO fno;

    if (path == NULL || path[0] == '\0' || f_stat(path, &fno) != FR_OK)
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

    Create_Desktop_Shortcut(workspace_cont, 16,  16, LV_SYMBOL_DIRECTORY, "SD Files",    0x00E5FF, UI_SCREEN_FILES);
    Create_Desktop_Shortcut(workspace_cont, 168, 16, LV_SYMBOL_PLAY,      "Games",       0x00FF66, UI_SCREEN_GAMES);
    Create_Desktop_Shortcut(workspace_cont, 16,  114, LV_SYMBOL_SETTINGS, "Settings",    0xFFAE00, UI_SCREEN_SETTINGS);
    Create_Desktop_Shortcut(workspace_cont, 168, 114, LV_SYMBOL_LIST,     "System Info", 0xB388FF, UI_SCREEN_SYSINFO);
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

static void Open_Text_Preview(const char *fatfs_path)
{
    static FIL txt_file;
    UINT br = 0;
    const char *clean_path = (fatfs_path[0] == '/') ? (fatfs_path + 1) : fatfs_path;

    memset(EXT_TEXT_PREVIEW_BUF, 0, EXT_TEXT_PREVIEW_SIZE);
    if (f_open(&txt_file, clean_path, FA_READ) == FR_OK)
    {
        (void)f_read(&txt_file, EXT_TEXT_PREVIEW_BUF, EXT_TEXT_PREVIEW_SIZE - 1U, &br);
        f_close(&txt_file);
    }
    else
    {
        snprintf(EXT_TEXT_PREVIEW_BUF, EXT_TEXT_PREVIEW_SIZE, "Error opening file:\n%s", clean_path);
    }

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    file_list_obj = NULL;
    lv_obj_clean(workspace_cont);

    /* Top bar with Back button and filename */
    lv_obj_t * btn_close = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_close, 70, 26);
    lv_obj_align(btn_close, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_t * lbl = lv_label_create(btn_close);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl);

    lv_obj_t * title_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x00E5FF), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 84, 10);
    lv_label_set_text(title_lbl, clean_path);

    /* Scrollable Text Reader Panel */
    lv_obj_t * panel = lv_obj_create(workspace_cont);
    lv_obj_set_size(panel, 308, 172);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0E131D), 0);
    lv_obj_set_style_pad_all(panel, 8, 0);

    lv_obj_t * txt_lbl = lv_label_create(panel);
    lv_obj_set_width(txt_lbl, 284);
    lv_label_set_long_mode(txt_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(txt_lbl, lv_color_hex(0xE0E0E0), 0);
    lv_label_set_text(txt_lbl, EXT_TEXT_PREVIEW_BUF);

    lv_obj_add_event_cb(btn_close, close_preview_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_close, close_preview_cb, LV_EVENT_KEY, NULL);
    if (grp != NULL) lv_group_add_obj(grp, btn_close);
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

        if (strcmp(s_clicked_item, ".. (Root)") == 0)
        {
            OSTasks_RequestDirList("/");
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

        /* Build relative path for file viewers */
        if (strcmp(g_dir_cache.path, "/") == 0)
        {
            snprintf(s_next_path, sizeof(s_next_path), "%s", s_clicked_item);
        }
        else
        {
            const char *sub = (g_dir_cache.path[0] == '/') ? (g_dir_cache.path + 1) : g_dir_cache.path;
            snprintf(s_next_path, sizeof(s_next_path), "%.60s/%.50s", sub, s_clicked_item);
        }

        /* 1. Image files (.bmp, .png, .jpg) */
        if (Ends_With_IgnoreCase(s_clicked_item, ".bmp") ||
            Ends_With_IgnoreCase(s_clicked_item, ".png") ||
            Ends_With_IgnoreCase(s_clicked_item, ".jpg") ||
            Ends_With_IgnoreCase(s_clicked_item, ".sjpg"))
        {
            Open_Image_Preview(s_next_path);
        }
        /* 2. Text files (.txt, .md, .log, .ini) */
        else if (Ends_With_IgnoreCase(s_clicked_item, ".txt") ||
                 Ends_With_IgnoreCase(s_clicked_item, ".md")  ||
                 Ends_With_IgnoreCase(s_clicked_item, ".log") ||
                 Ends_With_IgnoreCase(s_clicked_item, ".ini"))
        {
            Open_Text_Preview(s_next_path);
        }
        /* 3. Native Binary Apps (.bin) */
        else if (Ends_With_IgnoreCase(s_clicked_item, ".bin"))
        {
            (void)AppLoader_LoadAndRun(s_next_path);
        }
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

    snprintf(s_title_buf, sizeof(s_title_buf), "Folder: %s", g_dir_cache.path);
    lv_list_add_text(file_list_obj, s_title_buf);

    if (!g_dir_cache.io_success)
    {
        lv_list_add_text(file_list_obj, "SD Card Error / Not Mounted");
        return;
    }

    if (strcmp(g_dir_cache.path, "/") != 0)
    {
        lv_obj_t * up_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_UP, ".. (Root)");
        lv_obj_add_event_cb(up_btn, file_list_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(up_btn, file_list_btn_event_cb, LV_EVENT_KEY, NULL);
        if (grp != NULL) lv_group_add_obj(grp, up_btn);
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
    if (s_games[idx].builtin)
    {
        (void)AppLoader_LoadAndRunImage(g_rom_arkanoid_bin,
                                        (uint32_t)(g_rom_arkanoid_bin_end - g_rom_arkanoid_bin),
                                        s_games[idx].file);
    }
    else
    {
        snprintf(s_next_path, sizeof(s_next_path), "games/%.60s", s_games[idx].file);
        (void)AppLoader_LoadAndRun(s_next_path);
    }
}

/* Looks for "<base>.bmp" next to the game in the cached /games listing */
static bool Games_FindIcon(const char *bin_name, char *out_path, size_t out_size)
{
    size_t base_len = strlen(bin_name);
    if (base_len <= 4U)
    {
        return false;
    }
    base_len -= 4U; /* strip ".bin" */

    for (uint32_t i = 0; i < g_dir_cache.count; i++)
    {
        const char *name = g_dir_cache.items[i].name;
        if (!g_dir_cache.items[i].is_dir && strlen(name) == base_len + 4U &&
            strncmp(name, bin_name, base_len) == 0 && Ends_With_IgnoreCase(name, ".bmp"))
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

static void Populate_Games_Grid(void)
{
    if (games_grid_obj == NULL) return;

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    lv_obj_clean(games_grid_obj);
    s_games_count = 0;

    /* 1. Built-in game (works without an SD card) */
    strncpy(s_games[0].file, "Arkanoid", sizeof(s_games[0].file) - 1U);
    s_games[0].file[sizeof(s_games[0].file) - 1U] = '\0';
    s_games[0].builtin = true;
    s_games_count = 1;
    Games_AddTile("Arkanoid", NULL, 0x00FF66);

    /* 2. SD card games */
    static char title[STORAGE_MAX_NAME_LEN];
    static char icon_path[STORAGE_MAX_PATH_LEN];
    bool games_listed = g_dir_cache.io_success && (strcmp(g_dir_cache.path, "/games") == 0);

    for (uint32_t i = 0; games_listed && i < g_dir_cache.count && s_games_count < GAMES_MAX_TILES; i++)
    {
        const char *fname = g_dir_cache.items[i].name;
        if (g_dir_cache.items[i].is_dir || !Ends_With_IgnoreCase(fname, ".bin"))
        {
            continue;
        }

        Game_Entry_t *g = &s_games[s_games_count];
        strncpy(g->file, fname, sizeof(g->file) - 1U);
        g->file[sizeof(g->file) - 1U] = '\0';
        g->builtin = false;
        s_games_count++;

        strncpy(title, fname, sizeof(title) - 1U);
        title[sizeof(title) - 1U] = '\0';
        size_t len = strlen(title);
        if (len > 4U) title[len - 4U] = '\0';

        bool has_icon = Games_FindIcon(fname, icon_path, sizeof(icon_path));
        Games_AddTile(title, has_icon ? icon_path : NULL, 0xFFAE00);
    }

    if (!SDCard_IsMounted())
    {
        lv_obj_t * lbl = lv_label_create(games_grid_obj);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
        lv_label_set_text(lbl, "No SD card: built-in games only");
    }

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
 * 5. Settings Screen (RTC Time Setup + Audio Mute in EEPROM)
 * ========================================================================== */

static void rtc_adjust_btn_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
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

        if (lbl_time_set != NULL)
        {
            lv_label_set_text_fmt(lbl_time_set, "Set RTC: %02u : %02u", edit_hours, edit_minutes);
        }
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

static lv_obj_t* Create_Setting_Btn(lv_obj_t *parent, int16_t x, int16_t y, int16_t w,
                                    const char *txt, intptr_t action)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, 32);
    lv_obj_set_pos(btn, x, y);
    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, rtc_adjust_btn_cb, LV_EVENT_CLICKED, (void *)action);
    lv_obj_add_event_cb(btn, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_add_obj(grp, btn);
    return btn;
}

static void Build_Settings_Screen(void)
{
    uint8_t sec_dummy = 0;
    RTC_GetDateTime(&edit_hours, &edit_minutes, &sec_dummy);

    lbl_time_set = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(lbl_time_set, lv_color_hex(0x00FF66), 0);
    lv_obj_align(lbl_time_set, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text_fmt(lbl_time_set, "Set RTC: %02u : %02u", edit_hours, edit_minutes);

    Create_Setting_Btn(workspace_cont, 20,  34, 60, "Hr +", 1);
    Create_Setting_Btn(workspace_cont, 90,  34, 60, "Hr -", 2);
    Create_Setting_Btn(workspace_cont, 170, 34, 60, "Min +", 3);
    Create_Setting_Btn(workspace_cont, 240, 34, 60, "Min -", 4);

    Create_Setting_Btn(workspace_cont, 80, 74, 160, LV_SYMBOL_OK " Save Time", 5);

    /* Audio Mute Switch (Persisted in AT24C02 EEPROM) */
    lv_obj_t * snd_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(snd_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(snd_lbl, 45, 122);
    lv_label_set_text(snd_lbl, LV_SYMBOL_AUDIO " Beeper Sound:");

    lv_obj_t * snd_sw = lv_switch_create(workspace_cont);
    lv_obj_set_pos(snd_sw, 200, 118);
    if (!Beeper_IsMuted())
    {
        lv_obj_add_state(snd_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(snd_sw, sound_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(snd_sw, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_add_obj(grp, snd_sw);

    /* Back to Home button */
    lv_obj_t * btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 120, 32);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -10);
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

static void Build_SysInfo_Screen(void)
{
    const POST_Report_t *rep = POST_GetReport();
    uint32_t total_mb = 0, free_mb = 0;
    (void)SDCard_GetFreeSpace(&total_mb, &free_mb);

    lv_obj_t * panel = lv_obj_create(workspace_cont);
    lv_obj_set_size(panel, 296, 145);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101520), 0);

    lv_obj_t * info_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(info_lbl, lv_color_hex(0xE0E0E0), 0);
    lv_label_set_text_fmt(info_lbl,
        "MCU: STM32F407ZGT6 @ 168 MHz\n"
        "FSMC SRAM: %lu KB (%s)\n"
        "CCM RAM: 64 KB (%s)\n"
        "SPI Flash W25Q: ID 0x%06X\n"
        "EEPROM AT24C02: %s\n"
        "SD Card: %lu MB Free / %lu MB",
        (unsigned long)rep->sram_size_kb, rep->sram_ok ? "OK" : "ERR",
        rep->ccmram_ok ? "OK" : "ERR",
        (unsigned int)rep->flash_id,
        rep->eeprom_ok ? "OK" : "Missing",
        (unsigned long)free_mb, (unsigned long)total_mb);

    lv_obj_t * btn_back = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_back, 120, 34);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t * lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_HOME " Home");
    lv_obj_center(lbl_b);
    lv_obj_add_event_cb(btn_back, btn_back_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_add_obj(grp, btn_back);
}
