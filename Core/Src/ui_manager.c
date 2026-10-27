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

/* Cyber-Pong Mini-Game State */
static lv_timer_t * game_timer     = NULL;
static lv_obj_t   * game_paddle    = NULL;
static lv_obj_t   * game_ball      = NULL;
static lv_obj_t   * game_score_lbl = NULL;
static int16_t      paddle_x       = 120;
static int16_t      ball_x         = 160;
static int16_t      ball_y         = 80;
static int16_t      ball_vx        = 3;
static int16_t      ball_vy        = 3;
static uint32_t     game_score     = 0;
static uint32_t     game_best      = 0;

/* Forward declarations */
static void Build_Home_Screen(void);
static void Build_Files_Screen(void);
static void Build_Games_Screen(void);
static void Build_Settings_Screen(void);
static void Build_SysInfo_Screen(void);
static void Populate_File_List_From_Cache(void);
static void Build_CyberPong_Screen(void);
static void Populate_Games_List_From_Cache(void);

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
 * Universal 16/24/32-bit BMP Loader into External SRAM (MM_EXT_IMAGE_BUF)
 * ========================================================================== */

static lv_img_dsc_t s_custom_bmp_dsc;
#define s_bmp_line_buf ((uint8_t *)MM_EXT_BMP_LINE_BUF)

static bool Load_BMP_To_ExtSRAM(const char *fatfs_path, lv_img_dsc_t *out_dsc)
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

    int32_t dst_w = (src_w > 320) ? 320 : src_w;
    int32_t dst_h = (src_h > 216) ? 216 : src_h;

    uint16_t *dst_fb = (uint16_t *)MM_EXT_IMAGE_BUF;
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
        if (Load_BMP_To_ExtSRAM(clean_path, &s_custom_bmp_dsc))
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

    /* Stop active internal game timer if leaving Cyber-Pong screen */
    if (game_timer != NULL)
    {
        lv_timer_del(game_timer);
        game_timer = NULL;
    }

    current_screen = screen;
    file_list_obj  = NULL;
    lbl_time_set   = NULL;
    game_paddle    = NULL;
    game_ball      = NULL;
    game_score_lbl = NULL;

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
            lv_label_set_text(lbl_screen_title, "Games Hub");
            Build_Games_Screen();
            break;

        case UI_SCREEN_CYBER_PONG:
            lv_label_set_text(lbl_screen_title, "ROM: Cyber-Pong");
            Build_CyberPong_Screen();
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

static void Build_Home_Screen(void)
{
    static const char * const wall_candidates[] = {
        "assets/wall.bmp",
        "wall.bmp",
        "assets/wall.png",
        "assets/wall.jpg"
    };

    static FILINFO fno;
    if (SDCard_IsMounted())
    {
        for (size_t i = 0; i < sizeof(wall_candidates) / sizeof(wall_candidates[0]); i++)
        {
            if (f_stat(wall_candidates[i], &fno) == FR_OK)
            {
                lv_obj_t * bg_img = lv_img_create(workspace_cont);
                if (Set_Image_From_SD(bg_img, wall_candidates[i]))
                {
                    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
                    break;
                }
                else
                {
                    lv_obj_del(bg_img);
                }
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

static void Open_Image_Preview(const char *fatfs_path)
{
    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    file_list_obj = NULL;
    lv_obj_clean(workspace_cont);

    lv_obj_t * img = lv_img_create(workspace_cont);
    if (Set_Image_From_SD(img, fatfs_path))
    {
        lv_obj_center(img);
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
    else if (current_screen == UI_SCREEN_GAMES && file_list_obj != NULL)
    {
        Populate_Games_List_From_Cache();
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
 * 4A. Games Hub Menu (Internal ROM + External .bin + Script .c)
 * ========================================================================== */

static void games_hub_btn_event_cb(lv_event_t * e)
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
        const char * txt = lv_list_get_btn_text(file_list_obj, obj);
        if (txt == NULL) return;

        Beeper_Click();

        if (strcmp(txt, "[ Home Screen ]") == 0)
        {
            UIManager_ShowScreen(UI_SCREEN_HOME);
            return;
        }

        if (strcmp(txt, "Internal: Cyber-Pong") == 0)
        {
            UIManager_ShowScreen(UI_SCREEN_CYBER_PONG);
            return;
        }

        /* External: <filename.bin> */
        if (strncmp(txt, "External: ", 10) == 0)
        {
            const char *fname = txt + 10;
            snprintf(s_next_path, sizeof(s_next_path), "games/%.60s", fname);
            (void)AppLoader_LoadAndRun(s_next_path);
            return;
        }
    }
}

static void Populate_Games_List_From_Cache(void)
{
    if (file_list_obj == NULL) return;

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_remove_all_objs(grp);

    lv_obj_clean(file_list_obj);

    /* 1. Back to Home Screen */
    lv_obj_t * home_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_HOME, "[ Home Screen ]");
    lv_obj_add_event_cb(home_btn, games_hub_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(home_btn, games_hub_btn_event_cb, LV_EVENT_KEY, NULL);
    if (grp != NULL) lv_group_add_obj(grp, home_btn);

    /* 2. Built-in Firmware Games */
    lv_list_add_text(file_list_obj, "Built-in Firmware Games:");
    lv_obj_t * rom_btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_PLAY, "Internal: Cyber-Pong");
    lv_obj_set_style_text_color(rom_btn, lv_color_hex(0x00FF66), 0);
    lv_obj_add_event_cb(rom_btn, games_hub_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(rom_btn, games_hub_btn_event_cb, LV_EVENT_KEY, NULL);
    if (grp != NULL) lv_group_add_obj(grp, rom_btn);

    /* 3. SD Card Games (/games folder) */
    lv_list_add_text(file_list_obj, "SD Card (/games):");

    uint32_t ext_count = 0;
    if (g_dir_cache.io_success)
    {
        for (uint32_t i = 0; i < g_dir_cache.count; i++)
        {
            if (g_dir_cache.items[i].is_dir) continue;

            const char *fname = g_dir_cache.items[i].name;
            if (Ends_With_IgnoreCase(fname, ".bin"))
            {
                snprintf(s_title_buf, sizeof(s_title_buf), "External: %.50s", fname);
                lv_obj_t * btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_CHARGE, s_title_buf);
                lv_obj_set_style_text_color(btn, lv_color_hex(0xFFAE00), 0);
                lv_obj_add_event_cb(btn, games_hub_btn_event_cb, LV_EVENT_CLICKED, NULL);
                lv_obj_add_event_cb(btn, games_hub_btn_event_cb, LV_EVENT_KEY, NULL);
                if (grp != NULL) lv_group_add_obj(grp, btn);
                ext_count++;
            }
            else if (Ends_With_IgnoreCase(fname, ".c"))
            {
                snprintf(s_title_buf, sizeof(s_title_buf), "Script: %.50s", fname);
                lv_obj_t * btn = lv_list_add_btn(file_list_obj, LV_SYMBOL_EDIT, s_title_buf);
                lv_obj_set_style_text_color(btn, lv_color_hex(0x00E5FF), 0);
                lv_obj_add_event_cb(btn, games_hub_btn_event_cb, LV_EVENT_CLICKED, NULL);
                lv_obj_add_event_cb(btn, games_hub_btn_event_cb, LV_EVENT_KEY, NULL);
                if (grp != NULL) lv_group_add_obj(grp, btn);
                ext_count++;
            }
        }
    }

    if (ext_count == 0)
    {
        lv_list_add_text(file_list_obj, "No .bin or .c files in /games");
    }
}

static void Build_Games_Screen(void)
{
    file_list_obj = lv_list_create(workspace_cont);
    lv_obj_set_size(file_list_obj, 308, 204);
    lv_obj_align(file_list_obj, LV_ALIGN_CENTER, 0, 0);

    Populate_Games_List_From_Cache();
    OSTasks_RequestDirList("/games");
}

/* ==========================================================================
 * 4. Built-in Arcade Game: Cyber-Pong (Gamepad / Touch / Mouse / Keys)
 * ========================================================================== */

static void game_loop_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (current_screen != UI_SCREEN_CYBER_PONG || game_paddle == NULL || game_ball == NULL)
    {
        return;
    }

    /* 1. Read USB Gamepad D-Pad */
    USB_GamepadState_t pad;
    if (USBHostManager_GetGamepadState(&pad))
    {
        if (pad.dpad_left || pad.btn_l1)  paddle_x -= 6;
        if (pad.dpad_right || pad.btn_r1) paddle_x += 6;
        if (pad.btn_b)
        {
            UIManager_ShowScreen(UI_SCREEN_HOME);
            return;
        }
    }

    /* 2. Read Physical Board Buttons KEY_0 (Left) & KEY_2 (Right) */
    if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) paddle_x -= 5;
    if (HAL_GPIO_ReadPin(KEY_2_GPIO_Port, KEY_2_Pin) == GPIO_PIN_RESET) paddle_x += 5;

    /* 3. Follow Touch / Mouse X if pressed */
    lv_indev_t * indev = lv_indev_get_act();
    lv_point_t pt;
    if (indev != NULL)
    {
        lv_indev_get_point(indev, &pt);
        if (pt.y > 60 && (pt.x > 0 && pt.x < 320))
        {
            /* Smoothly move towards pointer if mouse/touch active */
        }
    }

    if (paddle_x < 4)   paddle_x = 4;
    if (paddle_x > 246) paddle_x = 246;
    lv_obj_set_pos(game_paddle, paddle_x, 192);

    /* 4. Update Ball Physics */
    ball_x += ball_vx;
    ball_y += ball_vy;

    /* Bounce Left / Right walls */
    if (ball_x <= 4)
    {
        ball_x  = 4;
        ball_vx = -ball_vx;
        Beeper_Beep(8);
    }
    else if (ball_x >= 304)
    {
        ball_x  = 304;
        ball_vx = -ball_vx;
        Beeper_Beep(8);
    }

    /* Bounce Top ceiling */
    if (ball_y <= 34)
    {
        ball_y  = 34;
        ball_vy = -ball_vy;
        Beeper_Beep(8);
    }

    /* Check Paddle Collision */
    if (ball_vy > 0 && ball_y >= 180 && ball_y <= 194)
    {
        if (ball_x + 12 >= paddle_x && ball_x <= paddle_x + 70)
        {
            ball_y  = 179;
            ball_vy = -ball_vy;
            game_score++;
            if (game_score > game_best) game_best = game_score;
            Beeper_Click();

            if (game_score_lbl != NULL)
            {
                lv_label_set_text_fmt(game_score_lbl, "Score: %lu   Best: %lu",
                                      (unsigned long)game_score, (unsigned long)game_best);
            }
        }
    }

    /* Ball missed paddle -> Reset round */
    if (ball_y > 210)
    {
        Beeper_Beep(60);
        game_score = 0;
        ball_x     = 154;
        ball_y     = 60;
        ball_vy    = 3;
        if (game_score_lbl != NULL)
        {
            lv_label_set_text_fmt(game_score_lbl, "Score: %lu   Best: %lu",
                                  (unsigned long)game_score, (unsigned long)game_best);
        }
    }

    lv_obj_set_pos(game_ball, ball_x, ball_y);
}

static void game_touch_move_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSING)
    {
        lv_indev_t * indev = lv_indev_get_act();
        if (indev != NULL)
        {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            paddle_x = p.x - 35;
        }
    }
    else if (lv_event_get_code(e) == LV_EVENT_KEY)
    {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_ESC)
        {
            UIManager_ShowScreen(UI_SCREEN_GAMES);
        }
    }
}

static void Build_CyberPong_Screen(void)
{
    game_score = 0;
    paddle_x   = 125;
    ball_x     = 154;
    ball_y     = 60;
    ball_vx    = 3;
    ball_vy    = 3;

    /* Touch area on workspace to drag paddle with finger or mouse */
    lv_obj_add_event_cb(workspace_cont, game_touch_move_cb, LV_EVENT_PRESSING, NULL);

    /* Top bar: Back button & Score */
    lv_obj_t * btn_exit = lv_btn_create(workspace_cont);
    lv_obj_set_size(btn_exit, 64, 24);
    lv_obj_align(btn_exit, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_t * lbl = lv_label_create(btn_exit);
    lv_label_set_text(lbl, LV_SYMBOL_HOME " Exit");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn_exit, btn_back_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_exit, global_key_event_cb, LV_EVENT_KEY, NULL);

    lv_group_t * grp = lv_group_get_default();
    if (grp != NULL) lv_group_add_obj(grp, btn_exit);

    game_score_lbl = lv_label_create(workspace_cont);
    lv_obj_set_style_text_color(game_score_lbl, lv_color_hex(0x00FF66), 0);
    lv_obj_align(game_score_lbl, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_label_set_text_fmt(game_score_lbl, "Score: 0   Best: %lu", (unsigned long)game_best);

    /* Neon Paddle */
    game_paddle = lv_obj_create(workspace_cont);
    lv_obj_set_size(game_paddle, 70, 10);
    lv_obj_set_pos(game_paddle, paddle_x, 192);
    lv_obj_set_style_bg_color(game_paddle, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(game_paddle, 0, 0);
    lv_obj_set_style_radius(game_paddle, 4, 0);
    lv_obj_clear_flag(game_paddle, LV_OBJ_FLAG_SCROLLABLE);

    /* Neon Ball */
    game_ball = lv_obj_create(workspace_cont);
    lv_obj_set_size(game_ball, 12, 12);
    lv_obj_set_pos(game_ball, ball_x, ball_y);
    lv_obj_set_style_bg_color(game_ball, lv_color_hex(0xFFAE00), 0);
    lv_obj_set_style_border_width(game_ball, 0, 0);
    lv_obj_set_style_radius(game_ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(game_ball, LV_OBJ_FLAG_SCROLLABLE);

    /* Start 50 Hz Game Loop Timer */
    game_timer = lv_timer_create(game_loop_timer_cb, 20, NULL);
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
        "MCU: STM32F407ZET6 @ 168 MHz\n"
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
