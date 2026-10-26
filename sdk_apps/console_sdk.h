#ifndef CONSOLE_SDK_H
#define CONSOLE_SDK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* =========================================================================
 * 1. Screen & Color Constants
 * ========================================================================= */
#define LCD_W 320
#define LCD_H 240

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

/* Common Cyberpunk Palette */
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_CYBER_BG  RGB565(12, 8, 28)
#define COLOR_MAGENTA   RGB565(255, 0, 128)
#define COLOR_MINT      RGB565(0, 255, 180)
#define COLOR_GOLD      RGB565(255, 215, 0)
#define COLOR_CYAN      RGB565(0, 230, 255)
#define COLOR_RED       RGB565(255, 50, 60)

/* =========================================================================
 * 2. Input Bitmasks & Structures
 * ========================================================================= */
#define KEY_LEFT   (1UL << 0)
#define KEY_RIGHT  (1UL << 1)
#define KEY_UP     (1UL << 2)
#define KEY_DOWN   (1UL << 3)
#define KEY_A      (1UL << 4)
#define KEY_B      (1UL << 5)
#define KEY_X      (1UL << 6)
#define KEY_EXIT   (1UL << 7)

typedef struct {
    int8_t   left_x;
    int8_t   left_y;
    uint8_t  hat;
    uint16_t buttons;
    bool     dpad_up;
    bool     dpad_down;
    bool     dpad_left;
    bool     dpad_right;
    bool     btn_a;
    bool     btn_b;
    bool     btn_x;
    bool     btn_y;
    bool     btn_l1;
    bool     btn_r1;
    bool     btn_l2;
    bool     btn_r2;
    bool     btn_start;
    bool     btn_select;
} SDK_GamepadState_t;

typedef struct {
    int16_t x;
    int16_t y;
    int8_t  delta_x;
    int8_t  delta_y;
    bool    btn_left;
    bool    btn_right;
    bool    btn_middle;
} SDK_MouseEvent_t;

typedef struct {
    uint16_t    x;
    uint16_t    y;
    const char *str;
    uint16_t    fg_color;
    uint16_t    bg_color;
    uint8_t     scale;
} SDK_DrawTextParams_t;

/* =========================================================================
 * 3. Low-Level ABI-Safe Syscall Wrappers (SVC 1..16)
 * ========================================================================= */

/* SVC 1: Toggle board status LED (0=Heartbeat, 1=Disk, 2=Error) */
__attribute__((noinline)) static void sys_led_toggle(uint32_t led_id)
{
    register uint32_t r0 __asm("r0") = led_id;
    __asm volatile ("svc 1" : "+r"(r0) : : "memory");
}

/* SVC 2: Short non-blocking piezo beep */
__attribute__((noinline)) static void sys_beep(uint32_t duration_ms)
{
    register uint32_t r0 __asm("r0") = duration_ms;
    __asm volatile ("svc 2" : "+r"(r0) : : "memory");
}

/* SVC 3: System uptime in milliseconds */
__attribute__((noinline)) static uint32_t sys_get_tick(void)
{
    register uint32_t r0 __asm("r0");
    __asm volatile ("svc 3" : "=r"(r0) : : "memory");
    return r0;
}

/* SVC 4: Read full USB Gamepad state */
__attribute__((noinline)) static bool sys_get_gamepad(SDK_GamepadState_t *dst)
{
    register uint32_t r0 __asm("r0") = (uint32_t)dst;
    __asm volatile ("svc 4" : "+r"(r0) : : "memory");
    return (r0 != 0U);
}

/* SVC 5: Fill rectangle on LCD */
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

/* SVC 6: Get unified input key bitmask */
__attribute__((noinline)) static uint32_t sys_get_keys(void)
{
    register uint32_t r0 __asm("r0");
    __asm volatile ("svc 6" : "=r"(r0) : : "memory");
    return r0;
}

/* SVC 8: Terminate sandbox application and return to LVGL Games Hub */
__attribute__((noinline)) static void sys_exit(void)
{
    __asm volatile ("svc 8" : : : "memory");
}

/* SVC 9: Draw single RGB565 pixel */
__attribute__((noinline)) static void sys_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    register uint32_t r0 __asm("r0") = x;
    register uint32_t r1 __asm("r1") = y;
    register uint32_t r2 __asm("r2") = color;
    __asm volatile ("svc 9" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
}

/* SVC 10: Blit RGB565 sprite array directly to LCD GRAM */
__attribute__((noinline)) static void sys_draw_sprite_raw(uint32_t packed_xy, uint32_t packed_wh, const uint16_t *bitmap)
{
    register uint32_t r0 __asm("r0") = packed_xy;
    register uint32_t r1 __asm("r1") = packed_wh;
    register uint32_t r2 __asm("r2") = (uint32_t)bitmap;
    __asm volatile ("svc 10" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
}

static inline void sys_draw_sprite(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *bitmap)
{
    uint32_t xy = ((uint32_t)x << 16) | ((uint32_t)y & 0xFFFFU);
    uint32_t wh = ((uint32_t)w << 16) | ((uint32_t)h & 0xFFFFU);
    sys_draw_sprite_raw(xy, wh, bitmap);
}

/* SVC 11: Draw ASCII string using Kernel built-in 5x7 font */
__attribute__((noinline)) static void sys_draw_text_raw(const SDK_DrawTextParams_t *params)
{
    register uint32_t r0 __asm("r0") = (uint32_t)params;
    __asm volatile ("svc 11" : "+r"(r0) : : "memory");
}

static inline void sys_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t fg_col, uint16_t bg_col, uint8_t scale)
{
    SDK_DrawTextParams_t p = {
        .x        = x,
        .y        = y,
        .str      = str,
        .fg_color = fg_col,
        .bg_color = bg_col,
        .scale    = scale
    };
    sys_draw_text_raw(&p);
}

/* SVC 12: Instant full-screen clear (320x240) */
__attribute__((noinline)) static void sys_clear_screen(uint16_t color)
{
    register uint32_t r0 __asm("r0") = color;
    __asm volatile ("svc 12" : "+r"(r0) : : "memory");
}

/* SVC 13: Read USB Mouse or Touchscreen state */
__attribute__((noinline)) static bool sys_get_mouse(SDK_MouseEvent_t *dst)
{
    register uint32_t r0 __asm("r0") = (uint32_t)dst;
    __asm volatile ("svc 13" : "+r"(r0) : : "memory");
    return (r0 != 0U);
}

/* SVC 14: Play tone (respects system mute setting) */
__attribute__((noinline)) static void sys_play_tone(uint16_t freq_hz, uint16_t duration_ms)
{
    register uint32_t r0 __asm("r0") = freq_hz;
    register uint32_t r1 __asm("r1") = duration_ms;
    __asm volatile ("svc 14" : "+r"(r0) : "r"(r1) : "memory");
}

/* SVC 15: Save game state / high score to /saves/<filename> on SD card */
__attribute__((noinline)) static bool sys_save_state(const char *filename, const void *data, uint32_t len)
{
    register uint32_t r0 __asm("r0") = (uint32_t)filename;
    register uint32_t r1 __asm("r1") = (uint32_t)data;
    register uint32_t r2 __asm("r2") = len;
    __asm volatile ("svc 15" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
    return (r0 != 0U);
}

/* SVC 16: Load game state / high score from /saves/<filename> on SD card */
__attribute__((noinline)) static uint32_t sys_load_state(const char *filename, void *dst, uint32_t max_len)
{
    register uint32_t r0 __asm("r0") = (uint32_t)filename;
    register uint32_t r1 __asm("r1") = (uint32_t)dst;
    register uint32_t r2 __asm("r2") = max_len;
    __asm volatile ("svc 16" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
    return r0;
}

/* =========================================================================
 * 4. High-Level SDK Helpers (Numbers, Timing, Input Edge Detection)
 * ========================================================================= */

/**
 * @brief Render an unsigned integer with clean background erasure
 */
static inline void sys_draw_number_padded(uint16_t x, uint16_t y, uint32_t num,
                                          uint16_t fg, uint16_t bg, uint8_t scale, uint8_t digits)
{
    char buf[12];
    if (digits > 10U) digits = 10U;
    buf[digits] = '\0';

    for (int i = (int)digits - 1; i >= 0; i--)
    {
        buf[i] = (char)('0' + (num % 10U));
        num /= 10U;
    }
    sys_draw_text(x, y, buf, fg, bg, scale);
}

static inline void sys_delay_ms(uint32_t ms)
{
    uint32_t start = sys_get_tick();
    while ((sys_get_tick() - start) < ms)
    {
        __asm volatile ("wfi" : : : "memory");
    }
}

/**
 * @brief Helper for button edge detection (just pressed) & auto-repeat
 */
typedef struct {
    uint32_t prev_keys;
    uint32_t last_repeat_tick;
} SDK_InputTracker_t;

static inline uint32_t sdk_input_poll_edges(SDK_InputTracker_t *trk, uint32_t current_keys)
{
    uint32_t edges = current_keys & (~trk->prev_keys);
    trk->prev_keys = current_keys;
    return edges;
}

/* =========================================================================
 * 5. Freestanding Memory Functions (-nostdlib support)
 * ========================================================================= */
__attribute__((used)) static inline void *memset(void *s, int c, size_t n)
{
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

__attribute__((used)) static inline void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

__attribute__((used)) static inline void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s)
    {
        while (n--) *d++ = *s++;
    }
    else
    {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

#endif /* CONSOLE_SDK_H */
