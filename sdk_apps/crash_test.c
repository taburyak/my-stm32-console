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
    sys_draw_text(10, 114, "PAD X / Z : KERNEL SRAM 0X20000100", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, 130, "A / SPACE : KERNEL EXT SRAM 0X68000000", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, 146, "ESC / PAD B : NORMAL EXIT", COLOR_WHITE, COLOR_DARK_BG, 1);
}

void app_main(void)
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
            sys_draw_text(10, 180, "WRITING GPIOE->ODR...", COLOR_RED, COLOR_DARK_BG, 1);
            *(volatile uint32_t *)0x40021014UL = 0U;
        }
        if (just_pressed & KEY_RIGHT)
        {
            sys_draw_text(10, 180, "EXECUTING UDF...", COLOR_RED, COLOR_DARK_BG, 1);
            __asm volatile ("udf #0");
        }
        if (just_pressed & KEY_DOWN)
        {
            sys_draw_text(10, 180, "READING SCB->CPUID...", COLOR_RED, COLOR_DARK_BG, 1);
            (void)*(volatile uint32_t *)0xE000ED00UL;
        }
        if (just_pressed & KEY_UP)
        {
            sys_draw_text(10, 180, "HANGING. HOLD KEY3 1S", COLOR_RED, COLOR_DARK_BG, 1);
            for (;;)
            {
            }
        }
        /* Read-modify-write of the same value: harmless if the MPU does not block it */
        if (just_pressed & KEY_B)
        {
            sys_draw_text(10, 180, "ACCESSING KERNEL SRAM...", COLOR_RED, COLOR_DARK_BG, 1);
            volatile uint32_t *p = (volatile uint32_t *)0x20000100UL;
            *p = *p;
            sys_draw_text(10, 196, "NOT BLOCKED!", COLOR_YELLOW, COLOR_DARK_BG, 2);
        }
        if (just_pressed & KEY_A)
        {
            sys_draw_text(10, 180, "ACCESSING KERNEL EXT SRAM...", COLOR_RED, COLOR_DARK_BG, 1);
            volatile uint32_t *p = (volatile uint32_t *)0x68000000UL;
            *p = *p;
            sys_draw_text(10, 196, "NOT BLOCKED!", COLOR_YELLOW, COLOR_DARK_BG, 2);
        }
        if (just_pressed & KEY_EXIT)
        {
            sys_exit();
            return;
        }

        sys_delay_ms(16);
    }
}
