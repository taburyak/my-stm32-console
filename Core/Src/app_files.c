#include "app_files.h"
#include "app_loader.h"
#include "fatfs.h"
#include "sdcard_fatfs.h"
#include "status_led.h"
#include <stdio.h>
#include <string.h>

#define APP_FILES_DATA_ROOT  "appdata"
#define APP_FILES_RES_ROOT   "games"
#define APP_FILES_FULL_MAX   (sizeof(APP_FILES_DATA_ROOT) + SDK_APP_NAME_LEN + SDK_FILE_PATH_MAX + 4U)

typedef struct {
    FIL     fil;
    uint8_t used;
} AppFile_t;

/* FIL holds a sector buffer used by SDIO DMA: must stay out of CCM */
static AppFile_t s_files[SDK_FILE_MAX_OPEN];
static DIR       s_dir;
static FILINFO   s_info;

static int32_t AppFiles_MapResult(FRESULT res)
{
    switch (res)
    {
        case FR_OK:                  return SDK_OK;
        case FR_NO_FILE:
        case FR_NO_PATH:             return SDK_ERR_NOT_FOUND;
        case FR_EXIST:               return SDK_ERR_EXISTS;
        case FR_DENIED:
        case FR_WRITE_PROTECTED:     return SDK_ERR_DENIED;
        case FR_INVALID_NAME:        return SDK_ERR_INVALID;
        case FR_TOO_MANY_OPEN_FILES: return SDK_ERR_NO_HANDLES;
        default:                     return SDK_ERR_IO;
    }
}

static bool AppFiles_IsNameChar(char c)
{
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) ||
           (c == '_') || (c == '-') || (c == '.') || (c == ' ');
}

/*
 * Copies an app path into kernel memory and checks it byte by byte.
 * Sets *is_res for the "res:" prefix. An empty path (root) is accepted only when allow_empty.
 */
static bool AppFiles_CopyPath(const char *user, char *out, bool allow_empty, bool *is_res)
{
    char tmp[sizeof(SDK_RES_PREFIX) + SDK_FILE_PATH_MAX];
    uint32_t n = 0U;

    if (user != NULL)
    {
        for (;;)
        {
            if ((n >= sizeof(tmp)) || !AppLoader_IsUserBufferValid(&user[n], 1U))
            {
                return false;
            }
            tmp[n] = user[n];
            if (tmp[n] == '\0')
            {
                break;
            }
            n++;
        }
    }
    else
    {
        tmp[0] = '\0';
    }

    const char *p = tmp;
    *is_res = (strncmp(p, SDK_RES_PREFIX, sizeof(SDK_RES_PREFIX) - 1U) == 0);
    if (*is_res)
    {
        p += sizeof(SDK_RES_PREFIX) - 1U;
    }

    size_t len = strlen(p);
    if ((len > SDK_FILE_PATH_MAX) || ((len == 0U) && !allow_empty))
    {
        return false;
    }

    /* Components: non-empty, only name characters, not "." or ".." */
    size_t comp_start = 0U;
    for (size_t i = 0U; i <= len; i++)
    {
        if ((p[i] == '/') || (p[i] == '\0'))
        {
            size_t comp_len = i - comp_start;
            if (len != 0U)
            {
                if ((comp_len == 0U) ||
                    ((comp_len == 1U) && (p[comp_start] == '.')) ||
                    ((comp_len == 2U) && (p[comp_start] == '.') && (p[comp_start + 1U] == '.')))
                {
                    return false;
                }
            }
            comp_start = i + 1U;
        }
        else if (!AppFiles_IsNameChar(p[i]))
        {
            return false;
        }
    }

    memcpy(out, p, len + 1U);
    return true;
}

/* Builds "<root>/<app name>[/<rel>]" */
static void AppFiles_FullPath(char *full, bool is_res, const char *rel)
{
    snprintf(full, APP_FILES_FULL_MAX, "%s/%s%s%s",
             is_res ? APP_FILES_RES_ROOT : APP_FILES_DATA_ROOT, AppLoader_GetAppName(),
             (rel[0] != '\0') ? "/" : "", rel);
}

/* Creates every missing directory on the way to the file in a full path */
static void AppFiles_MakeParents(char *full)
{
    for (char *s = full; *s != '\0'; s++)
    {
        if (*s == '/')
        {
            *s = '\0';
            (void)f_mkdir(full);
            *s = '/';
        }
    }
}

static AppFile_t *AppFiles_Get(uint32_t handle)
{
    if ((handle >= SDK_FILE_MAX_OPEN) || (s_files[handle].used == 0U))
    {
        return NULL;
    }
    return &s_files[handle];
}

int32_t AppFiles_Open(const char *path, uint32_t flags)
{
    char rel[SDK_FILE_PATH_MAX + 1U];
    char full[APP_FILES_FULL_MAX];
    bool is_res;

    const uint32_t write_flags = SDK_FILE_WRITE | SDK_FILE_CREATE | SDK_FILE_TRUNC | SDK_FILE_APPEND;
    if (((flags & (SDK_FILE_READ | SDK_FILE_WRITE)) == 0U) ||
        (((flags & (SDK_FILE_CREATE | SDK_FILE_TRUNC | SDK_FILE_APPEND)) != 0U) && ((flags & SDK_FILE_WRITE) == 0U)) ||
        !AppFiles_CopyPath(path, rel, false, &is_res))
    {
        return SDK_ERR_INVALID;
    }
    if (is_res && ((flags & write_flags) != 0U))
    {
        return SDK_ERR_DENIED;
    }
    if (!SDCard_IsMounted())
    {
        return SDK_ERR_NO_SD;
    }

    uint32_t h = 0U;
    while ((h < SDK_FILE_MAX_OPEN) && (s_files[h].used != 0U))
    {
        h++;
    }
    if (h == SDK_FILE_MAX_OPEN)
    {
        return SDK_ERR_NO_HANDLES;
    }

    BYTE mode = 0U;
    if ((flags & SDK_FILE_READ) != 0U)   mode |= FA_READ;
    if ((flags & SDK_FILE_WRITE) != 0U)  mode |= FA_WRITE;
    if ((flags & SDK_FILE_TRUNC) != 0U)  mode |= FA_CREATE_ALWAYS;
    else if ((flags & SDK_FILE_APPEND) != 0U) mode |= FA_OPEN_APPEND;
    else if ((flags & SDK_FILE_CREATE) != 0U) mode |= FA_OPEN_ALWAYS;

    AppFiles_FullPath(full, is_res, rel);
    if ((mode & (FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)) != 0U)
    {
        AppFiles_MakeParents(full);
    }

    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);
    FRESULT res = f_open(&s_files[h].fil, full, mode);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    if (res != FR_OK)
    {
        return AppFiles_MapResult(res);
    }

    s_files[h].used = 1U;
    return (int32_t)h;
}

int32_t AppFiles_Read(uint32_t handle, void *buf, uint32_t len)
{
    AppFile_t *f = AppFiles_Get(handle);
    if (f == NULL)
    {
        return SDK_ERR_INVALID;
    }
    if (len == 0U)
    {
        return 0;
    }
    if ((len > 0x7FFFFFFFUL) || !AppLoader_IsUserBufferValid(buf, len))
    {
        return SDK_ERR_INVALID;
    }

    UINT br = 0U;
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);
    FRESULT res = f_read(&f->fil, buf, (UINT)len, &br);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    return (res == FR_OK) ? (int32_t)br : AppFiles_MapResult(res);
}

int32_t AppFiles_Write(uint32_t handle, const void *buf, uint32_t len)
{
    AppFile_t *f = AppFiles_Get(handle);
    if (f == NULL)
    {
        return SDK_ERR_INVALID;
    }
    if (len == 0U)
    {
        return 0;
    }
    if ((len > 0x7FFFFFFFUL) || !AppLoader_IsUserBufferValid(buf, len))
    {
        return SDK_ERR_INVALID;
    }

    UINT bw = 0U;
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);
    FRESULT res = f_write(&f->fil, buf, (UINT)len, &bw);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    return (res == FR_OK) ? (int32_t)bw : AppFiles_MapResult(res);
}

int32_t AppFiles_Seek(uint32_t handle, uint32_t offset)
{
    AppFile_t *f = AppFiles_Get(handle);
    if (f == NULL)
    {
        return SDK_ERR_INVALID;
    }
    return AppFiles_MapResult(f_lseek(&f->fil, (FSIZE_t)offset));
}

int32_t AppFiles_Close(uint32_t handle)
{
    AppFile_t *f = AppFiles_Get(handle);
    if (f == NULL)
    {
        return SDK_ERR_INVALID;
    }
    FRESULT res = f_close(&f->fil);
    f->used = 0U;
    return AppFiles_MapResult(res);
}

int32_t AppFiles_Size(uint32_t handle)
{
    AppFile_t *f = AppFiles_Get(handle);
    if (f == NULL)
    {
        return SDK_ERR_INVALID;
    }
    return (int32_t)f_size(&f->fil);
}

/* Returns 1 and fills *out with the index-th entry (names longer than SDK_DIR_NAME_MAX are skipped), 0 at the end */
int32_t AppFiles_DirRead(const char *dir, uint32_t index, SDK_DirEntry_t *out)
{
    char rel[SDK_FILE_PATH_MAX + 1U];
    char full[APP_FILES_FULL_MAX];
    bool is_res;

    if (!AppLoader_IsUserBufferValid(out, sizeof(*out)) || !AppFiles_CopyPath(dir, rel, true, &is_res))
    {
        return SDK_ERR_INVALID;
    }
    if (!SDCard_IsMounted())
    {
        return SDK_ERR_NO_SD;
    }

    AppFiles_FullPath(full, is_res, rel);
    FRESULT res = f_opendir(&s_dir, full);
    if (res != FR_OK)
    {
        return AppFiles_MapResult(res);
    }

    int32_t result = 0;
    uint32_t n = 0U;
    for (;;)
    {
        res = f_readdir(&s_dir, &s_info);
        if ((res != FR_OK) || (s_info.fname[0] == '\0'))
        {
            result = (res == FR_OK) ? 0 : AppFiles_MapResult(res);
            break;
        }
        size_t name_len = strlen(s_info.fname);
        if (name_len > SDK_DIR_NAME_MAX)
        {
            continue;
        }
        if (n++ == index)
        {
            memset(out, 0, sizeof(*out));
            out->size   = (uint32_t)s_info.fsize;
            out->is_dir = ((s_info.fattrib & AM_DIR) != 0U) ? 1U : 0U;
            memcpy(out->name, s_info.fname, name_len);
            result = 1;
            break;
        }
    }
    (void)f_closedir(&s_dir);
    return result;
}

int32_t AppFiles_Remove(const char *path)
{
    char rel[SDK_FILE_PATH_MAX + 1U];
    char full[APP_FILES_FULL_MAX];
    bool is_res;

    if (!AppFiles_CopyPath(path, rel, false, &is_res))
    {
        return SDK_ERR_INVALID;
    }
    if (is_res)
    {
        return SDK_ERR_DENIED;
    }
    if (!SDCard_IsMounted())
    {
        return SDK_ERR_NO_SD;
    }

    AppFiles_FullPath(full, false, rel);
    return AppFiles_MapResult(f_unlink(full));
}

void AppFiles_CloseAll(void)
{
    for (uint32_t h = 0U; h < SDK_FILE_MAX_OPEN; h++)
    {
        if (s_files[h].used != 0U)
        {
            (void)f_close(&s_files[h].fil);
            s_files[h].used = 0U;
        }
    }
}
