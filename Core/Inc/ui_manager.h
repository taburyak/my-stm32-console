#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Console UI Screen Identifiers
 */
typedef enum {
    UI_SCREEN_HOME = 0,      /* Main Dashboard / Launcher with icons & wallpaper */
    UI_SCREEN_FILES,         /* SD Card File & Directory Explorer (+ BMP & TXT viewer) */
    UI_SCREEN_GAMES,         /* Games launcher: built-in ROM game + games/<name>.bin icon tiles */
    UI_SCREEN_SETTINGS,      /* RTC Clock, Audio Mute & EEPROM Settings */
    UI_SCREEN_SYSINFO,       /* Hardware POST & Memory Diagnostics */
    UI_SCREEN_WIFI           /* Wi-Fi Setup & Network Status */
} UI_Screen_t;

/**
 * @brief Initialize the Console UI Manager, Top Status Bar, and Home Screen
 */
void UIManager_Init(void);

/**
 * @brief Periodic UI update (updates RTC clock and USB/SD status icons in Status Bar)
 *        Call inside GuiRenderTask loop.
 */
void UIManager_Update(void);

/**
 * @brief Switch active workspace screen
 * @param screen Target screen ID
 */
void UIManager_ShowScreen(UI_Screen_t screen);

/**
 * @brief Notify UI Manager that async directory scan in g_dir_cache is ready
 */
void UIManager_OnDirReady(void);

/**
 * @brief Restore UI screen after a sandboxed app/script exits
 */
void UIManager_OnAppExit(void);

/**
 * @brief Get and set current UI theme index (0..3)
 */
uint8_t UIManager_GetThemeId(void);
void UIManager_SetThemeId(uint8_t id);

/**
 * @brief Set full hardware RTC date and time (e.g. from NTP sync)
 */
void UIManager_SetDateTime(uint16_t year, uint8_t month, uint8_t date, uint8_t weekday,
                           uint8_t hours, uint8_t minutes, uint8_t seconds);

/**
 * @brief Open interactive Wi-Fi setup dialog
 */
void UIManager_OpenWifiSetup(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
