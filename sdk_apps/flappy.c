#include "console_sdk.h"

/* =========================================================================
 * Flappy Bird: Smooth Q8 physics, gentle flap impulse, dirty-strip pipe
 * scrolling, and zero-FSMC-contention animated ground strip.
 * Controls: A / UP / B / click / tap flap, START (pad L2/R2, key P) pause, ESC exit.
 * Best score: SD:/appdata/flappy/best.dat
 * ========================================================================= */

#define FP              256
#define HUD_H           16
#define GROUND_Y        220
#define GROUND_H        (SDK_SCREEN_H - GROUND_Y)
#define STRIPE_H        6
#define GROUND_PERIOD   16

#define BIRD_X          60
#define BIRD_W          14
#define BIRD_H          10

/* Gentle, controllable flap & gravity parameters */
#define GRAVITY         48          /* ~0.19 px/frame^2 (smooth arc) */
#define FLAP_V          (-780)      /* ~3.0 px/frame upward velocity (~19 px jump height) */
#define MAX_FALL        (3 * FP + FP / 2)

#define PIPES           3
#define PIPE_W          26
#define CAP_OVER        2
#define CAP_H           8
#define PIPE_SPACING    120
#define SCROLL          2
#define GAP_START       76
#define GAP_MIN         56

#define SKY             0x5D1FU
#define PIPE_BODY       0x05E0U
#define PIPE_CAP        0x07E0U
#define DIRT_COLOR      0xB3A0U

enum { ST_READY, ST_PLAY, ST_PAUSE, ST_DEAD, ST_OVER };

typedef struct {
    int16_t x;        /* body left edge */
    int16_t gap_top;
    int16_t gap_bot;
    bool    scored;
} Pipe_t;

static const char *const s_bird_rows[2][BIRD_H] = {
    { "....YYYYY.....", "..YYYYYYWWY...", ".YYYYYYWWKW...", "wwwYYYYYWWY...", "wwwwYYYYYOOOO.",
      ".wwYYYYYOOOOO.", "..YYYYYYYOOO..", "...YYYYYYY....", "....YYYYY.....", ".............." },
    { "....YYYYY.....", "..YYYYYYWWY...", ".YYYYYYWWKW...", ".YYYYYYYWWY...", "YYYYYYYYYOOOO.",
      "wwwwYYYYOOOOO.", "wwwYYYYYYOOO..", ".wwYYYYYYY....", "....YYYYY.....", ".............." },
};

static const SDK_Note_t s_snd_flap[]  = { { 1, 6 } };
static const SDK_Note_t s_snd_point[] = { { 1, 20 } };
static const SDK_Note_t s_snd_hit[]   = { { 1, 160 }, { 0, 60 }, { 1, 240 } };

/* Compact sprites in fast Internal SRAM (zero FSMC bus contention!) */
static uint16_t s_bird[2][BIRD_W * BIRD_H];
static uint16_t s_grass_strip[SDK_SCREEN_W * STRIPE_H];

static Pipe_t   s_pipes[PIPES];
static int32_t  s_y, s_vy;            /* Q8 */
static int16_t  s_drawn_y = -1;
static uint8_t  s_wing;
static uint8_t  s_ground_off;
static uint8_t  s_state = ST_READY;
static uint32_t s_state_tick;
static uint32_t s_score, s_best, s_best_saved;

static void sound(const SDK_Note_t *notes, uint32_t n)
{
    (void)sys_audio_play(notes, n, 0);
}

/* fill_rect safely clipped to the screen horizontally and vertically */
static void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (x < 0)
    {
        w = (int16_t)(w + x);
        x = 0;
    }
    if (y < HUD_H)
    {
        h = (int16_t)(h - (HUD_H - y));
        y = HUD_H;
    }
    if ((w <= 0) || (h <= 0) || (x >= (int16_t)SDK_SCREEN_W) || (y >= GROUND_Y))
    {
        return;
    }
    if ((int16_t)(x + w) > (int16_t)SDK_SCREEN_W)
    {
        w = (int16_t)((int16_t)SDK_SCREEN_W - x);
    }
    if ((int16_t)(y + h) > GROUND_Y)
    {
        h = (int16_t)(GROUND_Y - y);
    }
    if ((w <= 0) || (h <= 0))
    {
        return;
    }
    sys_lcd_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

static void build_bird_sprites(void)
{
    for (uint8_t f = 0; f < 2U; f++)
    {
        for (uint8_t y = 0; y < BIRD_H; y++)
        {
            for (uint8_t x = 0; x < BIRD_W; x++)
            {
                uint16_t c = SKY;
                switch (s_bird_rows[f][y][x])
                {
                    case 'Y': c = 0xFFE0U; break;
                    case 'W': c = 0xFFFFU; break;
                    case 'K': c = 0x0000U; break;
                    case 'O': c = 0xFC00U; break;
                    case 'w': c = 0xEF7DU; break;
                    default: break;
                }
                s_bird[f][y * BIRD_W + x] = c;
            }
        }
    }
}

/* ---------------------------------------------------------------- drawing */

static void draw_ground_scroll(void)
{
    for (uint16_t y = 0; y < STRIPE_H; y++)
    {
        for (uint16_t x = 0; x < SDK_SCREEN_W; x++)
        {
            uint16_t c;
            if (y < 2U)      c = 0x07E0U;
            else if (y < 3U) c = 0x0400U;
            else             c = (((x + s_ground_off + y) % GROUND_PERIOD) < (GROUND_PERIOD / 2U)) ? 0xB3A0U : 0x8A60U;
            s_grass_strip[y * SDK_SCREEN_W + x] = c;
        }
    }
    sys_draw_sprite(0, GROUND_Y, SDK_SCREEN_W, STRIPE_H, s_grass_strip);
}

static void draw_bird(void)
{
    int16_t y = (int16_t)(s_y / FP);
    if (y < HUD_H) y = HUD_H;
    if (y + BIRD_H > GROUND_Y) y = GROUND_Y - BIRD_H;
    int16_t old = s_drawn_y;

    if (old >= 0)
    {
        int16_t d = (int16_t)(y - old);
        if ((d >= BIRD_H) || (d <= -BIRD_H))
        {
            fill(BIRD_X, old, BIRD_W, BIRD_H, SKY);
        }
        else if (d > 0)
        {
            fill(BIRD_X, old, BIRD_W, d, SKY);
        }
        else if (d < 0)
        {
            fill(BIRD_X, (int16_t)(y + BIRD_H), BIRD_W, (int16_t)(-d), SKY);
        }
    }
    sys_draw_sprite(BIRD_X, (uint16_t)y, BIRD_W, BIRD_H, s_bird[s_wing & 1U]);
    s_drawn_y = y;
}

static void pipe_columns(const Pipe_t *p, int16_t body_x, int16_t cap_x, uint16_t body_color, uint16_t cap_color)
{
    fill(body_x, HUD_H, SCROLL, (int16_t)(p->gap_top - CAP_H - HUD_H), body_color);
    fill(body_x, (int16_t)(p->gap_bot + CAP_H), SCROLL, (int16_t)(GROUND_Y - p->gap_bot - CAP_H), body_color);
    fill(cap_x, (int16_t)(p->gap_top - CAP_H), SCROLL, CAP_H, cap_color);
    fill(cap_x, p->gap_bot, SCROLL, CAP_H, cap_color);
}

/* After moving SCROLL px left: paint the new left edge, clear the old right edge */
static void draw_pipe_edges(const Pipe_t *p)
{
    pipe_columns(p, p->x, (int16_t)(p->x - CAP_OVER), PIPE_BODY, PIPE_CAP);
    pipe_columns(p, (int16_t)(p->x + PIPE_W), (int16_t)(p->x + PIPE_W + CAP_OVER), SKY, SKY);
}

static void draw_num(uint16_t x, uint32_t v, uint16_t color)
{
    char buf[12];
    sdk_uint_to_str(v, buf, 3);
    sys_draw_text(x, 4, buf, color, COLOR_PANEL_BG, 1);
}

static void draw_hud(void)
{
    sys_draw_text(6, 4, "SCORE", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(42, s_score, COLOR_NEON_GREEN);
    sys_draw_text(90, 4, "BEST", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(120, s_best, COLOR_AMBER);
    sys_draw_text(250, 4, (s_state == ST_PAUSE) ? "PAUSED" : "      ", COLOR_YELLOW, COLOR_PANEL_BG, 1);
}

static void center_text(uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    uint16_t n = 0;
    while (s[n] != '\0') n++;
    sys_draw_text((uint16_t)((SDK_SCREEN_W - n * 6U * scale) / 2U), y, s, fg, bg, scale);
}

/* ---------------------------------------------------------------- persistence */

static void load_best(void)
{
    int32_t fd = sys_file_open("best.dat", SDK_FILE_READ);
    if (fd >= 0)
    {
        (void)sys_file_read(fd, &s_best, sizeof(s_best));
        (void)sys_file_close(fd);
    }
    s_best_saved = s_best;
}

static void save_best(void)
{
    if (s_best <= s_best_saved) return;
    int32_t fd = sys_file_open("best.dat", SDK_FILE_WRITE | SDK_FILE_TRUNC);
    if (fd >= 0)
    {
        if (sys_file_write(fd, &s_best, sizeof(s_best)) == (int32_t)sizeof(s_best))
        {
            s_best_saved = s_best;
        }
        (void)sys_file_close(fd);
    }
}

/* ---------------------------------------------------------------- game flow */

static void place_gap(Pipe_t *p)
{
    int16_t gap = (int16_t)(GAP_START - (int16_t)(s_score / 5U) * 2);
    if (gap < GAP_MIN) gap = GAP_MIN;
    int16_t lo = HUD_H + CAP_H + 16;
    int16_t hi = (int16_t)(GROUND_Y - CAP_H - 16 - gap);
    p->gap_top = (int16_t)(lo + (int16_t)sys_random_range((uint32_t)(hi - lo)));
    p->gap_bot = (int16_t)(p->gap_top + gap);
    p->scored = false;
}

static void reset_world(void)
{
    sys_clear_screen(SKY);
    sys_lcd_fill_rect(0, 0, SDK_SCREEN_W, HUD_H, COLOR_PANEL_BG);
    sys_lcd_fill_rect(0, GROUND_Y + STRIPE_H, SDK_SCREEN_W, GROUND_H - STRIPE_H, DIRT_COLOR);

    s_score = 0;
    for (uint8_t i = 0; i < PIPES; i++)
    {
        s_pipes[i].x = (int16_t)(SDK_SCREEN_W + 40 + i * PIPE_SPACING);
        place_gap(&s_pipes[i]);
    }
    s_y = 110 * FP;
    s_vy = 0;
    s_drawn_y = -1;
    s_state = ST_READY;
    draw_hud();
    draw_ground_scroll();
    center_text(60, "FLAPPY", COLOR_YELLOW, SKY, 4);
    center_text(150, "A / UP / TAP TO FLAP", COLOR_WHITE, SKY, 1);
}

static void flap(void)
{
    s_vy = FLAP_V;
    sound(s_snd_flap, 1);
}

static void die(uint32_t now)
{
    s_state = ST_DEAD;
    s_state_tick = now;
    if (s_vy < 0) s_vy = 0;
    sound(s_snd_hit, 3);
}

static void show_over(void)
{
    s_state = ST_OVER;
    save_best();
    sys_lcd_fill_rect(90, 80, 140, 70, COLOR_PANEL_BG);
    center_text(88, "GAME OVER", COLOR_RED, COLOR_PANEL_BG, 2);
    char buf[12];
    sys_draw_text(110, 112, "SCORE", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sdk_uint_to_str(s_score, buf, 1);
    sys_draw_text(170, 112, buf, COLOR_NEON_GREEN, COLOR_PANEL_BG, 1);
    sys_draw_text(110, 124, "BEST", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sdk_uint_to_str(s_best, buf, 1);
    sys_draw_text(170, 124, buf, COLOR_AMBER, COLOR_PANEL_BG, 1);
    center_text(138, "A: AGAIN", COLOR_WHITE, COLOR_PANEL_BG, 1);
}

static bool hits_pipe(const Pipe_t *p, int16_t by)
{
    int16_t l = (int16_t)(BIRD_X + 2);
    int16_t r = (int16_t)(BIRD_X + BIRD_W - 2);
    if ((r <= p->x - CAP_OVER) || (l >= p->x + PIPE_W + CAP_OVER)) return false;
    return (by + 1 < p->gap_top) || (by + BIRD_H - 1 > p->gap_bot);
}

static void physics(void)
{
    s_vy += GRAVITY;
    if (s_vy > MAX_FALL) s_vy = MAX_FALL;
    s_y += s_vy;
    if (s_y < HUD_H * FP)
    {
        s_y = HUD_H * FP;
        s_vy = 0;
    }
}

static void scroll_world(void)
{
    for (uint8_t i = 0; i < PIPES; i++)
    {
        Pipe_t *p = &s_pipes[i];
        p->x = (int16_t)(p->x - SCROLL);
        draw_pipe_edges(p);

        if (!p->scored && (p->x + PIPE_W < BIRD_X))
        {
            p->scored = true;
            s_score++;
            if (s_score > s_best) s_best = s_score;
            draw_hud();
            sound(s_snd_point, 1);
        }

        if (p->x + PIPE_W + CAP_OVER + SCROLL < 0)
        {
            /* Ensure any residual pixel at left border x=0..SCROLL is cleared */
            fill(0, HUD_H, SCROLL * 2, GROUND_Y - HUD_H, SKY);

            int16_t rightmost = 0;
            for (uint8_t k = 0; k < PIPES; k++)
            {
                if (s_pipes[k].x > rightmost) rightmost = s_pipes[k].x;
            }
            p->x = (int16_t)(rightmost + PIPE_SPACING);
            place_gap(p);
        }
    }
    s_ground_off = (uint8_t)((s_ground_off + SCROLL) % GROUND_PERIOD);
}

void app_main(void)
{
    SDK_Input_t in;
    SDK_InputState_t st;
    uint32_t next_frame = 0;
    uint32_t frame = 0;

    build_bird_sprites();
    load_best();
    reset_world();
    sdk_input_init(&in);

    for (;;)
    {
        uint32_t now = sys_get_tick();
        (void)sys_input_get_state(&st);
        uint32_t keys = st.keys;
        if ((st.mouse.buttons & SDK_MOUSE_BTN_LEFT) != 0U) keys |= KEY_A;
        sdk_input_update(&in, keys, now);
        bool flap_pressed = (in.pressed & (KEY_A | KEY_UP | KEY_B)) != 0U;
        frame++;

        if (in.pressed & KEY_EXIT)
        {
            save_best();
            return;
        }

        switch (s_state)
        {
            case ST_READY:
                s_y = (int32_t)(110 + (((frame / 8U) & 1U) ? 2 : 0)) * FP;
                s_wing = (uint8_t)((frame / 8U) & 1U);
                s_ground_off = (uint8_t)((s_ground_off + SCROLL) % GROUND_PERIOD);
                if (flap_pressed)
                {
                    sys_lcd_fill_rect(0, 56, SDK_SCREEN_W, 34, SKY);
                    sys_lcd_fill_rect(0, 146, SDK_SCREEN_W, 12, SKY);
                    s_state = ST_PLAY;
                    flap();
                }
                break;

            case ST_PLAY:
            {
                if (in.pressed & KEY_START)
                {
                    s_state = ST_PAUSE;
                    draw_hud();
                    break;
                }
                if (flap_pressed) flap();
                physics();
                s_wing = (s_vy < 0) ? 1U : (uint8_t)((frame / 6U) & 1U);
                scroll_world();
                int16_t by = (int16_t)(s_y / FP);
                if (by + BIRD_H >= GROUND_Y)
                {
                    die(now);
                    break;
                }
                for (uint8_t i = 0; i < PIPES; i++)
                {
                    if (hits_pipe(&s_pipes[i], by))
                    {
                        die(now);
                        break;
                    }
                }
                break;
            }

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    s_state = ST_PLAY;
                    draw_hud();
                }
                break;

            case ST_DEAD:
                physics();
                if ((s_y / FP) + BIRD_H >= GROUND_Y)
                {
                    draw_bird();
                    show_over();
                    s_state_tick = now;
                }
                break;

            case ST_OVER:
                if (flap_pressed && ((now - s_state_tick) > 600U))
                {
                    reset_world();
                }
                break;

            default:
                break;
        }

        if ((s_state == ST_READY) || (s_state == ST_PLAY) || (s_state == ST_DEAD))
        {
            draw_bird();
        }
        if ((s_state == ST_READY) || (s_state == ST_PLAY))
        {
            draw_ground_scroll();
        }
        sdk_frame_sync(&next_frame, 16U);
    }
}
