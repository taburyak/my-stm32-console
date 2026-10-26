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
    UI_SCREEN_GAMES,         /* Games Hub Menu (Internal ROMs + External .bin + Scripts) */
    UI_SCREEN_CYBER_PONG,    /* Built-in Internal Demo Game: Cyber-Pong */
    UI_SCREEN_SETTINGS,      /* RTC Clock, Audio Mute & EEPROM Settings */
    UI_SCREEN_SYSINFO        /* Hardware POST & Memory Diagnostics */
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

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
