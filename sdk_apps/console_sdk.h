#ifndef CONSOLE_SDK_H
#define CONSOLE_SDK_H

#include <stdint.h>
#include <stdbool.h>

/* Screen dimensions */
#define LCD_W 320
#define LCD_H 240

/* RGB565 Color helper */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

/* Input Key Bitmask */
#define KEY_LEFT   (1UL << 0)
#define KEY_RIGHT  (1UL << 1)
#define KEY_UP     (1UL << 2)
#define KEY_DOWN   (1UL << 3)
#define KEY_A      (1UL << 4)
#define KEY_B      (1UL << 5)
#define KEY_EXIT   (1UL << 7)

/* Low-level ABI-safe syscall wrappers (noinline guarantees r0, r1, r2 follow ARM AAPCS!) */
__attribute__((noinline)) static void sys_led_toggle(uint32_t led_id)
{
    __asm volatile ("svc 1" : "+r"(led_id) : : "memory");
}

__attribute__((noinline)) static void sys_beep(uint32_t duration_ms)
{
    __asm volatile ("svc 2" : "+r"(duration_ms) : : "memory");
}

__attribute__((noinline)) static uint32_t sys_get_tick(void)
{
    uint32_t tick = 0;
    __asm volatile ("svc 3" : "=r"(tick) : : "memory");
    return tick;
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
    uint32_t keys = 0;
    __asm volatile ("svc 6" : "=r"(keys) : : "memory");
    return keys;
}

__attribute__((noinline)) static void sys_exit(void)
{
    __asm volatile ("svc 8" : : : "memory");
}

/* Delay in milliseconds in Thread Mode (SysTick & TIM4 interrupts run freely!) */
static inline void sys_delay_ms(uint32_t ms)
{
    uint32_t start = sys_get_tick();
    while ((sys_get_tick() - start) < ms)
    {
        for (volatile uint32_t i = 0; i < 1000U; i++)
        {
            __asm volatile ("nop");
        }
    }
}

#endif /* CONSOLE_SDK_H */