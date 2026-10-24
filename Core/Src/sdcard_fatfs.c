#include "sdcard_fatfs.h"
#include "status_led.h"

static FATFS sd_fatfs;
static bool  is_mounted = false;

bool SDCard_Mount(void)
{
    if (is_mounted)
    {
        return true;
    }

    /* Signal SD card activity via Disk LED */
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);

    FRESULT res = f_mount(&sd_fatfs, SDPath, 1);
    if (res == FR_OK)
    {
        is_mounted = true;
    }
    else
    {
        is_mounted = false;
    }

    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    return is_mounted;
}

void SDCard_Unmount(void)
{
    if (is_mounted)
    {
        f_mount(NULL, SDPath, 1);
        is_mounted = false;
    }
}

bool SDCard_IsMounted(void)
{
    return is_mounted;
}

bool SDCard_GetFreeSpace(uint32_t *total_mb, uint32_t *free_mb)
{
    if (!is_mounted || !total_mb || !free_mb)
    {
        return false;
    }

    FATFS *fs;
    DWORD free_clusters;

    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);

    FRESULT res = f_getfree(SDPath, &free_clusters, &fs);
    if (res == FR_OK)
    {
        uint32_t total_sectors = (fs->n_fatent - 2) * fs->csize;
        uint32_t free_sectors  = free_clusters * fs->csize;

        /* Standard 512 bytes per sector -> 1 MB = 2048 sectors */
        *total_mb = total_sectors / 2048U;
        *free_mb  = free_sectors / 2048U;

        StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
        return true;
    }

    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    return false;
}

bool SDCard_InitSystemDirs(void)
{
    if (!SDCard_Mount())
    {
        printf("[SD] Mount failed, skipping directory initialization.\r\n");
        return false;
    }

    static FILINFO fno;
    FRESULT fr;
    const char *required_dirs[] =
    {
        "system",
        "games",
        "assets",
        "saves"
    };

    printf("\r\n--- [SD Card Directory Initialization] ---\r\n");

    for (size_t i = 0; i < sizeof(required_dirs) / sizeof(required_dirs[0]); i++)
    {
        fr = f_stat(required_dirs[i], &fno);
        if (fr == FR_NO_FILE)
        {
            fr = f_mkdir(required_dirs[i]);
            if (fr == FR_OK)
            {
                printf("[SD] Created directory: /%s\r\n", required_dirs[i]);
            }
            else
            {
                printf("[SD] Failed to create /%s (Error %d)\r\n", required_dirs[i], fr);
            }
        }
        else if (fr == FR_OK)
        {
            if (fno.fattrib & AM_DIR)
            {
                printf("[SD] Directory exists: /%s\r\n", required_dirs[i]);
            }
            else
            {
                printf("[SD] Warning: /%s exists but is a file!\r\n", required_dirs[i]);
            }
        }
        else
        {
            printf("[SD] Error checking /%s (Code %d)\r\n", required_dirs[i], fr);
        }
    }

    printf("[SD] Directory Structure Ready.\r\n");
    return true;
}
