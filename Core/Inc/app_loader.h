#ifndef APP_LOADER_H
#define APP_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

/* Dedicated Sandbox Region in External SRAM (Upper 512 KB: 0x68080000) */
#define APP_SANDBOX_BASE_ADDR   0x68080000UL
#define APP_SANDBOX_MAX_SIZE    (256U * 1024U) /* 256 KB for .bin code + data */

/* Unified Input Bitmask returned by SVC 6 (sys_get_keys) */
#define APP_KEY_LEFT            (1UL << 0)
#define APP_KEY_RIGHT           (1UL << 1)
#define APP_KEY_UP              (1UL << 2)
#define APP_KEY_DOWN            (1UL << 3)
#define APP_KEY_A               (1UL << 4)
#define APP_KEY_B               (1UL << 5)
#define APP_KEY_EXIT            (1UL << 7)

/**
 * @brief Load a raw binary (.bin) file from SD card into 0x68080000 and start execution
 * @param fatfs_path Path to .bin file (e.g. "games/pong.bin")
 * @return true if loaded and started successfully, false otherwise
 */
bool AppLoader_LoadAndRun(const char *fatfs_path);

/**
 * @brief Check if an external sandbox application is currently running
 */
bool AppLoader_IsRunning(void);

/**
 * @brief Request termination of the running external app (called from SVC 8 or Fault handler)
 */
void AppLoader_RequestExit(void);

/**
 * @brief Poll function called from SandboxTask (Unprivileged Mode) to execute the loaded app
 */
void AppLoader_SandboxTaskLoop(void);

/**
 * @brief Helper called from SVC 6 to read combined state of Gamepad, Keyboard, and Board Buttons
 */
uint32_t AppLoader_ReadInputMask_Privileged(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOADER_H */
