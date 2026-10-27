#include "console_sdk.h"

/* =========================================================================
 * Cyber Lode Runner (SDK app)
 * 28x16 tile arena (11x13 px tiles), 4 multi-story levels, brick digging
 * left/right, timed hole regeneration, rope traversal, smart guards that
 * fall into pits and climb out, hidden escape ladder on collecting all gold!
 *
 * Controls:
 *  - D-Pad / Arrows: Move / Climb ladder / Traverse rope / Drop from rope
 *  - B (Pad X / Key Z,X): Dig Brick Bottom-LEFT
 *  - X (Pad Y / Key C):   Dig Brick Bottom-RIGHT
 *  - A (Pad A / Space):   Dig Brick in current facing direction
 *  - START (Pad L2/R2, Key P): Pause
 *  - ESC (Pad B): Exit
 * Best score: SD:/appdata/loderunner/best.dat
 * ========================================================================= */

#define HUD_H           16
#define MAP_W           28
#define MAP_H           16
#define TW              11
#define TH              13
#define MAP_X           6       /* (320 - 28*11) / 2 = 6 */
#define MAP_Y           22      /* 22 + 16*13 = 230 */

#define MAX_GUARDS      3
#define MAX_HOLES       12
#define HOLE_OPEN_MS    6800U
#define HOLE_WARN_MS    1500U
#define TRAP_TIME_MS    3400U
#define LEVEL_COUNT     4U

#define BG              COLOR_BLACK
#define COLOR_BRICK     0xC200U /* Terra-cotta red brick */
#define COLOR_MORTAR    0x6100U
#define COLOR_SOLID     0x7BEF  /* Concrete steel grey */
#define COLOR_LADDER    COLOR_CYAN
#define COLOR_ROPE      COLOR_AMBER
#define COLOR_GOLD      COLOR_YELLOW

enum {
    T_EMPTY  = 0,
    T_BRICK  = 1,
    T_SOLID  = 2,
    T_LADDER = 3,
    T_ROPE   = 4,
    T_GOLD   = 5,
    T_HLAD   = 6,   /* Hidden escape ladder until all gold collected */
    T_HOLE   = 7    /* Dug brick pit */
};

enum { ST_READY, ST_PLAY, ST_DYING, ST_PAUSE, ST_OVER };

typedef struct {
    uint8_t  tx, ty;
    uint32_t close_tick;
    uint8_t  last_vis;
    bool     active;
} Hole_t;

typedef struct {
    int16_t  px, py;
    int16_t  prev_px, prev_py;
    int8_t   facing;      /* -1 left, +1 right */
    uint8_t  anim;
    bool     in_pit;
    uint32_t release_tick;
    bool     active;
} Guard_t;

/* 4 Classic Lode Runner Multi-Story Levels (28x16)
 * '#' brick, '@' solid, 'H' ladder, '-' rope, '$' gold, '^' hidden exit ladder,
 * 'P' player start, 'G' guard start */
static const char *const s_levels[LEVEL_COUNT][MAP_H] = {
    /* Level 1: Classic Terraces */
    {
        "                  ^         ",
        "    $            ^    $     ",
        "#######H#########H##########",
        "       H         H          ",
        "  G    H   ---   H     $   G",
        "###H######     ######H######",
        "   H                 H      ",
        "   H   $         $   H      ",
        "#########H#####H############",
        "         H     H            ",
        "    $    H  G  H    $       ",
        "####H###############H#######",
        "    H               H       ",
        "    H       P       H   $   ",
        "############################",
        "@@@@@@@@@@@@@@@@@@@@@@@@@@@@"
    },
    /* Level 2: High Wire Rope Crossing */
    {
        "             ^              ",
        "  $          ^          $   ",
        "#####H#######H#######H######",
        "     H  -----------  H      ",
        " G   H       $       H   G  ",
        "########H#########H#########",
        "        H         H         ",
        "  $     H ------- H     $   ",
        "#####H###         ###H######",
        "     H       G       H      ",
        "     H   $       $   H      ",
        "#########H#######H##########",
        "         H       H          ",
        "   $     H   P   H     $    ",
        "############################",
        "@@@@@@@@@@@@@@@@@@@@@@@@@@@@"
    },
    /* Level 3: TheTwin Towers */
    {
        "             ^              ",
        "   $   G     ^     G   $    ",
        "######H#############H#######",
        "      H             H       ",
        "  $   H   $     $   H   $   ",
        "###H#####H#######H#####H####",
        "   H     H       H     H    ",
        "   H --- H  ---  H --- H    ",
        "####     #########     #####",
        "   $         G         $    ",
        "######H#############H#######",
        "      H             H       ",
        "  $   H      P      H   $   ",
        "###H###################H####",
        "############################",
        "@@@@@@@@@@@@@@@@@@@@@@@@@@@@"
    },
    /* Level 4: Pitfall Fortress */
    {
        "                          ^ ",
        "  $        $   $          ^ ",
        "####H#################H#####",
        "    H  -------------  H     ",
        " G  H                 H  G  ",
        "#######H###########H########",
        "       H     $     H        ",
        "  $    H   #####   H    $   ",
        "####H#####       #####H#####",
        "    H    ---------    H     ",
        "    H  $     G     $  H     ",
        "########H#########H#########",
        "        H         H         ",
        "   $    H    P    H    $    ",
        "############################",
        "@@@@@@@@@@@@@@@@@@@@@@@@@@@@"
    }
};

/* 11x13 Runner / Guard Sprite Templates (2 animation frames) */
static const char *const s_actor_art[2][TH] = {
    {
        "...HHHHH...",
        "..HHHHHHH..",
        "...FFFFF...",
        "...FWWWF...",
        "....FFF....",
        "..BBBBBBB..",
        ".BB.BBB.BB.",
        ".B..BBB..B.",
        "....BBB....",
        "...LL.LL...",
        "..LL...LL..",
        ".LL.....LL.",
        ".L.......L."
    },
    {
        "...HHHHH...",
        "..HHHHHHH..",
        "...FFFFF...",
        "...FWWWF...",
        "....FFF....",
        "...BBBBB...",
        "..BBBBBBB..",
        "....BBB....",
        "...LLLLL...",
        "..LL...LL..",
        "...L...L...",
        "..LL..LL...",
        ".......... "
    }
};

static const SDK_Note_t s_snd_dig[]   = { { 1, 18 }, { 0, 10 }, { 1, 12 } };
static const SDK_Note_t s_snd_gold[]  = { { 1, 25 }, { 0, 15 }, { 1, 45 } };
static const SDK_Note_t s_snd_open[]  = { { 1, 40 }, { 0, 25 }, { 1, 40 }, { 0, 25 }, { 1, 90 } };
static const SDK_Note_t s_snd_trap[]  = { { 1, 20 } };
static const SDK_Note_t s_snd_die[]   = { { 1, 120 }, { 0, 60 }, { 1, 140 }, { 0, 60 }, { 1, 260 } };
static const SDK_Note_t s_snd_win[]   = { { 1, 50 }, { 0, 30 }, { 1, 50 }, { 0, 30 }, { 1, 50 }, { 0, 30 }, { 1, 180 } };

static SDK_EXT_BSS uint16_t s_spr_player[2][TW * TH];
static SDK_EXT_BSS uint16_t s_spr_guard[2][TW * TH];
static SDK_EXT_BSS uint16_t s_spr_tile[TW * TH];

static uint8_t  s_map[MAP_H][MAP_W];
static Hole_t   s_holes[MAX_HOLES];
static Guard_t  s_guards[MAX_GUARDS];
static uint8_t  s_guard_count;

static uint16_t s_gold_left;
static bool     s_exit_open;

static int16_t  s_px, s_py;
static int16_t  s_prev_px = -1, s_prev_py = -1;
static int8_t   s_facing = 1;
static uint8_t  s_anim;

static uint8_t  s_state = ST_READY;
static uint32_t s_state_tick;
static uint32_t s_score, s_best, s_best_saved;
static uint8_t  s_lives;
static uint8_t  s_level;

static void sound(const SDK_Note_t *notes, uint32_t n)
{
    (void)sys_audio_play(notes, n, 0);
}

static int16_t iabs16(int16_t v)
{
    return (v < 0) ? (int16_t)-v : v;
}

/* ---------------------------------------------------------------- sprites */

static void build_actor_sprite(uint16_t *dst, uint8_t frame, uint16_t helmet, uint16_t suit, uint16_t boots)
{
    for (uint8_t y = 0; y < TH; y++)
    {
        for (uint8_t x = 0; x < TW; x++)
        {
            char ch = s_actor_art[frame][y][x];
            uint16_t c = BG;
            if (ch == 'H')      c = helmet;
            else if (ch == 'F') c = COLOR_AMBER;
            else if (ch == 'W') c = COLOR_WHITE;
            else if (ch == 'B') c = suit;
            else if (ch == 'L') c = boots;
            dst[y * TW + x] = c;
        }
    }
}

static void build_sprites(void)
{
    for (uint8_t f = 0; f < 2U; f++)
    {
        build_actor_sprite(s_spr_player[f], f, COLOR_WHITE,   COLOR_CYAN, COLOR_NEON_GREEN);
        build_actor_sprite(s_spr_guard[f],  f, COLOR_MAGENTA, COLOR_RED,  COLOR_AMBER);
    }
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
    sys_draw_text(4, 4, "SCORE", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(38, s_score, 5, COLOR_NEON_GREEN);
    sys_draw_text(76, 4, "GOLD", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(104, s_gold_left, 2, COLOR_GOLD);
    sys_draw_text(124, 4, "LIVES", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(158, s_lives, 1, COLOR_RED);
    sys_draw_text(172, 4, "LVL", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(194, (uint32_t)s_level + 1U, 2, COLOR_CYAN);
    sys_draw_text(214, 4, "BEST", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(242, s_best, 5, COLOR_AMBER);
    sys_draw_text(280, 4, (s_state == ST_PAUSE) ? "PAUSE" : "     ", COLOR_YELLOW, COLOR_PANEL_BG, 1);
}

static void draw_tile(int8_t tx, int8_t ty)
{
    if ((tx < 0) || (tx >= MAP_W) || (ty < 0) || (ty >= MAP_H)) return;
    uint8_t t = s_map[ty][tx];

    for (uint8_t i = 0; i < TW * TH; i++) s_spr_tile[i] = BG;

    if (t == T_BRICK)
    {
        for (uint8_t y = 0; y < TH; y++)
        {
            bool mortar_row = (y == 0U) || (y == 6U) || (y == 12U);
            for (uint8_t x = 0; x < TW; x++)
            {
                bool mortar_col = ((y < 6U) && (x == 5U)) || ((y > 6U) && ((x == 2U) || (x == 8U)));
                s_spr_tile[y * TW + x] = (mortar_row || mortar_col) ? COLOR_MORTAR : COLOR_BRICK;
            }
        }
    }
    else if (t == T_SOLID)
    {
        for (uint8_t y = 0; y < TH; y++)
        {
            for (uint8_t x = 0; x < TW; x++)
            {
                bool border = (y == 0U) || (y == TH - 1U) || (x == 0U) || (x == TW - 1U);
                s_spr_tile[y * TW + x] = border ? COLOR_WHITE : COLOR_SOLID;
            }
        }
    }
    else if (t == T_LADDER)
    {
        for (uint8_t y = 0; y < TH; y++)
        {
            s_spr_tile[y * TW + 2] = COLOR_LADDER;
            s_spr_tile[y * TW + 8] = COLOR_LADDER;
            if ((y % 4U) == 1U)
            {
                for (uint8_t x = 2; x <= 8U; x++) s_spr_tile[y * TW + x] = COLOR_LADDER;
            }
        }
    }
    else if (t == T_ROPE)
    {
        for (uint8_t x = 0; x < TW; x++)
        {
            s_spr_tile[1 * TW + x] = COLOR_ROPE;
            s_spr_tile[2 * TW + x] = COLOR_ROPE;
        }
    }
    else if (t == T_GOLD)
    {
        for (uint8_t y = 5; y <= 11U; y++)
        {
            for (uint8_t x = 2; x <= 8U; x++)
            {
                bool rim = (y == 5U) || (y == 11U) || (x == 2U) || (x == 8U);
                s_spr_tile[y * TW + x] = rim ? COLOR_AMBER : COLOR_GOLD;
            }
        }
        s_spr_tile[8 * TW + 5] = COLOR_WHITE; /* Keyhole sparkle */
    }
    else if (t == T_HOLE)
    {
        /* Faint broken brick debris on sides */
        s_spr_tile[2 * TW + 0] = COLOR_MORTAR;
        s_spr_tile[9 * TW + TW - 1] = COLOR_MORTAR;
    }

    sys_draw_sprite((uint16_t)(MAP_X + tx * TW), (uint16_t)(MAP_Y + ty * TH), TW, TH, s_spr_tile);
}

static void restore_under(int16_t px, int16_t py)
{
    if (px < 0 || py < 0) return;
    int8_t tx0 = (int8_t)(px / TW);
    int8_t ty0 = (int8_t)(py / TH);
    int8_t tx1 = (int8_t)((px + TW - 1) / TW);
    int8_t ty1 = (int8_t)((py + TH - 1) / TH);

    draw_tile(tx0, ty0);
    if (tx1 != tx0) draw_tile(tx1, ty0);
    if (ty1 != ty0) draw_tile(tx0, ty1);
    if ((tx1 != tx0) && (ty1 != ty0)) draw_tile(tx1, ty1);
}

static void draw_full_map(void)
{
    sys_clear_screen(BG);
    sys_lcd_fill_rect(0, 0, SDK_SCREEN_W, HUD_H, COLOR_PANEL_BG);
    draw_hud();
    for (int8_t y = 0; y < MAP_H; y++)
    {
        for (int8_t x = 0; x < MAP_W; x++)
        {
            draw_tile(x, y);
        }
    }
}

static void render_actors(void)
{
    restore_under(s_prev_px, s_prev_py);
    for (uint8_t i = 0; i < s_guard_count; i++)
    {
        if (s_guards[i].active) restore_under(s_guards[i].prev_px, s_guards[i].prev_py);
    }

    /* Draw Player */
    sys_draw_sprite((uint16_t)(MAP_X + s_px), (uint16_t)(MAP_Y + s_py), TW, TH,
                    s_spr_player[(s_anim >> 2) & 1U]);
    s_prev_px = s_px;
    s_prev_py = s_py;

    /* Draw Guards */
    for (uint8_t i = 0; i < s_guard_count; i++)
    {
        Guard_t *g = &s_guards[i];
        if (!g->active) continue;
        sys_draw_sprite((uint16_t)(MAP_X + g->px), (uint16_t)(MAP_Y + g->py), TW, TH,
                        s_spr_guard[(g->anim >> 2) & 1U]);
        g->prev_px = g->px;
        g->prev_py = g->py;
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

/* ---------------------------------------------------------------- level setup */

static void load_level(void)
{
    const char *const *rows = s_levels[s_level % LEVEL_COUNT];
    s_gold_left   = 0;
    s_guard_count = 0;
    s_exit_open   = false;
    sdk_memset(s_holes, 0, sizeof(s_holes));

    for (uint8_t y = 0; y < MAP_H; y++)
    {
        for (uint8_t x = 0; x < MAP_W; x++)
        {
            char ch = rows[y][x];
            s_map[y][x] = T_EMPTY;
            if (ch == '#')      s_map[y][x] = T_BRICK;
            else if (ch == '@') s_map[y][x] = T_SOLID;
            else if (ch == 'H') s_map[y][x] = T_LADDER;
            else if (ch == '-') s_map[y][x] = T_ROPE;
            else if (ch == '^') s_map[y][x] = T_HLAD;
            else if (ch == '$')
            {
                s_map[y][x] = T_GOLD;
                s_gold_left++;
            }
            else if (ch == 'P')
            {
                s_px = (int16_t)(x * TW);
                s_py = (int16_t)(y * TH);
                s_prev_px = -1;
                s_facing = 1;
            }
            else if ((ch == 'G') && (s_guard_count < MAX_GUARDS))
            {
                Guard_t *g = &s_guards[s_guard_count++];
                g->px = (int16_t)(x * TW);
                g->py = (int16_t)(y * TH);
                g->prev_px = -1;
                g->facing = -1;
                g->anim = 0;
                g->in_pit = false;
                g->active = true;
            }
        }
    }

    draw_full_map();
    render_actors();
}

static void new_game(void)
{
    s_score = 0;
    s_lives = 3;
    s_level = 0;
    load_level();
    s_state = ST_PLAY;
}

/* ---------------------------------------------------------------- physics & digging */

static uint8_t tile_at(int8_t tx, int8_t ty)
{
    if ((tx < 0) || (tx >= MAP_W) || (ty < 0) || (ty >= MAP_H)) return T_SOLID;
    return s_map[ty][tx];
}

static bool is_solid(int8_t tx, int8_t ty)
{
    uint8_t t = tile_at(tx, ty);
    return (t == T_BRICK) || (t == T_SOLID);
}

static bool guard_standing_at(int8_t tx, int8_t ty)
{
    for (uint8_t i = 0; i < s_guard_count; i++)
    {
        if (!s_guards[i].active) continue;
        int8_t gx = (int8_t)((s_guards[i].px + TW / 2) / TW);
        int8_t gy = (int8_t)((s_guards[i].py + TH / 2) / TH);
        if ((gx == tx) && (gy == ty)) return true;
    }
    return false;
}

static bool has_support(int16_t px, int16_t py)
{
    int8_t cx = (int8_t)((px + TW / 2) / TW);
    int8_t cy = (int8_t)(py / TH);

    /* On ladder anywhere */
    if (tile_at(cx, cy) == T_LADDER) return true;

    /* Aligned vertically with floor / rope / ladder top */
    if ((py % TH) == 0)
    {
        if (tile_at(cx, cy) == T_ROPE) return true;
        uint8_t below = tile_at(cx, (int8_t)(cy + 1));
        if ((below == T_BRICK) || (below == T_SOLID) || (below == T_LADDER)) return true;
        if ((below == T_HOLE) && guard_standing_at(cx, (int8_t)(cy + 1))) return true;
    }
    return false;
}

static void try_dig(int8_t dir, uint32_t now)
{
    int8_t cx = (int8_t)((s_px + TW / 2) / TW);
    int8_t cy = (int8_t)((s_py + TH / 2) / TH);
    int8_t tx = (int8_t)(cx + dir);
    int8_t ty = (int8_t)(cy + 1);

    if ((tx < 0) || (tx >= MAP_W) || (ty >= MAP_H)) return;

    /* Must have empty space above the brick to dig it, and brick must be T_BRICK */
    uint8_t above = tile_at(tx, cy);
    if ((above != T_EMPTY) && (above != T_HLAD)) return;
    if (tile_at(tx, ty) != T_BRICK) return;

    for (uint8_t i = 0; i < MAX_HOLES; i++)
    {
        if (!s_holes[i].active)
        {
            s_holes[i].active     = true;
            s_holes[i].tx         = (uint8_t)tx;
            s_holes[i].ty         = (uint8_t)ty;
            s_holes[i].close_tick = now + HOLE_OPEN_MS;
            s_holes[i].last_vis   = 0;

            s_map[ty][tx] = T_HOLE;
            /* Snap player neatly to tile center when digging */
            s_px = (int16_t)(cx * TW);
            draw_tile(tx, ty);

            /* Brief laser beam flash */
            sys_lcd_fill_rect((uint16_t)(MAP_X + tx * TW + 3), (uint16_t)(MAP_Y + ty * TH),
                              5, TH, COLOR_AMBER);
            sound(s_snd_dig, 3);
            return;
        }
    }
}

static void update_holes(uint32_t now)
{
    for (uint8_t i = 0; i < MAX_HOLES; i++)
    {
        Hole_t *h = &s_holes[i];
        if (!h->active) continue;

        int32_t rem = (int32_t)(h->close_tick - now);
        if (rem <= 0)
        {
            h->active = false;
            s_map[h->ty][h->tx] = T_BRICK;
            draw_tile((int8_t)h->tx, (int8_t)h->ty);

            /* Check if player is crushed inside regenerated brick */
            int8_t px = (int8_t)((s_px + TW / 2) / TW);
            int8_t py = (int8_t)((s_py + TH / 2) / TH);
            if ((px == (int8_t)h->tx) && (py == (int8_t)h->ty))
            {
                s_state = ST_DYING;
                s_state_tick = now;
                sound(s_snd_die, 5);
            }

            /* Check if any guard is crushed -> respawn guard at top of screen! */
            for (uint8_t k = 0; k < s_guard_count; k++)
            {
                Guard_t *g = &s_guards[k];
                int8_t gx = (int8_t)((g->px + TW / 2) / TW);
                int8_t gy = (int8_t)((g->py + TH / 2) / TH);
                if ((gx == (int8_t)h->tx) && (gy == (int8_t)h->ty))
                {
                    restore_under(g->prev_px, g->prev_py);
                    g->px = (int16_t)(((h->tx + 5U) % (MAP_W - 2U) + 1U) * TW);
                    g->py = TH;
                    g->prev_px = -1;
                    g->in_pit = false;
                    add_score(75U);
                }
            }
        }
        else if (rem <= (int32_t)HOLE_WARN_MS)
        {
            uint8_t vis = (uint8_t)(((uint32_t)rem / 150U) & 1U);
            if (vis != h->last_vis)
            {
                h->last_vis = vis;
                if (vis != 0U)
                {
                    sys_lcd_fill_rect((uint16_t)(MAP_X + h->tx * TW + 2),
                                      (uint16_t)(MAP_Y + h->ty * TH + 4),
                                      TW - 4U, 4, COLOR_BRICK);
                }
                else
                {
                    draw_tile((int8_t)h->tx, (int8_t)h->ty);
                }
            }
        }
    }
}

static void reveal_escape_ladder(void)
{
    if (s_exit_open) return;
    s_exit_open = true;
    for (int8_t y = 0; y < MAP_H; y++)
    {
        for (int8_t x = 0; x < MAP_W; x++)
        {
            if (s_map[y][x] == T_HLAD)
            {
                s_map[y][x] = T_LADDER;
                draw_tile(x, y);
            }
        }
    }
    sound(s_snd_open, 5);
}

static void step_player(const SDK_Input_t *in, uint32_t now)
{
    /* 1. Gravity when unsupported */
    if (!has_support(s_px, s_py))
    {
        int8_t cx = (int8_t)((s_px + TW / 2) / TW);
        s_px = (int16_t)(cx * TW);
        s_py++;
        s_anim++;
    }
    else
    {
        /* 2. Horizontal movement */
        int8_t dx = 0;
        if (in->held & KEY_LEFT)  { dx = -1; s_facing = -1; }
        if (in->held & KEY_RIGHT) { dx =  1; s_facing =  1; }

        if (dx != 0)
        {
            /* Snap Y to tile grid when turning horizontally from a ladder */
            if ((s_py % TH) != 0)
            {
                int8_t near_y = (int8_t)((s_py + TH / 2) / TH);
                if (iabs16((int16_t)(s_py - near_y * TH)) <= 4)
                {
                    s_py = (int16_t)(near_y * TH);
                }
            }

            if ((s_py % TH) == 0)
            {
                int16_t nx = (int16_t)(s_px + dx);
                int8_t check_tx = (dx < 0) ? (int8_t)(nx / TW) : (int8_t)((nx + TW - 1) / TW);
                int8_t cy = (int8_t)(s_py / TH);
                if ((nx >= 0) && (nx <= (MAP_W - 1) * TW) && !is_solid(check_tx, cy))
                {
                    s_px = nx;
                    s_anim++;
                }
            }
        }

        /* 3. Vertical movement (Ladders & Dropping from Rope) */
        int8_t cx = (int8_t)((s_px + TW / 2) / TW);
        int8_t cy = (int8_t)(s_py / TH);

        if (in->held & KEY_UP)
        {
            if ((tile_at(cx, cy) == T_LADDER) || (((s_py % TH) != 0) && (tile_at(cx, (int8_t)(cy + 1)) == T_LADDER)))
            {
                int16_t ny = (int16_t)(s_py - 1);
                if ((ny >= 0) && !is_solid(cx, (int8_t)(ny / TH)))
                {
                    s_px = (int16_t)(cx * TW);
                    s_py = ny;
                    s_anim++;
                }
            }
        }
        else if (in->held & KEY_DOWN)
        {
            uint8_t below = tile_at(cx, (int8_t)((s_py + TH) / TH));
            /* Drop off rope or climb down ladder */
            if ((below != T_BRICK) && (below != T_SOLID))
            {
                s_px = (int16_t)(cx * TW);
                s_py++;
                s_anim++;
            }
        }

        /* 4. Digging bricks: B = Left, X = Right, A = Current facing direction */
        if (in->pressed & KEY_B) try_dig(-1, now);
        if (in->pressed & KEY_X) try_dig(1, now);
        if (in->pressed & KEY_A) try_dig(s_facing, now);
    }

    /* 5. Collect Gold */
    int8_t cx = (int8_t)((s_px + TW / 2) / TW);
    int8_t cy = (int8_t)((s_py + TH / 2) / TH);
    if (tile_at(cx, cy) == T_GOLD)
    {
        s_map[cy][cx] = T_EMPTY;
        draw_tile(cx, cy);
        s_gold_left--;
        add_score(250U);
        sound(s_snd_gold, 3);
        if (s_gold_left == 0U)
        {
            reveal_escape_ladder();
        }
    }
}

static void step_guards(uint32_t now, uint32_t frame)
{
    /* Guards move on every 2nd frame (~50% speed of player) for fair classic gameplay */
    if ((frame & 1U) == 0U) return;

    for (uint8_t i = 0; i < s_guard_count; i++)
    {
        Guard_t *g = &s_guards[i];
        if (!g->active) continue;

        int8_t gx = (int8_t)((g->px + TW / 2) / TW);
        int8_t gy = (int8_t)(g->py / TH);

        /* Handle Guard trapped in dug pit */
        if (g->in_pit)
        {
            if ((int32_t)(now - g->release_tick) >= 0)
            {
                /* Climb out of pit */
                g->py--;
                if (g->py <= (gy * TH))
                {
                    g->py = (int16_t)(gy * TH);
                    g->in_pit = false;
                }
            }
            continue;
        }

        /* Gravity for Guard */
        if (!has_support(g->px, g->py))
        {
            g->px = (int16_t)(gx * TW);
            g->py++;
            if (((g->py % TH) == 0) && (tile_at(gx, (int8_t)(g->py / TH)) == T_HOLE))
            {
                g->in_pit = true;
                g->release_tick = now + TRAP_TIME_MS;
                add_score(75U);
                sound(s_snd_trap, 1);
            }
            continue;
        }

        /* AI Chase Logic: try vertical first if on ladder and player is on different floor */
        bool moved = false;
        if ((iabs16((int16_t)(s_py - g->py)) >= TH / 2) &&
            ((tile_at(gx, gy) == T_LADDER) || (tile_at(gx, (int8_t)(gy + 1)) == T_LADDER)))
        {
            int8_t dy = (s_py < g->py) ? -1 : 1;
            int16_t ny = (int16_t)(g->py + dy);
            int8_t check_ty = (dy < 0) ? (int8_t)(ny / TH) : (int8_t)((ny + TH - 1) / TH);
            if ((ny >= 0) && !is_solid(gx, check_ty) &&
                ((dy < 0 && tile_at(gx, gy) == T_LADDER) || (dy > 0 && tile_at(gx, check_ty) == T_LADDER)))
            {
                g->px = (int16_t)(gx * TW);
                g->py = ny;
                g->anim++;
                moved = true;
            }
        }

        /* Horizontal chase towards player */
        if (!moved)
        {
            g->py = (int16_t)(((g->py + TH / 2) / TH) * TH);
            gy = (int8_t)(g->py / TH);

            int8_t dx = (s_px < g->px) ? -1 : ((s_px > g->px) ? 1 : g->facing);
            int16_t nx = (int16_t)(g->px + dx);
            int8_t check_tx = (dx < 0) ? (int8_t)(nx / TW) : (int8_t)((nx + TW - 1) / TW);

            if ((nx >= 0) && (nx <= (MAP_W - 1) * TW) && !is_solid(check_tx, gy))
            {
                g->px = nx;
                g->facing = dx;
                g->anim++;
            }
            else
            {
                g->facing = (int8_t)-dx;
            }
        }

        /* Check if Guard catches Player */
        if ((iabs16((int16_t)(g->px - s_px)) <= 6) &&
            (iabs16((int16_t)(g->py - s_py)) <= 7))
        {
            s_state = ST_DYING;
            s_state_tick = now;
            sound(s_snd_die, 5);
            return;
        }
    }
}

static void show_banner(const char *msg, uint16_t color)
{
    uint16_t len = 0;
    while (msg[len] != '\0') len++;
    uint16_t w = (uint16_t)(len * 6U + 12U);
    uint16_t x = (uint16_t)((SDK_SCREEN_W - w) / 2U);
    sys_lcd_fill_rect(x, 114, w, 16, COLOR_PANEL_BG);
    sys_draw_text((uint16_t)(x + 6U), 118, msg, color, COLOR_PANEL_BG, 1);
}

void app_main(void)
{
    SDK_Input_t in;
    uint32_t next_frame = 0;
    uint32_t frame = 0;

    build_sprites();
    load_best();
    s_lives = 3;
    s_level = 0;
    load_level();
    show_banner("A: START  B/X: DIG L/R", COLOR_AMBER);
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
                    new_game();
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

                update_holes(now);
                if (s_state != ST_PLAY) break;

                step_player(&in, now);
                if (s_state != ST_PLAY) break;

                /* Check Level Escape (Player climbed to top row with all gold) */
                if (s_exit_open && (s_py <= 2))
                {
                    s_level++;
                    add_score(1000U);
                    sound(s_snd_win, 7);
                    load_level();
                    break;
                }

                step_guards(now, frame);
                if (s_state == ST_PLAY)
                {
                    render_actors();
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
                        load_level();
                        s_state = ST_PLAY;
                    }
                }
                break;

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    s_state = ST_PLAY;
                    draw_full_map();
                    render_actors();
                }
                break;

            case ST_OVER:
                if ((in.pressed & (KEY_A | KEY_UP)) && ((now - s_state_tick) > 600U))
                {
                    new_game();
                }
                break;

            default:
                break;
        }

        sdk_frame_sync(&next_frame, 18U);
    }
}
