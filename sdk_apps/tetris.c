#include "console_sdk.h"

/* =========================================================================
 * Cyber Tetris: 7-bag randomizer, SRS rotation states with simple wall kicks,
 * lock delay, levels, standard scoring, ghost piece, hold, pause, line flash.
 * Controls: LEFT/RIGHT move, DOWN soft drop, A/UP rotate, B hard drop,
 *           X (pad Y / key C) hold, START (pad L2/R2 / key P) pause, ESC exit.
 * Best score: SD:/appdata/tetris/best.dat
 * ========================================================================= */

#define COLS            10
#define ROWS            20
#define CELL            10
#define BOARD_X         60
#define BOARD_Y         28

#define LOCK_DELAY_MS   400U
#define LOCK_RESETS_MAX 15U
#define SOFT_DROP_MS    40U
#define CLEAR_FLASH_MS  240U
#define NO_PIECE        0xFFU
#define COLOR_HINT      0x8410U

/* Display layer codes: 0 empty, 1..7 piece colours, 8..14 ghost of piece 1..7, 15 flash */
#define CODE_GHOST      7U
#define CODE_FLASH      15U
#define CODE_UNKNOWN    0xFFU

enum { ST_READY, ST_PLAY, ST_CLEAR, ST_PAUSE, ST_OVER };

/* SRS states 0, R, 2, L as 4x4 masks (bit 15 = row 0, column 0) */
static const uint16_t s_shapes[7][4] = {
    /* I */ { 0x0F00, 0x2222, 0x00F0, 0x4444 },
    /* O */ { 0x6600, 0x6600, 0x6600, 0x6600 },
    /* T */ { 0x4E00, 0x4640, 0x0E40, 0x4C40 },
    /* S */ { 0x6C00, 0x4620, 0x06C0, 0x8C40 },
    /* Z */ { 0xC600, 0x2640, 0x0C60, 0x4C80 },
    /* J */ { 0x8E00, 0x6440, 0x0E20, 0x44C0 },
    /* L */ { 0x2E00, 0x4460, 0x0E80, 0xC440 },
};

static const uint16_t s_colors[8] = {
    COLOR_DARK_BG, COLOR_CYAN, COLOR_YELLOW, COLOR_MAGENTA,
    COLOR_NEON_GREEN, COLOR_RED, COLOR_BLUE, COLOR_AMBER,
};

static const int8_t s_kicks[][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { -2, 0 }, { 2, 0 }, { 0, -1 } };
static const uint16_t s_line_points[5] = { 0, 40, 100, 300, 1200 };

static const SDK_Note_t s_snd_move[]  = { { 1, 4 } };
static const SDK_Note_t s_snd_lock[]  = { { 1, 12 } };
static const SDK_Note_t s_snd_clear[] = { { 1, 40 }, { 0, 30 }, { 1, 40 }, { 0, 30 }, { 1, 80 } };
static const SDK_Note_t s_snd_over[]  = { { 1, 150 }, { 0, 80 }, { 1, 150 }, { 0, 80 }, { 1, 400 } };

static uint8_t  s_board[ROWS][COLS];
static uint8_t  s_shown[ROWS][COLS];
static uint8_t  s_bag[7];
static uint8_t  s_bag_pos = 7;

static uint8_t  s_cur, s_rot, s_next, s_hold = NO_PIECE;
static bool     s_hold_used;
static int8_t   s_x, s_y;

static uint8_t  s_state = ST_READY;
static uint32_t s_score, s_lines, s_level, s_best, s_best_saved;
static uint32_t s_fall_tick, s_lock_tick, s_clear_tick;
static uint8_t  s_lock_resets;
static bool     s_on_ground;
static uint8_t  s_clear_rows[4];
static uint8_t  s_clear_n;

static void sound(const SDK_Note_t *notes, uint32_t n)
{
    (void)sys_audio_play(notes, n, 0);
}

static uint16_t str_len(const char *s)
{
    uint16_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* ---------------------------------------------------------------- pieces */

static uint8_t bag_next(void)
{
    if (s_bag_pos >= 7U)
    {
        for (uint8_t i = 0; i < 7U; i++)
        {
            s_bag[i] = i;
        }
        for (uint8_t i = 6; i > 0U; i--)
        {
            uint8_t j = (uint8_t)sys_random_range(i + 1U);
            uint8_t t = s_bag[i];
            s_bag[i] = s_bag[j];
            s_bag[j] = t;
        }
        s_bag_pos = 0;
    }
    return s_bag[s_bag_pos++];
}

static bool cell_set(uint8_t type, uint8_t rot, int8_t r, int8_t c)
{
    return (s_shapes[type][rot & 3U] & (0x8000U >> (r * 4 + c))) != 0U;
}

static bool collides(uint8_t type, uint8_t rot, int8_t x, int8_t y)
{
    for (int8_t r = 0; r < 4; r++)
    {
        for (int8_t c = 0; c < 4; c++)
        {
            if (!cell_set(type, rot, r, c)) continue;
            int8_t gx = (int8_t)(x + c);
            int8_t gy = (int8_t)(y + r);
            if ((gx < 0) || (gx >= COLS) || (gy >= ROWS)) return true;
            if ((gy >= 0) && (s_board[gy][gx] != 0U)) return true;
        }
    }
    return false;
}

static int8_t ghost_y(void)
{
    int8_t y = s_y;
    while (!collides(s_cur, s_rot, s_x, (int8_t)(y + 1)))
    {
        y++;
    }
    return y;
}

/* ---------------------------------------------------------------- drawing */

static void draw_cell(uint8_t c, uint8_t r, uint8_t code)
{
    uint16_t px = (uint16_t)(BOARD_X + c * CELL);
    uint16_t py = (uint16_t)(BOARD_Y + r * CELL);

    if (code == CODE_FLASH)
    {
        sys_lcd_fill_rect(px, py, CELL - 1U, CELL - 1U, COLOR_WHITE);
    }
    else if (code > CODE_GHOST)
    {
        sys_lcd_fill_rect(px, py, CELL - 1U, CELL - 1U, s_colors[code - CODE_GHOST]);
        sys_lcd_fill_rect((uint16_t)(px + 1U), (uint16_t)(py + 1U), CELL - 3U, CELL - 3U, COLOR_DARK_BG);
    }
    else
    {
        sys_lcd_fill_rect(px, py, CELL - 1U, CELL - 1U, s_colors[code]);
    }
}

static void put_piece(uint8_t want[ROWS][COLS], uint8_t type, uint8_t rot, int8_t x, int8_t y, uint8_t code)
{
    for (int8_t r = 0; r < 4; r++)
    {
        for (int8_t c = 0; c < 4; c++)
        {
            int8_t gx = (int8_t)(x + c);
            int8_t gy = (int8_t)(y + r);
            if (cell_set(type, rot, r, c) && (gy >= 0) && (gy < ROWS) && (gx >= 0) && (gx < COLS))
            {
                want[gy][gx] = code;
            }
        }
    }
}

/* Draws only the cells whose content changed since the last call */
static void render_board(uint32_t now)
{
    static uint8_t want[ROWS][COLS];
    sdk_memcpy(want, s_board, sizeof(want));

    if (s_state == ST_PLAY)
    {
        put_piece(want, s_cur, s_rot, s_x, ghost_y(), (uint8_t)(CODE_GHOST + 1U + s_cur));
        put_piece(want, s_cur, s_rot, s_x, s_y, (uint8_t)(s_cur + 1U));
    }
    else if ((s_state == ST_CLEAR) && ((((now - s_clear_tick) / 60U) % 2U) == 0U))
    {
        for (uint8_t i = 0; i < s_clear_n; i++)
        {
            sdk_memset(want[s_clear_rows[i]], CODE_FLASH, COLS);
        }
    }

    for (uint8_t r = 0; r < ROWS; r++)
    {
        for (uint8_t c = 0; c < COLS; c++)
        {
            if (want[r][c] != s_shown[r][c])
            {
                draw_cell(c, r, want[r][c]);
                s_shown[r][c] = want[r][c];
            }
        }
    }
}

static void draw_mini(uint16_t x0, uint16_t y0, uint8_t type, uint16_t bg)
{
    sys_lcd_fill_rect(x0, y0, 48, 32, bg);
    if (type == NO_PIECE) return;
    for (int8_t r = 0; r < 3; r++)
    {
        for (int8_t c = 0; c < 4; c++)
        {
            if (cell_set(type, 0, r, c))
            {
                sys_lcd_fill_rect((uint16_t)(x0 + 4U + c * 10U), (uint16_t)(y0 + 2U + r * 10U), 9, 9, s_colors[type + 1U]);
            }
        }
    }
}

static void draw_value(uint16_t y, uint32_t v, uint8_t digits, uint16_t color)
{
    char buf[12];
    sdk_uint_to_str(v, buf, digits);
    sys_draw_text(190, y, buf, color, COLOR_PANEL_BG, 1);
}

static void draw_stats(void)
{
    draw_value(108, s_score, 6, COLOR_NEON_GREEN);
    draw_value(138, s_level, 2, COLOR_MAGENTA);
    draw_value(168, s_lines, 3, COLOR_CYAN);
    draw_value(198, s_best, 6, COLOR_AMBER);
}

static void draw_static_ui(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_lcd_fill_rect(0, 0, SDK_SCREEN_W, 20, COLOR_PANEL_BG);
    sys_lcd_fill_rect(0, 20, SDK_SCREEN_W, 2, COLOR_CYAN);
    sys_draw_text(10, 6, "CYBER TETRIS", COLOR_CYAN, COLOR_PANEL_BG, 1);
    sys_draw_text(214, 6, "ESC: EXIT", COLOR_WHITE, COLOR_PANEL_BG, 1);

    sys_lcd_fill_rect(BOARD_X - 2, BOARD_Y - 2, COLS * CELL + 4, 2, COLOR_CYAN);
    sys_lcd_fill_rect(BOARD_X - 2, BOARD_Y + ROWS * CELL, COLS * CELL + 4, 2, COLOR_CYAN);
    sys_lcd_fill_rect(BOARD_X - 2, BOARD_Y, 2, ROWS * CELL, COLOR_CYAN);
    sys_lcd_fill_rect(BOARD_X + COLS * CELL, BOARD_Y, 2, ROWS * CELL, COLOR_CYAN);

    sys_draw_text(6, 36, "HOLD", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_mini(4, 48, s_hold, COLOR_DARK_BG);
    sys_draw_text(2, 110, "ROT A/UP", COLOR_HINT, COLOR_DARK_BG, 1);
    sys_draw_text(2, 124, "DROP B", COLOR_HINT, COLOR_DARK_BG, 1);
    sys_draw_text(2, 138, "HOLD Y/C", COLOR_HINT, COLOR_DARK_BG, 1);
    sys_draw_text(2, 152, "PAUSE L2", COLOR_HINT, COLOR_DARK_BG, 1);
    sys_draw_text(2, 166, "   OR P", COLOR_HINT, COLOR_DARK_BG, 1);

    sys_lcd_fill_rect(180, 28, 124, 200, COLOR_PANEL_BG);
    sys_draw_text(190, 36, "NEXT", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sys_draw_text(190, 96, "SCORE", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sys_draw_text(190, 126, "LEVEL", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sys_draw_text(190, 156, "LINES", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sys_draw_text(190, 186, "BEST", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_stats();

    sdk_memset(s_shown, CODE_UNKNOWN, sizeof(s_shown));
}

static void draw_board_message(const char *l1, const char *l2, uint16_t color)
{
    sys_lcd_fill_rect(BOARD_X + 4, 96, COLS * CELL - 8, 40, COLOR_PANEL_BG);
    sys_draw_text((uint16_t)(BOARD_X + 50U - str_len(l1) * 3U), 104, l1, color, COLOR_PANEL_BG, 1);
    sys_draw_text((uint16_t)(BOARD_X + 50U - str_len(l2) * 3U), 120, l2, COLOR_WHITE, COLOR_PANEL_BG, 1);
    sdk_memset(s_shown, CODE_UNKNOWN, sizeof(s_shown));
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
        (void)sys_load_state("tetris.sav", &s_best, sizeof(s_best));
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

static uint32_t gravity_ms(void)
{
    uint32_t dec = s_level * 35U;
    return (dec >= 370U) ? 80U : (450U - dec);
}

static void add_score(uint32_t points)
{
    s_score += points;
    if (s_score > s_best) s_best = s_score;
    draw_stats();
}

static void game_over(void)
{
    s_state = ST_OVER;
    save_best();
    sound(s_snd_over, 5);
    draw_board_message("GAME OVER", "A: AGAIN", COLOR_RED);
}

static bool spawn(uint8_t type, uint32_t now)
{
    s_cur = type;
    s_rot = 0;
    s_x = 3;
    s_y = (type == 0U) ? -1 : 0;
    s_on_ground = false;
    s_lock_resets = 0;
    s_fall_tick = now;
    if (collides(s_cur, s_rot, s_x, s_y))
    {
        game_over();
        return false;
    }
    return true;
}

static void spawn_next(uint32_t now)
{
    uint8_t type = s_next;
    s_next = bag_next();
    draw_mini(190, 48, s_next, COLOR_PANEL_BG);
    s_hold_used = false;
    (void)spawn(type, now);
}

static void start_game(uint32_t now)
{
    sdk_memset(s_board, 0, sizeof(s_board));
    s_score = 0;
    s_lines = 0;
    s_level = 0;
    s_hold = NO_PIECE;
    s_bag_pos = 7;
    s_next = bag_next();
    s_state = ST_PLAY;
    draw_static_ui();
    spawn_next(now);
}

static void lock_reset(uint32_t now)
{
    if (s_on_ground && (s_lock_resets < LOCK_RESETS_MAX))
    {
        s_lock_tick = now;
        s_lock_resets++;
    }
}

static void try_move(int8_t dx, uint32_t now)
{
    if (!collides(s_cur, s_rot, (int8_t)(s_x + dx), s_y))
    {
        s_x = (int8_t)(s_x + dx);
        lock_reset(now);
        sound(s_snd_move, 1);
    }
}

static void try_rotate(uint32_t now)
{
    uint8_t nr = (uint8_t)((s_rot + 1U) & 3U);
    for (uint8_t k = 0; k < sizeof(s_kicks) / sizeof(s_kicks[0]); k++)
    {
        int8_t nx = (int8_t)(s_x + s_kicks[k][0]);
        int8_t ny = (int8_t)(s_y + s_kicks[k][1]);
        if (!collides(s_cur, nr, nx, ny))
        {
            s_rot = nr;
            s_x = nx;
            s_y = ny;
            lock_reset(now);
            sound(s_snd_move, 1);
            return;
        }
    }
}

static void lock_piece(uint32_t now)
{
    for (int8_t r = 0; r < 4; r++)
    {
        for (int8_t c = 0; c < 4; c++)
        {
            int8_t gy = (int8_t)(s_y + r);
            if (cell_set(s_cur, s_rot, r, c) && (gy >= 0))
            {
                s_board[gy][s_x + c] = (uint8_t)(s_cur + 1U);
            }
        }
    }

    s_clear_n = 0;
    for (uint8_t r = 0; r < ROWS; r++)
    {
        bool full = true;
        for (uint8_t c = 0; c < COLS; c++)
        {
            if (s_board[r][c] == 0U)
            {
                full = false;
                break;
            }
        }
        if (full)
        {
            s_clear_rows[s_clear_n++] = r;
        }
    }

    if (s_clear_n > 0U)
    {
        s_state = ST_CLEAR;
        s_clear_tick = now;
        sound(s_snd_clear, 5);
    }
    else
    {
        sound(s_snd_lock, 1);
        spawn_next(now);
    }
}

static void finish_clear(uint32_t now)
{
    int8_t dst = ROWS - 1;
    for (int8_t src = ROWS - 1; src >= 0; src--)
    {
        bool cleared = false;
        for (uint8_t i = 0; i < s_clear_n; i++)
        {
            cleared |= (s_clear_rows[i] == (uint8_t)src);
        }
        if (!cleared)
        {
            if (dst != src)
            {
                sdk_memcpy(s_board[dst], s_board[src], COLS);
            }
            dst--;
        }
    }
    while (dst >= 0)
    {
        sdk_memset(s_board[dst], 0, COLS);
        dst--;
    }

    s_lines += s_clear_n;
    add_score(s_line_points[s_clear_n] * (s_level + 1U));
    s_level = s_lines / 10U;
    draw_stats();

    s_state = ST_PLAY;
    spawn_next(now);
}

static void hold_piece(uint32_t now)
{
    if (s_hold_used) return;
    uint8_t prev = s_hold;
    s_hold = s_cur;
    draw_mini(4, 48, s_hold, COLOR_DARK_BG);
    if (prev == NO_PIECE)
    {
        spawn_next(now);
    }
    else
    {
        (void)spawn(prev, now);
    }
    s_hold_used = true;
}

static void play_frame(const SDK_Input_t *in, uint32_t now, uint32_t *last_h)
{
    if (in->pressed & KEY_START)
    {
        s_state = ST_PAUSE;
        draw_board_message("PAUSED", "START: GO", COLOR_YELLOW);
        return;
    }
    if (in->pressed & KEY_X)
    {
        hold_piece(now);
        if (s_state != ST_PLAY) return;
    }

    if (in->pressed & KEY_LEFT)  *last_h = KEY_LEFT;
    if (in->pressed & KEY_RIGHT) *last_h = KEY_RIGHT;
    uint32_t h = in->held & (KEY_LEFT | KEY_RIGHT);
    if (h == (KEY_LEFT | KEY_RIGHT)) h = *last_h;
    if ((in->repeat & h) != 0U)
    {
        try_move((h == KEY_LEFT) ? -1 : 1, now);
    }

    if (in->pressed & (KEY_A | KEY_UP))
    {
        try_rotate(now);
    }

    if (in->pressed & KEY_B)
    {
        int8_t gy = ghost_y();
        add_score(2U * (uint32_t)(gy - s_y));
        s_y = gy;
        lock_piece(now);
        return;
    }

    bool soft = (in->held & KEY_DOWN) != 0U;
    if ((now - s_fall_tick) >= (soft ? SOFT_DROP_MS : gravity_ms()))
    {
        s_fall_tick = now;
        if (!collides(s_cur, s_rot, s_x, (int8_t)(s_y + 1)))
        {
            s_y++;
            if (soft) add_score(1U);
        }
    }

    bool grounded = collides(s_cur, s_rot, s_x, (int8_t)(s_y + 1));
    if (grounded && !s_on_ground)
    {
        s_lock_tick = now;
    }
    s_on_ground = grounded;
    if (s_on_ground && ((now - s_lock_tick) >= LOCK_DELAY_MS))
    {
        lock_piece(now);
    }
}

/* ---------------------------------------------------------------- main loop */

void app_main(void)
{
    SDK_Input_t in;
    uint32_t next_frame = 0;
    uint32_t last_h = 0;

    load_best();
    draw_static_ui();
    draw_board_message("PRESS A", "TO START", COLOR_AMBER);
    sdk_input_init(&in);

    for (;;)
    {
        uint32_t now = sys_get_tick();
        sdk_input_update(&in, sys_get_keys(), now);

        if (in.pressed & KEY_EXIT)
        {
            save_best();
            return;
        }

        switch (s_state)
        {
            case ST_READY:
            case ST_OVER:
                if (in.pressed & (KEY_A | KEY_UP))
                {
                    start_game(now);
                }
                break;

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    s_state = ST_PLAY;
                    sdk_memset(s_shown, CODE_UNKNOWN, sizeof(s_shown));
                    s_fall_tick = now;
                    s_lock_tick = now;
                }
                break;

            case ST_CLEAR:
                if ((now - s_clear_tick) >= CLEAR_FLASH_MS)
                {
                    finish_clear(now);
                }
                break;

            case ST_PLAY:
                play_frame(&in, now, &last_h);
                break;

            default:
                break;
        }

        if ((s_state == ST_PLAY) || (s_state == ST_CLEAR))
        {
            render_board(now);
        }
        sdk_frame_sync(&next_frame, 16U);
    }
}
