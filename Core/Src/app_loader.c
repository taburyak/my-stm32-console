#include "app_loader.h"
#include "fatfs.h"
#include "ili9341_simple.h"
#include "usb_host_manager.h"
#include "buttons_manager.h"
#include "beeper.h"
#include "status_led.h"
#include "sdcard_fatfs.h"
#include "XPT2046_touch.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* Entry point signature for external binary apps */
typedef void (*App_Entry_t)(void);

/* s_app_running stays true until the sandbox task is deleted by AppLoader_Service() */
static volatile bool     s_app_running       = false;
static volatile bool     s_app_exit_request  = false;
static volatile uint32_t s_loaded_app_size   = 0;

static StaticTask_t      s_sbx_tcb;
static osThreadId_t      s_sbx_handle        = NULL;
static uint32_t          s_app_entry         = 0;
static const char       *s_last_error        = "";

static volatile bool                  s_fault_pending = false;
static volatile AppLoader_FaultInfo_t s_fault_info;

static void Sandbox_TaskEntry(void *argument);
static void AppLoader_ParkLoop(void);

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
    if (ptr == NULL || len == 0U)
    {
        return false;
    }

    uint32_t start = (uint32_t)ptr;
    uint32_t end   = start + len - 1U;
    if (end < start)
    {
        return false; /* Overflow check */
    }

    /* App code/data/stack sandbox in internal SRAM */
    bool in_app_ram = (start >= MM_APP_RAM_BASE) &&
                      (end <= (MM_APP_RAM_BASE + MM_APP_RAM_SIZE - 1UL));

    /* App data sandbox in external FSMC SRAM */
    bool in_app_ext = (start >= MM_EXT_APP_BASE) &&
                      (end <= (MM_EXT_APP_BASE + MM_EXT_APP_SIZE - 1UL));

    return (in_app_ram || in_app_ext);
}

static void AppLoader_ClearSandboxes(void)
{
    /* App image area only: the task stack at the top of the sandbox is managed by FreeRTOS */
    memset((void *)APP_SANDBOX_BASE_ADDR, 0, APP_SANDBOX_MAX_SIZE);
    memset((void *)APP_EXT_DATA_BASE_ADDR, 0, APP_EXT_DATA_MAX_SIZE);
}

/* CRC-32 (IEEE 802.3, reflected 0xEDB88320), same as sdk_apps/sdk_app_crc.cmake */
static uint32_t AppLoader_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= data[i];
        for (uint32_t k = 0U; k < 8U; k++)
        {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* Checks the image loaded at APP_SANDBOX_BASE_ADDR; sets s_last_error on failure */
static bool AppLoader_ValidateImage(const SDK_AppHeader_t *hdr, uint32_t size)
{
    const uint32_t base = APP_SANDBOX_BASE_ADDR;

    if (hdr->magic != SDK_APP_MAGIC)
    {
        s_last_error = "Not an SDK app\n(rebuild with the SDK)";
        return false;
    }
    if (hdr->api_version == 0U || hdr->api_version > SDK_API_VERSION)
    {
        s_last_error = "App needs a newer OS\n(SDK API version)";
        return false;
    }
    if (hdr->header_size < sizeof(SDK_AppHeader_t) || hdr->image_size != size ||
        (size & 3U) != 0U || size < (hdr->header_size + 4U))
    {
        s_last_error = "Bad image header";
        return false;
    }
    uint32_t stored_crc = *(const uint32_t *)(base + size - 4U);
    if (AppLoader_Crc32((const uint8_t *)base, size - 4U) != stored_crc)
    {
        s_last_error = "CRC mismatch\n(file corrupted)";
        return false;
    }
    if ((hdr->entry & 1U) == 0U || (hdr->entry & ~1U) < (base + hdr->header_size) ||
        (hdr->entry & ~1U) >= (base + size - 4U))
    {
        s_last_error = "Bad entry point";
        return false;
    }
    if (hdr->bss_end < (base + size) || hdr->bss_end > (base + APP_SANDBOX_MAX_SIZE))
    {
        s_last_error = "App RAM too large";
        return false;
    }
    return true;
}

/* Starts the image already placed at APP_SANDBOX_BASE_ADDR */
static bool AppLoader_StartLoadedImage(uint32_t size)
{
    const SDK_AppHeader_t *hdr = (const SDK_AppHeader_t *)APP_SANDBOX_BASE_ADDR;
    if (!AppLoader_ValidateImage(hdr, size))
    {
        printf("[LOADER] Rejected: %s\r\n", s_last_error);
        return false;
    }
    s_app_entry = hdr->entry;

    /* Flush all pending key/button events before starting the game */
    USBHostManager_FlushInputQueues();
    Buttons_Init();

    /* Flush CPU data/instruction pipelines before jumping to RAM code */
    __DSB();
    __ISB();

    const osThreadAttr_t sbx_attr = {
        .name       = "sandboxTask",
        .cb_mem     = &s_sbx_tcb,
        .cb_size    = sizeof(s_sbx_tcb),
        .stack_mem  = (void *)MM_APP_STACK_BASE,
        .stack_size = MM_APP_STACK_SIZE,
        .priority   = (osPriority_t)osPriorityLow
    };

    s_loaded_app_size  = size;
    s_app_exit_request = false;
    s_fault_pending    = false;

    /* Lower priority than the caller (GUI), so the tag is set before the task first runs */
    s_sbx_handle = osThreadNew(Sandbox_TaskEntry, NULL, &sbx_attr);
    if (s_sbx_handle == NULL)
    {
        s_last_error = "Cannot create app task";
        printf("[LOADER] Failed to create sandbox task\r\n");
        return false;
    }
    vTaskSetApplicationTaskTag((TaskHandle_t)s_sbx_handle, (TaskHookFunction_t)1);
    s_app_running = true;

    printf("[LOADER] Loaded '%.16s' API %u (%lu bytes) at 0x%08lX. Launching in Sandbox!\r\n",
           hdr->name, (unsigned int)hdr->api_version, (unsigned long)size,
           (unsigned long)APP_SANDBOX_BASE_ADDR);

    return true;
}

bool AppLoader_LoadAndRunImage(const uint8_t *image, uint32_t size)
{
    if (s_app_running || image == NULL)
    {
        return false;
    }
    if (size < sizeof(SDK_AppHeader_t) || size > APP_SANDBOX_MAX_SIZE)
    {
        s_last_error = "Bad image size";
        return false;
    }

    AppLoader_ClearSandboxes();
    memcpy((void *)APP_SANDBOX_BASE_ADDR, image, size);
    return AppLoader_StartLoadedImage(size);
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
        s_last_error = "Cannot open file";
        printf("[LOADER] Failed to open: %s\r\n", clean_path);
        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return false;
    }

    FSIZE_t fsize = f_size(&bin_file);
    if (fsize < sizeof(SDK_AppHeader_t) || fsize > APP_SANDBOX_MAX_SIZE)
    {
        s_last_error = "Bad image size";
        printf("[LOADER] Invalid binary size: %lu bytes (max %u)\r\n",
               (unsigned long)fsize, (unsigned int)APP_SANDBOX_MAX_SIZE);
        f_close(&bin_file);
        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return false;
    }

    AppLoader_ClearSandboxes();

    uint8_t *dst = (uint8_t *)APP_SANDBOX_BASE_ADDR;
    FRESULT res = f_read(&bin_file, dst, (UINT)fsize, &br);
    f_close(&bin_file);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);

    if (res != FR_OK || br != fsize)
    {
        s_last_error = "SD read error";
        printf("[LOADER] Read error (%d), read %u / %lu\r\n", res, br, (unsigned long)fsize);
        return false;
    }

    return AppLoader_StartLoadedImage((uint32_t)fsize);
}

const char *AppLoader_GetLastError(void)
{
    return s_last_error;
}

bool AppLoader_IsRunning(void)
{
    return s_app_running;
}

bool AppLoader_IsAppActive(void)
{
    return s_app_running && !s_app_exit_request;
}

/* Safe from SVC handler and any task; the GUI task deletes the sandbox in AppLoader_Service() */
void AppLoader_RequestExit(void)
{
    s_app_exit_request = true;
}

void AppLoader_Service(void)
{
    if (!s_app_running || !s_app_exit_request || (g_sandbox_in_gate != 0U))
    {
        return;
    }

    if (s_sbx_handle != NULL)
    {
        vTaskDelete((TaskHandle_t)s_sbx_handle);
        s_sbx_handle = NULL;
    }

    /* Flush any residual key/button events so LVGL does not auto-scroll on return */
    USBHostManager_FlushInputQueues();
    Buttons_Init();

    s_app_running = false;
    printf("[LOADER] App terminated%s\r\n", s_fault_pending ? " (FAULT)" : "");
}

bool AppLoader_HandleFault(uint32_t exc_return)
{
    /* Only faults of the unprivileged app itself (PSP, sandbox task, not inside the kernel gate) */
    if (((exc_return & 0x4U) == 0U) || (s_sbx_handle == NULL) ||
        (xTaskGetCurrentTaskHandle() != (TaskHandle_t)s_sbx_handle) ||
        (g_sandbox_in_gate != 0U))
    {
        return false;
    }

    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;
    bool stacking_error = (cfsr & (SCB_CFSR_MSTKERR_Msk | SCB_CFSR_STKERR_Msk)) != 0U;

    s_fault_info.cfsr       = cfsr;
    s_fault_info.hfsr       = hfsr;
    s_fault_info.fault_addr = ((cfsr & SCB_CFSR_MMARVALID_Msk) != 0U) ? SCB->MMFAR :
                              (((cfsr & SCB_CFSR_BFARVALID_Msk) != 0U) ? SCB->BFAR : 0U);
    s_fault_info.pc         = stacking_error ? 0U : ((uint32_t *)__get_PSP())[6];

    SCB->CFSR = cfsr;
    SCB->HFSR = hfsr;

    /* Fresh exception frame near the top of the app stack (room for an FPU frame) */
    uint32_t *frame = (uint32_t *)(MM_APP_RAM_BASE + MM_APP_RAM_SIZE - 128U);
    for (uint32_t i = 0U; i < 32U; i++)
    {
        frame[i] = 0U;
    }
    frame[6] = ((uint32_t)AppLoader_ParkLoop) & ~1U;
    frame[7] = 0x01000000U; /* xPSR: Thumb */
    __set_PSP((uint32_t)frame);

    s_fault_pending    = true;
    s_app_exit_request = true;
    return true;
}

bool AppLoader_TakeFaultInfo(AppLoader_FaultInfo_t *out)
{
    if (!s_fault_pending || (out == NULL))
    {
        return false;
    }
    out->cfsr       = s_fault_info.cfsr;
    out->hfsr       = s_fault_info.hfsr;
    out->fault_addr = s_fault_info.fault_addr;
    out->pc         = s_fault_info.pc;
    s_fault_pending = false;
    return true;
}

/* Runs unprivileged: waits for the GUI task to delete the sandbox */
static void AppLoader_ParkLoop(void)
{
    for (;;)
    {
        __WFI();
    }
}

/* Sandbox task body (unprivileged via task tag 1) */
static void Sandbox_TaskEntry(void *argument)
{
    (void)argument;

    App_Entry_t app_entry = (App_Entry_t)s_app_entry;
    app_entry();

    __asm volatile ("svc 8" : : : "memory"); /* sys_exit() if the app returned without it */
    AppLoader_ParkLoop();
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

bool AppLoader_GetGamepad_Privileged(SDK_GamepadState_t *dst)
{
    USB_GamepadState_t pad;

    memset(dst, 0, sizeof(*dst));
    dst->hat = 0x0FU;
    if (!USBHostManager_GetGamepadState(&pad))
    {
        return false;
    }

    dst->connected = 1U;
    dst->left_x    = pad.left_x;
    dst->left_y    = pad.left_y;
    dst->hat       = pad.hat;

    uint32_t b = 0U;
    if (pad.dpad_up)    b |= SDK_PAD_UP;
    if (pad.dpad_down)  b |= SDK_PAD_DOWN;
    if (pad.dpad_left)  b |= SDK_PAD_LEFT;
    if (pad.dpad_right) b |= SDK_PAD_RIGHT;
    if (pad.btn_a)      b |= SDK_PAD_A;
    if (pad.btn_b)      b |= SDK_PAD_B;
    if (pad.btn_x)      b |= SDK_PAD_X;
    if (pad.btn_y)      b |= SDK_PAD_Y;
    if (pad.btn_l1)     b |= SDK_PAD_L1;
    if (pad.btn_r1)     b |= SDK_PAD_R1;
    if (pad.btn_l2)     b |= SDK_PAD_L2;
    if (pad.btn_r2)     b |= SDK_PAD_R2;
    if (pad.btn_start)  b |= SDK_PAD_START;
    if (pad.btn_select) b |= SDK_PAD_SELECT;
    dst->buttons = b;
    return true;
}

bool AppLoader_GetMouse_Privileged(SDK_MouseState_t *dst)
{
    USB_MouseEvent_t mse;

    memset(dst, 0, sizeof(*dst));
    if (USBHostManager_GetMouseState(&mse))
    {
        dst->x       = mse.x;
        dst->y       = mse.y;
        dst->dx      = mse.delta_x;
        dst->dy      = mse.delta_y;
        dst->buttons = (uint8_t)((mse.btn_left   ? SDK_MOUSE_BTN_LEFT   : 0U) |
                                 (mse.btn_right  ? SDK_MOUSE_BTN_RIGHT  : 0U) |
                                 (mse.btn_middle ? SDK_MOUSE_BTN_MIDDLE : 0U));
        dst->source  = SDK_MOUSE_SRC_USB;
        return true;
    }

    uint16_t tx = 0U, ty = 0U;
    if (XPT2046_TouchPressed() && XPT2046_TouchGetCoordinates(&tx, &ty))
    {
        dst->x       = (int16_t)tx;
        dst->y       = (int16_t)ty;
        dst->buttons = SDK_MOUSE_BTN_LEFT;
        dst->source  = SDK_MOUSE_SRC_TOUCH;
        return true;
    }
    return false;
}

void AppLoader_GetSysInfo_Privileged(SDK_SysInfo_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->api_version    = (uint16_t)SDK_API_VERSION;
    dst->screen_w       = (uint16_t)SDK_SCREEN_W;
    dst->screen_h       = (uint16_t)SDK_SCREEN_H;
    dst->app_ram_size   = APP_SANDBOX_MAX_SIZE;
    dst->app_stack_size = MM_APP_STACK_SIZE;
    dst->app_ext_size   = APP_EXT_DATA_MAX_SIZE;

    dst->devices = SDK_DEV_TOUCH;
    if (USBHostManager_IsKeyboardReady()) dst->devices |= SDK_DEV_KEYBOARD;
    if (USBHostManager_IsMouseReady())    dst->devices |= SDK_DEV_MOUSE;
    if (USBHostManager_IsGamepadReady())  dst->devices |= SDK_DEV_GAMEPAD;

    if (Beeper_IsMuted())     dst->flags |= SDK_INFO_SOUND_MUTED;
    if (SDCard_IsMounted())   dst->flags |= SDK_INFO_SD_MOUNTED;
}
