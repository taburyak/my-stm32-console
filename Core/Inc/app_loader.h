#ifndef APP_LOADER_H
#define APP_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "memory_map.h"
#include "console_abi.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Code sandbox in internal SRAM (0-WS) + data sandbox in external FSMC SRAM, see memory_map.h */
#define APP_SANDBOX_BASE_ADDR   MM_APP_RAM_BASE
#define APP_SANDBOX_MAX_SIZE    MM_APP_IMAGE_MAX_SIZE /* code + .data + .bss, task stack excluded */

#define APP_EXT_DATA_BASE_ADDR  MM_EXT_APP_BASE
#define APP_EXT_DATA_MAX_SIZE   MM_EXT_APP_SIZE

/* Unified Input Bitmask returned by SVC 6 (sys_get_keys), defined by the shared ABI */
#define APP_KEY_LEFT            SDK_KEY_LEFT
#define APP_KEY_RIGHT           SDK_KEY_RIGHT
#define APP_KEY_UP              SDK_KEY_UP
#define APP_KEY_DOWN            SDK_KEY_DOWN
#define APP_KEY_A               SDK_KEY_A
#define APP_KEY_B               SDK_KEY_B
#define APP_KEY_X               SDK_KEY_X
#define APP_KEY_EXIT            SDK_KEY_EXIT

typedef SDK_DrawTextParams_t App_DrawTextParams_t;

/* Fault captured from a crashed app (fault status registers and faulting PC) */
typedef struct {
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t fault_addr;
    uint32_t pc;
} AppLoader_FaultInfo_t;

bool AppLoader_LoadAndRun(const char *fatfs_path);
bool AppLoader_LoadAndRunImage(const uint8_t *image, uint32_t size, const char *name);
bool AppLoader_IsRunning(void);
bool AppLoader_IsAppActive(void);
void AppLoader_RequestExit(void);
void AppLoader_Service(void);
bool AppLoader_HandleFault(uint32_t exc_return);
bool AppLoader_TakeFaultInfo(AppLoader_FaultInfo_t *out);

/* Privileged Kernel Syscall Helpers */
bool     AppLoader_IsUserBufferValid(const void *ptr, uint32_t len);
uint32_t AppLoader_ReadInputMask_Privileged(void);
void     AppLoader_DrawText_Privileged(const App_DrawTextParams_t *p);
bool     AppLoader_SaveState_Privileged(const char *filename, const void *data, uint32_t len);
uint32_t AppLoader_LoadState_Privileged(const char *filename, void *dst, uint32_t max_len);
bool     AppLoader_GetGamepad_Privileged(SDK_GamepadState_t *dst);
bool     AppLoader_GetMouse_Privileged(SDK_MouseState_t *dst);
void     AppLoader_GetSysInfo_Privileged(SDK_SysInfo_t *dst);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOADER_H */
