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
    sys_lcd_fill_rect(0, 0, 320, 26, COLOR_PANEL_BG);
    sys_draw_text(10, 6, "SDK TEST", COLOR_CYAN, COLOR_PANEL_BG, 2);

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

    sys_draw_text(10, ROW(3), "MCU 0.1C:", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_num(75, ROW(3), sys_get_mcu_temp_deci(), 1);
    sys_draw_text(130, ROW(3), "EXT 0.1C:", COLOR_WHITE, COLOR_DARK_BG, 1);
    int32_t ext_dc = sys_get_ext_temp_deci();
    if (ext_dc != SDK_TEMP_INVALID)
    {
        draw_num(195, ROW(3), ext_dc, 1);
    }
    else
    {
        sys_draw_text(195, ROW(3), "N/A     ", COLOR_AMBER, COLOR_DARK_BG, 1);
    }

    sys_draw_text(10, ROW(4), "KEYS:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(150, ROW(4), "KBD:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(5), "PAD:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(140, ROW(5), "BTN:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(6), "RAW:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(7), "LXY/RXY:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(8), "MOUSE:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(9), "MOUSE XY:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(10), "SLEEP 500MS:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(12), "A:SLEEP  X:TEMP  ESC/PAD B:EXIT", COLOR_AMBER, COLOR_DARK_BG, 1);
}

void app_main(void)
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

        if (just_pressed & KEY_X)
        {
            draw_num(75, ROW(3), sys_get_mcu_temp_deci(), 1);
            int32_t ext_dc = sys_get_ext_temp_deci();
            if (ext_dc != SDK_TEMP_INVALID)
            {
                draw_num(195, ROW(3), ext_dc, 1);
            }
            else
            {
                sys_draw_text(195, ROW(3), "N/A     ", COLOR_AMBER, COLOR_DARK_BG, 1);
            }
        }

        (void)sys_get_info(&info);
        draw_hex(70, ROW(2), info.devices, 2);
        draw_hex(180, ROW(2), info.flags, 2);
        draw_hex(55, ROW(4), keys, 4);

        SDK_KeyEvent_t kevt;
        while (sys_get_key_event(&kevt))
        {
            draw_hex(185, ROW(4), kevt.scancode, 2);
            char ch_str[4] = { '[', (kevt.ascii >= 32 && kevt.ascii < 127) ? kevt.ascii : '.', ']', '\0' };
            sys_draw_text(205, ROW(4), ch_str, kevt.pressed ? COLOR_NEON_GREEN : COLOR_AMBER, COLOR_DARK_BG, 1);
            draw_hex(235, ROW(4), kevt.modifiers, 2);
            sys_draw_text(255, ROW(4), kevt.pressed ? "DN" : "UP",
                          kevt.pressed ? COLOR_NEON_GREEN : COLOR_AMBER, COLOR_DARK_BG, 1);
        }

        SDK_GamepadState_t pad;
        bool pad_ok = sys_get_gamepad(&pad);
        sys_draw_text(45, ROW(5), pad_ok ? "CONN " : "NONE ", COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
        draw_hex(175, ROW(5), pad.buttons, 4);
        for (uint8_t i = 0; i < 8U; i++)
        {
            draw_hex((uint16_t)(45U + i * 20U), ROW(6), pad.raw[i], 2);
        }
        draw_num(70,  ROW(7), pad.left_x,  1);
        draw_num(125, ROW(7), pad.left_y,  1);
        draw_num(180, ROW(7), pad.right_x, 1);
        draw_num(235, ROW(7), pad.right_y, 1);

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
