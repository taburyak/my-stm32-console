#include "console_sdk.h"

/* =========================================================================
 * Cyber Arkanoid Deluxe (SDK app, also built into the firmware ROM)
 *  - 8 multi-tier levels with normal ('1'), armored 2-hit ('2'), and
 *    indestructible gold bumper ('X') bricks
 *  - 3D beveled brick & capsule rendering
 *  - 6 Power-Up Capsules dropped randomly from destroyed bricks:
 *      [E] Expand Paddle (30 sec timed Wide / Mega-Wide)
 *      [S] Slow Down Balls (30 sec timed Slow-Mo)
 *      [M] Multi-Ball (Triple Ball split - active until balls are lost)
 *      [B] Bottom Laser Barrier Shield (20 sec timed safety floor)
 *      [C] Catch / Sticky Magnet Paddle (30 sec timed hold & aim)
 *      [+] Extra Life (+1 UP)
 *  - Controls: LEFT/RIGHT or Mouse/Touch move, A/UP/Click/Tap launch,
 *              START (pad L2/R2, key P) pause, ESC exit.
 *  - Best score: SD:/appdata/arkanoid/best.dat
 * ========================================================================= */

#define ARENA_L         4
#define ARENA_R         316
#define ARENA_T         22

#define PADDLE_W_NORM   54
#define PADDLE_W_WIDE   80
#define PADDLE_W_MEGA   100
#define PADDLE_H        7
#define PADDLE_Y        218
#define PADDLE_STEP     5

#define SHIELD_Y        233
#define SHIELD_H        3
#define BONUS_DURATION  15000U   /* 15 seconds for Expand, Sticky, Slow */
#define SHIELD_DURATION 20000U   /* 20 seconds for Bottom Barrier */
#define BONUS_WARN_MS   3000U    /* Blink warning during last 3 seconds */
#define STICKY_HOLD_MS  4500U

#define BALL            6
#define MAX_BALLS       3
#define MAX_DROPS       4
#define DROP_W          14
#define DROP_H          9

#define BRICK_ROWS      6
#define BRICK_COLS      8
#define BRICK_W         36
#define BRICK_H         10
#define BRICK_GAP       2
#define BRICK_X0        9
#define BRICK_Y0        34

#define FP              256      /* Q8 fixed point */
#define SPEED_MIN       (2 * FP)
#define SPEED_START     (2 * FP + (FP * 3) / 4)
#define SPEED_STEP      (FP / 5)
#define SPEED_MAX       (5 * FP)
#define START_LIVES     3U
#define LEVEL_COUNT     8U

#define COLOR_STRONG    0xC618U
#define COLOR_GOLD_BLK  0xFEA0U
#define COLOR_SHADOW    0x2104U

#define HP_INDESTRUCT   0xFFU

enum { ST_SERVE, ST_PLAY, ST_PAUSE, ST_OVER };

enum {
    PWR_NONE   = 0,
    PWR_EXPAND = 1, /* 'E' - Expand paddle (30s) */
    PWR_SLOW   = 2, /* 'S' - Slow ball (30s) */
    PWR_MULTI  = 3, /* 'M' - Triple ball (until lost) */
    PWR_SHIELD = 4, /* 'B' - Bottom barrier shield (20s) */
    PWR_CATCH  = 5, /* 'C' - Sticky magnet paddle (30s) */
    PWR_LIFE   = 6, /* '+' - Extra life */
    PWR_COUNT  = 7
};

typedef struct {
    int32_t  x, y;            /* Q8 top-left */
    int32_t  vx, vy;          /* Q8 per frame */
    int16_t  drawn_x, drawn_y;
    int16_t  stuck_offset;    /* Offset from paddle center when stuck */
    uint32_t stuck_until;
    bool     active;
    bool     stuck;
} Ball_t;

typedef struct {
    int16_t x, y;
    int16_t drawn_x, drawn_y;
    uint8_t type;
    bool    active;
} Drop_t;

/* '.' empty, '1' normal brick, '2' armored brick (2 hits), 'X' indestructible bumper */
static const char *const s_levels[LEVEL_COUNT][BRICK_ROWS] = {
    /* Level 1: Classic Neon Wave */
    { "11111111", "11111111", "11111111", "11111111", "........", "........" },
    /* Level 2: Armored Core */
    { "22222222", "1......1", "1.1111.1", "1.1221.1", "1.1111.1", "11111111" },
    /* Level 3: Twin Bunkers with Gold Bumpers */
    { "21122112", "12211221", "11111111", ".X.11.X.", "11111111", "2.2..2.2" },
    /* Level 4: Diamond Fortress */
    { "...22...", "..2112..", ".212212.", "212XX212", ".211112.", "..2112.." },
    /* Level 5: Checkerboard Assault */
    { "2.2.2.2.", ".1.1.1.1", "2.2.2.2.", ".1.1.1.1", "2.2.2.2.", ".1.1.1.1" },
    /* Level 6: The Gauntlet Gate */
    { "22222222", "11111111", "XX.22.XX", "11111111", "2.1111.2", "11....11" },
    /* Level 7: Reactor Chambers */
    { "212..212", "1X1221X1", "21211212", "11111111", ".222222.", "..1111.." },
    /* Level 8: Cyber Citadel */
    { "22222222", "21111112", "21X22X12", "21111112", "XX1221XX", "11111111" }
};

static const uint16_t s_row_colors[BRICK_ROWS] = {
    COLOR_MAGENTA, COLOR_RED, COLOR_AMBER, COLOR_YELLOW, COLOR_NEON_GREEN, COLOR_CYAN,
};

static const uint16_t s_pwr_colors[PWR_COUNT] = {
    COLOR_WHITE,
    COLOR_BLUE,       /* E: Expand */
    COLOR_NEON_GREEN, /* S: Slow */
    COLOR_MAGENTA,    /* M: Multi-ball */
    COLOR_AMBER,      /* B: Barrier Shield */
    COLOR_YELLOW,     /* C: Catch / Magnet */
    COLOR_RED         /* +: Extra Life */
};

static const char s_pwr_chars[PWR_COUNT][2] = {
    "?", "E", "S", "M", "B", "C", "+"
};

static const SDK_Note_t s_snd_wall[]   = { { 1, 6 } };
static const SDK_Note_t s_snd_brick[]  = { { 1, 14 } };
static const SDK_Note_t s_snd_metal[]  = { { 1, 5 }, { 0, 5 }, { 1, 5 } };
static const SDK_Note_t s_snd_paddle[] = { { 1, 10 } };
static const SDK_Note_t s_snd_power[]  = { { 1, 20 }, { 0, 15 }, { 1, 30 }, { 0, 15 }, { 1, 45 } };
static const SDK_Note_t s_snd_shield[] = { { 1, 18 }, { 0, 10 }, { 1, 18 } };
static const SDK_Note_t s_snd_lost[]   = { { 1, 120 }, { 0, 60 }, { 1, 200 } };
static const SDK_Note_t s_snd_level[]  = { { 1, 50 }, { 0, 40 }, { 1, 50 }, { 0, 40 }, { 1, 50 }, { 0, 40 }, { 1, 200 } };
static const SDK_Note_t s_snd_over[]   = { { 1, 150 }, { 0, 80 }, { 1, 150 }, { 0, 80 }, { 1, 400 } };

static uint8_t  s_hp[BRICK_ROWS][BRICK_COLS];
static uint16_t s_bricks_left;

static Ball_t   s_balls[MAX_BALLS];
static Drop_t   s_drops[MAX_DROPS];
static int32_t  s_base_speed;
static int32_t  s_speed;

static int16_t  s_paddle_x;
static int16_t  s_paddle_w      = PADDLE_W_NORM;
static int16_t  s_drawn_pad_x   = -1;
static int16_t  s_drawn_pad_w   = PADDLE_W_NORM;
static uint8_t  s_drawn_pad_st  = 0xFFU;

/* Timed Power-Up Expiration Timestamps */
static uint32_t s_expand_until  = 0;
static bool     s_sticky_mode   = false;
static uint32_t s_sticky_until  = 0;
static bool     s_slow_mode     = false;
static uint32_t s_slow_until    = 0;
static bool     s_shield_active = false;
static uint32_t s_shield_until  = 0;
static uint8_t  s_drawn_shield  = 0; /* 0=off, 1=cyan, 2=amber warning */

static uint8_t  s_state = ST_SERVE;
static uint8_t  s_level;
static uint8_t  s_lives;
static uint32_t s_score, s_best, s_best_saved;
static uint32_t s_over_tick;
static uint32_t s_pause_tick;
static int16_t  s_prev_mouse_x = -1;

static void sound(const SDK_Note_t *notes, uint32_t n)
{
    (void)sys_audio_play(notes, n, 0);
}

static int32_t iabs(int32_t v)
{
    return (v < 0) ? -v : v;
}

/* ---------------------------------------------------------------- drawing */

static uint16_t brick_x(uint8_t c) { return (uint16_t)(BRICK_X0 + c * (BRICK_W + BRICK_GAP)); }
static uint16_t brick_y(uint8_t r) { return (uint16_t)(BRICK_Y0 + r * (BRICK_H + BRICK_GAP)); }

static void draw_brick(uint8_t r, uint8_t c)
{
    uint16_t x  = brick_x(c);
    uint16_t y  = brick_y(r);
    uint8_t  hp = s_hp[r][c];

    if (hp == 0U)
    {
        sys_lcd_fill_rect(x, y, BRICK_W, BRICK_H, COLOR_DARK_BG);
        return;
    }

    if (hp == HP_INDESTRUCT)
    {
        /* Indestructible Gold Bumper Brick */
        sys_lcd_fill_rect(x, y, BRICK_W, BRICK_H, COLOR_GOLD_BLK);
        sys_lcd_fill_rect(x, y, BRICK_W, 1, COLOR_WHITE);
        sys_lcd_fill_rect(x, y, 1, BRICK_H, COLOR_WHITE);
        sys_lcd_fill_rect(x, (uint16_t)(y + BRICK_H - 1U), BRICK_W, 1, COLOR_SHADOW);
        sys_lcd_fill_rect((uint16_t)(x + 4U), (uint16_t)(y + 4U), BRICK_W - 8U, 2, COLOR_AMBER);
    }
    else if (hp == 2U)
    {
        /* 2-Hit Armored Brick: metallic silver frame with glowing colored core */
        sys_lcd_fill_rect(x, y, BRICK_W, BRICK_H, COLOR_STRONG);
        sys_lcd_fill_rect(x, y, BRICK_W, 1, COLOR_WHITE);
        sys_lcd_fill_rect(x, y, 1, BRICK_H, COLOR_WHITE);
        sys_lcd_fill_rect(x, (uint16_t)(y + BRICK_H - 1U), BRICK_W, 1, COLOR_SHADOW);
        sys_lcd_fill_rect((uint16_t)(x + 3U), (uint16_t)(y + 3U), BRICK_W - 6U, BRICK_H - 6U, s_row_colors[r]);
    }
    else
    {
        /* Standard 3D Beveled Neon Brick */
        uint16_t col = s_row_colors[r];
        sys_lcd_fill_rect(x, y, BRICK_W, BRICK_H, col);
        sys_lcd_fill_rect(x, y, BRICK_W - 1U, 1, COLOR_WHITE);
        sys_lcd_fill_rect(x, y, 1, BRICK_H - 1U, COLOR_WHITE);
        sys_lcd_fill_rect(x, (uint16_t)(y + BRICK_H - 1U), BRICK_W, 1, COLOR_SHADOW);
        sys_lcd_fill_rect((uint16_t)(x + BRICK_W - 1U), y, 1, BRICK_H, COLOR_SHADOW);
    }
}

/* Repaints live bricks overlapping a rectangle that was just erased */
static void redraw_bricks_in(int16_t x, int16_t y, int16_t w, int16_t h)
{
    for (uint8_t r = 0; r < BRICK_ROWS; r++)
    {
        int16_t by = (int16_t)brick_y(r);
        if ((y + h <= by) || (y >= by + BRICK_H)) continue;
        for (uint8_t c = 0; c < BRICK_COLS; c++)
        {
            int16_t bx = (int16_t)brick_x(c);
            if ((s_hp[r][c] != 0U) && (x + w > bx) && (x < bx + BRICK_W))
            {
                draw_brick(r, c);
            }
        }
    }
}

static void draw_num(uint16_t x, uint32_t v, uint8_t digits, uint16_t color)
{
    char buf[12];
    sdk_uint_to_str(v, buf, digits);
    sys_draw_text(x, 6, buf, color, COLOR_PANEL_BG, 1);
}

static void draw_hud(void)
{
    sys_draw_text(6, 6, "SCORE", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(42, s_score, 5, COLOR_NEON_GREEN);
    sys_draw_text(84, 6, "LIVES", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(120, s_lives, 1, COLOR_RED);
    sys_draw_text(138, 6, "LEVEL", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(174, (uint32_t)s_level + 1U, 2, COLOR_CYAN);
    sys_draw_text(198, 6, "BEST", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(228, s_best, 5, COLOR_AMBER);
    sys_draw_text(272, 6, (s_state == ST_PAUSE) ? "PAUSE" : "     ", COLOR_YELLOW, COLOR_PANEL_BG, 1);
}

static void draw_message(const char *text, uint16_t color)
{
    sys_draw_text(64, 152, text, color, COLOR_DARK_BG, 1);
}

static void clear_message(void)
{
    sys_lcd_fill_rect(ARENA_L, 148, ARENA_R - ARENA_L, 16, COLOR_DARK_BG);
}

static void draw_paddle(uint32_t now)
{
    /* Blink paddle tips/core during last 3 seconds of active Expand or Sticky bonus */
    bool exp_warn = (s_paddle_w > PADDLE_W_NORM) && ((int32_t)(s_expand_until - now) <= (int32_t)BONUS_WARN_MS) && (((now / 150U) & 1U) != 0U);
    bool stk_warn = s_sticky_mode && ((int32_t)(s_sticky_until - now) <= (int32_t)BONUS_WARN_MS) && (((now / 150U) & 1U) != 0U);
    uint8_t style_state = (uint8_t)((s_sticky_mode ? 1U : 0U) | (exp_warn ? 2U : 0U) | (stk_warn ? 4U : 0U));

    if ((s_drawn_pad_x == s_paddle_x) &&
        (s_drawn_pad_w == s_paddle_w) &&
        (s_drawn_pad_st == style_state))
    {
        return;
    }

    if (s_drawn_pad_x >= 0)
    {
        sys_lcd_fill_rect((uint16_t)s_drawn_pad_x, PADDLE_Y, (uint16_t)s_drawn_pad_w, PADDLE_H, COLOR_DARK_BG);
    }

    uint16_t body_col = (s_sticky_mode && !stk_warn) ? COLOR_NEON_GREEN : COLOR_CYAN;
    uint16_t tip_col  = (s_paddle_w > PADDLE_W_NORM && !exp_warn) ? COLOR_MAGENTA : COLOR_AMBER;

    sys_lcd_fill_rect((uint16_t)s_paddle_x, PADDLE_Y, (uint16_t)s_paddle_w, PADDLE_H, body_col);
    sys_lcd_fill_rect((uint16_t)(s_paddle_x + 4), PADDLE_Y, (uint16_t)(s_paddle_w - 8), 2, COLOR_WHITE);
    sys_lcd_fill_rect((uint16_t)s_paddle_x, PADDLE_Y, 4, PADDLE_H, tip_col);
    sys_lcd_fill_rect((uint16_t)(s_paddle_x + s_paddle_w - 4), PADDLE_Y, 4, PADDLE_H, tip_col);

    s_drawn_pad_x  = s_paddle_x;
    s_drawn_pad_w  = s_paddle_w;
    s_drawn_pad_st = style_state;
}

static void draw_shield(uint32_t now)
{
    uint8_t want = 0U;
    if (s_shield_active)
    {
        int32_t rem = (int32_t)(s_shield_until - now);
        if (rem <= (int32_t)BONUS_WARN_MS)
        {
            want = (uint8_t)((((uint32_t)rem / 120U) & 1U) ? 2U : 1U);
        }
        else
        {
            want = 1U;
        }
    }

    if (want != s_drawn_shield)
    {
        uint16_t col = (want == 1U) ? COLOR_CYAN : ((want == 2U) ? COLOR_AMBER : COLOR_DARK_BG);
        sys_lcd_fill_rect(ARENA_L, SHIELD_Y, ARENA_R - ARENA_L, SHIELD_H, col);
        s_drawn_shield = want;
    }
}

static void draw_balls(void)
{
    for (uint8_t i = 0; i < MAX_BALLS; i++)
    {
        Ball_t *b = &s_balls[i];
        int16_t x = (int16_t)(b->x / FP);
        int16_t y = (int16_t)(b->y / FP);

        if (!b->active)
        {
            if (b->drawn_x >= 0)
            {
                sys_lcd_fill_rect((uint16_t)b->drawn_x, (uint16_t)b->drawn_y, BALL, BALL, COLOR_DARK_BG);
                redraw_bricks_in(b->drawn_x, b->drawn_y, BALL, BALL);
                b->drawn_x = -1;
            }
            continue;
        }

        if ((x == b->drawn_x) && (y == b->drawn_y)) continue;

        if (b->drawn_x >= 0)
        {
            sys_lcd_fill_rect((uint16_t)b->drawn_x, (uint16_t)b->drawn_y, BALL, BALL, COLOR_DARK_BG);
            redraw_bricks_in(b->drawn_x, b->drawn_y, BALL, BALL);
        }

        if ((y >= ARENA_T) && (y < (int16_t)SDK_SCREEN_H - BALL))
        {
            uint16_t col = b->stuck ? COLOR_YELLOW : ((i == 0U) ? COLOR_WHITE : COLOR_CYAN);
            sys_lcd_fill_rect((uint16_t)x, (uint16_t)y, BALL, BALL, col);
            b->drawn_x = x;
            b->drawn_y = y;
        }
        else
        {
            b->drawn_x = -1;
        }
    }
}

static void draw_drops(void)
{
    for (uint8_t i = 0; i < MAX_DROPS; i++)
    {
        Drop_t *d = &s_drops[i];
        if (!d->active)
        {
            if (d->drawn_x >= 0)
            {
                sys_lcd_fill_rect((uint16_t)d->drawn_x, (uint16_t)d->drawn_y, DROP_W, DROP_H, COLOR_DARK_BG);
                redraw_bricks_in(d->drawn_x, d->drawn_y, DROP_W, DROP_H);
                d->drawn_x = -1;
            }
            continue;
        }

        if ((d->x == d->drawn_x) && (d->y == d->drawn_y)) continue;

        if (d->drawn_x >= 0)
        {
            sys_lcd_fill_rect((uint16_t)d->drawn_x, (uint16_t)d->drawn_y, DROP_W, DROP_H, COLOR_DARK_BG);
            redraw_bricks_in(d->drawn_x, d->drawn_y, DROP_W, DROP_H);
        }

        if (d->y + DROP_H < (int16_t)SDK_SCREEN_H)
        {
            uint16_t col = s_pwr_colors[d->type];
            sys_lcd_fill_rect((uint16_t)d->x, (uint16_t)d->y, DROP_W, DROP_H, col);
            sys_lcd_fill_rect((uint16_t)d->x, (uint16_t)d->y, DROP_W, 1, COLOR_WHITE);
            sys_draw_text((uint16_t)(d->x + 4), (uint16_t)(d->y + 1), s_pwr_chars[d->type], COLOR_BLACK, col, 1);
            d->drawn_x = d->x;
            d->drawn_y = d->y;
        }
        else
        {
            d->drawn_x = -1;
        }
    }
}

static void draw_arena(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_lcd_fill_rect(0, 0, SDK_SCREEN_W, 20, COLOR_PANEL_BG);
    sys_lcd_fill_rect(0, 20, SDK_SCREEN_W, 2, COLOR_CYAN);
    sys_lcd_fill_rect(0, ARENA_T, ARENA_L, SDK_SCREEN_H - ARENA_T, COLOR_CYAN);
    sys_lcd_fill_rect(ARENA_R, ARENA_T, SDK_SCREEN_W - ARENA_R, SDK_SCREEN_H - ARENA_T, COLOR_CYAN);
    for (uint8_t r = 0; r < BRICK_ROWS; r++)
    {
        for (uint8_t c = 0; c < BRICK_COLS; c++)
        {
            if (s_hp[r][c] != 0U) draw_brick(r, c);
        }
    }
    s_drawn_pad_x  = -1;
    s_drawn_shield = 0;
    for (uint8_t i = 0; i < MAX_BALLS; i++) s_balls[i].drawn_x = -1;
    for (uint8_t i = 0; i < MAX_DROPS; i++) s_drops[i].drawn_x = -1;
    draw_hud();
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
    else
    {
        (void)sys_load_state("arkanoid.sav", &s_best, sizeof(s_best));
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

/* ---------------------------------------------------------------- power-ups & game flow */

static void add_score(uint32_t points)
{
    s_score += points;
    if (s_score > s_best) s_best = s_score;
    draw_hud();
}

static void launch_ball_from_paddle(Ball_t *b)
{
    int32_t offset = (int32_t)b->stuck_offset;
    int32_t half_w = (int32_t)(s_paddle_w / 2);
    if (half_w < 10) half_w = 10;

    int32_t vx = (offset * s_speed * 5) / (half_w * 4);
    int32_t vmax = (s_speed * 3) / 2;
    if (vx > vmax) vx = vmax;
    if (vx < -vmax) vx = -vmax;
    if (iabs(vx) < FP / 4) vx = (offset < 0) ? -(FP / 3) : (FP / 3);

    b->vx    = vx;
    b->vy    = -s_speed;
    b->y     = (int32_t)(PADDLE_Y - BALL - 1) * FP;
    b->stuck = false;
}

static void release_stuck_balls(void)
{
    bool any = false;
    for (uint8_t i = 0; i < MAX_BALLS; i++)
    {
        if (s_balls[i].active && s_balls[i].stuck)
        {
            launch_ball_from_paddle(&s_balls[i]);
            any = true;
        }
    }
    if (any) sound(s_snd_paddle, 1);
}

static void reset_bonuses(void)
{
    if (s_drawn_pad_x >= 0)
    {
        sys_lcd_fill_rect((uint16_t)s_drawn_pad_x, PADDLE_Y, (uint16_t)s_drawn_pad_w, PADDLE_H, COLOR_DARK_BG);
        s_drawn_pad_x = -1;
    }
    s_paddle_w      = PADDLE_W_NORM;
    s_expand_until  = 0;
    s_sticky_mode   = false;
    s_sticky_until  = 0;
    s_slow_mode     = false;
    s_slow_until    = 0;
    s_speed         = s_base_speed;
    s_shield_active = false;
    s_shield_until  = 0;
    draw_shield(0);

    for (uint8_t i = 0; i < MAX_DROPS; i++)
    {
        if (s_drops[i].active && s_drops[i].drawn_x >= 0)
        {
            sys_lcd_fill_rect((uint16_t)s_drops[i].drawn_x, (uint16_t)s_drops[i].drawn_y, DROP_W, DROP_H, COLOR_DARK_BG);
            redraw_bricks_in(s_drops[i].drawn_x, s_drops[i].drawn_y, DROP_W, DROP_H);
        }
        s_drops[i].active  = false;
        s_drops[i].drawn_x = -1;
    }
}

static void spawn_drop(uint8_t r, uint8_t c)
{
    /* ~28% chance to drop a bonus capsule from a destroyed brick */
    if (sys_random_range(100U) >= 28U) return;

    for (uint8_t i = 0; i < MAX_DROPS; i++)
    {
        if (!s_drops[i].active)
        {
            uint32_t roll = sys_random_range(100U);
            uint8_t type;
            if (roll < 22U)      type = PWR_EXPAND;
            else if (roll < 42U) type = PWR_MULTI;
            else if (roll < 62U) type = PWR_SHIELD;
            else if (roll < 80U) type = PWR_CATCH;
            else if (roll < 94U) type = PWR_SLOW;
            else                 type = PWR_LIFE;

            s_drops[i].x       = (int16_t)(brick_x(c) + (BRICK_W - DROP_W) / 2);
            s_drops[i].y       = (int16_t)brick_y(r);
            s_drops[i].drawn_x = -1;
            s_drops[i].type    = type;
            s_drops[i].active  = true;
            return;
        }
    }
}

static void apply_powerup(uint8_t type, uint32_t now)
{
    sound(s_snd_power, 5);
    add_score(50U);

    switch (type)
    {
        case PWR_EXPAND:
        {
            int16_t new_w = (s_paddle_w < PADDLE_W_WIDE) ? PADDLE_W_WIDE : PADDLE_W_MEGA;
            s_paddle_x = (int16_t)(s_paddle_x - (new_w - s_paddle_w) / 2);
            if (s_paddle_x < ARENA_L) s_paddle_x = ARENA_L;
            if (s_paddle_x + new_w > ARENA_R) s_paddle_x = (int16_t)(ARENA_R - new_w);
            s_paddle_w     = new_w;
            s_expand_until = now + BONUS_DURATION;
            break;
        }

        case PWR_SLOW:
            s_slow_mode  = true;
            s_slow_until = now + BONUS_DURATION;
            s_speed      = (s_base_speed > SPEED_MIN + FP / 2) ? (s_base_speed - FP / 2) : SPEED_MIN;
            for (uint8_t i = 0; i < MAX_BALLS; i++)
            {
                if (s_balls[i].active && !s_balls[i].stuck)
                {
                    s_balls[i].vx = (s_balls[i].vx * 3) / 4;
                    s_balls[i].vy = (s_balls[i].vy < 0) ? -s_speed : s_speed;
                }
            }
            break;

        case PWR_MULTI:
        {
            /* Split active ball into 3 balls (permanent until balls are lost) */
            int8_t src = -1;
            for (uint8_t i = 0; i < MAX_BALLS; i++)
            {
                if (s_balls[i].active) { src = (int8_t)i; break; }
            }
            if (src >= 0)
            {
                if (s_balls[src].stuck)
                {
                    launch_ball_from_paddle(&s_balls[src]);
                }
                for (uint8_t i = 0; i < MAX_BALLS; i++)
                {
                    if (!s_balls[i].active)
                    {
                        s_balls[i].x       = s_balls[src].x;
                        s_balls[i].y       = s_balls[src].y;
                        s_balls[i].vy      = -iabs(s_balls[src].vy);
                        s_balls[i].vx      = (i == 1U) ? (-s_speed * 2 / 3) : (s_speed * 2 / 3);
                        s_balls[i].drawn_x = -1;
                        s_balls[i].stuck   = false;
                        s_balls[i].active  = true;
                    }
                }
            }
            break;
        }

        case PWR_SHIELD:
            s_shield_active = true;
            s_shield_until  = now + SHIELD_DURATION;
            draw_shield(now);
            break;

        case PWR_CATCH:
            s_sticky_mode  = true;
            s_sticky_until = now + BONUS_DURATION;
            break;

        case PWR_LIFE:
            if (s_lives < 9U) s_lives++;
            draw_hud();
            break;

        default:
            break;
    }
}

/* Checks expiration of all timed bonuses (30s Expand, 30s Sticky, 30s Slow, 20s Shield) */
static void update_bonus_timers(uint32_t now)
{
    /* 1. Expand Paddle expiration (30s) */
    if ((s_paddle_w > PADDLE_W_NORM) && ((int32_t)(now - s_expand_until) >= 0))
    {
        int16_t shrink = (int16_t)(s_paddle_w - PADDLE_W_NORM);
        s_paddle_x = (int16_t)(s_paddle_x + shrink / 2);
        s_paddle_w = PADDLE_W_NORM;
        for (uint8_t i = 0; i < MAX_BALLS; i++)
        {
            if (s_balls[i].active && s_balls[i].stuck)
            {
                int16_t max_off = (int16_t)(PADDLE_W_NORM / 2 - BALL / 2);
                if (s_balls[i].stuck_offset < -max_off) s_balls[i].stuck_offset = (int16_t)-max_off;
                if (s_balls[i].stuck_offset > max_off)  s_balls[i].stuck_offset = max_off;
            }
        }
    }

    /* 2. Sticky Magnet Paddle expiration (30s) */
    if (s_sticky_mode && ((int32_t)(now - s_sticky_until) >= 0))
    {
        s_sticky_mode = false;
        release_stuck_balls();
    }

    /* 3. Slow Ball expiration (30s) */
    if (s_slow_mode && ((int32_t)(now - s_slow_until) >= 0))
    {
        s_slow_mode = false;
        s_speed     = s_base_speed;
        for (uint8_t i = 0; i < MAX_BALLS; i++)
        {
            if (s_balls[i].active && !s_balls[i].stuck)
            {
                s_balls[i].vy = (s_balls[i].vy < 0) ? -s_speed : s_speed;
            }
        }
    }

    /* 4. Bottom Laser Shield expiration (20s) */
    if (s_shield_active && ((int32_t)(now - s_shield_until) >= 0))
    {
        s_shield_active = false;
    }
    draw_shield(now);
}

static void shift_bonus_timers(uint32_t pause_delta)
{
    if (s_paddle_w > PADDLE_W_NORM) s_expand_until += pause_delta;
    if (s_sticky_mode)              s_sticky_until += pause_delta;
    if (s_slow_mode)                s_slow_until   += pause_delta;
    if (s_shield_active)            s_shield_until += pause_delta;
    for (uint8_t i = 0; i < MAX_BALLS; i++)
    {
        if (s_balls[i].active && s_balls[i].stuck)
        {
            s_balls[i].stuck_until += pause_delta;
        }
    }
}

static void update_drops(uint32_t now)
{
    for (uint8_t i = 0; i < MAX_DROPS; i++)
    {
        Drop_t *d = &s_drops[i];
        if (!d->active) continue;

        d->y = (int16_t)(d->y + 2);

        /* Check paddle catch */
        if ((d->y + DROP_H >= PADDLE_Y) && (d->y <= PADDLE_Y + PADDLE_H) &&
            (d->x + DROP_W >= s_paddle_x) && (d->x <= s_paddle_x + s_paddle_w))
        {
            d->active = false;
            draw_drops();
            apply_powerup(d->type, now);
            continue;
        }

        if (d->y >= (int16_t)SDK_SCREEN_H)
        {
            d->active = false;
        }
    }
    draw_drops();
}

static void load_level(void)
{
    const char *const *rows = s_levels[s_level % LEVEL_COUNT];
    s_bricks_left = 0;
    for (uint8_t r = 0; r < BRICK_ROWS; r++)
    {
        for (uint8_t c = 0; c < BRICK_COLS; c++)
        {
            char ch = rows[r][c];
            if (ch == 'X')
            {
                s_hp[r][c] = HP_INDESTRUCT;
            }
            else if (ch == '2')
            {
                s_hp[r][c] = 2U;
                s_bricks_left++;
            }
            else if (ch == '1')
            {
                s_hp[r][c] = 1U;
                s_bricks_left++;
            }
            else
            {
                s_hp[r][c] = 0U;
            }
        }
    }
    int32_t speed = SPEED_START + (int32_t)s_level * SPEED_STEP;
    s_base_speed = (speed > SPEED_MAX) ? SPEED_MAX : speed;
    s_speed      = s_base_speed;
}

static void serve(void)
{
    reset_bonuses();
    s_state = ST_SERVE;
    for (uint8_t i = 0; i < MAX_BALLS; i++)
    {
        s_balls[i].active  = false;
        s_balls[i].stuck   = false;
        s_balls[i].drawn_x = -1;
    }
    s_balls[0].active       = true;
    s_balls[0].stuck        = true;
    s_balls[0].stuck_offset = 0;
    s_balls[0].vx           = s_speed / 3;
    s_balls[0].vy           = -s_speed;
    s_balls[0].x            = (int32_t)(s_paddle_x + (s_paddle_w - BALL) / 2) * FP;
    s_balls[0].y            = (int32_t)(PADDLE_Y - BALL - 1) * FP;
    draw_message("A / SPACE / CLICK TO LAUNCH", COLOR_AMBER);
}

static void new_game(void)
{
    s_level = 0;
    s_lives = START_LIVES;
    s_score = 0;
    load_level();
    draw_arena();
    serve();
}

static void game_over(uint32_t now)
{
    s_state = ST_OVER;
    s_over_tick = now;
    save_best();
    sound(s_snd_over, 5);
    sys_draw_text(106, 120, "GAME OVER", COLOR_RED, COLOR_DARK_BG, 2);
    draw_message("     A: PLAY AGAIN", COLOR_WHITE);
}

static void hit_brick(uint8_t r, uint8_t c)
{
    if (s_hp[r][c] == HP_INDESTRUCT)
    {
        sound(s_snd_metal, 3);
        return;
    }

    s_hp[r][c]--;
    draw_brick(r, c);
    sound(s_snd_brick, 1);
    if (s_hp[r][c] == 0U)
    {
        s_bricks_left--;
        add_score(10U * ((uint32_t)s_level + 1U));
        spawn_drop(r, c);
    }
    else
    {
        add_score(5U);
    }
}

/* Resolves one brick collision for ball `b`: the axis with the smaller overlap is reflected */
static bool collide_bricks(Ball_t *b)
{
    int16_t bx = (int16_t)(b->x / FP);
    int16_t by = (int16_t)(b->y / FP);

    for (uint8_t r = 0; r < BRICK_ROWS; r++)
    {
        int16_t ry = (int16_t)brick_y(r);
        if ((by + BALL <= ry) || (by >= ry + BRICK_H)) continue;
        for (uint8_t c = 0; c < BRICK_COLS; c++)
        {
            int16_t rx = (int16_t)brick_x(c);
            if ((s_hp[r][c] == 0U) || (bx + BALL <= rx) || (bx >= rx + BRICK_W)) continue;

            int16_t ox = (int16_t)(((bx + BALL < rx + BRICK_W) ? (bx + BALL) : (rx + BRICK_W)) - ((bx > rx) ? bx : rx));
            int16_t oy = (int16_t)(((by + BALL < ry + BRICK_H) ? (by + BALL) : (ry + BRICK_H)) - ((by > ry) ? by : ry));
            bool ball_left  = (bx * 2 + BALL) < (rx * 2 + BRICK_W);
            bool ball_above = (by * 2 + BALL) < (ry * 2 + BRICK_H);

            if (ox < oy)
            {
                b->vx = ball_left ? -iabs(b->vx) : iabs(b->vx);
                b->x += (ball_left ? -ox : ox) * FP;
            }
            else
            {
                b->vy = ball_above ? -iabs(b->vy) : iabs(b->vy);
                b->y += (ball_above ? -oy : oy) * FP;
            }
            hit_brick(r, c);
            return true;
        }
    }
    return false;
}

static void collide_paddle(Ball_t *b, uint32_t now)
{
    int16_t bx = (int16_t)(b->x / FP);
    int16_t by = (int16_t)(b->y / FP);
    if ((b->vy <= 0) || (by + BALL < PADDLE_Y) || (by >= PADDLE_Y + PADDLE_H + 2) ||
        (bx + BALL <= s_paddle_x) || (bx >= s_paddle_x + s_paddle_w))
    {
        return;
    }

    int16_t offset = (int16_t)((bx + BALL / 2) - (s_paddle_x + s_paddle_w / 2));
    int16_t max_off = (int16_t)(s_paddle_w / 2 - BALL / 2);
    if (offset < -max_off) offset = (int16_t)-max_off;
    if (offset > max_off)  offset = max_off;

    b->stuck_offset = offset;

    if (s_sticky_mode)
    {
        b->stuck       = true;
        b->stuck_until = now + STICKY_HOLD_MS;
        b->x           = (int32_t)(s_paddle_x + s_paddle_w / 2 + offset - BALL / 2) * FP;
        b->y           = (int32_t)(PADDLE_Y - BALL - 1) * FP;
    }
    else
    {
        launch_ball_from_paddle(b);
    }
    sound(s_snd_paddle, 1);
}

static void update_balls(uint32_t now, bool fire_pressed)
{
    if (fire_pressed)
    {
        release_stuck_balls();
    }

    update_bonus_timers(now);

    for (uint8_t b_idx = 0; b_idx < MAX_BALLS; b_idx++)
    {
        Ball_t *b = &s_balls[b_idx];
        if (!b->active) continue;

        /* Ball currently magnetized to the paddle */
        if (b->stuck)
        {
            int16_t bx = (int16_t)(s_paddle_x + s_paddle_w / 2 + b->stuck_offset - BALL / 2);
            if (bx < ARENA_L) bx = ARENA_L;
            if (bx > ARENA_R - BALL) bx = ARENA_R - BALL;
            b->x = (int32_t)bx * FP;
            b->y = (int32_t)(PADDLE_Y - BALL - 1) * FP;

            if ((int32_t)(now - b->stuck_until) >= 0)
            {
                launch_ball_from_paddle(b);
                sound(s_snd_paddle, 1);
            }
            continue;
        }

        int32_t vmax  = (iabs(b->vx) > iabs(b->vy)) ? iabs(b->vx) : iabs(b->vy);
        int32_t steps = vmax / (3 * FP) + 1;

        for (int32_t i = 0; i < steps; i++)
        {
            b->x += b->vx / steps;
            b->y += b->vy / steps;

            if (b->x < ARENA_L * FP)
            {
                b->x  = ARENA_L * FP;
                b->vx = iabs(b->vx);
                sound(s_snd_wall, 1);
            }
            else if (b->x > (ARENA_R - BALL) * FP)
            {
                b->x  = (ARENA_R - BALL) * FP;
                b->vx = -iabs(b->vx);
                sound(s_snd_wall, 1);
            }
            if (b->y < ARENA_T * FP)
            {
                b->y  = ARENA_T * FP;
                b->vy = iabs(b->vy);
                sound(s_snd_wall, 1);
            }

            if (collide_bricks(b) && (s_bricks_left == 0U))
            {
                s_level++;
                sound(s_snd_level, 7);
                load_level();
                draw_arena();
                serve();
                return;
            }

            collide_paddle(b, now);
            if (b->stuck) break;

            /* Bottom Laser Barrier Shield bounce */
            if (s_shield_active && (b->vy > 0) && (b->y + BALL * FP >= SHIELD_Y * FP))
            {
                b->y  = (SHIELD_Y - BALL) * FP;
                b->vy = -iabs(b->vy);
                sound(s_snd_shield, 3);
            }
            else if (b->y > (int32_t)SDK_SCREEN_H * FP)
            {
                b->active = false;
                break;
            }
        }
    }

    /* Life is lost ONLY when the last remaining ball falls out */
    uint8_t alive = 0;
    for (uint8_t i = 0; i < MAX_BALLS; i++)
    {
        if (s_balls[i].active) alive++;
    }

    if (alive == 0U)
    {
        draw_balls();
        if (--s_lives == 0U)
        {
            reset_bonuses();
            draw_hud();
            game_over(now);
        }
        else
        {
            sound(s_snd_lost, 3);
            draw_hud();
            serve();
        }
    }
}

/* ---------------------------------------------------------------- main loop */

static void move_paddle(const SDK_InputState_t *st)
{
    if (st->keys & KEY_LEFT)  s_paddle_x = (int16_t)(s_paddle_x - PADDLE_STEP);
    if (st->keys & KEY_RIGHT) s_paddle_x = (int16_t)(s_paddle_x + PADDLE_STEP);

    bool touch = (st->mouse.source == SDK_MOUSE_SRC_TOUCH);
    bool mouse_moved = (st->mouse.source == SDK_MOUSE_SRC_USB) && (st->mouse.x != s_prev_mouse_x);
    if (touch || mouse_moved)
    {
        s_paddle_x = (int16_t)(st->mouse.x - s_paddle_w / 2);
    }
    s_prev_mouse_x = (st->mouse.source == SDK_MOUSE_SRC_USB) ? st->mouse.x : -1;

    if (s_paddle_x < ARENA_L) s_paddle_x = ARENA_L;
    if (s_paddle_x > ARENA_R - s_paddle_w) s_paddle_x = (int16_t)(ARENA_R - s_paddle_w);
}

void app_main(void)
{
    SDK_Input_t in;
    SDK_InputState_t st;
    uint32_t next_frame = 0;

    load_best();
    s_paddle_x = (SDK_SCREEN_W - PADDLE_W_NORM) / 2;
    new_game();
    sdk_input_init(&in);

    for (;;)
    {
        uint32_t now = sys_get_tick();
        (void)sys_input_get_state(&st);
        uint32_t keys = st.keys;
        if ((st.mouse.buttons & SDK_MOUSE_BTN_LEFT) != 0U)
        {
            keys |= KEY_A; /* Left mouse click or touchscreen tap launches ball */
        }
        sdk_input_update(&in, keys, now);
        bool fire_pressed = (in.pressed & (KEY_A | KEY_UP | KEY_B)) != 0U;

        if (in.pressed & KEY_EXIT)
        {
            save_best();
            return;
        }

        switch (s_state)
        {
            case ST_SERVE:
                move_paddle(&st);
                s_balls[0].x = (int32_t)(s_paddle_x + (s_paddle_w - BALL) / 2) * FP;
                s_balls[0].y = (int32_t)(PADDLE_Y - BALL - 1) * FP;
                if (fire_pressed)
                {
                    clear_message();
                    s_balls[0].stuck = false;
                    s_state = ST_PLAY;
                    sound(s_snd_paddle, 1);
                }
                break;

            case ST_PLAY:
                if (in.pressed & KEY_START)
                {
                    s_state = ST_PAUSE;
                    s_pause_tick = now;
                    draw_hud();
                    break;
                }
                move_paddle(&st);
                update_drops(now);
                update_balls(now, fire_pressed);
                break;

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    shift_bonus_timers(now - s_pause_tick);
                    s_state = ST_PLAY;
                    draw_hud();
                }
                break;

            case ST_OVER:
                if (fire_pressed && ((now - s_over_tick) > 500U))
                {
                    new_game();
                }
                break;

            default:
                break;
        }

        if (s_state != ST_OVER)
        {
            draw_paddle(now);
            draw_balls();
        }
        sdk_frame_sync(&next_frame, 16U);
    }
}