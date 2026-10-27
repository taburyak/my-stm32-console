#include "console_sdk.h"

/* =========================================================================
 * Media test: random numbers, input snapshot, LCD blit (DMA and stride paths),
 * background note sequences and the GPIO skeleton.
 *   A: play melody   DOWN: loop melody   UP: stop sound
 *   B (pad X / key X): full-screen blit benchmark   ESC: exit
 * ========================================================================= */

#define ANIM_W   160U
#define ANIM_H   64U
#define ANIM_X   0U
#define ANIM_Y   150U
#define ROW(n)   ((uint16_t)(30U + (n) * 14U))

static SDK_EXT_BSS uint16_t s_anim[ANIM_W * ANIM_H];
static SDK_EXT_BSS uint16_t s_full[SDK_SCREEN_W * SDK_SCREEN_H];

static const SDK_Note_t s_melody[] = {
    { 1, 120 }, { 0, 60 }, { 1, 120 }, { 0, 60 }, { 1, 300 }, { 0, 150 },
    { 1, 80 },  { 0, 40 }, { 1, 80 },  { 0, 40 }, { 1, 80 },  { 0, 300 },
};

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

static void draw_int(uint16_t x, uint16_t y, int32_t v)
{
    char buf[14];
    char *p = buf;
    uint32_t mag = (uint32_t)v;
    if (v < 0)
    {
        *p++ = '-';
        mag = (uint32_t)(-v);
    }
    sdk_uint_to_str(mag, p, 1);
    sys_draw_text(x, y, "      ", COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
    sys_draw_text(x, y, buf, COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
}

static void draw_static(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_draw_text(10, 8, "MEDIA TEST", COLOR_CYAN, COLOR_DARK_BG, 2);
    sys_draw_text(10, ROW(0), "RANDOM:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(170, ROW(0), "DICE:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(1), "KEYS:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(170, ROW(1), "DEV:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(2), "PAD:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(170, ROW(2), "MOUSE:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(3), "AUDIO:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(170, ROW(3), "BLIT MS:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(4), "GPIO PINS:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(170, ROW(4), "READ(0):", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(6), "A:MELODY DOWN:LOOP UP:STOP B:BENCH", COLOR_AMBER, COLOR_DARK_BG, 1);
    sys_draw_text(10, ROW(7), "LEFT: DMA BLIT  RIGHT: 2 STRIDE BLITS", COLOR_AMBER, COLOR_DARK_BG, 1);

    SDK_GpioCaps_t caps;
    sdk_memset(&caps, 0, sizeof(caps));
    (void)sys_gpio_get_caps(&caps);
    draw_int(100, ROW(4), (int32_t)caps.pin_count);
    draw_int(240, ROW(4), sys_gpio_read(0));
}

static void render_anim(uint32_t t)
{
    for (uint32_t y = 0; y < ANIM_H; y++)
    {
        for (uint32_t x = 0; x < ANIM_W; x++)
        {
            uint32_t r = (x + t) & 0x1FU;
            uint32_t g = (y * 2U + t) & 0x3FU;
            uint32_t b = ((x ^ y) + (t >> 1)) & 0x1FU;
            s_anim[y * ANIM_W + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static int32_t benchmark(void)
{
    for (uint32_t i = 0; i < SDK_SCREEN_W * SDK_SCREEN_H; i++)
    {
        s_full[i] = (uint16_t)(sys_random() & 0xFFFFU);
    }
    uint32_t t0 = sys_get_tick();
    for (uint32_t n = 0; n < 10U; n++)
    {
        (void)sys_lcd_blit(0, 0, SDK_SCREEN_W, SDK_SCREEN_H, s_full, SDK_SCREEN_W);
    }
    return (int32_t)((sys_get_tick() - t0) / 10U);
}

void app_main(void)
{
    int32_t blit_ms = -1;
    draw_static();

    while (sys_get_keys() & (KEY_A | KEY_EXIT))
    {
        sys_delay_ms(10);
    }

    uint32_t prev_keys = 0;
    uint32_t t = 0;
    for (;;)
    {
        SDK_InputState_t in;
        (void)sys_input_get_state(&in);
        uint32_t pressed = in.keys & ~prev_keys;
        prev_keys = in.keys;

        if (pressed & KEY_EXIT)
        {
            return;
        }
        if (pressed & KEY_A)
        {
            (void)sys_audio_play(s_melody, sizeof(s_melody) / sizeof(s_melody[0]), 0U);
        }
        if (pressed & KEY_DOWN)
        {
            (void)sys_audio_play(s_melody, sizeof(s_melody) / sizeof(s_melody[0]), SDK_AUDIO_LOOP);
        }
        if (pressed & KEY_UP)
        {
            sys_audio_stop();
        }
        if (pressed & KEY_B)
        {
            blit_ms = benchmark();
            draw_static();
        }

        draw_hex(70, ROW(0), sys_random(), 8);
        draw_int(220, ROW(0), (int32_t)sys_random_range(6U) + 1);
        draw_hex(70, ROW(1), in.keys, 8);
        draw_hex(210, ROW(1), in.devices, 2);
        sys_draw_text(70, ROW(2), in.pad.connected ? "ON " : "OFF", COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
        draw_int(220, ROW(2), in.mouse.x);
        draw_int(260, ROW(2), in.mouse.y);
        sys_draw_text(70, ROW(3), sys_audio_is_playing() ? "PLAYING" : "IDLE   ", COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
        draw_int(230, ROW(3), blit_ms);

        render_anim(t);
        (void)sys_lcd_blit(ANIM_X, ANIM_Y, ANIM_W, ANIM_H, s_anim, ANIM_W);
        /* Left half of the same buffer, stride 160 (CPU path) */
        (void)sys_lcd_blit(ANIM_X + ANIM_W, ANIM_Y, ANIM_W / 2U, ANIM_H, s_anim, ANIM_W);
        (void)sys_lcd_blit(ANIM_X + ANIM_W + ANIM_W / 2U, ANIM_Y, ANIM_W / 2U, ANIM_H, &s_anim[ANIM_W / 2U], ANIM_W);

        t++;
        sys_delay_ms(30);
    }
}
