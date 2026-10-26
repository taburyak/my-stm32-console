#ifndef APP_LOADER_H
#define APP_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Dedicated 0-Wait-State Code Sandbox in Internal SRAM2 (0x20018000, 16 KB)
 * + 512 KB External SRAM Data Sandbox (0x68080000) */
#define APP_SANDBOX_BASE_ADDR   0x20018000UL
#define APP_SANDBOX_MAX_SIZE    (16U * 1024U) /* 16 KB 0-WS Code + .data */

#define APP_EXT_DATA_BASE_ADDR  0x68080000UL
#define APP_EXT_DATA_MAX_SIZE   (512U * 1024U)

/* Unified Input Bitmask returned by SVC 6 (sys_get_keys) */
#define APP_KEY_LEFT            (1UL << 0)
#define APP_KEY_RIGHT           (1UL << 1)
#define APP_KEY_UP              (1UL << 2)
#define APP_KEY_DOWN            (1UL << 3)
#define APP_KEY_A               (1UL << 4)
#define APP_KEY_B               (1UL << 5)
#define APP_KEY_X               (1UL << 6)
#define APP_KEY_EXIT            (1UL << 7)

/**
 * @brief Parameter block passed to SVC 11 (sys_draw_text)
 */
typedef struct {
    uint16_t    x;
    uint16_t    y;
    const char *str;
    uint16_t    fg_color;
    uint16_t    bg_color;
    uint8_t     scale;
} App_DrawTextParams_t;

bool AppLoader_LoadAndRun(const char *fatfs_path);
bool AppLoader_IsRunning(void);
void AppLoader_RequestExit(void);
void AppLoader_SandboxTaskLoop(void);

/* Privileged Kernel Syscall Helpers */
bool     AppLoader_IsUserBufferValid(const void *ptr, uint32_t len);
uint32_t AppLoader_ReadInputMask_Privileged(void);
void     AppLoader_DrawText_Privileged(const App_DrawTextParams_t *p);
bool     AppLoader_SaveState_Privileged(const char *filename, const void *data, uint32_t len);
uint32_t AppLoader_LoadState_Privileged(const char *filename, void *dst, uint32_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOADER_H */
