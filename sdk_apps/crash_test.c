#include "console_sdk.h"

/* =========================================================================
 * Crash Test: verifies that faults and hangs of an app do not stop the OS.
 * ========================================================================= */

static void draw_menu(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_draw_text(10, 10, "CRASH TEST", COLOR_CYAN, COLOR_DARK_BG, 2);
    sys_draw_text(10, 50, "LEFT  : WRITE GPIO REGISTER (MEMMANAGE)", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, 66, "RIGHT : UNDEFINED INSTRUCTION (USAGE)", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, 82, "DOWN  : READ SCB REGISTER (BUSFAULT)", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, 98, "UP    : HANG (LONG PRESS KEY3 TO KILL)", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, 114, "A     : NORMAL EXIT", COLOR_WHITE, COLOR_DARK_BG, 1);
}

__attribute__((section(".text.entry"))) void _start(void)
{
    draw_menu();

    while (sys_get_keys() & (KEY_A | KEY_EXIT))
    {
        sys_delay_ms(10);
    }

    uint32_t prev_keys = 0;
    for (;;)
    {
        uint32_t keys = sys_get_keys();
        uint32_t just_pressed = keys & ~prev_keys;
        prev_keys = keys;

        if (just_pressed & KEY_LEFT)
        {
            sys_draw_text(10, 150, "WRITING GPIOE->ODR...", COLOR_RED, COLOR_DARK_BG, 1);
            *(volatile uint32_t *)0x40021014UL = 0U;
        }
        if (just_pressed & KEY_RIGHT)
        {
            sys_draw_text(10, 150, "EXECUTING UDF...", COLOR_RED, COLOR_DARK_BG, 1);
            __asm volatile ("udf #0");
        }
        if (just_pressed & KEY_DOWN)
        {
            sys_draw_text(10, 150, "READING SCB->CPUID...", COLOR_RED, COLOR_DARK_BG, 1);
            (void)*(volatile uint32_t *)0xE000ED00UL;
        }
        if (just_pressed & KEY_UP)
        {
            sys_draw_text(10, 150, "HANGING. HOLD KEY3 1S", COLOR_RED, COLOR_DARK_BG, 1);
            for (;;)
            {
            }
        }
        if (just_pressed & (KEY_A | KEY_EXIT))
        {
            sys_exit();
            return;
        }

        sys_delay_ms(16);
    }
}
