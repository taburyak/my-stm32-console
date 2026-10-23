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

    FRESULT res = f_mount(&sd_fatfs, "", 1);
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
        f_mount(NULL, "", 1);
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

    FRESULT res = f_getfree("", &free_clusters, &fs);
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
