#include "app_loader.h"
#include "fatfs.h"
#include "ili9341_simple.h"
#include "usb_host_manager.h"
#include "beeper.h"
#include "status_led.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

/* Entry point signature for external binary apps */
typedef void (*App_Entry_t)(void);

static volatile bool     s_app_running       = false;
static volatile bool     s_app_exit_request  = false;
static volatile uint32_t s_loaded_app_size   = 0;

bool AppLoader_LoadAndRun(const char *fatfs_path)
{
    if (s_app_running || fatfs_path == NULL)
    {
        return false;
    }

    const char *clean_path = (fatfs_path[0] == '/') ? (fatfs_path + 1) : fatfs_path;
    static FIL bin_file;
    UINT br = 0;

    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);

    if (f_open(&bin_file, clean_path, FA_READ) != FR_OK)
    {
        printf("[LOADER] Failed to open: %s\r\n", clean_path);
        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return false;
    }

    FSIZE_t fsize = f_size(&bin_file);
    if (fsize == 0 || fsize > APP_SANDBOX_MAX_SIZE)
    {
        printf("[LOADER] Invalid binary size: %lu bytes\r\n", (unsigned long)fsize);
        f_close(&bin_file);
        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return false;
    }

    /* Clear sandbox memory region before loading */
    memset((void *)APP_SANDBOX_BASE_ADDR, 0, APP_SANDBOX_MAX_SIZE);

    /* Load binary directly into External SRAM at 0x68080000 */
    uint8_t *dst = (uint8_t *)APP_SANDBOX_BASE_ADDR;
    FRESULT res = f_read(&bin_file, dst, (UINT)fsize, &br);
    f_close(&bin_file);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);

    if (res != FR_OK || br != fsize)
    {
        printf("[LOADER] Read error (%d), read %u / %lu\r\n", res, br, (unsigned long)fsize);
        return false;
    }

    /* Flush CPU data/instruction pipelines before jumping to RAM code */
    __DSB();
    __ISB();

    s_loaded_app_size  = (uint32_t)fsize;
    s_app_exit_request = false;
    s_app_running      = true;

    printf("[LOADER] Loaded '%s' (%lu bytes) at 0x%08lX. Launching in Sandbox!\r\n",
           clean_path, (unsigned long)fsize, (unsigned long)APP_SANDBOX_BASE_ADDR);

    return true;
}

bool AppLoader_IsRunning(void)
{
    return s_app_running;
}

void AppLoader_RequestExit(void)
{
    s_app_exit_request = true;
    s_app_running      = false;
}

uint32_t AppLoader_ReadInputMask_Privileged(void)
{
    uint32_t mask = 0;

    /* 1. Physical Board Buttons (Active-Low) */
    if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) mask |= APP_KEY_LEFT;
    if (HAL_GPIO_ReadPin(KEY_1_GPIO_Port, KEY_1_Pin) == GPIO_PIN_RESET) mask |= APP_KEY_UP;
    if (HAL_GPIO_ReadPin(KEY_2_GPIO_Port, KEY_2_Pin) == GPIO_PIN_RESET) mask |= APP_KEY_RIGHT;
    if (HAL_GPIO_ReadPin(KEY_3_GPIO_Port, KEY_3_Pin) == GPIO_PIN_RESET) mask |= APP_KEY_EXIT;

    /* 2. USB HID Gamepad */
    USB_GamepadState_t pad;
    if (USBHostManager_GetGamepadState(&pad))
    {
        if (pad.dpad_left  || pad.btn_l1) mask |= APP_KEY_LEFT;
        if (pad.dpad_right || pad.btn_r1) mask |= APP_KEY_RIGHT;
        if (pad.dpad_up)                  mask |= APP_KEY_UP;
        if (pad.dpad_down)                mask |= APP_KEY_DOWN;
        if (pad.btn_a)                    mask |= APP_KEY_A;
        if (pad.btn_b || pad.btn_select)  mask |= APP_KEY_EXIT;
    }

    /* 3. USB Mouse / Touch drag support (Left half / Right half if clicked) */
    USB_MouseEvent_t mse;
    if (USBHostManager_GetMouseState(&mse) && mse.btn_left)
    {
        if (mse.x < 160) mask |= APP_KEY_LEFT;
        else             mask |= APP_KEY_RIGHT;
    }

    return mask;
}

void AppLoader_SandboxTaskLoop(void)
{
    if (s_app_running && !s_app_exit_request)
    {
        /* ARM Cortex-M Thumb-2 requires bit 0 of function pointer to be 1! */
        App_Entry_t app_entry = (App_Entry_t)(APP_SANDBOX_BASE_ADDR | 1UL);

        /* Execute external application in Unprivileged Mode! */
        app_entry();

        /* When app_entry() returns, mark app as finished */
        s_app_running = false;
    }
    else
    {
        /* Sleep until next interrupt when no external app is active */
        __WFI();
    }
}
