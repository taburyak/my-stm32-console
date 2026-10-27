#ifndef APP_FILES_H
#define APP_FILES_H

#include "console_abi.h"
#include <stdint.h>

/* SD file access for the sandboxed app (SDK_SVC_FILE_*); called from the syscall gate */
int32_t AppFiles_Open(const char *path, uint32_t flags);
int32_t AppFiles_Read(uint32_t handle, void *buf, uint32_t len);
int32_t AppFiles_Write(uint32_t handle, const void *buf, uint32_t len);
int32_t AppFiles_Seek(uint32_t handle, uint32_t offset);
int32_t AppFiles_Close(uint32_t handle);
int32_t AppFiles_Size(uint32_t handle);
int32_t AppFiles_DirRead(const char *dir, uint32_t index, SDK_DirEntry_t *out);
int32_t AppFiles_Remove(const char *path);

/* Closes every handle left open by the app (privileged task context) */
void AppFiles_CloseAll(void);

#endif /* APP_FILES_H */
