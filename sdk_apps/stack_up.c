#include "console_sdk.h"

/* =========================================================================
 * STACK UP (ZX Spectrum 1991 style arcade puzzle for STM32 Console)
 *
 * ZX Spectrum-inspired presentation and a 22-round modifier campaign:
 *  - Rainbow diagonal ZX Spectrum dithered background
 *  - 6x12 Playfield Well with 16x16 px beveled symbol blocks:
 *      1: Red Heart      2: Magenta Circle-Slash   3: Green Triangle
 *      4: Cyan Arrow     5: Yellow Diamond         6: Grey Steel Block
 *  - Authentic Split-Fall Mechanic: when 1 or 2 blocks of a horizontal
 *    triplet land on a stack, they lock in place while the remaining
 *    unsupported block(s) keep falling and remain steerable Left/Right!
 *  - Best score: SD:/appdata/stack_up/best.dat
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

#define MOD_STEEL_BASE  0x01U
#define MOD_STEEL_DROP  0x02U
#define MOD_DIAGONAL    0x04U
#define MOD_RANDOM      0x08U
#define MOD_REVERSE     0x10U

typedef enum {
    PAT_NONE = 0,       /* Empty well */
    PAT_WALLS_LOW,      /* U-shape: side columns medium, next shorter, center empty */
    PAT_WALLS_HIGH,     /* Deep U-shape: side columns up to mid-well, center empty */
    PAT_SLOPE_RIGHT,    /* Diagonal staircase: highest on right edge, stepping down left */
    PAT_SLOPE_LEFT,     /* Diagonal staircase: highest on left edge, stepping down right */
    PAT_VALLEY_STEP,    /* 3-step V-valley from both walls toward the middle */
    PAT_TOWERS,         /* Alternating tall/short columns */
    PAT_FORTRESS        /* Tall side walls + low center floor */
} FillPattern;

typedef struct {
    uint8_t pattern;
    uint8_t lines;
    uint8_t modifiers;
    uint16_t fall_ms;
    uint16_t spawn_ms;
} LevelConfig;

/* Column height profiles (6 columns, max height 6 = half of the 12-row well) */
static const uint8_t s_pat_heights[][COLS] = {
    [PAT_NONE]        = { 0, 0, 0, 0, 0, 0 },
    [PAT_WALLS_LOW]   = { 4, 2, 0, 0, 2, 4 },
    [PAT_WALLS_HIGH]  = { 6, 3, 0, 0, 3, 6 },
    [PAT_SLOPE_RIGHT] = { 1, 2, 3, 4, 5, 6 },
    [PAT_SLOPE_LEFT]  = { 6, 5, 4, 3, 2, 1 },
    [PAT_VALLEY_STEP] = { 6, 4, 1, 1, 4, 6 },
    [PAT_TOWERS]      = { 5, 1, 4, 1, 4, 5 },
    [PAT_FORTRESS]    = { 6, 5, 2, 2, 5, 6 }
};

static const LevelConfig s_levels[] = {
    { PAT_NONE,        12, 0,                                                                          650,    0 },
    { PAT_NONE,        14, 0,                                                                          620,    0 },
    { PAT_WALLS_LOW,   15, 0,                                                                          600,    0 },
    { PAT_SLOPE_RIGHT, 16, 0,                                                                          550,    0 },
    { PAT_WALLS_HIGH,  18, MOD_STEEL_BASE,                                                             500,    0 },
    { PAT_NONE,        10, MOD_DIAGONAL,                                                               650,    0 },
    { PAT_SLOPE_LEFT,  12, MOD_STEEL_BASE | MOD_DIAGONAL,                                              580,    0 },
    { PAT_NONE,        16, MOD_REVERSE,                                                                550,    0 },
    { PAT_WALLS_HIGH,  18, MOD_REVERSE,                                                                480,    0 },
    { PAT_NONE,        16, MOD_RANDOM,                                                                 520, 8000 },
    { PAT_VALLEY_STEP, 18, MOD_RANDOM,                                                                460, 7500 },
    { PAT_TOWERS,      18, MOD_STEEL_BASE | MOD_STEEL_DROP,                                           440,    0 },
    { PAT_SLOPE_RIGHT, 12, MOD_STEEL_BASE | MOD_DIAGONAL | MOD_REVERSE,                               500,    0 },
    { PAT_VALLEY_STEP, 20, MOD_STEEL_BASE | MOD_REVERSE,                                              400,    0 },
    { PAT_WALLS_LOW,   14, MOD_DIAGONAL | MOD_RANDOM,                                                 450, 7000 },
    { PAT_SLOPE_LEFT,  20, MOD_STEEL_BASE | MOD_RANDOM,                                               380, 6500 },
    { PAT_WALLS_HIGH,  14, MOD_STEEL_BASE | MOD_STEEL_DROP | MOD_DIAGONAL,                            350,    0 },
    { PAT_TOWERS,      22, MOD_STEEL_BASE | MOD_REVERSE | MOD_RANDOM,                                 320, 6000 },
    { PAT_VALLEY_STEP, 15, MOD_STEEL_BASE | MOD_DIAGONAL | MOD_REVERSE,                               300,    0 },
    { PAT_SLOPE_RIGHT, 16, MOD_STEEL_BASE | MOD_STEEL_DROP | MOD_DIAGONAL | MOD_RANDOM,               270, 5500 },
    { PAT_FORTRESS,    24, MOD_STEEL_BASE | MOD_STEEL_DROP | MOD_REVERSE | MOD_RANDOM,                240, 5000 },
    { PAT_FORTRESS,    18, MOD_STEEL_BASE | MOD_STEEL_DROP | MOD_DIAGONAL | MOD_REVERSE | MOD_RANDOM, 210, 4500 }
};

#define LEVEL_COUNT (sizeof(s_levels) / sizeof(s_levels[0]))

enum { ST_READY, ST_INTRO, ST_FALL, ST_FLASH, ST_CASCADE, ST_ROUND_WIN, ST_PAUSE, ST_OVER, ST_COMPLETE };

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

/* Active falling piece (up to 3 independent blocks that can split on landing!) */
static uint8_t  s_cur[3];
static int8_t   s_bx[3];
static int8_t   s_by[3];
static bool     s_bactive[3];
static uint8_t  s_next[3];

static uint8_t  s_state = ST_READY;
static uint32_t s_state_tick;
static uint32_t s_fall_tick;
static uint16_t s_chain_mult;

static uint32_t s_score;
static uint32_t s_best = 3000U, s_best_saved = 3000U;
static uint8_t  s_round;
static int16_t  s_lines_left;
static uint8_t  s_speed_lvl;
static uint16_t s_round_pieces;
static uint16_t s_match_lines;
static uint32_t s_match_pts;
static uint32_t s_spawn_elapsed;

static const LevelConfig *level_config(void)
{
    return &s_levels[s_round];
}

static const char *round_rule_hint(void)
{
    uint8_t m = level_config()->modifiers;
    uint8_t cnt = 0;
    if (m & MOD_DIAGONAL) cnt++;
    if (m & MOD_REVERSE)  cnt++;
    if (m & MOD_RANDOM)   cnt++;
    if (m & (MOD_STEEL_BASE | MOD_STEEL_DROP)) cnt++;

    if (cnt >= 2U) return "COMBO RULES";
    if (m & MOD_DIAGONAL)   return "DIAG ONLY!";
    if (m & MOD_REVERSE)    return "REVERSE L/R";
    if (m & MOD_RANDOM)     return "RANDOM BLK";
    if (m & MOD_STEEL_DROP) return "STEEL DROP";
    if (m & MOD_STEEL_BASE) return "STEEL BASE";
    if (level_config()->pattern == PAT_SLOPE_RIGHT || level_config()->pattern == PAT_SLOPE_LEFT)
        return "SLOPE WELL";
    if (level_config()->pattern != PAT_NONE) return "VALLEY WELL";
    return "ALL LINES";
}

static uint32_t fall_interval(void)
{
    uint32_t base = level_config()->fall_ms;
    uint32_t reduction = (uint32_t)(s_round_pieces / 3U) * 10U;
    uint32_t limit = base / 3U;
    if (reduction > limit) reduction = limit;
    return base - reduction;
}

static void sound(const SDK_Note_t *notes, uint32_t n)
{
    (void)sys_audio_play(notes, n, 0);
}

/* ---------------------------------------------------------------- graphics build */

static void build_sprites(void)
{
    for (uint16_t i = 0; i < CELL * CELL; i++) s_tile_spr[SYM_EMPTY][i] = COLOR_BLACK;
    for (uint16_t i = 0; i < CELL * CELL; i++) s_tile_spr[SYM_FLASH][i] = COLOR_WHITE;

    for (uint8_t s = 1; s <= 6U; s++)
    {
        uint16_t bg = s_sym_colors[s];
        const uint16_t *bmp = s_sym_bmp[s - 1U];

        for (uint8_t y = 0; y < CELL; y++)
        {
            for (uint8_t x = 0; x < CELL; x++)
            {
                uint16_t c = bg;
                if (((y == 0U || y == 15U) && (x == 0U || x == 15U)))
                {
                    c = COLOR_BLACK;
                }
                else if (y == 0U || x == 0U)
                {
                    c = COLOR_WHITE;
                }
                else if (y == 15U || x == 15U)
                {
                    c = 0x2104U;
                }
                else if ((y == 2U || y == 13U) && (x == 2U || x == 13U))
                {
                    c = COLOR_BLACK;
                }
                else if ((bmp[y] & (0x8000U >> x)) != 0U)
                {
                    c = (s == SYM_STEEL) ? COLOR_WHITE : COLOR_BLACK;
                }
                s_tile_spr[s][y * CELL + x] = c;
            }
        }
    }

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

static void update_rules_panel(void)
{
    uint8_t modifiers = level_config()->modifiers;
    sys_lcd_fill_rect(234, 93, 68, 18, COLOR_BLACK);
    sys_draw_text(234, 93, (modifiers & MOD_DIAGONAL) ? "DIAG" : "ALL", COLOR_YELLOW, COLOR_BLACK, 1);
    if (modifiers & MOD_REVERSE)
        sys_draw_text(276, 93, "REV", COLOR_CYAN, COLOR_BLACK, 1);
    if (modifiers & (MOD_STEEL_BASE | MOD_STEEL_DROP))
        sys_draw_text(234, 103, "STEEL", COLOR_WHITE, COLOR_BLACK, 1);
    if (modifiers & MOD_RANDOM)
        sys_draw_text(276, 103, "RND", COLOR_MAGENTA, COLOR_BLACK, 1);
}

static void update_speed_panel(void)
{
    s_speed_lvl = (uint8_t)(1U + (650U - fall_interval()) / 65U);
    sys_lcd_fill_rect(236, 40, 64, 14, COLOR_BLACK);
    uint8_t bars = (uint8_t)((s_speed_lvl > 8U) ? 8U : s_speed_lvl);
    for (uint8_t i = 0; i < bars; i++)
    {
        uint16_t col = (i < 3U) ? COLOR_BLUE : ((i < 6U) ? COLOR_MAGENTA : COLOR_RED);
        sys_lcd_fill_rect((uint16_t)(236U + i * 8U), 40U, 7U, 14U, col);
    }
}

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

    sys_lcd_fill_rect(WELL_X - 2, WELL_Y - 2, WELL_W + 4, WELL_H + 4, COLOR_BLACK);
    sys_lcd_fill_rect(WELL_X, WELL_Y, WELL_W, WELL_H, COLOR_BLACK);

    draw_ui_box(8, 20, 82, 56);
    sys_draw_text(34, 28, "1UP", COLOR_WHITE, COLOR_BLACK, 1);
    sys_draw_text(34, 50, "TOP", COLOR_WHITE, COLOR_BLACK, 1);

    draw_ui_box(8, 86, 82, 72);
    sys_draw_text(28, 96,  "ROUND", COLOR_WHITE, COLOR_BLACK, 1);
    sys_draw_text(28, 120, "LINES", COLOR_WHITE, COLOR_BLACK, 1);
    sys_draw_text(31, 130, "LEFT",  COLOR_WHITE, COLOR_BLACK, 1);

    draw_ui_box(8, 168, 82, 48);
    sys_draw_text(34, 178, "NEXT", COLOR_WHITE, COLOR_BLACK, 1);

    draw_ui_box(226, 20, 84, 44);
    sys_draw_text(242, 28, "SPEED", COLOR_WHITE, COLOR_BLACK, 1);

    draw_ui_box(226, 74, 84, 42);
    sys_draw_text(242, 82, "RULES", COLOR_WHITE, COLOR_BLACK, 1);
    update_rules_panel();

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
            if (s_bactive[i] && s_bx[i] >= 0 && s_bx[i] < COLS && s_by[i] >= 0 && s_by[i] < ROWS)
            {
                want[s_by[i]][s_bx[i]] = s_cur[i];
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
    if ((level_config()->modifiers & MOD_STEEL_DROP) && sys_random_range(6U) == 0U)
    {
        s_next[sys_random_range(3U)] = SYM_STEEL;
    }
}

static bool spawn_piece(uint32_t now)
{
    for (uint8_t i = 0; i < 3U; i++)
    {
        s_cur[i]     = s_next[i];
        s_bx[i]      = (int8_t)(1 + i);
        s_by[i]      = 0;
        s_bactive[i] = true;
    }
    roll_next_piece();
    update_next_panel();
    s_fall_tick = now;

    /* Game Over if any of the 3 spawn cells at row 0 are already occupied */
    for (uint8_t i = 0; i < 3U; i++)
    {
        if (s_grid[s_by[i]][s_bx[i]] != SYM_EMPTY)
        {
            return false;
        }
    }
    return true;
}

static uint8_t active_block_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < 3U; i++)
    {
        if (s_bactive[i]) n++;
    }
    return n;
}

static bool spawn_random_block(void)
{
    uint8_t first_col = (uint8_t)sys_random_range(COLS);
    for (uint8_t offset = 0; offset < COLS; offset++)
    {
        uint8_t col = (uint8_t)((first_col + offset) % COLS);
        int8_t row = 0;
        while (row < ROWS && s_grid[row][col] == SYM_EMPTY) row++;
        row--;
        if (row <= 0) continue;

        bool blocked = false;
        for (uint8_t block = 0; block < 3U; block++)
        {
            if (s_bactive[block] && s_bx[block] == col && s_by[block] >= row)
            {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        s_grid[row][col] = (uint8_t)(1U + sys_random_range(SYM_COUNT));
        update_blocks_stats();
        sound(s_snd_lock, 1);
        return true;
    }
    return false;
}

static void update_random_spawn(uint32_t elapsed)
{
    uint32_t interval = level_config()->spawn_ms;
    if (s_state != ST_FALL || !(level_config()->modifiers & MOD_RANDOM) || interval == 0U) return;
    if (elapsed >= interval - s_spawn_elapsed)
    {
        s_spawn_elapsed = 0;
        (void)spawn_random_block();
    }
    else
    {
        s_spawn_elapsed += elapsed;
    }
}

/* Move all currently falling (un-landed) blocks Left (-1) or Right (+1) if clear */
static bool try_move_active_horiz(int8_t dx)
{
    if (level_config()->modifiers & MOD_REVERSE) dx = (int8_t)-dx;
    for (uint8_t i = 0; i < 3U; i++)
    {
        if (!s_bactive[i]) continue;
        int8_t nx = (int8_t)(s_bx[i] + dx);
        int8_t ny = s_by[i];
        if (nx < 0 || nx >= COLS) return false;
        if (ny >= 0 && s_grid[ny][nx] != SYM_EMPTY) return false;
    }

    for (uint8_t i = 0; i < 3U; i++)
    {
        if (s_bactive[i])
        {
            s_bx[i] = (int8_t)(s_bx[i] + dx);
        }
    }
    return true;
}

/* Cycle colors among all currently falling blocks (works for 3 or 2 active blocks!) */
static void cycle_active_colors(void)
{
    uint8_t idx[3];
    uint8_t cnt = 0;
    for (uint8_t i = 0; i < 3U; i++)
    {
        if (s_bactive[i]) idx[cnt++] = i;
    }
    if (cnt < 2U) return;

    uint8_t last_col = s_cur[idx[cnt - 1U]];
    for (uint8_t k = cnt - 1U; k > 0U; k--)
    {
        s_cur[idx[k]] = s_cur[idx[k - 1U]];
    }
    s_cur[idx[0]] = last_col;
    sound(s_snd_cycle, 1);
}

/* Flip intact 3-block piece between Horizontal and Vertical */
static void try_flip_orientation(void)
{
    if (active_block_count() != 3U) return;

    bool is_vert = (s_bx[0] == s_bx[1]) && (s_bx[1] == s_bx[2]);
    if (!is_vert)
    {
        /* Horizontal -> Vertical around middle block s_bx[1] */
        int8_t cx = s_bx[1];
        int8_t top_y = s_by[1];
        if ((top_y + 2 < ROWS) &&
            (s_grid[top_y][cx] == SYM_EMPTY) &&
            (s_grid[top_y + 1][cx] == SYM_EMPTY) &&
            (s_grid[top_y + 2][cx] == SYM_EMPTY))
        {
            s_bx[0] = cx; s_by[0] = top_y;
            s_bx[1] = cx; s_by[1] = (int8_t)(top_y + 1);
            s_bx[2] = cx; s_by[2] = (int8_t)(top_y + 2);
            sound(s_snd_cycle, 1);
        }
    }
    else
    {
        /* Vertical -> Horizontal around middle block */
        int8_t cy = s_by[1];
        int8_t left_x = (int8_t)(s_bx[1] - 1);
        if (left_x < 0) left_x = 0;
        if (left_x > COLS - 3) left_x = COLS - 3;

        if ((s_grid[cy][left_x] == SYM_EMPTY) &&
            (s_grid[cy][left_x + 1] == SYM_EMPTY) &&
            (s_grid[cy][left_x + 2] == SYM_EMPTY))
        {
            s_bx[0] = left_x;            s_by[0] = cy;
            s_bx[1] = (int8_t)(left_x + 1); s_by[1] = cy;
            s_bx[2] = (int8_t)(left_x + 2); s_by[2] = cy;
            sound(s_snd_cycle, 1);
        }
    }
}

/* Steps active blocks down by 1 row. Any block that hits a stack below locks immediately,
 * while remaining unsupported blocks stay active and steerable! */
static bool step_active_down(void)
{
    bool any_locked = false;

    /* Lock from bottom to top so vertical triplets stack cleanly */
    bool locked_pass = true;
    while (locked_pass)
    {
        locked_pass = false;
        for (int8_t i = 2; i >= 0; i--)
        {
            if (!s_bactive[i]) continue;
            int8_t cx = s_bx[i];
            int8_t ny = (int8_t)(s_by[i] + 1);
            if ((ny >= ROWS) || (s_grid[ny][cx] != SYM_EMPTY))
            {
                s_grid[s_by[i]][cx] = s_cur[i];
                s_bactive[i] = false;
                locked_pass  = true;
                any_locked   = true;
            }
        }
    }

    if (any_locked)
    {
        sound(s_snd_lock, 1);
        update_blocks_stats();
    }

    /* Advance all remaining active blocks down by 1 row */
    bool still_falling = false;
    for (uint8_t i = 0; i < 3U; i++)
    {
        if (s_bactive[i])
        {
            s_by[i]++;
            still_falling = true;
        }
    }

    return still_falling;
}

/* Scan for 3+ matching blocks horizontally, vertically, and diagonally */
static uint16_t find_matches(uint16_t *out_lines)
{
    sdk_memset(s_mark, 0, sizeof(s_mark));
    uint16_t lines_found = 0;
    uint32_t pts_found = 0;

    static const int8_t dr[4] = { 0, 1, 1,  1 };
    static const int8_t dc[4] = { 1, 0, 1, -1 };
    static const uint16_t base_pts[4] = { 500U, 200U, 1000U, 1000U };

    for (uint8_t r = 0; r < ROWS; r++)
    {
        for (uint8_t c = 0; c < COLS; c++)
        {
            uint8_t sym = s_grid[r][c];
            if (sym < 1U || sym > SYM_COUNT) continue;

            for (uint8_t d = 0; d < 4U; d++)
            {
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
                    uint16_t k_len = (uint16_t)(len - 2U);
                    if (!(level_config()->modifiers & MOD_DIAGONAL) || (d >= 2U))
                    {
                        lines_found += k_len;
                    }
                    pts_found += (uint32_t)base_pts[d] * k_len;
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
    *out_lines  = lines_found;
    s_match_pts = pts_found;
    return marked_cells;
}

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

static void fill_round(void)
{
    sdk_memset(s_grid, 0, sizeof(s_grid));
    const LevelConfig *cfg = level_config();
    const uint8_t *heights = s_pat_heights[cfg->pattern];

    /* Fill bottom-up according to the column height profile without pre-made 3-in-a-row matches */
    for (int8_t row = ROWS - 1; row >= 0; row--)
    {
        for (uint8_t col = 0; col < COLS; col++)
        {
            uint8_t col_h = heights[col];
            if ((uint8_t)(ROWS - 1 - row) >= col_h) continue;

            uint8_t symbol = (uint8_t)(1U + sys_random_range(SYM_COUNT));
            for (uint8_t attempt = 0; attempt < SYM_COUNT; attempt++)
            {
                bool match = (col >= 2U && s_grid[row][col - 1U] == symbol && s_grid[row][col - 2U] == symbol)
                    || (row + 2 < ROWS && s_grid[row + 1][col] == symbol && s_grid[row + 2][col] == symbol)
                    || (row + 2 < ROWS && col >= 2U && s_grid[row + 1][col - 1U] == symbol && s_grid[row + 2][col - 2U] == symbol)
                    || (row + 2 < ROWS && col + 2U < COLS && s_grid[row + 1][col + 1U] == symbol && s_grid[row + 2][col + 2U] == symbol);
                if (!match) break;
                symbol = (uint8_t)(1U + (symbol % SYM_COUNT));
            }
            s_grid[row][col] = symbol;
        }
    }

    if (cfg->modifiers & MOD_STEEL_BASE)
    {
        uint8_t first_col = (uint8_t)sys_random_range(COLS);
        uint8_t placed = 0;
        for (uint8_t i = 0; i < COLS && placed < 3U; i++)
        {
            uint8_t c = (uint8_t)((first_col + i * 2U + (i & 1U)) % COLS);
            uint8_t h = heights[c];
            if (h > 0U)
            {
                uint8_t depth = (uint8_t)(placed % h);
                s_grid[ROWS - 1U - depth][c] = SYM_STEEL;
                placed++;
            }
        }
    }
}

static void setup_round(uint32_t now)
{
    fill_round();
    s_lines_left = level_config()->lines;
    s_round_pieces = 0;
    s_match_lines = 0;
    s_match_pts = 0;
    s_spawn_elapsed = 0;

    roll_next_piece();
    (void)spawn_piece(now);
    update_round_panel();
    update_speed_panel();
    update_blocks_stats();
    update_rules_panel();
    sdk_memset(s_shown, 0xFFU, sizeof(s_shown));
}

static void show_well_banner(const char *l1, const char *l2, uint16_t col)
{
    sys_lcd_fill_rect(WELL_X + 4, 96, WELL_W - 8, 44, COLOR_PANEL_BG);
    draw_ui_box(WELL_X + 4, 96, WELL_W - 8, 44);
    sys_draw_text(WELL_X + 14, 106, l1, col, COLOR_BLACK, 1);
    sys_draw_text(WELL_X + 14, 122, l2, COLOR_WHITE, COLOR_BLACK, 1);
    sdk_memset(s_shown, 0xFFU, sizeof(s_shown));
}

static void finish_resolution(uint32_t now)
{
    s_state_tick = now;
    if (s_lines_left <= 0)
    {
        s_state = ST_ROUND_WIN;
        sound(s_snd_round, 7);
        show_well_banner("ROUND CLEAR", "CLEAR+SPACE", COLOR_NEON_GREEN);
    }
    else if (spawn_piece(now))
    {
        s_state = ST_FALL;
    }
    else
    {
        s_state = ST_OVER;
        save_best();
        sound(s_snd_over, 5);
        show_well_banner("GAME OVER", "A: RETRY", COLOR_RED);
    }
}

static void on_all_blocks_landed(uint32_t now)
{
    if (s_round_pieces < UINT16_MAX) s_round_pieces++;
    s_score += 50U;
    if (s_score > s_best) s_best = s_score;
    update_score_panel();
    update_speed_panel();
    (void)apply_column_gravity();
    update_blocks_stats();

    s_chain_mult = 1U;
    if (find_matches(&s_match_lines) > 0U)
    {
        s_state = ST_FLASH;
        s_state_tick = now;
        sound(s_snd_match, 5);
    }
    else
    {
        finish_resolution(now);
    }
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
    show_well_banner("STACK UP!", "L/R:RND A:GO", COLOR_YELLOW);
    sdk_input_init(&in);
    uint32_t previous_tick = sys_get_tick();

    for (;;)
    {
        uint32_t now = sys_get_tick();
        uint32_t elapsed = now - previous_tick;
        previous_tick = now;
        sdk_input_update(&in, sys_get_keys(), now);

        if (in.pressed & KEY_EXIT)
        {
            save_best();
            return;
        }

        switch (s_state)
        {
            case ST_READY:
                if (in.pressed & (KEY_LEFT | KEY_RIGHT))
                {
                    if (in.pressed & KEY_LEFT)
                    {
                        s_round = (uint8_t)((s_round == 0U) ? (LEVEL_COUNT - 1U) : (s_round - 1U));
                    }
                    else
                    {
                        s_round = (uint8_t)((s_round + 1U) % LEVEL_COUNT);
                    }
                    sound(s_snd_move, 1);
                    setup_round(now);
                    render_well(now);
                    char title[12] = "ROUND ";
                    sdk_uint_to_str((uint32_t)s_round + 1U, &title[6], 2);
                    show_well_banner(title, round_rule_hint(), COLOR_YELLOW);
                }
                else if (in.pressed & (KEY_A | KEY_UP))
                {
                    s_state = ST_FALL;
                    s_fall_tick = now;
                    sdk_memset(s_shown, 0xFFU, sizeof(s_shown));
                    render_well(now);
                }
                break;

            case ST_INTRO:
                if ((now - s_state_tick) >= 1200U)
                {
                    s_state = ST_FALL;
                    s_fall_tick = now;
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

                update_random_spawn(elapsed);

                /* Move all still-falling blocks Left / Right */
                if ((in.repeat & KEY_LEFT) && try_move_active_horiz(-1))
                {
                    sound(s_snd_move, 1);
                }
                if ((in.repeat & KEY_RIGHT) && try_move_active_horiz(1))
                {
                    sound(s_snd_move, 1);
                }

                /* Cycle colors of active blocks (A or UP) */
                if (in.pressed & (KEY_A | KEY_UP))
                {
                    cycle_active_colors();
                }

                /* Flip orientation Horizontal <-> Vertical when all 3 blocks are intact (X / Key C) */
                if (in.pressed & KEY_X)
                {
                    try_flip_orientation();
                }

                /* Instant Hard Drop (B button) */
                if (in.pressed & KEY_B)
                {
                    while (step_active_down())
                    {
                        s_score += 2U;
                    }
                    if (s_score > s_best) s_best = s_score;
                    update_score_panel();
                    on_all_blocks_landed(now);
                    break;
                }

                /* Gravity & Soft Drop */
                uint32_t delay = (in.held & KEY_DOWN) ? 75U : fall_interval();
                if ((now - s_fall_tick) >= delay)
                {
                    s_fall_tick = now;
                    if (step_active_down())
                    {
                        if (in.held & KEY_DOWN)
                        {
                            s_score++;
                            if (s_score > s_best) s_best = s_score;
                            update_score_panel();
                        }
                    }
                    else
                    {
                        on_all_blocks_landed(now);
                    }
                }
                break;
            }

            case ST_FLASH:
                if ((now - s_state_tick) >= CLEAR_FLASH_MS)
                {
                    for (uint8_t r = 0; r < ROWS; r++)
                    {
                        for (uint8_t c = 0; c < COLS; c++)
                        {
                            if (s_mark[r][c] != 0U)
                            {
                                s_grid[r][c] = SYM_EMPTY;
                            }
                        }
                    }

                    s_score += s_match_pts * (uint32_t)s_chain_mult;
                    if (s_score > s_best) s_best = s_score;
                    s_lines_left = (int16_t)(s_lines_left - (int16_t)s_match_lines);
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

                    if (find_matches(&s_match_lines) > 0U)
                    {
                        if (s_chain_mult < 3U) s_chain_mult++;
                        s_state = ST_FLASH;
                        s_state_tick = now;
                        sound(s_snd_match, 5);
                    }
                    else
                    {
                        finish_resolution(now);
                    }
                }
                break;

            case ST_ROUND_WIN:
                if ((now - s_state_tick) >= 1400U)
                {
                    uint32_t free_cells = 0;
                    for (uint8_t r = 0; r < ROWS; r++)
                    {
                        for (uint8_t c = 0; c < COLS; c++)
                        {
                            if (s_grid[r][c] == SYM_EMPTY) free_cells++;
                        }
                    }
                    s_score += 500U * ((uint32_t)s_round + 1U) + free_cells * 25U;
                    if (s_score > s_best) s_best = s_score;
                    update_score_panel();
                    save_best();
                    s_state_tick = now;
                    if (s_round + 1U >= LEVEL_COUNT)
                    {
                        s_state = ST_COMPLETE;
                        show_well_banner("WELL DONE!", "22 CLEARED", COLOR_NEON_GREEN);
                    }
                    else
                    {
                        s_round++;
                        setup_round(now);
                        s_state = ST_INTRO;
                        render_well(now);
                        char title[12] = "ROUND ";
                        sdk_uint_to_str((uint32_t)s_round + 1U, &title[6], 2);
                        show_well_banner(title, round_rule_hint(), COLOR_YELLOW);
                    }
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

            case ST_COMPLETE:
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
