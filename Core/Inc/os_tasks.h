#ifndef OS_TASKS_H
#define OS_TASKS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "cmsis_os2.h"
#include <stdbool.h>
#include <stdint.h>

/* Maximum number of directory entries cached in External SRAM */
#define STORAGE_MAX_DIR_ITEMS  64U
#define STORAGE_MAX_NAME_LEN   64U
#define STORAGE_MAX_PATH_LEN   128U

/**
 * @brief Commands sent from GuiRenderTask to StorageIoTask
 */
typedef enum {
    STORAGE_CMD_LIST_DIR = 0, /* Scan directory on SD card */
    STORAGE_CMD_LOAD_FILE,    /* Load file into External SRAM */
    STORAGE_CMD_SAVE_STATE    /* Save data to SD card / EEPROM */
} Storage_CmdType_t;

/**
 * @brief Request message structure for qStorageReq
 */
typedef struct {
    Storage_CmdType_t cmd;
    char              path[STORAGE_MAX_PATH_LEN];
    void             *dest_buf;
    uint32_t          max_len;
} Storage_Request_t;

/**
 * @brief Single directory entry cached by StorageIoTask
 */
typedef struct __attribute__((aligned(4))) {
    char name[STORAGE_MAX_NAME_LEN];
    bool is_dir;
} Storage_DirItem_t;

/**
 * @brief Shared directory listing result stored in External SRAM
 */
typedef struct {
    char              path[STORAGE_MAX_PATH_LEN];
    bool              io_success;
    uint32_t          count;
    Storage_DirItem_t items[STORAGE_MAX_DIR_ITEMS];
} Storage_DirCache_t;

/**
 * @brief Completion events sent from StorageIoTask to GuiRenderTask
 */
typedef enum {
    GUI_EVT_NONE = 0,
    GUI_EVT_DIR_READY,    /* Directory scan finished (check g_dir_cache) */
    GUI_EVT_FILE_LOADED,  /* File loaded into memory */
    GUI_EVT_STORAGE_ERROR /* I/O operation failed */
} Gui_EventType_t;

typedef struct {
    Gui_EventType_t type;
    uint32_t        result_Count;
} Gui_EventMsg_t;

/* Shared Directory Cache located in External SRAM */
extern Storage_DirCache_t g_dir_cache;

/* --- Public API --- */

/**
 * @brief Create RTOS queues and spawn InputAudioTask and StorageIoTask
 *        (Called from StartDefaultTask after hardware POST & init)
 */
void OSTasks_Init(void);

/**
 * @brief Request an asynchronous directory listing from StorageIoTask
 * @param path Directory path on SD card (e.g., "/" or "/games")
 * @return true if request was queued, false if queue is full
 */
bool OSTasks_RequestDirList(const char *path);

/**
 * @brief Poll for completion events from background tasks (non-blocking)
 * @param evt Pointer to store the received event
 * @return true if an event was received, false if queue is empty
 */
bool OSTasks_GetGuiEvent(Gui_EventMsg_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* OS_TASKS_H */
