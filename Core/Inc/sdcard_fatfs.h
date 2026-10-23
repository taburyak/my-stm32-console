#ifndef SDCARD_FATFS_H
#define SDCARD_FATFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "fatfs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @brief Mount SD card filesystem (FAT32)
 * @return true if successfully mounted, false otherwise
 */
bool SDCard_Mount(void);

/**
 * @brief Safely unmount SD card filesystem
 */
void SDCard_Unmount(void);

/**
 * @brief Check if SD card is currently mounted
 * @return true if mounted, false otherwise
 */
bool SDCard_IsMounted(void);

/**
 * @brief Calculate total and free space on SD card in Megabytes
 * @param total_mb Pointer to store total capacity in MB
 * @param free_mb Pointer to store available free space in MB
 * @return true on success, false if unmounted or failed
 */
bool SDCard_GetFreeSpace(uint32_t *total_mb, uint32_t *free_mb);

/**
 * @brief Checks and creates mandatory system directories (/system, /games, /assets, /saves)
 * @return true if all directories are ready, false otherwise
 */
bool SDCard_InitSystemDirs(void);

#ifdef __cplusplus
}
#endif

#endif /* SDCARD_FATFS_H */
