#include "console_sdk.h"

/* =========================================================================
 * Cyber-Man (Pac-Man): 27x19 symmetric maze (11x11 px tiles), wrap tunnels,
 * 4 ghosts with distinct AI (Blinky, Pinky, Inky, Clyde), Scatter/Chase/
 * Frightened/Eaten states, buffered cornering, 3 lives, progressive waves.
 * Controls: D-Pad / Arrows turn, A start, START (pad L2/R2, key P) pause, ESC exit.
 * Best score: SD:/appdata/pacman/best.dat
 * ========================================================================= */

#define HUD_H           16
#define MAZE_W          27
#define MAZE_H          19
#define TILE            11
#define MAP_X           11      /* (320 - 27*11) / 2 = 11 */
#define MAP_Y           22

#define PAC_START_X     13
#define PAC_START_Y     15
#define GHOST_HOME_X    13
#define GHOST_HOME_Y    9
#define GHOST_EXIT_Y    7

#define FRIGHT_MS       6000U
#define FLASH_WARN_MS   1800U

#define BG              COLOR_BLACK
#define COLOR_WALL      0x19FFU /* Neon Blue wall */
#define COLOR_GATE      COLOR_MAGENTA
#define COLOR_DOT       0xF6B2U /* Warm Cream pellet */
#define COLOR_POWER     COLOR_AMBER
#define COLOR_FRIGHT    0x001FU /* Deep Blue */

enum { DIR_UP = 0, DIR_LEFT = 1, DIR_DOWN = 2, DIR_RIGHT = 3, DIR_NONE = 4 };
enum { G_BLINKY = 0, G_PINKY = 1, G_INKY = 2, G_CLYDE = 3, GHOST_COUNT = 4 };
enum { GM_SCATTER = 0, GM_CHASE = 1, GM_FRIGHT = 2, GM_EATEN = 3 };
enum { ST_READY, ST_PLAY, ST_DYING, ST_PAUSE, ST_OVER };

enum { T_EMPTY = 0, T_WALL = 1, T_DOT = 2, T_POWER = 3, T_GATE = 4 };

static const int8_t s_dx[4] = { 0, -1, 0, 1 };
static const int8_t s_dy[4] = { -1, 0, 1, 0 };
static const uint8_t s_opp[4] = { DIR_DOWN, DIR_RIGHT, DIR_UP, DIR_LEFT };

/* 27x19 Arcade Maze ('#' wall, '.' dot, 'o' energizer, '=' ghost gate, ' ' empty/tunnel) */
static const char *const s_maze_template[MAZE_H] = {
    "###########################",
    "#o...........#...........o#",
    "#.####.#####.#.#####.####.#",
    "#.####.#####.#.#####.####.#",
    "#.........................#",
    "#.####.##.#######.##.####.#",
    "#......##....#....##......#",
    "######.##### # #####.######",
    "     #.##         ##.#     ",
    "######.## ###=### ##.######",
    "      .   #     #   .      ",
    "######.## ####### ##.######",
    "     #.##         ##.#     ",
    "######.## ####### ##.######",
    "#............#............#",
    "#.####.#####.#.#####.####.#",
    "#o..##....... .......##..o#",
    "###.##.##.#######.##.##.###",
    "###########################"
};

/* 11x11 Pac-Man Sprite Templates (Closed + 4 directional open-mouth frames) */
static const char *const s_pac_art[5][TILE] = {
    /* 0: Closed mouth */
    { "...XXXXX...", "..XXXXXXX..", ".XXXXXXXXX.", "XXXXXXXXXXX", "XXXXXXXXXXX",
      "XXXXXXXXXXX", "XXXXXXXXXXX", "XXXXXXXXXXX", ".XXXXXXXXX.", "..XXXXXXX..", "...XXXXX..." },
    /* 1: Up */
    { "...X...X...", "..XX...XX..", ".XXX...XXX.", "XXXX...XXXX", "XXXXX.XXXXX",
      "XXXXXXXXXXX", "XXXXXXXXXXX", "XXXXXXXXXXX", ".XXXXXXXXX.", "..XXXXXXX..", "...XXXXX..." },
    /* 2: Left */
    { "...XXXXX...", "..XXXXXXX..", ".XXXXXXXXX.", "....XXXXXXX", "......XXXXX",
      "........XXX", "......XXXXX", "....XXXXXXX", ".XXXXXXXXX.", "..XXXXXXX..", "...XXXXX..." },
    /* 3: Down */
    { "...XXXXX...", "..XXXXXXX..", ".XXXXXXXXX.", "XXXXXXXXXXX", "XXXXXXXXXXX",
      "XXXXXXXXXXX", "XXXXX.XXXXX", "XXXX...XXXX", ".XXX...XXX.", "..XX...XX..", "...X...X..." },
    /* 4: Right */
    { "...XXXXX...", "..XXXXXXX..", ".XXXXXXXXX.", "XXXXXXX....", "XXXXX......",
      "XXX........", "XXXXX......", "XXXXXXX....", ".XXXXXXXXX.", "..XXXXXXX..", "...XXXXX..." }
};

/* 11x11 Ghost Sprite Template ('B' body, 'W' eye sclera, 'P' pupil, 'M' frightened mouth) */
static const char *const s_ghost_art[TILE] = {
    "...BBBBB...",
    "..BBBBBBB..",
    ".BBBBBBBBB.",
    ".BWWB.BWWB.",
    "BWWPPBWWPPB",
    "BWWPPBWWPPB",
    "BBBBBBBBBBB",
    "BBBBBBBBBBB",
    "BBBBBBBBBBB",
    "BBBBBBBBBBB",
    "BB.BB.BB.BB"
};

static const SDK_Note_t s_snd_chomp[]  = { { 1, 5 } };
static const SDK_Note_t s_snd_power[]  = { { 1, 20 }, { 0, 15 }, { 1, 30 } };
static const SDK_Note_t s_snd_eat_g[]  = { { 1, 35 }, { 0, 20 }, { 1, 60 } };
static const SDK_Note_t s_snd_die[]    = { { 1, 100 }, { 0, 60 }, { 1, 120 }, { 0, 60 }, { 1, 250 } };
static const SDK_Note_t s_snd_start[]  = { { 1, 50 }, { 0, 40 }, { 1, 50 }, { 0, 40 }, { 1, 100 } };

typedef struct {
    int16_t px, py;         /* Pixel coordinates relative to MAP_X, MAP_Y */
    int16_t prev_px, prev_py;
    uint8_t dir;
    uint8_t mode;
    bool    in_house;
    uint16_t release_delay;
} Ghost_t;

/* Pre-rendered 11x11 sprites in External SRAM */
static SDK_EXT_BSS uint16_t s_spr_pac[5][TILE * TILE];
static SDK_EXT_BSS uint16_t s_spr_ghost[GHOST_COUNT][TILE * TILE];
static SDK_EXT_BSS uint16_t s_spr_fright[2][TILE * TILE];
static SDK_EXT_BSS uint16_t s_spr_eyes[TILE * TILE];
static SDK_EXT_BSS uint16_t s_spr_tile[TILE * TILE];

static uint8_t  s_map[MAZE_H][MAZE_W];
static uint16_t s_dots_left;

static int16_t  s_pac_px, s_pac_py;
static int16_t  s_pac_prev_px = -1, s_pac_prev_py = -1;
static uint8_t  s_pac_dir, s_want_dir;
static uint8_t  s_pac_step;

static Ghost_t  s_ghosts[GHOST_COUNT];
static uint16_t s_ghost_combo;
static uint32_t s_fright_end_tick;
static uint32_t s_mode_tick;
static bool     s_scatter_phase;

static uint8_t  s_state = ST_READY;
static uint32_t s_state_tick;
static uint32_t s_score, s_best, s_best_saved;
static uint8_t  s_lives;
static uint8_t  s_wave;

static void sound(const SDK_Note_t *notes, uint32_t n)
{
    (void)sys_audio_play(notes, n, 0);
}

static int16_t iabs16(int16_t v)
{
    return (v < 0) ? (int16_t)-v : v;
}

/* ---------------------------------------------------------------- sprites */

static void build_ghost_sprite(uint16_t *dst, uint16_t body_col, uint16_t eye_w, uint16_t pupil, bool fright)
{
    for (uint8_t y = 0; y < TILE; y++)
    {
        for (uint8_t x = 0; x < TILE; x++)
        {
            char ch = s_ghost_art[y][x];
            uint16_t c = BG;
            if (fright && (y == 8U) && ((x == 2U) || (x == 4U) || (x == 6U) || (x == 8U)))
            {
                c = pupil; /* Wavy frightened mouth */
            }
            else if (ch == 'B') c = body_col;
            else if (ch == 'W') c = eye_w;
            else if (ch == 'P') c = pupil;
            dst[y * TILE + x] = c;
        }
    }
}

static void build_sprites(void)
{
    for (uint8_t f = 0; f < 5U; f++)
    {
        for (uint8_t y = 0; y < TILE; y++)
        {
            for (uint8_t x = 0; x < TILE; x++)
            {
                s_spr_pac[f][y * TILE + x] = (s_pac_art[f][y][x] == 'X') ? COLOR_YELLOW : BG;
            }
        }
    }

    static const uint16_t g_cols[GHOST_COUNT] = {
        COLOR_RED, COLOR_MAGENTA, COLOR_CYAN, COLOR_AMBER
    };
    for (uint8_t g = 0; g < GHOST_COUNT; g++)
    {
        build_ghost_sprite(s_spr_ghost[g], g_cols[g], COLOR_WHITE, COLOR_BLUE, false);
    }
    build_ghost_sprite(s_spr_fright[0], COLOR_FRIGHT, COLOR_AMBER, COLOR_AMBER, true);
    build_ghost_sprite(s_spr_fright[1], COLOR_WHITE,  COLOR_RED,   COLOR_RED,   true);
    build_ghost_sprite(s_spr_eyes,      BG,           COLOR_WHITE, COLOR_CYAN,  false);
}

/* ---------------------------------------------------------------- drawing */

static void draw_num(uint16_t x, uint32_t v, uint8_t digits, uint16_t color)
{
    char buf[12];
    sdk_uint_to_str(v, buf, digits);
    sys_draw_text(x, 4, buf, color, COLOR_PANEL_BG, 1);
}

static void draw_hud(void)
{
    sys_draw_text(6, 4, "SCORE", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(42, s_score, 5, COLOR_NEON_GREEN);
    sys_draw_text(84, 4, "LIVES", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(120, s_lives, 1, COLOR_YELLOW);
    sys_draw_text(138, 4, "WAVE", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(168, (uint32_t)s_wave + 1U, 2, COLOR_CYAN);
    sys_draw_text(192, 4, "BEST", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(222, s_best, 5, COLOR_AMBER);
    sys_draw_text(266, 4, (s_state == ST_PAUSE) ? "PAUSE" : "     ", COLOR_YELLOW, COLOR_PANEL_BG, 1);
}

static void draw_tile(int8_t tx, int8_t ty)
{
    if ((tx < 0) || (tx >= MAZE_W) || (ty < 0) || (ty >= MAZE_H)) return;
    uint8_t t = s_map[ty][tx];

    for (uint8_t i = 0; i < TILE * TILE; i++) s_spr_tile[i] = BG;

    if (t == T_WALL)
    {
        for (uint8_t y = 1; y < TILE - 1U; y++)
        {
            for (uint8_t x = 1; x < TILE - 1U; x++)
            {
                bool edge = (y == 1U) || (y == TILE - 2U) || (x == 1U) || (x == TILE - 2U);
                s_spr_tile[y * TILE + x] = edge ? COLOR_WALL : 0x0843U;
            }
        }
    }
    else if (t == T_GATE)
    {
        for (uint8_t x = 0; x < TILE; x++)
        {
            s_spr_tile[4 * TILE + x] = COLOR_GATE;
            s_spr_tile[5 * TILE + x] = COLOR_GATE;
        }
    }
    else if (t == T_DOT)
    {
        s_spr_tile[5 * TILE + 5] = COLOR_DOT;
        s_spr_tile[5 * TILE + 6] = COLOR_DOT;
        s_spr_tile[6 * TILE + 5] = COLOR_DOT;
        s_spr_tile[6 * TILE + 6] = COLOR_DOT;
    }
    else if (t == T_POWER)
    {
        for (uint8_t y = 3; y <= 7U; y++)
        {
            for (uint8_t x = 3; x <= 7U; x++)
            {
                if (((y == 3U) || (y == 7U)) && ((x == 3U) || (x == 7U))) continue;
                s_spr_tile[y * TILE + x] = COLOR_POWER;
            }
        }
    }

    sys_draw_sprite((uint16_t)(MAP_X + tx * TILE), (uint16_t)(MAP_Y + ty * TILE), TILE, TILE, s_spr_tile);
}

/* Restores up to 2x2 maze tiles under a moving sprite at (px, py) */
static void restore_under(int16_t px, int16_t py)
{
    if (px < 0 || py < 0) return;
    int8_t tx0 = (int8_t)(px / TILE);
    int8_t ty0 = (int8_t)(py / TILE);
    int8_t tx1 = (int8_t)((px + TILE - 1) / TILE);
    int8_t ty1 = (int8_t)((py + TILE - 1) / TILE);

    draw_tile(tx0, ty0);
    if (tx1 != tx0) draw_tile(tx1, ty0);
    if (ty1 != ty0) draw_tile(tx0, ty1);
    if ((tx1 != tx0) && (ty1 != ty0)) draw_tile(tx1, ty1);
}

static void draw_full_maze(void)
{
    sys_clear_screen(BG);
    sys_lcd_fill_rect(0, 0, SDK_SCREEN_W, HUD_H, COLOR_PANEL_BG);
    draw_hud();
    for (int8_t y = 0; y < MAZE_H; y++)
    {
        for (int8_t x = 0; x < MAZE_W; x++)
        {
            draw_tile(x, y);
        }
    }
}

static void render_actors(uint32_t now)
{
    /* 1. Restore background under old positions */
    restore_under(s_pac_prev_px, s_pac_prev_py);
    for (uint8_t g = 0; g < GHOST_COUNT; g++)
    {
        restore_under(s_ghosts[g].prev_px, s_ghosts[g].prev_py);
    }

    /* 2. Draw Pac-Man */
    uint8_t pframe = ((s_pac_step & 2U) == 0U || s_pac_dir >= 4U) ? 0U : (uint8_t)(s_pac_dir + 1U);
    sys_draw_sprite((uint16_t)(MAP_X + s_pac_px), (uint16_t)(MAP_Y + s_pac_py), TILE, TILE, s_spr_pac[pframe]);
    s_pac_prev_px = s_pac_px;
    s_pac_prev_py = s_pac_py;

    /* 3. Draw 4 Ghosts */
    for (uint8_t g = 0; g < GHOST_COUNT; g++)
    {
        Ghost_t *gh = &s_ghosts[g];
        const uint16_t *spr = s_spr_ghost[g];
        if (gh->mode == GM_EATEN)
        {
            spr = s_spr_eyes;
        }
        else if (gh->mode == GM_FRIGHT)
        {
            bool flash = ((s_fright_end_tick - now) <= FLASH_WARN_MS) && (((now / 150U) & 1U) != 0U);
            spr = s_spr_fright[flash ? 1U : 0U];
        }
        sys_draw_sprite((uint16_t)(MAP_X + gh->px), (uint16_t)(MAP_Y + gh->py), TILE, TILE, spr);
        gh->prev_px = gh->px;
        gh->prev_py = gh->py;
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

static void add_score(uint32_t pts)
{
    s_score += pts;
    if (s_score > s_best) s_best = s_score;
    draw_hud();
}

/* ---------------------------------------------------------------- setup */

static void reset_positions(uint32_t now)
{
    s_pac_px = PAC_START_X * TILE;
    s_pac_py = PAC_START_Y * TILE;
    s_pac_prev_px = -1;
    s_pac_dir  = DIR_LEFT;
    s_want_dir = DIR_LEFT;
    s_pac_step = 0;

    static const int8_t gx0[GHOST_COUNT] = { 13, 13, 12, 14 };
    static const int8_t gy0[GHOST_COUNT] = {  7,  9,  9,  9 };
    static const uint16_t delays[GHOST_COUNT] = { 0, 30, 90, 150 };

    for (uint8_t g = 0; g < GHOST_COUNT; g++)
    {
        s_ghosts[g].px = (int16_t)(gx0[g] * TILE);
        s_ghosts[g].py = (int16_t)(gy0[g] * TILE);
        s_ghosts[g].prev_px = -1;
        s_ghosts[g].dir = (g == 0U) ? DIR_LEFT : DIR_UP;
        s_ghosts[g].mode = GM_SCATTER;
        s_ghosts[g].in_house = (g != 0U);
        s_ghosts[g].release_delay = delays[g];
    }

    s_scatter_phase = true;
    s_mode_tick = now + 7000U;
    s_fright_end_tick = 0;
}

static void load_maze(uint32_t now)
{
    s_dots_left = 0;
    for (uint8_t y = 0; y < MAZE_H; y++)
    {
        for (uint8_t x = 0; x < MAZE_W; x++)
        {
            char ch = s_maze_template[y][x];
            if (ch == '#')      s_map[y][x] = T_WALL;
            else if (ch == '=') s_map[y][x] = T_GATE;
            else if (ch == '.') { s_map[y][x] = T_DOT;   s_dots_left++; }
            else if (ch == 'o') { s_map[y][x] = T_POWER; s_dots_left++; }
            else                s_map[y][x] = T_EMPTY;
        }
    }
    reset_positions(now);
    draw_full_maze();
    render_actors(now);
}

static void new_game(uint32_t now)
{
    s_score = 0;
    s_lives = 3;
    s_wave  = 0;
    load_maze(now);
    s_state = ST_PLAY;
    sound(s_snd_start, 5);
}

/* ---------------------------------------------------------------- movement & AI */

static bool can_enter(int8_t tx, int8_t ty, bool allow_gate)
{
    if ((ty < 0) || (ty >= MAZE_H)) return false;
    if ((tx < 0) || (tx >= MAZE_W)) return true; /* Tunnel wrap */
    uint8_t t = s_map[ty][tx];
    if (t == T_WALL) return false;
    if ((t == T_GATE) && !allow_gate) return false;
    return true;
}

static void move_pacman(uint32_t now)
{
    bool aligned_x = (s_pac_px % TILE) == 0;
    bool aligned_y = (s_pac_py % TILE) == 0;

    if (aligned_x && aligned_y)
    {
        int8_t tx = (int8_t)(s_pac_px / TILE);
        int8_t ty = (int8_t)(s_pac_py / TILE);

        /* Eat pellet on current tile */
        if (s_map[ty][tx] == T_DOT)
        {
            s_map[ty][tx] = T_EMPTY;
            s_dots_left--;
            add_score(10U);
            sound(s_snd_chomp, 1);
        }
        else if (s_map[ty][tx] == T_POWER)
        {
            s_map[ty][tx] = T_EMPTY;
            s_dots_left--;
            add_score(50U);
            s_ghost_combo = 200U;
            uint32_t dur = (s_wave < 5U) ? (FRIGHT_MS - s_wave * 800U) : 2000U;
            s_fright_end_tick = now + dur;
            for (uint8_t g = 0; g < GHOST_COUNT; g++)
            {
                if (s_ghosts[g].mode != GM_EATEN)
                {
                    s_ghosts[g].mode = GM_FRIGHT;
                    s_ghosts[g].dir  = s_opp[s_ghosts[g].dir & 3U];
                }
            }
            sound(s_snd_power, 3);
        }

        /* Try buffered turn first */
        if ((s_want_dir < 4U) && can_enter((int8_t)(tx + s_dx[s_want_dir]), (int8_t)(ty + s_dy[s_want_dir]), false))
        {
            s_pac_dir = s_want_dir;
        }
        /* Stop if blocked ahead */
        if ((s_pac_dir < 4U) && !can_enter((int8_t)(tx + s_dx[s_pac_dir]), (int8_t)(ty + s_dy[s_pac_dir]), false))
        {
            return;
        }
    }
    else if ((s_want_dir < 4U) && (s_want_dir == s_opp[s_pac_dir & 3U]))
    {
        /* Instant 180-degree reversal allowed between tile centers */
        s_pac_dir = s_want_dir;
    }

    if (s_pac_dir < 4U)
    {
        s_pac_px = (int16_t)(s_pac_px + s_dx[s_pac_dir]);
        s_pac_py = (int16_t)(s_pac_py + s_dy[s_pac_dir]);
        if (s_pac_px < 0) s_pac_px = (MAZE_W - 1) * TILE;
        if (s_pac_px >= MAZE_W * TILE) s_pac_px = 0;
        s_pac_step++;
    }
}

static void get_ghost_target(uint8_t g, int8_t *target_x, int8_t *target_y)
{
    static const int8_t corners_x[GHOST_COUNT] = { MAZE_W - 2, 1, MAZE_W - 2, 1 };
    static const int8_t corners_y[GHOST_COUNT] = { 0, 0, MAZE_H - 1, MAZE_H - 1 };

    Ghost_t *gh = &s_ghosts[g];
    if (gh->mode == GM_EATEN)
    {
        *target_x = GHOST_HOME_X;
        *target_y = GHOST_EXIT_Y;
        return;
    }
    if (gh->mode == GM_SCATTER)
    {
        *target_x = corners_x[g];
        *target_y = corners_y[g];
        return;
    }

    int8_t px = (int8_t)(s_pac_px / TILE);
    int8_t py = (int8_t)(s_pac_py / TILE);
    uint8_t pdir = (s_pac_dir < 4U) ? s_pac_dir : DIR_LEFT;

    switch (g)
    {
        case G_BLINKY: /* Red: Direct chaser */
            *target_x = px;
            *target_y = py;
            break;

        case G_PINKY:  /* Pink: Ambush 4 tiles ahead of Pac-Man */
            *target_x = (int8_t)(px + 4 * s_dx[pdir]);
            *target_y = (int8_t)(py + 4 * s_dy[pdir]);
            break;

        case G_INKY:   /* Cyan: Flank vector using Blinky's position */
        {
            int8_t ax = (int8_t)(px + 2 * s_dx[pdir]);
            int8_t ay = (int8_t)(py + 2 * s_dy[pdir]);
            int8_t bx = (int8_t)(s_ghosts[G_BLINKY].px / TILE);
            int8_t by = (int8_t)(s_ghosts[G_BLINKY].py / TILE);
            *target_x = (int8_t)(ax + (ax - bx));
            *target_y = (int8_t)(ay + (ay - by));
            break;
        }

        default:       /* Clyde: Chase when far, retreat to corner when close */
        {
            int16_t dx = (int16_t)(px - (gh->px / TILE));
            int16_t dy = (int16_t)(py - (gh->py / TILE));
            if (dx * dx + dy * dy > 36)
            {
                *target_x = px;
                *target_y = py;
            }
            else
            {
                *target_x = corners_x[G_CLYDE];
                *target_y = corners_y[G_CLYDE];
            }
            break;
        }
    }
}

static void step_single_ghost(uint8_t g)
{
    Ghost_t *gh = &s_ghosts[g];

    if (gh->release_delay > 0U)
    {
        gh->release_delay--;
        return;
    }

    /* Exit Ghost House smoothly */
    if (gh->in_house)
    {
        int16_t target_px = GHOST_HOME_X * TILE;
        int16_t target_py = GHOST_EXIT_Y * TILE;
        if (gh->px < target_px)      gh->px++;
        else if (gh->px > target_px) gh->px--;
        else if (gh->py > target_py) gh->py--;
        else
        {
            gh->in_house = false;
            gh->dir = DIR_LEFT;
        }
        return;
    }

    if (((gh->px % TILE) == 0) && ((gh->py % TILE) == 0))
    {
        int8_t tx = (int8_t)(gh->px / TILE);
        int8_t ty = (int8_t)(gh->py / TILE);

        /* Respawn when eaten eyes reach the Ghost House entrance */
        if ((gh->mode == GM_EATEN) && (tx == GHOST_HOME_X) && (ty == GHOST_EXIT_Y))
        {
            gh->mode = s_scatter_phase ? GM_SCATTER : GM_CHASE;
        }

        int8_t targ_x, targ_y;
        get_ghost_target(g, &targ_x, &targ_y);

        uint8_t best_dir = gh->dir;
        int32_t best_dist = 0x7FFFFFFF;
        uint8_t rev_dir = s_opp[gh->dir & 3U];

        for (uint8_t d = 0; d < 4U; d++)
        {
            if (d == rev_dir) continue;
            int8_t nx = (int8_t)(tx + s_dx[d]);
            int8_t ny = (int8_t)(ty + s_dy[d]);
            if (!can_enter(nx, ny, false)) continue;

            if (gh->mode == GM_FRIGHT)
            {
                /* Pseudo-random turn when frightened */
                if ((best_dist == 0x7FFFFFFF) || (sys_random_range(3U) == 0U))
                {
                    best_dist = 0;
                    best_dir = d;
                }
            }
            else
            {
                int32_t dx = (int32_t)nx - (int32_t)targ_x;
                int32_t dy = (int32_t)ny - (int32_t)targ_y;
                int32_t dist = dx * dx + dy * dy;
                if (dist < best_dist)
                {
                    best_dist = dist;
                    best_dir = d;
                }
            }
        }

        /* Dead-end fallback */
        if (best_dist == 0x7FFFFFFF)
        {
            best_dir = rev_dir;
        }
        gh->dir = best_dir;
    }

    gh->px = (int16_t)(gh->px + s_dx[gh->dir]);
    gh->py = (int16_t)(gh->py + s_dy[gh->dir]);
    if (gh->px < 0) gh->px = (MAZE_W - 1) * TILE;
    if (gh->px >= MAZE_W * TILE) gh->px = 0;
}

static void check_collisions(uint32_t now)
{
    for (uint8_t g = 0; g < GHOST_COUNT; g++)
    {
        Ghost_t *gh = &s_ghosts[g];
        if ((iabs16((int16_t)(gh->px - s_pac_px)) <= 6) &&
            (iabs16((int16_t)(gh->py - s_pac_py)) <= 6))
        {
            if (gh->mode == GM_FRIGHT)
            {
                gh->mode = GM_EATEN;
                add_score(s_ghost_combo);
                if (s_ghost_combo < 1600U) s_ghost_combo <<= 1;
                sound(s_snd_eat_g, 3);
            }
            else if (gh->mode != GM_EATEN)
            {
                s_state = ST_DYING;
                s_state_tick = now;
                sound(s_snd_die, 5);
                return;
            }
        }
    }
}

static void update_timers(uint32_t now, uint32_t frame)
{
    /* End Frightened mode when timer expires */
    if ((s_fright_end_tick != 0U) && ((int32_t)(now - s_fright_end_tick) >= 0))
    {
        s_fright_end_tick = 0;
        for (uint8_t g = 0; g < GHOST_COUNT; g++)
        {
            if (s_ghosts[g].mode == GM_FRIGHT)
            {
                s_ghosts[g].mode = s_scatter_phase ? GM_SCATTER : GM_CHASE;
            }
        }
    }

    /* Alternate Scatter (7s) and Chase (20s) waves */
    if ((s_fright_end_tick == 0U) && ((int32_t)(now - s_mode_tick) >= 0))
    {
        s_scatter_phase = !s_scatter_phase;
        s_mode_tick = now + (s_scatter_phase ? 7000U : 20000U);
        for (uint8_t g = 0; g < GHOST_COUNT; g++)
        {
            if ((s_ghosts[g].mode == GM_SCATTER) || (s_ghosts[g].mode == GM_CHASE))
            {
                s_ghosts[g].mode = s_scatter_phase ? GM_SCATTER : GM_CHASE;
                s_ghosts[g].dir  = s_opp[s_ghosts[g].dir & 3U];
            }
        }
    }

    /* Step Pac-Man (2 px per frame) */
    for (uint8_t step = 0; step < 1U; step++)
    {
        move_pacman(now);
        check_collisions(now);
        if (s_state != ST_PLAY) return;
    }

    /* Step Ghosts (Frightened: 1px/frame, Eaten: 3px/frame, Normal: ~1.75px/frame) */
    for (uint8_t g = 0; g < GHOST_COUNT; g++)
    {
        uint8_t steps = 1U;
        if (s_ghosts[g].mode == GM_FRIGHT)     steps = 1U;
        else if (s_ghosts[g].mode == GM_EATEN) steps = 3U;
        else if ((frame & 3U) == 0U)           steps = 1U;

        for (uint8_t s = 0; s < steps; s++)
        {
            step_single_ghost(g);
            check_collisions(now);
            if (s_state != ST_PLAY) return;
        }
    }

    if (s_dots_left == 0U)
    {
        s_wave++;
        sound(s_snd_start, 5);
        load_maze(now);
    }
}

static void show_banner(const char *msg, uint16_t color)
{
    uint16_t len = 0;
    while (msg[len] != '\0') len++;
    uint16_t w = (uint16_t)(len * 6U + 12U);
    uint16_t x = (uint16_t)((SDK_SCREEN_W - w) / 2U);
    sys_lcd_fill_rect(x, 126, w, 16, COLOR_PANEL_BG);
    sys_draw_text((uint16_t)(x + 6U), 130, msg, color, COLOR_PANEL_BG, 1);
}

void app_main(void)
{
    SDK_Input_t in;
    uint32_t next_frame = 0;
    uint32_t frame = 0;

    build_sprites();
    load_best();
    s_lives = 3;
    load_maze(sys_get_tick());
    show_banner("PRESS A TO START", COLOR_AMBER);
    sdk_input_init(&in);

    for (;;)
    {
        uint32_t now = sys_get_tick();
        sdk_input_update(&in, sys_get_keys(), now);
        frame++;

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
                    new_game(now);
                }
                break;

            case ST_PLAY:
                if (in.pressed & KEY_START)
                {
                    s_state = ST_PAUSE;
                    draw_hud();
                    show_banner("PAUSED", COLOR_YELLOW);
                    break;
                }
                if (in.held & KEY_UP)    s_want_dir = DIR_UP;
                if (in.held & KEY_LEFT)  s_want_dir = DIR_LEFT;
                if (in.held & KEY_DOWN)  s_want_dir = DIR_DOWN;
                if (in.held & KEY_RIGHT) s_want_dir = DIR_RIGHT;

                update_timers(now, frame);
                if (s_state == ST_PLAY)
                {
                    render_actors(now);
                }
                break;

            case ST_DYING:
                if ((now - s_state_tick) >= 1000U)
                {
                    if (--s_lives == 0U)
                    {
                        draw_hud();
                        save_best();
                        s_state = ST_OVER;
                        s_state_tick = now;
                        show_banner("GAME OVER - A: RETRY", COLOR_RED);
                    }
                    else
                    {
                        draw_hud();
                        reset_positions(now);
                        draw_full_maze();
                        render_actors(now);
                        s_state = ST_PLAY;
                    }
                }
                break;

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    s_state = ST_PLAY;
                    draw_full_maze();
                    render_actors(now);
                }
                break;

            case ST_OVER:
                if ((in.pressed & (KEY_A | KEY_UP)) && ((now - s_state_tick) > 600U))
                {
                    new_game(now);
                }
                break;

            default:
                break;
        }

        sdk_frame_sync(&next_frame, 16U);
    }
}
