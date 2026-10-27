#include "console_sdk.h"

/* =========================================================================
 * SDK Test: system info, live gamepad / mouse / key state, sleep timing.
 * ========================================================================= */

#define ROW(n)  ((uint16_t)(34U + (n) * 14U))

static void draw_hex(uint16_t x, uint16_t y, uint32_t v, uint8_t digits)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    for (uint8_t i = 0; i < digits && i < 8U; i++)
    {
        buf[i] = hex[(v >> ((digits - 1U - i) * 4U)) & 0xFU];
    }
    buf[digits] = '\0';
    sys_draw_text(x, y, buf, COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
}

static void draw_num(uint16_t x, uint16_t y, int32_t v, uint8_t min_digits)
{
    char buf[14];
    uint32_t mag = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
    char *p = buf;
    if (v < 0) *p++ = '-';
    sdk_uint_to_str(mag, p, min_digits);
    sys_draw_text(x, y, "        ", COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
    sys_draw_text(x, y, buf, COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
}

static void draw_static(const SDK_SysInfo_t *info)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_draw_text(10, 8, "SDK TEST", COLOR_CYAN, COLOR_DARK_BG, 2);

    sys_draw_text(10, ROW(0), "API:", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_num(70, ROW(0), (int32_t)info->api_version, 1);
    sys_draw_text(130, ROW(0), "SCREEN:", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_num(180, ROW(0), (int32_t)info->screen_w, 1);
    draw_num(210, ROW(0), (int32_t)info->screen_h, 1);

    sys_draw_text(10, ROW(1), "RAM:", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_num(70, ROW(1), (int32_t)(info->app_ram_size / 1024U), 1);
    sys_draw_text(130, ROW(1), "STACK:", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_num(180, ROW(1), (int32_t)(info->app_stack_size / 1024U), 1);
    sys_draw_text(220, ROW(1), "EXT:", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_num(250, ROW(1), (int32_t)(info->app_ext_size / 1024U), 1);

    sys_draw_text(10, ROW(2), "DEVICES:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(130, ROW(2), "FLAGS:", COLOR_WHITE, COLOR_DARK_BG, 1);

    sys_draw_text(10, ROW(4), "KEYS:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(5), "PAD:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(6), "PAD BTN:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(7), "PAD X/Y:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(8), "MOUSE:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(9), "MOUSE XY:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(10), "SLEEP 500MS:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(12), "A: SLEEP TEST   ESC / PAD B: EXIT", COLOR_AMBER, COLOR_DARK_BG, 1);
}

__attribute__((section(".text.entry"))) void _start(void)
{
    SDK_SysInfo_t info;
    sdk_memset(&info, 0, sizeof(info));
    (void)sys_get_info(&info);
    draw_static(&info);

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

        if (just_pressed & KEY_EXIT)
        {
            sys_exit();
            return;
        }

        if (just_pressed & KEY_A)
        {
            sys_draw_text(100, ROW(10), "...     ", COLOR_YELLOW, COLOR_DARK_BG, 1);
            uint32_t t0 = sys_get_tick();
            sys_sleep_ms(500);
            draw_num(100, ROW(10), (int32_t)(sys_get_tick() - t0), 1);
        }

        (void)sys_get_info(&info);
        draw_hex(70, ROW(2), info.devices, 2);
        draw_hex(180, ROW(2), info.flags, 2);
        draw_hex(70, ROW(4), keys, 8);

        SDK_GamepadState_t pad;
        bool pad_ok = sys_get_gamepad(&pad);
        sys_draw_text(70, ROW(5), pad_ok ? "CONNECTED " : "NONE      ", COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
        draw_hex(70, ROW(6), pad.buttons, 4);
        draw_num(70, ROW(7), pad.left_x, 1);
        draw_num(130, ROW(7), pad.left_y, 1);

        SDK_MouseState_t mse;
        bool mse_ok = sys_get_mouse(&mse);
        sys_draw_text(70, ROW(8),
                      !mse_ok ? "NONE      " : (mse.source == SDK_MOUSE_SRC_TOUCH ? "TOUCH     " : "USB       "),
                      COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
        draw_num(70, ROW(9), mse.x, 1);
        draw_num(130, ROW(9), mse.y, 1);
        draw_hex(190, ROW(9), mse.buttons, 2);

        sys_delay_ms(50);
    }
}
