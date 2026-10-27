#include "os_tasks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ccmram.h"
#include "sram_ext.h"
#include "buttons_manager.h"
#include "usb_host_manager.h"
#include "beeper.h"
#include "status_led.h"
#include "sdcard_fatfs.h"
#include "fatfs.h"
#include "app_loader.h"
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * 1. CCM RAM Static Allocations for RTOS Tasks (0 Wait-State CPU D-Bus)
 * ========================================================================== */

#define INPUT_TASK_STACK_WORDS    512U  /* 512 * 4 = 2048 Bytes (2 KB) in CCM RAM */
#define STORAGE_TASK_STACK_WORDS  1536U /* 1536 * 4 = 6144 Bytes (6 KB) in CCM RAM */

CCMRAM_BUF static StackType_t  inputTaskStack[INPUT_TASK_STACK_WORDS];
CCMRAM_BUF static StaticTask_t inputTaskTCB;
static osThreadId_t            inputAudioTaskHandle = NULL;

CCMRAM_BUF static StackType_t  storageTaskStack[STORAGE_TASK_STACK_WORDS];
CCMRAM_BUF static StaticTask_t storageTaskTCB;
static osThreadId_t            storageIoTaskHandle = NULL;

/* ==========================================================================
 * 2. Shared Buffers & Inter-Task Queues
 * ========================================================================== */

/* Place directory cache in aligned Internal SRAM for fast & safe string operations */
/* CPU-only data (filled by memcpy from FatFs FILINFO), so it can live in CCM RAM */
CCMRAM_BUF Storage_DirCache_t g_dir_cache __attribute__((aligned(4)));

/* FatFs working structures MUST reside in DMA-capable Internal SRAM (NOT CCM RAM!) */
static DIR     s_fatfs_dir;
static FILINFO s_fatfs_fno;

/* Message Queues */
static osMessageQueueId_t qStorageReq = NULL;
static osMessageQueueId_t qGuiEvent   = NULL;

/* Forward declarations of worker threads */
static void InputAudioTask(void *argument);
static void StorageIoTask(void *argument);

/* ==========================================================================
 * 3. Public Initialization & Queue Helpers
 * ========================================================================== */

void OSTasks_Init(void)
{
    /* 1. Create Inter-Task Message Queues */
    qStorageReq = osMessageQueueNew(4U, sizeof(Storage_Request_t), NULL);
    qGuiEvent   = osMessageQueueNew(4U, sizeof(Gui_EventMsg_t), NULL);

    /* 2. Spawn High-Priority InputAudioTask (osPriorityAboveNormal, 5ms period) */
    const osThreadAttr_t inputTask_attr = {
        .name       = "InputAudioTask",
        .cb_mem     = &inputTaskTCB,
        .cb_size    = sizeof(inputTaskTCB),
        .stack_mem  = inputTaskStack,
        .stack_size = sizeof(inputTaskStack),
        .priority   = (osPriority_t)osPriorityAboveNormal
    };
    inputAudioTaskHandle = osThreadNew(InputAudioTask, NULL, &inputTask_attr);

    /* 3. Spawn Background StorageIoTask (osPriorityBelowNormal, event-driven) */
    const osThreadAttr_t storageTask_attr = {
        .name       = "StorageIoTask",
        .cb_mem     = &storageTaskTCB,
        .cb_size    = sizeof(storageTaskTCB),
        .stack_mem  = storageTaskStack,
        .stack_size = sizeof(storageTaskStack),
        .priority   = (osPriority_t)osPriorityBelowNormal
    };
    storageIoTaskHandle = osThreadNew(StorageIoTask, NULL, &storageTask_attr);

    printf("[RTOS] InputAudioTask & StorageIoTask spawned in CCM RAM.\r\n");
}

bool OSTasks_RequestDirList(const char *path)
{
    if (qStorageReq == NULL || path == NULL)
    {
        return false;
    }

    /* Static struct avoids allocating 140+ bytes on the caller's (LVGL) stack! */
    static Storage_Request_t s_req;
    memset(&s_req, 0, sizeof(s_req));
    s_req.cmd = STORAGE_CMD_LIST_DIR;
    strncpy(s_req.path, path, STORAGE_MAX_PATH_LEN - 1U);

    return (osMessageQueuePut(qStorageReq, &s_req, 0U, 0U) == osOK);
}

bool OSTasks_GetGuiEvent(Gui_EventMsg_t *evt)
{
    if (qGuiEvent == NULL || evt == NULL)
    {
        return false;
    }

    return (osMessageQueueGet(qGuiEvent, evt, NULL, 0U) == osOK);
}

/* ==========================================================================
 * 4. Task Implementations
 * ========================================================================== */

/**
 * @brief High-priority real-time task (5 ms period) for physical buttons,
 *        USB Host HID polling (Keyboard, Mouse, Gamepad), and Beeper audio.
 */
static void InputAudioTask(void *argument)
{
    (void)argument;
    Button_Message_t btn_msg;

    for (;;)
    {
        /* 1. Poll physical board buttons (debounce & long-press state machine) */
        Buttons_Process();

        /* 2. Consume physical button events for acoustic & LED feedback */
        while (Buttons_GetEvent(&btn_msg))
        {
            switch (btn_msg.event)
            {
                case BUTTON_EVENT_PRESSED:
                    printf("[BTN] KEY_%d PRESSED\r\n", btn_msg.id);
                    Beeper_Click();
                    break;

                case BUTTON_EVENT_SHORT_RELEASE:
                    printf("[BTN] KEY_%d SHORT RELEASE (<1s)\r\n", btn_msg.id);
                    break;

                case BUTTON_EVENT_LONG_PRESS:
                    printf("[BTN] KEY_%d LONG PRESS (>=1s)!\r\n", btn_msg.id);
                    if (btn_msg.id == BUTTON_KEY3)
                    {
                        Beeper_ErrorAlert();
                        if (AppLoader_IsRunning())
                        {
                            AppLoader_RequestExit();
                        }
                    }
                    else
                    {
                        Beeper_Click();
                    }
                    break;

                default:
                    break;
            }
        }

        /* 3. Supervise USB Host & poll HID endpoints (Keyboard, Mouse, Gamepad) */
        USBHostManager_TaskPoll();

        /* 4. Update non-blocking Beeper timers & sound patterns */
        Beeper_Process();

        /* 5. Sleep for 5 ms (200 Hz deterministic polling rate) */
        osDelay(5U);
    }
}

/**
 * @brief Background I/O task for blocking SD Card (FatFs), SPI Flash, and EEPROM operations.
 */
static void StorageIoTask(void *argument)
{
    (void)argument;
    Storage_Request_t req;

    for (;;)
    {
        /* Block indefinitely until GuiRenderTask sends a storage command */
        if (osMessageQueueGet(qStorageReq, &req, NULL, osWaitForever) == osOK)
        {
            if (req.cmd == STORAGE_CMD_LIST_DIR)
            {
                StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);

                memset(&g_dir_cache, 0, sizeof(g_dir_cache));
                strncpy(g_dir_cache.path, req.path, STORAGE_MAX_PATH_LEN - 1U);

                FRESULT fr = f_opendir(&s_fatfs_dir, req.path);
                if (fr == FR_OK)
                {
                    uint32_t idx = 0U;
                    while (idx < STORAGE_MAX_DIR_ITEMS)
                    {
                        fr = f_readdir(&s_fatfs_dir, &s_fatfs_fno);
                        if (fr != FR_OK || s_fatfs_fno.fname[0] == 0)
                        {
                            break;
                        }

                        strncpy(g_dir_cache.items[idx].name,
                                s_fatfs_fno.fname,
                                STORAGE_MAX_NAME_LEN - 1U);
                        g_dir_cache.items[idx].is_dir = ((s_fatfs_fno.fattrib & AM_DIR) != 0) ? 1U : 0U;
                        idx++;
                    }
                    f_closedir(&s_fatfs_dir);

                    g_dir_cache.count      = idx;
                    g_dir_cache.io_success = true;

                    Gui_EventMsg_t evt = {
                        .type         = GUI_EVT_DIR_READY,
                        .result_Count = idx
                    };
                    (void)osMessageQueuePut(qGuiEvent, &evt, 0U, 0U);
                }
                else
                {
                    g_dir_cache.io_success = false;
                    Gui_EventMsg_t evt = {
                        .type         = GUI_EVT_STORAGE_ERROR,
                        .result_Count = 0U
                    };
                    (void)osMessageQueuePut(qGuiEvent, &evt, 0U, 0U);
                }

                StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
            }
        }
    }
}
