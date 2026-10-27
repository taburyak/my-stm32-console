#ifndef CONSOLE_SDK_H
#define CONSOLE_SDK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ==========================================================================
 * 1. Memory Sandbox Definitions
 * ========================================================================== */
#define SDK_CODE_BASE_ADDR      0x20018000UL /* 16 KB 0-Wait-State Internal SRAM2 */
#define SDK_EXT_RAM_BASE_ADDR   0x68080000UL /* 512 KB External FSMC SRAM for App Data */

/* ==========================================================================
 * 2. Unified Input Bitmask (Returned by sys_get_keys / SVC 6)
 * ========================================================================== */
#define KEY_LEFT                (1UL << 0) /* Gamepad Left  / Keyboard Left Arrow or 'A' / Board KEY_0 */
#define KEY_RIGHT               (1UL << 1) /* Gamepad Right / Keyboard Right Arrow or 'D'/ Board KEY_2 */
#define KEY_UP                  (1UL << 2) /* Gamepad Up    / Keyboard Up Arrow or 'W'   / Board KEY_1 */
#define KEY_DOWN                (1UL << 3) /* Gamepad Down  / Keyboard Down Arrow or 'S' */
#define KEY_A                   (1UL << 4) /* Gamepad 'A'   / Keyboard Space or Enter    */
#define KEY_B                   (1UL << 5) /* Gamepad 'X'   / Keyboard 'X' or 'Z'        */
#define KEY_X                   (1UL << 6)
#define KEY_EXIT                (1UL << 7) /* Gamepad 'B' or Select / Keyboard ESC / Board KEY_3 */

/* ==========================================================================
 * 3. RGB565 Color Macros & Palette
 * ========================================================================== */
#define RGB565(r, g, b)         ((uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | (((b) & 0xF8U) >> 3)))

#define COLOR_BLACK             0x0000U
#define COLOR_WHITE             0xFFFFU
#define COLOR_CYAN              0x07FFU
#define COLOR_NEON_GREEN        0x07E0U
#define COLOR_AMBER             0xFD20U
#define COLOR_RED               0xF800U
#define COLOR_BLUE              0x001FU
#define COLOR_MAGENTA           0xF81FU
#define COLOR_YELLOW            0xFFE0U
#define COLOR_DARK_BG           0x0862U
#define COLOR_PANEL_BG          0x10A4U
#define COLOR_GRID_LINE         0x1926U

/* ==========================================================================
 * 4. Shared Structures with OS Kernel
 * ========================================================================== */
typedef struct {
    uint16_t    x;
    uint16_t    y;
    const char *str;
    uint16_t    fg_color;
    uint16_t    bg_color;
    uint8_t     scale;
} SDK_DrawTextParams_t;

/* ==========================================================================
 * 5. Freestanding Memory Helpers (-nostdlib & GCC 14 -O2 safe)
 * ========================================================================== */

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

#define sdk_memset  memset
#define sdk_memcpy  memcpy
#define sdk_memmove memmove

/* Convert unsigned integer to string (avoids bulky sprintf in -nostdlib) */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static inline void sdk_uint_to_str(uint32_t val, char *buf, uint8_t min_digits)
{
    char tmp[12];
    uint8_t i = 0;
    if (val == 0U)
    {
        tmp[i++] = '0';
    }
    else
    {
        while (val > 0U && i < 10U)
        {
            tmp[i++] = (char)('0' + (val % 10U));
            val /= 10U;
        }
    }
    while (i < min_digits && i < 10U)
    {
        tmp[i++] = '0';
    }
    uint8_t j = 0;
    while (i > 0U)
    {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

/* ==========================================================================
 * 6. OS System Calls (SVC Wrappers - Strictly Non-Inlined for Register Safety)
 * ========================================================================== */

__attribute__((noinline)) static void sys_led_toggle(uint32_t led_id)
{
    register uint32_t r0 __asm("r0") = led_id;
    __asm volatile ("svc 1" : : "r"(r0) : "memory");
}

__attribute__((noinline)) static void sys_beep(uint16_t duration_ms)
{
    register uint32_t r0 __asm("r0") = (uint32_t)duration_ms;
    __asm volatile ("svc 2" : : "r"(r0) : "memory");
}

__attribute__((noinline)) static uint32_t sys_get_tick(void)
{
    register uint32_t r0 __asm("r0");
    __asm volatile ("svc 3" : "=r"(r0) : : "memory");
    return r0;
}

__attribute__((noinline)) static void sys_lcd_fill_raw(uint32_t packed_xy, uint32_t packed_wh, uint32_t color)
{
    register uint32_t r0 __asm("r0") = packed_xy;
    register uint32_t r1 __asm("r1") = packed_wh;
    register uint32_t r2 __asm("r2") = color;
    __asm volatile ("svc 5" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
}

static inline void sys_lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t xy = ((uint32_t)x << 16) | ((uint32_t)y & 0xFFFFU);
    uint32_t wh = ((uint32_t)w << 16) | ((uint32_t)h & 0xFFFFU);
    sys_lcd_fill_raw(xy, wh, (uint32_t)color);
}

__attribute__((noinline)) static uint32_t sys_get_keys(void)
{
    register uint32_t r0 __asm("r0");
    __asm volatile ("svc 6" : "=r"(r0) : : "memory");
    return r0;
}

__attribute__((noinline)) static void sys_exit(void)
{
    __asm volatile ("svc 8" : : : "memory");
}

__attribute__((noinline)) static void sys_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    register uint32_t r0 __asm("r0") = (uint32_t)x;
    register uint32_t r1 __asm("r1") = (uint32_t)y;
    register uint32_t r2 __asm("r2") = (uint32_t)color;
    __asm volatile ("svc 9" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
}

__attribute__((noinline)) static void sys_draw_sprite_raw(uint32_t packed_xy, uint32_t packed_wh, const uint16_t *bmp)
{
    register uint32_t r0 __asm("r0") = packed_xy;
    register uint32_t r1 __asm("r1") = packed_wh;
    register uint32_t r2 __asm("r2") = (uint32_t)bmp;
    __asm volatile ("svc 10" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
}

static inline void sys_draw_sprite(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *bmp)
{
    uint32_t xy = ((uint32_t)x << 16) | ((uint32_t)y & 0xFFFFU);
    uint32_t wh = ((uint32_t)w << 16) | ((uint32_t)h & 0xFFFFU);
    sys_draw_sprite_raw(xy, wh, bmp);
}

__attribute__((noinline)) static void sys_draw_text_ptr(const SDK_DrawTextParams_t *params)
{
    register uint32_t r0 __asm("r0") = (uint32_t)params;
    /* Note: "m"(*params) forces GCC at -O2 to flush struct fields to stack before SVC 11! */
    __asm volatile ("svc 11" : "+r"(r0) : "m"(*params) : "memory");
}

static inline void sys_draw_text(uint16_t x, uint16_t y, const char *str,
                                 uint16_t fg_color, uint16_t bg_color, uint8_t scale)
{
    volatile SDK_DrawTextParams_t p;
    p.x        = x;
    p.y        = y;
    p.str      = str;
    p.fg_color = fg_color;
    p.bg_color = bg_color;
    p.scale    = scale;
    sys_draw_text_ptr((const SDK_DrawTextParams_t *)&p);
}

__attribute__((noinline)) static void sys_clear_screen(uint16_t color)
{
    register uint32_t r0 __asm("r0") = (uint32_t)color;
    __asm volatile ("svc 12" : "+r"(r0) : : "memory");
}

__attribute__((noinline)) static void sys_play_tone(uint16_t freq_hz, uint16_t duration_ms)
{
    register uint32_t r0 __asm("r0") = (uint32_t)freq_hz;
    register uint32_t r1 __asm("r1") = (uint32_t)duration_ms;
    __asm volatile ("svc 14" : "+r"(r0) : "r"(r1) : "memory");
}

__attribute__((noinline)) static bool sys_save_state(const char *filename, const void *data, uint32_t len)
{
    register uint32_t r0 __asm("r0") = (uint32_t)filename;
    register uint32_t r1 __asm("r1") = (uint32_t)data;
    register uint32_t r2 __asm("r2") = len;
    __asm volatile ("svc 15" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
    return (r0 != 0U);
}

__attribute__((noinline)) static uint32_t sys_load_state(const char *filename, void *dst, uint32_t max_len)
{
    register uint32_t r0 __asm("r0") = (uint32_t)filename;
    register uint32_t r1 __asm("r1") = (uint32_t)dst;
    register uint32_t r2 __asm("r2") = max_len;
    __asm volatile ("svc 16" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
    return r0;
}

static inline void sys_delay_ms(uint32_t ms)
{
    uint32_t start = sys_get_tick();
    while ((sys_get_tick() - start) < ms)
    {
        __asm volatile ("wfi" : : : "memory");
    }
}

#endif /* CONSOLE_SDK_H */
