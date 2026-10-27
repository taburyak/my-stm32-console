#include "console_sdk.h"

/* App startup: image header for the loader and the entry that calls app_main(). */

#ifndef SDK_APP_NAME_STR
#define SDK_APP_NAME_STR "app"
#endif

extern uint8_t __sdk_image_size[];
extern uint8_t __sdk_bss_end[];

void app_main(void);
void _sdk_entry(void);

__attribute__((section(".app_header"), used))
const SDK_AppHeader_t g_sdk_app_header = {
    .magic       = SDK_APP_MAGIC,
    .api_version = SDK_API_VERSION,
    .header_size = sizeof(SDK_AppHeader_t),
    .image_size  = (uint32_t)__sdk_image_size,
    .bss_end     = (uint32_t)__sdk_bss_end,
    .entry       = (uint32_t)_sdk_entry,
    .name        = SDK_APP_NAME_STR,
};

/* .bss and the external data area are zeroed by the loader */
void _sdk_entry(void)
{
    app_main();
    sys_exit();
    for (;;)
    {
    }
}

/* -nostdlib: GCC emits calls to these for struct copies and loops */
__attribute__((used, optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int val, size_t len)
{
    uint8_t *p = (uint8_t *)dst;
    while (len--)
    {
        *p++ = (uint8_t)val;
    }
    return dst;
}

__attribute__((used, optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (len--)
    {
        *d++ = *s++;
    }
    return dst;
}

__attribute__((used, optimize("no-tree-loop-distribute-patterns")))
void *memmove(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s)
    {
        while (len--)
        {
            *d++ = *s++;
        }
    }
    else if (d > s)
    {
        d += len;
        s += len;
        while (len--)
        {
            *--d = *--s;
        }
    }
    return dst;
}
