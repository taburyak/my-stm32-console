#ifndef CONSOLE_SDK_H
#define CONSOLE_SDK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "console_abi.h"

/* ==========================================================================
 * 1. Memory Sandbox Definitions
 * ========================================================================== */
#define SDK_CODE_BASE_ADDR      0x20018000UL /* 28 KB 0-WS internal SRAM: code + data + BSS */
#define SDK_CODE_MAX_SIZE       (28UL * 1024UL)
#define SDK_STACK_SIZE          (4UL * 1024UL) /* App stack, provided by the OS */
#define SDK_EXT_RAM_BASE_ADDR   0x68080000UL /* 512 KB External FSMC SRAM for App Data */

/* ==========================================================================
 * 2. Unified Input Bitmask (Returned by sys_get_keys), see console_abi.h
 * ========================================================================== */
#define KEY_LEFT                SDK_KEY_LEFT
#define KEY_RIGHT               SDK_KEY_RIGHT
#define KEY_UP                  SDK_KEY_UP
#define KEY_DOWN                SDK_KEY_DOWN
#define KEY_A                   SDK_KEY_A
#define KEY_B                   SDK_KEY_B
#define KEY_X                   SDK_KEY_X
#define KEY_EXIT                SDK_KEY_EXIT

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
 * 4. Shared Structures with OS Kernel: see console_abi.h
 * ========================================================================== */

/* ==========================================================================
 * 5. Freestanding Memory Helpers (defined in sdk_runtime.c)
 * ========================================================================== */

void *memset(void *dst, int val, size_t len);
void *memcpy(void *dst, const void *src, size_t len);
void *memmove(void *dst, const void *src, size_t len);

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
 * 6. OS System Calls
 *    Arguments are evaluated into temporaries before being bound to r0..r2
 *    (GCC may otherwise reuse r0 while computing r1). Wrappers stay noinline.
 *    Blocking syscalls run through the kernel gate and clobber r1-r3, r12, flags.
 * ========================================================================== */

#define SDK_SVC_CLOBBERS "r3", "r12", "cc", "memory"

#define SDK_SVC_CALL0(num) __extension__ ({                                                     \
    register uint32_t _r0 __asm("r0");                                                         \
    __asm volatile ("svc %[n]" : "=r"(_r0) : [n] "I"(num) : "r1", "r2", SDK_SVC_CLOBBERS);     \
    _r0; })

#define SDK_SVC_CALL1(num, a0) __extension__ ({                                                 \
    uint32_t _a0 = (uint32_t)(a0);                                                             \
    register uint32_t _r0 __asm("r0") = _a0;                                                   \
    __asm volatile ("svc %[n]" : "+r"(_r0) : [n] "I"(num) : "r1", "r2", SDK_SVC_CLOBBERS);     \
    _r0; })

#define SDK_SVC_CALL2(num, a0, a1) __extension__ ({                                             \
    uint32_t _a0 = (uint32_t)(a0), _a1 = (uint32_t)(a1);                                       \
    register uint32_t _r0 __asm("r0") = _a0;                                                   \
    register uint32_t _r1 __asm("r1") = _a1;                                                   \
    __asm volatile ("svc %[n]" : "+r"(_r0), "+r"(_r1) : [n] "I"(num) : "r2", SDK_SVC_CLOBBERS); \
    _r0; })

#define SDK_SVC_CALL3(num, a0, a1, a2) __extension__ ({                                         \
    uint32_t _a0 = (uint32_t)(a0), _a1 = (uint32_t)(a1), _a2 = (uint32_t)(a2);                 \
    register uint32_t _r0 __asm("r0") = _a0;                                                   \
    register uint32_t _r1 __asm("r1") = _a1;                                                   \
    register uint32_t _r2 __asm("r2") = _a2;                                                   \
    __asm volatile ("svc %[n]" : "+r"(_r0), "+r"(_r1), "+r"(_r2) : [n] "I"(num) : SDK_SVC_CLOBBERS); \
    _r0; })

#define SDK_SYSCALL __attribute__((noinline, unused)) static

SDK_SYSCALL void sys_led_toggle(uint32_t led_id)
{
    (void)SDK_SVC_CALL1(SDK_SVC_LED_TOGGLE, led_id);
}

SDK_SYSCALL void sys_beep(uint16_t duration_ms)
{
    (void)SDK_SVC_CALL1(SDK_SVC_BEEP, duration_ms);
}

SDK_SYSCALL uint32_t sys_get_tick(void)
{
    return SDK_SVC_CALL0(SDK_SVC_GET_TICK);
}

SDK_SYSCALL bool sys_get_gamepad(SDK_GamepadState_t *dst)
{
    return SDK_SVC_CALL1(SDK_SVC_GET_GAMEPAD, dst) != 0U;
}

SDK_SYSCALL void sys_lcd_fill_raw(uint32_t packed_xy, uint32_t packed_wh, uint32_t color)
{
    (void)SDK_SVC_CALL3(SDK_SVC_LCD_FILL_RECT, packed_xy, packed_wh, color);
}

static inline void sys_lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t xy = ((uint32_t)x << 16) | ((uint32_t)y & 0xFFFFU);
    uint32_t wh = ((uint32_t)w << 16) | ((uint32_t)h & 0xFFFFU);
    sys_lcd_fill_raw(xy, wh, (uint32_t)color);
}

SDK_SYSCALL uint32_t sys_get_keys(void)
{
    return SDK_SVC_CALL0(SDK_SVC_GET_KEYS);
}

SDK_SYSCALL void sys_exit(void)
{
    (void)SDK_SVC_CALL0(SDK_SVC_EXIT);
}

SDK_SYSCALL void sys_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    (void)SDK_SVC_CALL3(SDK_SVC_DRAW_PIXEL, x, y, color);
}

SDK_SYSCALL void sys_draw_sprite_raw(uint32_t packed_xy, uint32_t packed_wh, const uint16_t *bmp)
{
    (void)SDK_SVC_CALL3(SDK_SVC_DRAW_SPRITE, packed_xy, packed_wh, bmp);
}

static inline void sys_draw_sprite(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *bmp)
{
    uint32_t xy = ((uint32_t)x << 16) | ((uint32_t)y & 0xFFFFU);
    uint32_t wh = ((uint32_t)w << 16) | ((uint32_t)h & 0xFFFFU);
    sys_draw_sprite_raw(xy, wh, bmp);
}

SDK_SYSCALL void sys_draw_text_ptr(const SDK_DrawTextParams_t *params)
{
    (void)SDK_SVC_CALL1(SDK_SVC_DRAW_TEXT, params);
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

SDK_SYSCALL void sys_clear_screen(uint16_t color)
{
    (void)SDK_SVC_CALL1(SDK_SVC_CLEAR_SCREEN, color);
}

/* USB mouse, or the touch screen when no mouse is connected */
SDK_SYSCALL bool sys_get_mouse(SDK_MouseState_t *dst)
{
    return SDK_SVC_CALL1(SDK_SVC_GET_MOUSE, dst) != 0U;
}

SDK_SYSCALL void sys_play_tone(uint16_t freq_hz, uint16_t duration_ms)
{
    (void)SDK_SVC_CALL2(SDK_SVC_PLAY_TONE, freq_hz, duration_ms);
}

SDK_SYSCALL bool sys_save_state(const char *filename, const void *data, uint32_t len)
{
    return SDK_SVC_CALL3(SDK_SVC_SAVE_STATE, filename, data, len) != 0U;
}

SDK_SYSCALL uint32_t sys_load_state(const char *filename, void *dst, uint32_t max_len)
{
    return SDK_SVC_CALL3(SDK_SVC_LOAD_STATE, filename, dst, max_len);
}

SDK_SYSCALL bool sys_get_info(SDK_SysInfo_t *dst)
{
    return SDK_SVC_CALL1(SDK_SVC_GET_INFO, dst) != 0U;
}

/* Blocks the app task (CPU goes to other tasks); at most SDK_SLEEP_MAX_MS per call */
SDK_SYSCALL void sys_sleep_ms(uint32_t ms)
{
    (void)SDK_SVC_CALL1(SDK_SVC_SLEEP_MS, ms);
}

static inline void sys_delay_ms(uint32_t ms)
{
    while (ms > SDK_SLEEP_MAX_MS)
    {
        sys_sleep_ms(SDK_SLEEP_MAX_MS);
        ms -= SDK_SLEEP_MAX_MS;
    }
    sys_sleep_ms(ms);
}

#endif /* CONSOLE_SDK_H */
