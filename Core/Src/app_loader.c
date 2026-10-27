#include "app_loader.h"
#include "fatfs.h"
#include "ili9341_simple.h"
#include "usb_host_manager.h"
#include "buttons_manager.h"
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

/* Built-in 5x7 ASCII Font Table (' ' 32 .. 'Z' 90) stored in MCU Flash */
static const uint8_t s_kernel_font5x7[59][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* '\'' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}  /* 'Z' */
};

bool AppLoader_IsUserBufferValid(const void *ptr, uint32_t len)
{
    if (ptr == NULL || len == 0U || len > 0x00080000UL)
    {
        return false;
    }

    uint32_t start = (uint32_t)ptr;
    uint32_t end   = start + len - 1U;
    if (end < start)
    {
        return false; /* Overflow check */
    }

    /* Allowed Region A: Internal SRAM (Stacks & Code Sandbox 0x20000000 .. 0x2001FFFF) */
    bool in_int_sram = (start >= 0x20000000UL && end <= 0x2001FFFFUL);

    /* Allowed Region B: External FSMC SRAM (0x68000000 .. 0x680FFFFF) */
    bool in_ext_sram = (start >= 0x68000000UL && end <= 0x680FFFFFUL);

    return (in_int_sram || in_ext_sram);
}

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
        printf("[LOADER] Invalid binary size: %lu bytes (max %u)\r\n",
               (unsigned long)fsize, (unsigned int)APP_SANDBOX_MAX_SIZE);
        f_close(&bin_file);
        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return false;
    }

    /* Clear both Internal Code Sandbox and External Data Sandbox (.bss) */
    memset((void *)APP_SANDBOX_BASE_ADDR, 0, APP_SANDBOX_MAX_SIZE);
    memset((void *)APP_EXT_DATA_BASE_ADDR, 0, 64U * 1024U);

    /* Load binary into 0-Wait-State Internal SRAM2 at 0x20018000 */
    uint8_t *dst = (uint8_t *)APP_SANDBOX_BASE_ADDR;
    FRESULT res = f_read(&bin_file, dst, (UINT)fsize, &br);
    f_close(&bin_file);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);

    if (res != FR_OK || br != fsize)
    {
        printf("[LOADER] Read error (%d), read %u / %lu\r\n", res, br, (unsigned long)fsize);
        return false;
    }

    /* Flush all pending key/button events before starting the game */
    USBHostManager_FlushInputQueues();
    Buttons_Init();

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

    /* Flush any residual key/button events so LVGL does not auto-scroll on return! */
    USBHostManager_FlushInputQueues();
    Buttons_Init();
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
        if (pad.btn_x)                    mask |= APP_KEY_B; /* X acts as Secondary Action / Hard Drop */
        if (pad.btn_b || pad.btn_select)  mask |= APP_KEY_EXIT;
    }

    /* 2.5. USB HID Keyboard (Cursor Arrows, WASD, Space, Enter, ESC) */
    mask |= USBHostManager_GetKeyboardGameMask();

    /* 3. USB Mouse drag support (Left half / Right half if clicked) */
    USB_MouseEvent_t mse;
    if (USBHostManager_GetMouseState(&mse))
    {
        if (mse.btn_left)
        {
            if (mse.x < 160) mask |= APP_KEY_LEFT;
            else             mask |= APP_KEY_RIGHT;
        }
        if (mse.btn_right)
        {
            mask |= APP_KEY_A;
        }
    }

    return mask;
}

static void Kernel_FillRect_Fast(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= 320U || y >= 240U || w == 0U || h == 0U) return;
    if (x + w > 320U) w = 320U - x;
    if (y + h > 240U) h = 240U - y;

    lcdSetWindow(x, y, x + w - 1U, y + h - 1U);
    uint32_t total = (uint32_t)w * (uint32_t)h;
    while (total--)
    {
        LCD_DataWrite(color);
    }
}

void AppLoader_DrawText_Privileged(const App_DrawTextParams_t *p)
{
    if (!AppLoader_IsUserBufferValid(p, sizeof(App_DrawTextParams_t)) ||
        !AppLoader_IsUserBufferValid(p->str, 1U))
    {
        return;
    }

    uint16_t cur_x = p->x;
    uint16_t cur_y = p->y;
    uint8_t  scale = (p->scale == 0U) ? 1U : p->scale;
    const char *s  = p->str;

    /* Limit max string length to 64 chars per call for safety */
    for (uint8_t char_cnt = 0; char_cnt < 64U && *s != '\0'; char_cnt++, s++)
    {
        if (!AppLoader_IsUserBufferValid(s, 1U)) break;

        char c = *s;
        /* Convert lowercase a..z to uppercase A..Z for 5x7 font */
        if (c >= 'a' && c <= 'z')
        {
            c = (char)(c - 32);
        }
        if (c < ' ' || c > 'Z')
        {
            c = ' ';
        }

        uint8_t idx = (uint8_t)(c - ' ');

        if (scale == 1U)
        {
            /* Ultra-fast single-window streaming for 1x scale (6x7 px) */
            if (cur_x + 6U <= 320U && cur_y + 7U <= 240U)
            {
                lcdSetWindow(cur_x, cur_y, cur_x + 5U, cur_y + 6U);
                for (uint8_t row = 0; row < 7U; row++)
                {
                    for (uint8_t col = 0; col < 5U; col++)
                    {
                        uint8_t line = s_kernel_font5x7[idx][col];
                        LCD_DataWrite((line & (1U << row)) ? p->fg_color : p->bg_color);
                    }
                    LCD_DataWrite(p->bg_color); /* 1px spacing column */
                }
            }
        }
        else
        {
            for (uint8_t col = 0; col < 5U; col++)
            {
                uint8_t line = s_kernel_font5x7[idx][col];
                for (uint8_t row = 0; row < 7U; row++)
                {
                    uint16_t color = (line & (1U << row)) ? p->fg_color : p->bg_color;
                    Kernel_FillRect_Fast(cur_x + (uint16_t)(col * scale),
                                         cur_y + (uint16_t)(row * scale),
                                         scale, scale, color);
                }
            }
            Kernel_FillRect_Fast(cur_x + (uint16_t)(5U * scale),
                                 cur_y,
                                 scale,
                                 (uint16_t)(7U * scale),
                                 p->bg_color);
        }

        cur_x += (uint16_t)(6U * scale);
        if (cur_x >= 320U) break;
    }

    __DSB();
    __ISB();
}

static bool Build_Save_Path(const char *filename, char *out_path, size_t max_out)
{
    if (!AppLoader_IsUserBufferValid(filename, 1U)) return false;

    /* Sanitize filename: strip leading slashes or '..' */
    const char *clean = filename;
    while (*clean == '/' || *clean == '\\' || *clean == '.') clean++;
    if (*clean == '\0') return false;

    snprintf(out_path, max_out, "saves/%.32s", clean);
    return true;
}

bool AppLoader_SaveState_Privileged(const char *filename, const void *data, uint32_t len)
{
    if (len == 0U || len > 4096U || !AppLoader_IsUserBufferValid(data, len))
    {
        return false;
    }

    char path[48];
    if (!Build_Save_Path(filename, path, sizeof(path)))
    {
        return false;
    }

    static FIL save_file;
    UINT bw = 0;
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);

    if (f_open(&save_file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return false;
    }

    FRESULT res = f_write(&save_file, data, (UINT)len, &bw);
    f_close(&save_file);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);

    return (res == FR_OK && bw == len);
}

uint32_t AppLoader_LoadState_Privileged(const char *filename, void *dst, uint32_t max_len)
{
    if (max_len == 0U || max_len > 4096U || !AppLoader_IsUserBufferValid(dst, max_len))
    {
        return 0U;
    }

    char path[48];
    if (!Build_Save_Path(filename, path, sizeof(path)))
    {
        return 0U;
    }

    static FIL load_file;
    UINT br = 0;
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);

    if (f_open(&load_file, path, FA_READ) != FR_OK)
    {
        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return 0U;
    }

    FRESULT res = f_read(&load_file, dst, (UINT)max_len, &br);
    f_close(&load_file);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);

    return (res == FR_OK) ? (uint32_t)br : 0U;
}

void AppLoader_SandboxTaskLoop(void)
{
    if (s_app_running && !s_app_exit_request)
    {
        /* ARM Cortex-M Thumb-2 requires bit 0 of function pointer to be 1! */
        App_Entry_t app_entry = (App_Entry_t)(APP_SANDBOX_BASE_ADDR | 1UL);

        /* Execute external application in Unprivileged Mode! */
        app_entry();

        /* When app_entry() returns, mark app as finished and flush queues */
        AppLoader_RequestExit();
    }
    else
    {
        /* Sleep until next interrupt when no external app is active */
        __WFI();
    }
}
