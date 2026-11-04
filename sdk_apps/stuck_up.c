#include "console_sdk.h"

/* =========================================================================
 * STACK UP (ZX Spectrum 1991 style arcade puzzle for STM32 Console)
 *
 * Faithful recreation of the screenshot layout:
 *  - Rainbow diagonal ZX Spectrum dithered background
 *  - 6x12 Playfield Well with 16x16 px beveled symbol blocks:
 *      1: Red Heart      2: Magenta Circle-Slash   3: Green Triangle
 *      4: Cyan Arrow     5: Yellow Diamond         6: Grey Steel Block
 *  - Left Panels:  1UP / TOP score, ROUND / LINES LEFT, NEXT 3-block preview
 *  - Right Panels: SPEED bar, CREDIT, live BLOCKS counter per symbol + Sigma sum
 *  - Mechanics: Horizontal & Vertical 3-block piece, color cycling (A/UP),
 *               orientation flip (X), chain-reaction cascades, round clear!
 *  - Best score: SD:/appdata/stackup/best.dat
 * ========================================================================= */

#define COLS            6
#define ROWS            12
#define CELL            16

/* Playfield Well coordinates (96 x 192 px, centered on 320x240 screen) */
#define WELL_X          112
#define WELL_Y          24
#define WELL_W          (COLS * CELL)
#define WELL_H          (ROWS * CELL)

#define SYM_EMPTY       0U
#define SYM_HEART       1U /* Red Heart */
#define SYM_CIRCLE      2U /* Magenta Slashed Circle */
#define SYM_TRIANGLE    3U /* Green Triangle */
#define SYM_ARROW       4U /* Cyan Right Arrow */
#define SYM_DIAMOND     5U /* Yellow Diamond */
#define SYM_STEEL       6U /* Grey Indestructible Block */
#define SYM_FLASH       7U /* White clear flash */
#define SYM_COUNT       5U /* 5 playable symbol types */

#define CLEAR_FLASH_MS  260U
#define CASCADE_STEP_MS 90U

enum { ST_READY, ST_FALL, ST_FLASH, ST_CASCADE, ST_ROUND_WIN, ST_PAUSE, ST_OVER };

/* 16x16 Block Symbol Bitmaps (1 bit per pixel: 1 = black ink outline/icon) */
static const uint16_t s_sym_bmp[6][16] = {
    /* 0: Red Heart */
    { 0x0000, 0x0000, 0x0000, 0x0E38, 0x1F7C, 0x3FFE, 0x3FFE, 0x3FFE,
      0x1FFC, 0x0FF8, 0x07F0, 0x03E0, 0x01C0, 0x0080, 0x0000, 0x0000 },
    /* 1: Magenta Slashed Circle */
    { 0x0000, 0x0000, 0x07E0, 0x1818, 0x200C, 0x4012, 0x4022, 0x4042,
      0x4082, 0x4102, 0x4202, 0x3004, 0x1818, 0x07E0, 0x0000, 0x0000 },
    /* 2: Green Triangle */
    { 0x0000, 0x0000, 0x0180, 0x0240, 0x0240, 0x0420, 0x05A0, 0x0990,
      0x0810, 0x1008, 0x13C8, 0x2004, 0x3FFC, 0x0000, 0x0000, 0x0000 },
    /* 3: Cyan Right Arrow */
    { 0x0000, 0x0000, 0x0000, 0x00C0, 0x0060, 0x3F30, 0x2018, 0x200C,
      0x2018, 0x3F30, 0x0060, 0x00C0, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* 4: Yellow Diamond */
    { 0x0000, 0x0000, 0x0180, 0x03C0, 0x0660, 0x0C30, 0x1818, 0x318C,
      0x318C, 0x1818, 0x0C30, 0x0660, 0x03C0, 0x0180, 0x0000, 0x0000 },
    /* 5: Grey Steel Block */
    { 0x0000, 0x0000, 0x1FF8, 0x1008, 0x1008, 0x1008, 0x1008, 0x1008,
      0x1008, 0x1008, 0x1008, 0x1008, 0x1008, 0x1FF8, 0x0000, 0x0000 }
};

/* 8x8 Mini Icons for the bottom-right BLOCKS statistics panel */
static const uint8_t s_mini_bmp[5][8] = {
    { 0x00, 0x66, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00 }, /* Heart */
    { 0x3C, 0x46, 0x4A, 0x52, 0x62, 0x42, 0x3C, 0x00 }, /* Circle */
    { 0x18, 0x24, 0x24, 0x42, 0x42, 0x81, 0xFF, 0x00 }, /* Triangle */
    { 0x08, 0x0C, 0xFE, 0x83, 0xFE, 0x0C, 0x08, 0x00 }, /* Arrow */
    { 0x18, 0x3C, 0x66, 0xC3, 0x66, 0x3C, 0x18, 0x00 }  /* Diamond */
};

static const uint16_t s_sym_colors[8] = {
    COLOR_BLACK,
    0xF800U, /* 1: Red */
    0xF81FU, /* 2: Magenta */
    0x07E0U, /* 3: Green */
    0x07FFU, /* 4: Cyan */
    0xFFE0U, /* 5: Yellow */
    0x8410U, /* 6: Steel Grey */
    0xFFFFU  /* 7: Flash White */
};

static const SDK_Note_t s_snd_move[]  = { { 1, 5 } };
static const SDK_Note_t s_snd_cycle[] = { { 1, 10 } };
static const SDK_Note_t s_snd_lock[]  = { { 1, 14 } };
static const SDK_Note_t s_snd_match[] = { { 1, 35 }, { 0, 20 }, { 1, 50 }, { 0, 20 }, { 1, 70 } };
static const SDK_Note_t s_snd_round[] = { { 1, 50 }, { 0, 30 }, { 1, 50 }, { 0, 30 }, { 1, 60 }, { 0, 30 }, { 1, 180 } };
static const SDK_Note_t s_snd_over[]  = { { 1, 150 }, { 0, 80 }, { 1, 150 }, { 0, 80 }, { 1, 350 } };

/* Pre-rendered 16x16 tile sprites & 8x8 mini icons in External SRAM */
static SDK_EXT_BSS uint16_t s_tile_spr[8][CELL * CELL];
static SDK_EXT_BSS uint16_t s_mini_spr[5][8 * 8];
static SDK_EXT_BSS uint16_t s_row_buf[SDK_SCREEN_W];

static uint8_t  s_grid[ROWS][COLS];
static uint8_t  s_shown[ROWS][COLS];
static uint8_t  s_mark[ROWS][COLS];

/* Active falling 3-block piece & next piece */
static uint8_t  s_cur[3];
static uint8_t  s_next[3];
static int8_t   s_px, s_py;
static bool     s_vertical;     /* false = horizontal 3x1, true = vertical 1x3 */

static uint8_t  s_state = ST_READY;
static uint32_t s_state_tick;
static uint32_t s_fall_tick;
static uint16_t s_chain_mult;

static uint32_t s_score;
static uint32_t s_best = 3000U, s_best_saved = 3000U;
static uint8_t  s_round;
static int16_t  s_lines_left;
static uint8_t  s_speed_lvl;
static uint8_t  s_credits = 2U;

static void sound(const SDK_Note_t *notes, uint32_t n)
{
    (void)sys_audio_play(notes, n, 0);
}

/* ---------------------------------------------------------------- graphics build */

static void build_sprites(void)
{
    /* Empty black cell */
    for (uint16_t i = 0; i < CELL * CELL; i++) s_tile_spr[SYM_EMPTY][i] = COLOR_BLACK;

    /* Flash white cell */
    for (uint16_t i = 0; i < CELL * CELL; i++) s_tile_spr[SYM_FLASH][i] = COLOR_WHITE;

    /* 6 Beveled Symbol Blocks (16x16) */
    for (uint8_t s = 1; s <= 6U; s++)
    {
        uint16_t bg = s_sym_colors[s];
        const uint16_t *bmp = s_sym_bmp[s - 1U];

        for (uint8_t y = 0; y < CELL; y++)
        {
            for (uint8_t x = 0; x < CELL; x++)
            {
                uint16_t c = bg;
                /* Rounded dark corners */
                if (((y == 0U || y == 15U) && (x == 0U || x == 15U)))
                {
                    c = COLOR_BLACK;
                }
                /* Top/Left 3D bevel highlight */
                else if (y == 0U || x == 0U)
                {
                    c = COLOR_WHITE;
                }
                /* Bottom/Right 3D shadow */
                else if (y == 15U || x == 15U)
                {
                    c = 0x2104U;
                }
                /* Inner dotted corner accents (like ZX Spectrum tiles) */
                else if ((y == 2U || y == 13U) && (x == 2U || x == 13U))
                {
                    c = COLOR_BLACK;
                }
                /* Center symbol bitmap */
                else if ((bmp[y] & (0x8000U >> x)) != 0U)
                {
                    c = (s == SYM_STEEL) ? COLOR_WHITE : COLOR_BLACK;
                }
                s_tile_spr[s][y * CELL + x] = c;
            }
        }
    }

    /* 5 Mini 8x8 Icons for BLOCKS panel */
    for (uint8_t s = 0; s < 5U; s++)
    {
        uint16_t bg = s_sym_colors[s + 1U];
        for (uint8_t y = 0; y < 8U; y++)
        {
            for (uint8_t x = 0; x < 8U; x++)
            {
                bool ink = (s_mini_bmp[s][y] & (0x80U >> x)) != 0U;
                s_mini_spr[s][y * 8U + x] = ink ? COLOR_BLACK : bg;
            }
        }
    }
}

/* Draws the iconic ZX Spectrum diagonal rainbow striped background */
static void draw_rainbow_background(void)
{
    static const uint16_t stripe_pal[8] = {
        COLOR_BLACK, 0x001FU, 0xF800U, 0xF81FU,
        0x07E0U,     0x07FFU, 0xFFE0U, 0xFFFFU
    };

    for (uint16_t y = 0; y < SDK_SCREEN_H; y++)
    {
        for (uint16_t x = 0; x < SDK_SCREEN_W; x++)
        {
            uint16_t band = (x + y) / 22U;
            uint16_t rem  = (x + y) % 22U;
            uint8_t  c1   = (uint8_t)(band & 7U);
            uint8_t  c2   = (uint8_t)((band + 1U) & 7U);

            /* Dither transition zone between rainbow stripes */
            uint16_t col;
            if (rem >= 16U && (((x ^ y) & 1U) != 0U))
            {
                col = stripe_pal[c2];
            }
            else
            {
                col = stripe_pal[c1];
            }
            s_row_buf[x] = col;
        }
        sys_draw_sprite(0, y, SDK_SCREEN_W, 1, s_row_buf);
    }
}

/* Draws a black UI box with double neon magenta border (matching screenshot) */
static void draw_ui_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    sys_lcd_fill_rect(x, y, w, h, COLOR_BLACK);
    sys_lcd_fill_rect((uint16_t)(x + 1U), (uint16_t)(y + 1U), (uint16_t)(w - 2U), 2, COLOR_MAGENTA);
    sys_lcd_fill_rect((uint16_t)(x + 1U), (uint16_t)(y + h - 3U), (uint16_t)(w - 2U), 2, COLOR_MAGENTA);
    sys_lcd_fill_rect((uint16_t)(x + 1U), (uint16_t)(y + 1U), 2, (uint16_t)(h - 2U), COLOR_MAGENTA);
    sys_lcd_fill_rect((uint16_t)(x + w - 3U), (uint16_t)(y + 1U), 2, (uint16_t)(h - 2U), COLOR_MAGENTA);
}

static void draw_num_padded(uint16_t x, uint16_t y, uint32_t v, uint8_t digits, uint16_t fg)
{
    char buf[12];
    sdk_uint_to_str(v, buf, digits);
    sys_draw_text(x, y, buf, fg, COLOR_BLACK, 1);
}

static void update_score_panel(void)
{
    draw_num_padded(28, 38, s_score, 6, COLOR_WHITE);
    draw_num_padded(28, 60, s_best,  6, COLOR_WHITE);
}

static void update_round_panel(void)
{
    draw_num_padded(38, 108, (uint32_t)s_round + 1U, 2, COLOR_WHITE);
    draw_num_padded(38, 140, (uint32_t)((s_lines_left > 0) ? s_lines_left : 0), 2, COLOR_WHITE);
}

static void update_next_panel(void)
{
    for (uint8_t i = 0; i < 3U; i++)
    {
        sys_draw_sprite((uint16_t)(20U + i * 18U), 192U, CELL, CELL, s_tile_spr[s_next[i]]);
    }
}

static void update_speed_panel(void)
{
    sys_lcd_fill_rect(236, 40, 64, 14, COLOR_BLACK);
    uint8_t bars = (uint8_t)((s_speed_lvl > 8U) ? 8U : s_speed_lvl);
    for (uint8_t i = 0; i < bars; i++)
    {
        uint16_t col = (i < 3U) ? COLOR_BLUE : ((i < 6U) ? COLOR_MAGENTA : COLOR_RED);
        sys_lcd_fill_rect((uint16_t)(236U + i * 8U), 40U, 7U, 14U, col);
    }
}

/* Counts live blocks in the well and updates the bottom-right BLOCKS table */
static void update_blocks_stats(void)
{
    uint16_t counts[SYM_COUNT + 1U] = { 0 };
    uint16_t total = 0;

    for (uint8_t r = 0; r < ROWS; r++)
    {
        for (uint8_t c = 0; c < COLS; c++)
        {
            uint8_t v = s_grid[r][c];
            if (v >= 1U && v <= SYM_COUNT)
            {
                counts[v]++;
                total++;
            }
        }
    }

    for (uint8_t s = 1; s <= SYM_COUNT; s++)
    {
        uint16_t y = (uint16_t)(144U + (s - 1U) * 11U);
        sys_lcd_fill_rect(274, y, 24, 8, COLOR_BLACK);
        draw_num_padded(278, y, counts[s], 2, COLOR_WHITE);
    }
    sys_lcd_fill_rect(274, 200, 24, 8, COLOR_BLACK);
    draw_num_padded(278, 200, total, 2, COLOR_YELLOW);
}

static void draw_static_layout(void)
{
    draw_rainbow_background();

    /* Center Well Frame */
    sys_lcd_fill_rect(WELL_X - 2, WELL_Y - 2, WELL_W + 4, WELL_H + 4, COLOR_BLACK);
    sys_lcd_fill_rect(WELL_X, WELL_Y, WELL_W, WELL_H, COLOR_BLACK);

    /* Left Panel 1: 1UP & TOP */
    draw_ui_box(8, 20, 82, 56);
    sys_draw_text(34, 28, "1UP", COLOR_WHITE, COLOR_BLACK, 1);
    sys_draw_text(34, 50, "TOP", COLOR_WHITE, COLOR_BLACK, 1);

    /* Left Panel 2: ROUND & LINES LEFT */
    draw_ui_box(8, 86, 82, 72);
    sys_draw_text(28, 96,  "ROUND", COLOR_WHITE, COLOR_BLACK, 1);
    sys_draw_text(28, 120, "LINES", COLOR_WHITE, COLOR_BLACK, 1);
    sys_draw_text(31, 130, "LEFT",  COLOR_WHITE, COLOR_BLACK, 1);

    /* Left Panel 3: NEXT Piece */
    draw_ui_box(8, 168, 82, 48);
    sys_draw_text(34, 178, "NEXT", COLOR_WHITE, COLOR_BLACK, 1);

    /* Right Panel 1: SPEED */
    draw_ui_box(226, 20, 84, 44);
    sys_draw_text(242, 28, "SPEED", COLOR_WHITE, COLOR_BLACK, 1);

    /* Right Panel 2: CREDIT */
    draw_ui_box(226, 74, 84, 42);
    sys_draw_text(238, 84, "CREDIT", COLOR_WHITE, COLOR_BLACK, 1);
    draw_num_padded(258, 96, s_credits, 2, COLOR_WHITE);

    /* Right Panel 3: BLOCKS Statistics */
    draw_ui_box(226, 124, 84, 92);
    sys_draw_text(238, 132, "BLOCKS", COLOR_WHITE, COLOR_BLACK, 1);
    for (uint8_t s = 0; s < SYM_COUNT; s++)
    {
        uint16_t y = (uint16_t)(144U + s * 11U);
        sys_draw_sprite(236, y, 8, 8, s_mini_spr[s]);
        sys_draw_text(248, y, "=", COLOR_WHITE, COLOR_BLACK, 1);
    }
    sys_draw_text(236, 200, "SUM=", COLOR_YELLOW, COLOR_BLACK, 1);

    update_score_panel();
    update_round_panel();
    update_speed_panel();
    update_blocks_stats();
    sdk_memset(s_shown, 0xFFU, sizeof(s_shown));
}

static void render_well(uint32_t now)
{
    uint8_t want[ROWS][COLS];
    sdk_memcpy(want, s_grid, sizeof(want));

    if (s_state == ST_FALL)
    {
        for (uint8_t i = 0; i < 3U; i++)
        {
            int8_t cx = s_vertical ? s_px : (int8_t)(s_px + i);
            int8_t cy = s_vertical ? (int8_t)(s_py + i) : s_py;
            if (cx >= 0 && cx < COLS && cy >= 0 && cy < ROWS)
            {
                want[cy][cx] = s_cur[i];
            }
        }
    }
    else if (s_state == ST_FLASH)
    {
        bool flash_on = (((now - s_state_tick) / 55U) & 1U) == 0U;
        for (uint8_t r = 0; r < ROWS; r++)
        {
            for (uint8_t c = 0; c < COLS; c++)
            {
                if (s_mark[r][c] != 0U)
                {
                    want[r][c] = flash_on ? SYM_FLASH : SYM_EMPTY;
                }
            }
        }
    }

    for (uint8_t r = 0; r < ROWS; r++)
    {
        for (uint8_t c = 0; c < COLS; c++)
        {
            uint8_t v = want[r][c];
            if (v != s_shown[r][c])
            {
                sys_draw_sprite((uint16_t)(WELL_X + c * CELL),
                                (uint16_t)(WELL_Y + r * CELL),
                                CELL, CELL, s_tile_spr[v]);
                s_shown[r][c] = v;
            }
        }
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
    if (s_best < 1000U) s_best = 3000U;
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

/* ---------------------------------------------------------------- game logic */

static void roll_next_piece(void)
{
    for (uint8_t i = 0; i < 3U; i++)
    {
        s_next[i] = (uint8_t)(1U + sys_random_range(SYM_COUNT));
    }
}

static bool piece_fits(int8_t px, int8_t py, bool vert)
{
    for (uint8_t i = 0; i < 3U; i++)
    {
        int8_t cx = vert ? px : (int8_t)(px + i);
        int8_t cy = vert ? (int8_t)(py + i) : py;
        if (cx < 0 || cx >= COLS || cy >= ROWS) return false;
        if (cy >= 0 && s_grid[cy][cx] != SYM_EMPTY) return false;
    }
    return true;
}

static bool spawn_piece(uint32_t now)
{
    for (uint8_t i = 0; i < 3U; i++) s_cur[i] = s_next[i];
    roll_next_piece();
    update_next_panel();

    s_vertical = false;
    s_px = 1;
    s_py = 0;
    s_fall_tick = now;

    if (!piece_fits(s_px, s_py, s_vertical))
    {
        return false;
    }
    return true;
}

/* Scan for 3+ matching blocks horizontally, vertically, and diagonally */
static uint16_t find_matches(uint16_t *out_lines)
{
    sdk_memset(s_mark, 0, sizeof(s_mark));
    uint16_t lines_found = 0;

    static const int8_t dr[4] = { 0, 1, 1,  1 };
    static const int8_t dc[4] = { 1, 0, 1, -1 };

    for (uint8_t r = 0; r < ROWS; r++)
    {
        for (uint8_t c = 0; c < COLS; c++)
        {
            uint8_t sym = s_grid[r][c];
            if (sym < 1U || sym > SYM_COUNT) continue;

            for (uint8_t d = 0; d < 4U; d++)
            {
                /* Skip if previous cell in this direction had the same symbol */
                int8_t pr = (int8_t)(r - dr[d]);
                int8_t pc = (int8_t)(c - dc[d]);
                if (pr >= 0 && pr < ROWS && pc >= 0 && pc < COLS && s_grid[pr][pc] == sym)
                {
                    continue;
                }

                uint8_t len = 1U;
                while (len < ROWS)
                {
                    int8_t nr = (int8_t)(r + dr[d] * len);
                    int8_t nc = (int8_t)(c + dc[d] * len);
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS || s_grid[nr][nc] != sym)
                    {
                        break;
                    }
                    len++;
                }

                if (len >= 3U)
                {
                    lines_found++;
                    for (uint8_t k = 0; k < len; k++)
                    {
                        s_mark[r + dr[d] * k][c + dc[d] * k] = 1U;
                    }
                }
            }
        }
    }

    uint16_t marked_cells = 0;
    for (uint8_t r = 0; r < ROWS; r++)
    {
        for (uint8_t c = 0; c < COLS; c++)
        {
            if (s_mark[r][c] != 0U) marked_cells++;
        }
    }
    *out_lines = lines_found;
    return marked_cells;
}

/* Drops individual blocks down if there are empty spaces below them */
static bool apply_column_gravity(void)
{
    bool moved = false;
    for (uint8_t c = 0; c < COLS; c++)
    {
        int8_t write_r = ROWS - 1;
        for (int8_t r = ROWS - 1; r >= 0; r--)
        {
            if (s_grid[r][c] != SYM_EMPTY)
            {
                if (write_r != r)
                {
                    s_grid[write_r][c] = s_grid[r][c];
                    s_grid[r][c] = SYM_EMPTY;
                    moved = true;
                }
                write_r--;
            }
        }
    }
    return moved;
}

static void setup_round(uint32_t now)
{
    sdk_memset(s_grid, 0, sizeof(s_grid));
    s_lines_left = (int16_t)(12 + s_round * 4);
    s_speed_lvl  = (uint8_t)(1U + s_round / 2U);
    if (s_speed_lvl > 8U) s_speed_lvl = 8U;

    /* On Round 2+, populate side columns and corner steel blocks (like screenshot!) */
    if (s_round >= 1U)
    {
        s_grid[ROWS - 1][0]        = SYM_STEEL;
        s_grid[ROWS - 1][COLS - 1] = SYM_STEEL;

        uint8_t stack_h = (uint8_t)((s_round < 5U) ? (s_round + 1U) : 6U);
        for (uint8_t r = ROWS - 2; r >= ROWS - 1 - stack_h; r--)
        {
            s_grid[r][0]        = (uint8_t)(1U + ((r + s_round) % SYM_COUNT));
            s_grid[r][COLS - 1] = (uint8_t)(1U + ((r + s_round + 2U) % SYM_COUNT));
        }
    }

    roll_next_piece();
    (void)spawn_piece(now);
    update_round_panel();
    update_speed_panel();
    update_blocks_stats();
    sdk_memset(s_shown, 0xFFU, sizeof(s_shown));
}

static uint32_t fall_interval(void)
{
    uint32_t dec = (uint32_t)s_speed_lvl * 42U;
    return (dec >= 340U) ? 100U : (440U - dec);
}

static void lock_and_check(uint32_t now)
{
    for (uint8_t i = 0; i < 3U; i++)
    {
        int8_t cx = s_vertical ? s_px : (int8_t)(s_px + i);
        int8_t cy = s_vertical ? (int8_t)(s_py + i) : s_py;
        if (cx >= 0 && cx < COLS && cy >= 0 && cy < ROWS)
        {
            s_grid[cy][cx] = s_cur[i];
        }
    }
    sound(s_snd_lock, 1);

    /* Settle any overhanging horizontal blocks immediately */
    (void)apply_column_gravity();
    update_blocks_stats();

    s_chain_mult = 1U;
    uint16_t lines = 0;
    if (find_matches(&lines) > 0U)
    {
        s_state = ST_FLASH;
        s_state_tick = now;
        sound(s_snd_match, 5);
    }
    else if (!spawn_piece(now))
    {
        s_state = ST_OVER;
        s_state_tick = now;
        save_best();
        sound(s_snd_over, 5);
    }
}

static void show_well_banner(const char *l1, const char *l2, uint16_t col)
{
    sys_lcd_fill_rect(WELL_X + 4, 96, WELL_W - 8, 44, COLOR_PANEL_BG);
    draw_ui_box(WELL_X + 4, 96, WELL_W - 8, 44);
    sys_draw_text(WELL_X + 14, 106, l1, col, COLOR_BLACK, 1);
    sys_draw_text(WELL_X + 14, 122, l2, COLOR_WHITE, COLOR_BLACK, 1);
    sdk_memset(s_shown, 0xFFU, sizeof(s_shown));
}

void app_main(void)
{
    SDK_Input_t in;
    uint32_t next_frame = 0;

    build_sprites();
    load_best();
    s_score = 0;
    s_round = 0;
    draw_static_layout();
    setup_round(sys_get_tick());
    render_well(sys_get_tick());
    show_well_banner("STACK UP!", "A: START", COLOR_YELLOW);
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
                if (in.pressed & (KEY_A | KEY_UP))
                {
                    s_state = ST_FALL;
                    s_fall_tick = now;
                    sdk_memset(s_shown, 0xFFU, sizeof(s_shown));
                    render_well(now);
                }
                break;

            case ST_FALL:
            {
                if (in.pressed & KEY_START)
                {
                    s_state = ST_PAUSE;
                    show_well_banner("PAUSED", "A: RESUME", COLOR_YELLOW);
                    break;
                }

                /* Left / Right movement */
                if ((in.repeat & KEY_LEFT) && piece_fits((int8_t)(s_px - 1), s_py, s_vertical))
                {
                    s_px--;
                    sound(s_snd_move, 1);
                }
                if ((in.repeat & KEY_RIGHT) && piece_fits((int8_t)(s_px + 1), s_py, s_vertical))
                {
                    s_px++;
                    sound(s_snd_move, 1);
                }

                /* Cycle colors inside the 3-block piece (A or UP) */
                if (in.pressed & (KEY_A | KEY_UP))
                {
                    uint8_t tmp = s_cur[2];
                    s_cur[2] = s_cur[1];
                    s_cur[1] = s_cur[0];
                    s_cur[0] = tmp;
                    sound(s_snd_cycle, 1);
                }

                /* Flip orientation Horizontal <-> Vertical (X button / Key C) */
                if (in.pressed & KEY_X)
                {
                    bool new_vert = !s_vertical;
                    int8_t nx = s_px;
                    if (!new_vert && (nx > COLS - 3)) nx = COLS - 3;
                    if (piece_fits(nx, s_py, new_vert))
                    {
                        s_px = nx;
                        s_vertical = new_vert;
                        sound(s_snd_cycle, 1);
                    }
                }

                /* Instant Hard Drop (B button) */
                if (in.pressed & KEY_B)
                {
                    while (piece_fits(s_px, (int8_t)(s_py + 1), s_vertical))
                    {
                        s_py++;
                        s_score += 2U;
                    }
                    if (s_score > s_best) s_best = s_score;
                    update_score_panel();
                    lock_and_check(now);
                    break;
                }

                /* Gravity & Soft Drop */
                uint32_t delay = (in.held & KEY_DOWN) ? 45U : fall_interval();
                if ((now - s_fall_tick) >= delay)
                {
                    s_fall_tick = now;
                    if (piece_fits(s_px, (int8_t)(s_py + 1), s_vertical))
                    {
                        s_py++;
                        if (in.held & KEY_DOWN)
                        {
                            s_score++;
                            if (s_score > s_best) s_best = s_score;
                            update_score_panel();
                        }
                    }
                    else
                    {
                        lock_and_check(now);
                    }
                }
                break;
            }

            case ST_FLASH:
                if ((now - s_state_tick) >= CLEAR_FLASH_MS)
                {
                    uint16_t cleared = 0;
                    for (uint8_t r = 0; r < ROWS; r++)
                    {
                        for (uint8_t c = 0; c < COLS; c++)
                        {
                            if (s_mark[r][c] != 0U)
                            {
                                s_grid[r][c] = SYM_EMPTY;
                                cleared++;
                            }
                        }
                    }

                    s_score += (uint32_t)cleared * 50U * s_chain_mult;
                    if (s_score > s_best) s_best = s_score;
                    s_lines_left = (int16_t)(s_lines_left - (int16_t)(cleared / 3U));
                    if (s_lines_left < 0) s_lines_left = 0;

                    update_score_panel();
                    update_round_panel();
                    update_blocks_stats();

                    s_state = ST_CASCADE;
                    s_state_tick = now;
                }
                break;

            case ST_CASCADE:
                if ((now - s_state_tick) >= CASCADE_STEP_MS)
                {
                    (void)apply_column_gravity();
                    update_blocks_stats();

                    uint16_t lines = 0;
                    if (find_matches(&lines) > 0U)
                    {
                        s_chain_mult++;
                        s_state = ST_FLASH;
                        s_state_tick = now;
                        sound(s_snd_match, 5);
                    }
                    else if (s_lines_left <= 0)
                    {
                        s_state = ST_ROUND_WIN;
                        s_state_tick = now;
                        sound(s_snd_round, 7);
                        show_well_banner("ROUND CLEAR", "BONUS +1000", COLOR_NEON_GREEN);
                    }
                    else if (spawn_piece(now))
                    {
                        s_state = ST_FALL;
                    }
                    else
                    {
                        s_state = ST_OVER;
                        s_state_tick = now;
                        save_best();
                        sound(s_snd_over, 5);
                        show_well_banner("GAME OVER", "A: RETRY", COLOR_RED);
                    }
                }
                break;

            case ST_ROUND_WIN:
                if ((now - s_state_tick) >= 1400U)
                {
                    s_round++;
                    s_score += 1000U;
                    if (s_score > s_best) s_best = s_score;
                    update_score_panel();
                    setup_round(now);
                    s_state = ST_FALL;
                }
                break;

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    s_state = ST_FALL;
                    s_fall_tick = now;
                    sdk_memset(s_shown, 0xFFU, sizeof(s_shown));
                }
                break;

            case ST_OVER:
                if ((in.pressed & (KEY_A | KEY_UP)) && ((now - s_state_tick) > 600U))
                {
                    s_score = 0;
                    s_round = 0;
                    update_score_panel();
                    setup_round(now);
                    s_state = ST_FALL;
                }
                break;

            default:
                break;
        }

        if ((s_state == ST_FALL) || (s_state == ST_FLASH) || (s_state == ST_CASCADE))
        {
            render_well(now);
        }

        sdk_frame_sync(&next_frame, 16U);
    }
}