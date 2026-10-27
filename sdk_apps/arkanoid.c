#include "console_sdk.h"

/* =========================================================================
 * Cyber Arkanoid (SDK app, also built into the firmware ROM)
 * 4 level patterns with strong bricks, 3 lives, paddle angle control,
 * side/top brick hits, pause, paddle by keys / USB mouse / touch.
 * Controls: LEFT/RIGHT move, A or click/tap launch, START (pad L2/R2, key P) pause, ESC exit.
 * Best score: SD:/appdata/arkanoid/best.dat
 * ========================================================================= */

#define ARENA_L         4
#define ARENA_R         316
#define ARENA_T         22

#define PADDLE_W        56
#define PADDLE_H        6
#define PADDLE_Y        220
#define PADDLE_STEP     5
#define BALL            6

#define BRICK_ROWS      6
#define BRICK_COLS      8
#define BRICK_W         36
#define BRICK_H         10
#define BRICK_GAP       2
#define BRICK_X0        9
#define BRICK_Y0        34

#define FP              256      /* Q8 fixed point */
#define SPEED_START     (3 * FP)
#define SPEED_STEP      (FP / 4)
#define SPEED_MAX       (6 * FP)
#define START_LIVES     3U
#define LEVEL_COUNT     4U

#define COLOR_STRONG    0xC618U

enum { ST_SERVE, ST_PLAY, ST_PAUSE, ST_OVER };

/* '.' empty, '1' normal brick, '2' strong brick (two hits) */
static const char *const s_levels[LEVEL_COUNT][BRICK_ROWS] = {
    { "11111111", "11111111", "11111111", "11111111", "........", "........" },
    { "22222222", "1......1", "1.1111.1", "1.1..1.1", "1.1111.1", "11111111" },
    { "2.2.2.2.", ".1.1.1.1", "2.2.2.2.", ".1.1.1.1", "2.2.2.2.", ".1.1.1.1" },
    { "...22...", "..2112..", ".211112.", "21111112", ".211112.", "..2112.." },
};

static const uint16_t s_row_colors[BRICK_ROWS] = {
    COLOR_MAGENTA, COLOR_RED, COLOR_AMBER, COLOR_YELLOW, COLOR_NEON_GREEN, COLOR_CYAN,
};

static const SDK_Note_t s_snd_wall[]   = { { 1, 6 } };
static const SDK_Note_t s_snd_brick[]  = { { 1, 15 } };
static const SDK_Note_t s_snd_paddle[] = { { 1, 10 } };
static const SDK_Note_t s_snd_lost[]   = { { 1, 120 }, { 0, 60 }, { 1, 200 } };
static const SDK_Note_t s_snd_level[]  = { { 1, 50 }, { 0, 40 }, { 1, 50 }, { 0, 40 }, { 1, 50 }, { 0, 40 }, { 1, 200 } };
static const SDK_Note_t s_snd_over[]   = { { 1, 150 }, { 0, 80 }, { 1, 150 }, { 0, 80 }, { 1, 400 } };

static uint8_t  s_hp[BRICK_ROWS][BRICK_COLS];
static uint16_t s_bricks_left;

static int32_t  s_ball_x, s_ball_y;   /* Q8 top-left */
static int32_t  s_vx, s_vy;           /* Q8 per frame */
static int32_t  s_speed;
static int16_t  s_paddle_x;
static int16_t  s_drawn_paddle_x = -1;
static int16_t  s_drawn_ball_x = -1, s_drawn_ball_y = -1;

static uint8_t  s_state = ST_SERVE;
static uint8_t  s_level;
static uint8_t  s_lives;
static uint32_t s_score, s_best, s_best_saved;
static uint32_t s_over_tick;
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
    uint16_t x = brick_x(c);
    uint16_t y = brick_y(r);
    if (s_hp[r][c] == 0U)
    {
        sys_lcd_fill_rect(x, y, BRICK_W, BRICK_H, COLOR_DARK_BG);
    }
    else if (s_hp[r][c] == 2U)
    {
        sys_lcd_fill_rect(x, y, BRICK_W, BRICK_H, COLOR_STRONG);
        sys_lcd_fill_rect((uint16_t)(x + 3U), (uint16_t)(y + 3U), BRICK_W - 6U, BRICK_H - 6U, s_row_colors[r]);
    }
    else
    {
        sys_lcd_fill_rect(x, y, BRICK_W, BRICK_H, s_row_colors[r]);
    }
}

/* Repaints live bricks under a rectangle that was just cleared */
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
    s_drawn_paddle_x = -1;
    s_drawn_ball_x = -1;
    draw_hud();
}

static void draw_message(const char *text, uint16_t color)
{
    sys_draw_text(70, 150, text, color, COLOR_DARK_BG, 1);
}

static void clear_message(void)
{
    sys_lcd_fill_rect(ARENA_L, 146, ARENA_R - ARENA_L, 16, COLOR_DARK_BG);
}

/* Paints only the paddle strips that changed */
static void draw_paddle(void)
{
    int16_t old = s_drawn_paddle_x;
    int16_t x = s_paddle_x;
    if (old == x) return;

    int16_t d = (int16_t)(x - old);
    if ((old < 0) || (iabs(d) >= PADDLE_W))
    {
        if (old >= 0) sys_lcd_fill_rect((uint16_t)old, PADDLE_Y, PADDLE_W, PADDLE_H, COLOR_DARK_BG);
        sys_lcd_fill_rect((uint16_t)x, PADDLE_Y, PADDLE_W, PADDLE_H, COLOR_CYAN);
    }
    else if (d > 0)
    {
        sys_lcd_fill_rect((uint16_t)old, PADDLE_Y, (uint16_t)d, PADDLE_H, COLOR_DARK_BG);
        sys_lcd_fill_rect((uint16_t)(old + PADDLE_W), PADDLE_Y, (uint16_t)d, PADDLE_H, COLOR_CYAN);
    }
    else
    {
        sys_lcd_fill_rect((uint16_t)(x + PADDLE_W), PADDLE_Y, (uint16_t)(-d), PADDLE_H, COLOR_DARK_BG);
        sys_lcd_fill_rect((uint16_t)x, PADDLE_Y, (uint16_t)(-d), PADDLE_H, COLOR_CYAN);
    }
    s_drawn_paddle_x = x;
}

static void draw_ball(void)
{
    int16_t x = (int16_t)(s_ball_x / FP);
    int16_t y = (int16_t)(s_ball_y / FP);
    if ((x == s_drawn_ball_x) && (y == s_drawn_ball_y)) return;

    if (s_drawn_ball_x >= 0)
    {
        sys_lcd_fill_rect((uint16_t)s_drawn_ball_x, (uint16_t)s_drawn_ball_y, BALL, BALL, COLOR_DARK_BG);
        redraw_bricks_in(s_drawn_ball_x, s_drawn_ball_y, BALL, BALL);
    }
    if (y < SDK_SCREEN_H - BALL)
    {
        sys_lcd_fill_rect((uint16_t)x, (uint16_t)y, BALL, BALL, COLOR_WHITE);
        s_drawn_ball_x = x;
        s_drawn_ball_y = y;
    }
    else
    {
        s_drawn_ball_x = -1;
    }
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

/* ---------------------------------------------------------------- game flow */

static void add_score(uint32_t points)
{
    s_score += points;
    if (s_score > s_best) s_best = s_score;
    draw_hud();
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
            s_hp[r][c] = (ch == '2') ? 2U : ((ch == '1') ? 1U : 0U);
            if (s_hp[r][c] != 0U) s_bricks_left++;
        }
    }
    int32_t speed = SPEED_START + (int32_t)s_level * SPEED_STEP;
    s_speed = (speed > SPEED_MAX) ? SPEED_MAX : speed;
}

static void serve(void)
{
    s_state = ST_SERVE;
    s_vx = s_speed / 3;
    s_vy = -s_speed;
    draw_message("PRESS A OR TAP TO LAUNCH", COLOR_AMBER);
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
    draw_message("    A: PLAY AGAIN", COLOR_WHITE);
}

static void hit_brick(uint8_t r, uint8_t c)
{
    s_hp[r][c]--;
    draw_brick(r, c);
    sound(s_snd_brick, 1);
    if (s_hp[r][c] == 0U)
    {
        s_bricks_left--;
        add_score(10U * ((uint32_t)s_level + 1U));
    }
    else
    {
        add_score(5U);
    }
}

/* Resolves one brick collision: the axis with the smaller overlap is reflected */
static bool collide_bricks(void)
{
    int16_t bx = (int16_t)(s_ball_x / FP);
    int16_t by = (int16_t)(s_ball_y / FP);

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
                s_vx = ball_left ? -iabs(s_vx) : iabs(s_vx);
                s_ball_x += (ball_left ? -ox : ox) * FP;
            }
            else
            {
                s_vy = ball_above ? -iabs(s_vy) : iabs(s_vy);
                s_ball_y += (ball_above ? -oy : oy) * FP;
            }
            hit_brick(r, c);
            return true;
        }
    }
    return false;
}

static void collide_paddle(void)
{
    int16_t bx = (int16_t)(s_ball_x / FP);
    int16_t by = (int16_t)(s_ball_y / FP);
    if ((s_vy <= 0) || (by + BALL < PADDLE_Y) || (by >= PADDLE_Y + PADDLE_H) ||
        (bx + BALL <= s_paddle_x) || (bx >= s_paddle_x + PADDLE_W))
    {
        return;
    }

    int32_t offset = (int32_t)(bx + BALL / 2) - (int32_t)(s_paddle_x + PADDLE_W / 2); /* -31..31 px */
    int32_t vx = offset * s_speed / 20;
    int32_t vmax = s_speed * 3 / 2;
    if (vx > vmax) vx = vmax;
    if (vx < -vmax) vx = -vmax;
    if (iabs(vx) < FP / 4) vx = (offset < 0) ? -(FP / 4) : (FP / 4);

    s_vx = vx;
    s_vy = -s_speed;
    s_ball_y = (int32_t)(PADDLE_Y - BALL) * FP;
    sound(s_snd_paddle, 1);
}

/* Moves the ball in sub-steps of at most 3 px so it cannot pass through a brick */
static void update_ball(uint32_t now)
{
    int32_t vmax = (iabs(s_vx) > iabs(s_vy)) ? iabs(s_vx) : iabs(s_vy);
    int32_t steps = vmax / (3 * FP) + 1;

    for (int32_t i = 0; i < steps; i++)
    {
        s_ball_x += s_vx / steps;
        s_ball_y += s_vy / steps;

        if (s_ball_x < ARENA_L * FP)
        {
            s_ball_x = ARENA_L * FP;
            s_vx = iabs(s_vx);
            sound(s_snd_wall, 1);
        }
        else if (s_ball_x > (ARENA_R - BALL) * FP)
        {
            s_ball_x = (ARENA_R - BALL) * FP;
            s_vx = -iabs(s_vx);
            sound(s_snd_wall, 1);
        }
        if (s_ball_y < ARENA_T * FP)
        {
            s_ball_y = ARENA_T * FP;
            s_vy = iabs(s_vy);
            sound(s_snd_wall, 1);
        }

        if (collide_bricks() && (s_bricks_left == 0U))
        {
            s_level++;
            sound(s_snd_level, 7);
            load_level();
            draw_arena();
            serve();
            return;
        }
        collide_paddle();

        if (s_ball_y > SDK_SCREEN_H * FP)
        {
            draw_ball();
            if (--s_lives == 0U)
            {
                draw_hud();
                game_over(now);
            }
            else
            {
                sound(s_snd_lost, 3);
                draw_hud();
                serve();
            }
            return;
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
        s_paddle_x = (int16_t)(st->mouse.x - PADDLE_W / 2);
    }
    s_prev_mouse_x = (st->mouse.source == SDK_MOUSE_SRC_USB) ? st->mouse.x : -1;

    if (s_paddle_x < ARENA_L) s_paddle_x = ARENA_L;
    if (s_paddle_x > ARENA_R - PADDLE_W) s_paddle_x = ARENA_R - PADDLE_W;
}

void app_main(void)
{
    SDK_Input_t in;
    SDK_InputState_t st;
    uint32_t next_frame = 0;

    load_best();
    s_paddle_x = (SDK_SCREEN_W - PADDLE_W) / 2;
    new_game();
    sdk_input_init(&in);

    for (;;)
    {
        uint32_t now = sys_get_tick();
        (void)sys_input_get_state(&st);
        uint32_t keys = st.keys;
        if ((st.mouse.buttons & SDK_MOUSE_BTN_LEFT) != 0U)
        {
            keys |= KEY_A; /* click / tap launches the ball */
        }
        sdk_input_update(&in, keys, now);

        if (in.pressed & KEY_EXIT)
        {
            save_best();
            return;
        }

        switch (s_state)
        {
            case ST_SERVE:
                move_paddle(&st);
                s_ball_x = (int32_t)(s_paddle_x + (PADDLE_W - BALL) / 2) * FP;
                s_ball_y = (int32_t)(PADDLE_Y - BALL - 1) * FP;
                if (in.pressed & (KEY_A | KEY_UP))
                {
                    clear_message();
                    s_state = ST_PLAY;
                }
                break;

            case ST_PLAY:
                if (in.pressed & KEY_START)
                {
                    s_state = ST_PAUSE;
                    draw_hud();
                    break;
                }
                move_paddle(&st);
                update_ball(now);
                break;

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    s_state = ST_PLAY;
                    draw_hud();
                }
                break;

            case ST_OVER:
                if ((in.pressed & (KEY_A | KEY_UP)) && ((now - s_over_tick) > 500U))
                {
                    new_game();
                }
                break;

            default:
                break;
        }

        if (s_state != ST_OVER)
        {
            draw_paddle();
            draw_ball();
        }
        sdk_frame_sync(&next_frame, 16U);
    }
}
