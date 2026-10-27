#include "console_sdk.h"

/* =========================================================================
 * Space Invaders: 11x5 marching fleet, destructible bunkers, mystery UFO,
 * march tempo grows as the fleet shrinks, 3 lives, waves start lower.
 * Controls: LEFT/RIGHT move, A / UP / B fire, START (pad L2/R2, key P) pause, ESC exit.
 * Best score: SD:/appdata/invaders/best.dat
 * ========================================================================= */

#define HUD_H           16
#define FLEET_COLS      11
#define FLEET_ROWS      5
#define PITCH_X         18
#define PITCH_Y         14
#define SPR_PAD         2
#define SPR_W           (12 + 2 * SPR_PAD)
#define SPR_H           8
#define FLEET_STEP      2
#define FLEET_DROP      8

#define PLAYER_Y        214
#define PLAYER_SPEED    2
#define GROUND_Y        228
#define BULLET_SPEED    5
#define BOMB_SPEED      2
#define MAX_BOMBS       3

#define BUNKERS         4
#define BUNKER_W        22
#define BUNKER_H        16
#define BUNKER_Y        176

#define UFO_Y           20
#define UFO_W           (16 + 2 * SPR_PAD)
#define UFO_H           7

#define COLOR_SQUID     COLOR_MAGENTA
#define COLOR_CRAB      COLOR_CYAN
#define COLOR_OCTO      COLOR_YELLOW
#define COLOR_PLAYER    COLOR_NEON_GREEN
#define COLOR_BUNKER    0x07E0U
#define BG              COLOR_BLACK

enum { ST_READY, ST_PLAY, ST_HIT, ST_PAUSE, ST_OVER };

/* ---------------------------------------------------------------- sprites */

static const char *const s_squid[2][SPR_H] = {
    { "....XXXX....", "...XXXXXX...", "..XXXXXXXX..", "..XX.XX.XX..",
      "..XXXXXXXX..", "....X..X....", "...X.XX.X...", "..X.X..X.X.." },
    { "....XXXX....", "...XXXXXX...", "..XXXXXXXX..", "..XX.XX.XX..",
      "..XXXXXXXX..", "...X.XX.X...", "..X......X..", "...X....X..." },
};
static const char *const s_crab[2][SPR_H] = {
    { "..X.....X...", "...X...X....", "..XXXXXXX...", ".XX.XXX.XX..",
      "XXXXXXXXXXX.", "X.XXXXXXX.X.", "X.X.....X.X.", "...XX.XX...." },
    { "..X.....X...", "X..X...X..X.", "X.XXXXXXX.X.", "XXX.XXX.XXX.",
      "XXXXXXXXXXX.", ".XXXXXXXXX..", "..X.....X...", ".X.......X.." },
};
static const char *const s_octo[2][SPR_H] = {
    { "....XXXX....", ".XXXXXXXXXX.", "XXXXXXXXXXXX", "XXX..XX..XXX",
      "XXXXXXXXXXXX", "...XX..XX...", "..XX.XX.XX..", "XX........XX" },
    { "....XXXX....", ".XXXXXXXXXX.", "XXXXXXXXXXXX", "XXX..XX..XXX",
      "XXXXXXXXXXXX", "..XXX..XXX..", ".XX..XX..XX.", "..XX....XX.." },
};
static const char *const s_player_rows[SPR_H] = {
    ".....XX.....", "....XXXX....", "....XXXX....", ".XXXXXXXXXX.",
    "XXXXXXXXXXXX", "XXXXXXXXXXXX", "XXXXXXXXXXXX", "XXXXXXXXXXXX",
};
static const char *const s_ufo_rows[UFO_H] = {
    ".....XXXXXX.....", "...XXXXXXXXXX...", "..XXXXXXXXXXXX..", ".XX.XX.XX.XX.XX.",
    "XXXXXXXXXXXXXXXX", "..XXX..XX..XXX..", "...X........X...",
};

static uint16_t s_spr_alien[3][2][SPR_W * SPR_H];
static uint16_t s_spr_player[SPR_W * SPR_H];
static uint16_t s_spr_player_hit[SPR_W * SPR_H];
static uint16_t s_spr_ufo[UFO_W * UFO_H];

static const uint8_t  s_row_type[FLEET_ROWS]  = { 0, 1, 1, 2, 2 };
static const uint8_t  s_type_points[3]        = { 30, 20, 10 };
static const uint16_t s_ufo_points[4]         = { 50, 100, 150, 300 };

static const SDK_Note_t s_snd_shot[]   = { { 1, 8 } };
static const SDK_Note_t s_snd_kill[]   = { { 1, 20 }, { 0, 10 }, { 1, 10 } };
static const SDK_Note_t s_snd_hit[]    = { { 1, 300 }, { 0, 100 }, { 1, 300 } };
static const SDK_Note_t s_snd_ufo[]    = { { 1, 40 }, { 0, 30 }, { 1, 40 }, { 0, 30 }, { 1, 40 } };
static const SDK_Note_t s_snd_march[4][1] = { { { 1, 30 } }, { { 1, 22 } }, { { 1, 30 } }, { { 1, 16 } } };
static const SDK_Note_t s_snd_over[]   = { { 1, 150 }, { 0, 80 }, { 1, 150 }, { 0, 80 }, { 1, 500 } };

/* ---------------------------------------------------------------- state */

typedef struct {
    bool    on;
    int16_t x, y;
} Shot_t;

static uint8_t  s_alive[FLEET_ROWS][FLEET_COLS];
static uint16_t s_alive_n;
static int16_t  s_fx, s_fy;
static int8_t   s_fdir;
static uint8_t  s_frame;
static uint8_t  s_march;
static uint32_t s_next_step, s_next_bomb;

static int16_t  s_px;
static Shot_t   s_shot;
static Shot_t   s_bombs[MAX_BOMBS];

static uint8_t  s_bunker[BUNKERS][BUNKER_H][BUNKER_W];

static bool     s_ufo_on;
static int16_t  s_ufo_x;
static int8_t   s_ufo_dir;
static uint32_t s_next_ufo;

static uint8_t  s_state = ST_READY;
static uint32_t s_state_tick;
static uint32_t s_score, s_best, s_best_saved;
static uint8_t  s_lives;
static uint8_t  s_wave;

static void sound(const SDK_Note_t *notes, uint32_t n)
{
    (void)sys_audio_play(notes, n, 0);
}

static void make_sprite(const char *const *rows, uint8_t w, uint8_t h, uint16_t color, uint16_t *out)
{
    uint8_t ow = (uint8_t)(w + 2 * SPR_PAD);
    for (uint8_t y = 0; y < h; y++)
    {
        for (uint8_t x = 0; x < ow; x++)
        {
            bool on = (x >= SPR_PAD) && (x < SPR_PAD + w) && (rows[y][x - SPR_PAD] == 'X');
            out[y * ow + x] = on ? color : BG;
        }
    }
}

static void build_sprites(void)
{
    static const uint16_t colors[3] = { COLOR_SQUID, COLOR_CRAB, COLOR_OCTO };
    const char *const (*src[3])[SPR_H] = { s_squid, s_crab, s_octo };
    for (uint8_t t = 0; t < 3U; t++)
    {
        for (uint8_t f = 0; f < 2U; f++)
        {
            make_sprite(src[t][f], 12, SPR_H, colors[t], s_spr_alien[t][f]);
        }
    }
    make_sprite(s_player_rows, 12, SPR_H, COLOR_PLAYER, s_spr_player);
    make_sprite(s_player_rows, 12, SPR_H, COLOR_RED, s_spr_player_hit);
    make_sprite(s_ufo_rows, 16, UFO_H, COLOR_RED, s_spr_ufo);
}

/* ---------------------------------------------------------------- HUD */

static void draw_num(uint16_t x, uint32_t v, uint8_t digits, uint16_t color)
{
    char buf[12];
    sdk_uint_to_str(v, buf, digits);
    sys_draw_text(x, 4, buf, color, BG, 1);
}

static void draw_hud(void)
{
    sys_draw_text(4, 4, "SCORE", COLOR_WHITE, BG, 1);
    draw_num(40, s_score, 5, COLOR_NEON_GREEN);
    sys_draw_text(92, 4, "HI", COLOR_WHITE, BG, 1);
    draw_num(110, s_best, 5, COLOR_AMBER);
    sys_draw_text(162, 4, "WAVE", COLOR_WHITE, BG, 1);
    draw_num(192, (uint32_t)s_wave + 1U, 2, COLOR_CYAN);
    sys_draw_text(216, 4, "LIVES", COLOR_WHITE, BG, 1);
    draw_num(252, s_lives, 1, COLOR_RED);
    sys_draw_text(270, 4, (s_state == ST_PAUSE) ? "PAUSE" : "     ", COLOR_YELLOW, BG, 1);
}

static void add_score(uint32_t points)
{
    s_score += points;
    if (s_score > s_best) s_best = s_score;
    draw_hud();
}

static void center_text(uint16_t y, const char *s, uint16_t color, uint8_t scale)
{
    uint16_t n = 0;
    while (s[n] != '\0') n++;
    sys_draw_text((uint16_t)((SDK_SCREEN_W - n * 6U * scale) / 2U), y, s, color, BG, scale);
}

/* ---------------------------------------------------------------- bunkers */

static int16_t bunker_x(uint8_t i)
{
    return (int16_t)(32 + i * 76);
}

static void draw_bunker_rows(uint8_t i, uint8_t r0, uint8_t r1, uint8_t c0, uint8_t c1)
{
    for (uint8_t r = r0; r < r1; r++)
    {
        uint8_t c = c0;
        while (c < c1)
        {
            uint8_t v = s_bunker[i][r][c];
            uint8_t start = c;
            while ((c < c1) && (s_bunker[i][r][c] == v)) c++;
            sys_lcd_fill_rect((uint16_t)(bunker_x(i) + start), (uint16_t)(BUNKER_Y + r), (uint16_t)(c - start), 1,
                              v ? COLOR_BUNKER : BG);
        }
    }
}

static void reset_bunkers(void)
{
    for (uint8_t i = 0; i < BUNKERS; i++)
    {
        for (uint8_t r = 0; r < BUNKER_H; r++)
        {
            for (uint8_t c = 0; c < BUNKER_W; c++)
            {
                bool bevel = (r < 4U) && ((c < 4U - r) || (c >= BUNKER_W - (4U - r)));
                bool arch  = (r >= 11U) && (c >= 6U) && (c < 16U);
                s_bunker[i][r][c] = (bevel || arch) ? 0U : 1U;
            }
        }
        draw_bunker_rows(i, 0, BUNKER_H, 0, BUNKER_W);
    }
}

/* Repaints bunker pixels inside a rectangle that was just cleared */
static void redraw_bunkers_in(int16_t x, int16_t y, int16_t w, int16_t h)
{
    if ((y + h <= BUNKER_Y) || (y >= BUNKER_Y + BUNKER_H)) return;
    int16_t r0 = (int16_t)((y > BUNKER_Y) ? (y - BUNKER_Y) : 0);
    int16_t r1 = (int16_t)(((y + h) < (BUNKER_Y + BUNKER_H)) ? (y + h - BUNKER_Y) : BUNKER_H);
    for (uint8_t i = 0; i < BUNKERS; i++)
    {
        int16_t bx = bunker_x(i);
        if ((x + w <= bx) || (x >= bx + BUNKER_W)) continue;
        int16_t c0 = (int16_t)((x > bx) ? (x - bx) : 0);
        int16_t c1 = (int16_t)(((x + w) < (bx + BUNKER_W)) ? (x + w - bx) : BUNKER_W);
        draw_bunker_rows(i, (uint8_t)r0, (uint8_t)r1, (uint8_t)c0, (uint8_t)c1);
    }
}

/* Returns the bunker index hit at pixel (x, y), or -1 */
static int8_t bunker_at(int16_t x, int16_t y)
{
    if ((y < BUNKER_Y) || (y >= BUNKER_Y + BUNKER_H)) return -1;
    for (uint8_t i = 0; i < BUNKERS; i++)
    {
        int16_t bx = bunker_x(i);
        if ((x >= bx) && (x < bx + BUNKER_W) && s_bunker[i][y - BUNKER_Y][x - bx])
        {
            return (int8_t)i;
        }
    }
    return -1;
}

static void erode(uint8_t i, int16_t x, int16_t y)
{
    int16_t bx = bunker_x(i);
    for (int16_t dy = -2; dy <= 2; dy++)
    {
        for (int16_t dx = -1; dx <= 1; dx++)
        {
            int16_t r = (int16_t)(y - BUNKER_Y + dy);
            int16_t c = (int16_t)(x - bx + dx);
            bool edge = ((dy == -2) || (dy == 2)) && (dx != 0) && ((sys_random() & 1U) != 0U);
            if ((r >= 0) && (r < BUNKER_H) && (c >= 0) && (c < BUNKER_W) && !edge)
            {
                s_bunker[i][r][c] = 0U;
            }
        }
    }
    redraw_bunkers_in((int16_t)(x - 1), (int16_t)(y - 2), 3, 5);
}

/* ---------------------------------------------------------------- fleet */

static void fleet_bounds(int8_t *minc, int8_t *maxc, int8_t *maxr)
{
    *minc = FLEET_COLS;
    *maxc = -1;
    *maxr = -1;
    for (int8_t r = 0; r < FLEET_ROWS; r++)
    {
        for (int8_t c = 0; c < FLEET_COLS; c++)
        {
            if (!s_alive[r][c]) continue;
            if (c < *minc) *minc = c;
            if (c > *maxc) *maxc = c;
            if (r > *maxr) *maxr = r;
        }
    }
}

static void draw_fleet(void)
{
    for (uint8_t r = 0; r < FLEET_ROWS; r++)
    {
        const uint16_t *spr = s_spr_alien[s_row_type[r]][s_frame];
        for (uint8_t c = 0; c < FLEET_COLS; c++)
        {
            if (s_alive[r][c])
            {
                sys_draw_sprite((uint16_t)(s_fx + c * PITCH_X), (uint16_t)(s_fy + r * PITCH_Y), SPR_W, SPR_H, spr);
            }
        }
    }
}

static uint32_t step_interval(void)
{
    return 20U + (uint32_t)s_alive_n * 10U;
}

static void game_over(uint32_t now);

static void fleet_step(uint32_t now)
{
    int8_t minc, maxc, maxr;
    fleet_bounds(&minc, &maxc, &maxr);
    if (maxc < 0) return;

    int16_t nx = (int16_t)(s_fx + s_fdir * FLEET_STEP);
    int16_t left  = (int16_t)(nx + minc * PITCH_X);
    int16_t right = (int16_t)(nx + maxc * PITCH_X + SPR_W);
    if ((left < 0) || (right > SDK_SCREEN_W))
    {
        int16_t bx = (int16_t)(s_fx + minc * PITCH_X);
        int16_t bw = (int16_t)((maxc - minc) * PITCH_X + SPR_W);
        int16_t bh = (int16_t)(maxr * PITCH_Y + SPR_H);
        sys_lcd_fill_rect((uint16_t)bx, (uint16_t)s_fy, (uint16_t)bw, (uint16_t)bh, BG);
        redraw_bunkers_in(bx, s_fy, bw, bh);
        s_fy = (int16_t)(s_fy + FLEET_DROP);
        s_fdir = (int8_t)-s_fdir;
    }
    else
    {
        s_fx = nx;
    }

    /* Aliens crush the bunkers they overlap */
    int16_t bottom = (int16_t)(s_fy + maxr * PITCH_Y + SPR_H);
    if (bottom > BUNKER_Y)
    {
        for (uint8_t r = 0; r < FLEET_ROWS; r++)
        {
            for (uint8_t c = 0; c < FLEET_COLS; c++)
            {
                if (!s_alive[r][c]) continue;
                int16_t ax = (int16_t)(s_fx + c * PITCH_X + SPR_PAD);
                int16_t ay = (int16_t)(s_fy + r * PITCH_Y);
                for (int16_t y = ay; y < ay + SPR_H; y++)
                {
                    for (int16_t x = ax; x < ax + 12; x++)
                    {
                        int8_t b = bunker_at(x, y);
                        if (b >= 0) s_bunker[b][y - BUNKER_Y][x - bunker_x((uint8_t)b)] = 0U;
                    }
                }
            }
        }
    }

    s_frame ^= 1U;
    draw_fleet();
    sound(s_snd_march[s_march], 1);
    s_march = (uint8_t)((s_march + 1U) & 3U);
    s_next_step = now + step_interval();

    if (bottom >= PLAYER_Y)
    {
        game_over(now);
    }
}

static void kill_alien(uint8_t r, uint8_t c)
{
    s_alive[r][c] = 0U;
    s_alive_n--;
    int16_t x = (int16_t)(s_fx + c * PITCH_X);
    int16_t y = (int16_t)(s_fy + r * PITCH_Y);
    sys_lcd_fill_rect((uint16_t)x, (uint16_t)y, SPR_W, SPR_H, BG);
    redraw_bunkers_in(x, y, SPR_W, SPR_H);
    add_score(s_type_points[s_row_type[r]]);
    sound(s_snd_kill, 3);
}

/* Returns true and the cell if pixel (x, y) lies on a live alien */
static bool alien_at(int16_t x, int16_t y, uint8_t *row, uint8_t *col)
{
    if ((x < s_fx) || (y < s_fy)) return false;
    int16_t c = (int16_t)((x - s_fx) / PITCH_X);
    int16_t r = (int16_t)((y - s_fy) / PITCH_Y);
    if ((c >= FLEET_COLS) || (r >= FLEET_ROWS) || !s_alive[r][c]) return false;
    int16_t lx = (int16_t)(x - s_fx - c * PITCH_X);
    int16_t ly = (int16_t)(y - s_fy - r * PITCH_Y);
    if ((lx < SPR_PAD) || (lx >= SPR_PAD + 12) || (ly >= SPR_H)) return false;
    *row = (uint8_t)r;
    *col = (uint8_t)c;
    return true;
}

/* ---------------------------------------------------------------- shots */

static void erase_shot(const Shot_t *s, uint8_t h)
{
    sys_lcd_fill_rect((uint16_t)s->x, (uint16_t)s->y, 1, h, BG);
}

static void draw_player(const uint16_t *spr)
{
    sys_draw_sprite((uint16_t)s_px, PLAYER_Y, SPR_W, SPR_H, spr);
}

static void update_shot(void)
{
    if (!s_shot.on) return;
    erase_shot(&s_shot, 4);

    for (uint8_t i = 0; i < BULLET_SPEED; i++)
    {
        s_shot.y--;
        int16_t x = s_shot.x;
        int16_t y = s_shot.y;
        uint8_t r, c;

        if (y <= HUD_H)
        {
            s_shot.on = false;
            return;
        }
        if (s_ufo_on && (x >= s_ufo_x + SPR_PAD) && (x < s_ufo_x + UFO_W - SPR_PAD) && (y >= UFO_Y) && (y < UFO_Y + UFO_H))
        {
            s_ufo_on = false;
            s_next_ufo = sys_get_tick() + 15000U + sys_random_range(10000U);
            sys_lcd_fill_rect((uint16_t)s_ufo_x, UFO_Y, UFO_W, UFO_H, BG);
            add_score(s_ufo_points[sys_random_range(4)]);
            sound(s_snd_ufo, 5);
            s_shot.on = false;
            return;
        }
        if (alien_at(x, y, &r, &c))
        {
            kill_alien(r, c);
            s_shot.on = false;
            return;
        }
        int8_t b = bunker_at(x, y);
        if (b >= 0)
        {
            erode((uint8_t)b, x, y);
            s_shot.on = false;
            return;
        }
        for (uint8_t k = 0; k < MAX_BOMBS; k++)
        {
            if (s_bombs[k].on && (s_bombs[k].x == x) && (y >= s_bombs[k].y) && (y < s_bombs[k].y + 5))
            {
                erase_shot(&s_bombs[k], 5);
                s_bombs[k].on = false;
                s_shot.on = false;
                return;
            }
        }
    }
    sys_lcd_fill_rect((uint16_t)s_shot.x, (uint16_t)s_shot.y, 1, 4, COLOR_WHITE);
}

static void player_hit(uint32_t now)
{
    s_state = ST_HIT;
    s_state_tick = now;
    draw_player(s_spr_player_hit);
    sound(s_snd_hit, 3);
}

static void update_bombs(uint32_t now)
{
    if (((int32_t)(now - s_next_bomb) >= 0) && (s_alive_n > 0U))
    {
        uint32_t base = (s_wave >= 8U) ? 300U : (1100U - s_wave * 100U);
        s_next_bomb = now + base / 2U + sys_random_range(base);

        uint8_t slot = MAX_BOMBS;
        for (uint8_t k = 0; k < MAX_BOMBS; k++)
        {
            if (!s_bombs[k].on) slot = k;
        }
        uint8_t col = (uint8_t)sys_random_range(FLEET_COLS);
        for (uint8_t tries = 0; (slot < MAX_BOMBS) && (tries < FLEET_COLS); tries++)
        {
            uint8_t c = (uint8_t)((col + tries) % FLEET_COLS);
            for (int8_t r = FLEET_ROWS - 1; r >= 0; r--)
            {
                if (s_alive[r][c])
                {
                    s_bombs[slot].on = true;
                    s_bombs[slot].x = (int16_t)(s_fx + c * PITCH_X + SPR_W / 2);
                    s_bombs[slot].y = (int16_t)(s_fy + r * PITCH_Y + SPR_H + 1);
                    slot = MAX_BOMBS;
                    break;
                }
            }
        }
    }

    for (uint8_t k = 0; k < MAX_BOMBS; k++)
    {
        Shot_t *b = &s_bombs[k];
        if (!b->on) continue;
        erase_shot(b, 5);
        for (uint8_t i = 0; i < BOMB_SPEED; i++)
        {
            b->y++;
            int16_t tip = (int16_t)(b->y + 4);
            int8_t bi = bunker_at(b->x, tip);
            if (bi >= 0)
            {
                erode((uint8_t)bi, b->x, tip);
                b->on = false;
                break;
            }
            if ((tip >= PLAYER_Y) && (tip < PLAYER_Y + SPR_H) &&
                (b->x >= s_px + SPR_PAD) && (b->x < s_px + SPR_PAD + 12))
            {
                b->on = false;
                player_hit(now);
                return;
            }
            if (tip >= GROUND_Y - 1)
            {
                b->on = false;
                break;
            }
        }
        if (b->on)
        {
            sys_lcd_fill_rect((uint16_t)b->x, (uint16_t)b->y, 1, 5, COLOR_AMBER);
        }
    }
}

static void update_ufo(uint32_t now)
{
    if (!s_ufo_on)
    {
        if (((int32_t)(now - s_next_ufo) >= 0) && (s_alive_n > 8U))
        {
            s_ufo_on = true;
            s_ufo_dir = (sys_random() & 1U) ? 1 : -1;
            s_ufo_x = (s_ufo_dir > 0) ? 0 : (int16_t)(SDK_SCREEN_W - UFO_W);
        }
        return;
    }
    s_ufo_x = (int16_t)(s_ufo_x + s_ufo_dir);
    if ((s_ufo_x < 0) || (s_ufo_x > SDK_SCREEN_W - UFO_W))
    {
        s_ufo_on = false;
        sys_lcd_fill_rect((uint16_t)((s_ufo_x < 0) ? 0 : (SDK_SCREEN_W - UFO_W)), UFO_Y, UFO_W, UFO_H, BG);
        s_next_ufo = now + 15000U + sys_random_range(10000U);
        return;
    }
    sys_draw_sprite((uint16_t)s_ufo_x, UFO_Y, UFO_W, UFO_H, s_spr_ufo);
}

/* ---------------------------------------------------------------- game flow */

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

static void start_wave(uint32_t now)
{
    sys_clear_screen(BG);
    sys_lcd_fill_rect(0, GROUND_Y, SDK_SCREEN_W, 1, COLOR_BUNKER);
    draw_hud();

    sdk_memset(s_alive, 1, sizeof(s_alive));
    s_alive_n = FLEET_ROWS * FLEET_COLS;
    s_fx = 20;
    s_fy = (int16_t)(HUD_H + 24 + ((s_wave < 5U) ? s_wave : 5U) * FLEET_DROP);
    s_fdir = 1;
    s_frame = 0;
    s_shot.on = false;
    for (uint8_t k = 0; k < MAX_BOMBS; k++) s_bombs[k].on = false;
    s_ufo_on = false;
    s_next_ufo = now + 15000U + sys_random_range(10000U);
    s_next_bomb = now + 1500U;
    s_next_step = now;

    reset_bunkers();
    s_px = (SDK_SCREEN_W - SPR_W) / 2;
    draw_player(s_spr_player);
    draw_fleet();
}

static void new_game(uint32_t now)
{
    s_score = 0;
    s_lives = 3;
    s_wave = 0;
    s_state = ST_PLAY;
    start_wave(now);
}

static void game_over(uint32_t now)
{
    s_state = ST_OVER;
    s_state_tick = now;
    save_best();
    sound(s_snd_over, 5);
    sys_lcd_fill_rect(80, 96, 160, 50, COLOR_PANEL_BG);
    sys_draw_text(106, 104, "GAME OVER", COLOR_RED, COLOR_PANEL_BG, 2);
    sys_draw_text(110, 128, "A: PLAY AGAIN", COLOR_WHITE, COLOR_PANEL_BG, 1);
}

static void show_title(void)
{
    sys_clear_screen(BG);
    center_text(40, "SPACE", COLOR_NEON_GREEN, 4);
    center_text(76, "INVADERS", COLOR_NEON_GREEN, 3);
    static const char *const labels[3] = { "= 30", "= 20", "= 10" };
    for (uint8_t t = 0; t < 3U; t++)
    {
        sys_draw_sprite(120, (uint16_t)(120 + t * 16), SPR_W, SPR_H, s_spr_alien[t][0]);
        sys_draw_text(142, (uint16_t)(120 + t * 16), labels[t], COLOR_WHITE, BG, 1);
    }
    sys_draw_sprite(120, 168, UFO_W, UFO_H, s_spr_ufo);
    sys_draw_text(146, 168, "= ???", COLOR_WHITE, BG, 1);
    center_text(196, "A: START   ESC: EXIT", COLOR_AMBER, 1);
    center_text(210, "FIRE A/UP/B  PAUSE L2/P", COLOR_WHITE, 1);
    draw_hud();
}

static void play_frame(const SDK_Input_t *in, uint32_t now)
{
    if (in->pressed & KEY_START)
    {
        s_state = ST_PAUSE;
        draw_hud();
        return;
    }

    int16_t old_px = s_px;
    if (in->held & KEY_LEFT)  s_px = (int16_t)(s_px - PLAYER_SPEED);
    if (in->held & KEY_RIGHT) s_px = (int16_t)(s_px + PLAYER_SPEED);
    if (s_px < 0) s_px = 0;
    if (s_px > SDK_SCREEN_W - SPR_W) s_px = SDK_SCREEN_W - SPR_W;
    if (s_px != old_px) draw_player(s_spr_player);

    if ((in->pressed & (KEY_A | KEY_UP | KEY_B)) && !s_shot.on)
    {
        s_shot.on = true;
        s_shot.x = (int16_t)(s_px + SPR_W / 2 - 1);
        s_shot.y = PLAYER_Y - 4;
        sound(s_snd_shot, 1);
    }

    update_shot();
    if ((int32_t)(now - s_next_step) >= 0)
    {
        fleet_step(now);
        if (s_state != ST_PLAY) return;
    }
    update_bombs(now);
    if (s_state != ST_PLAY) return;
    update_ufo(now);

    if (s_alive_n == 0U)
    {
        s_wave++;
        start_wave(now);
    }
}

void app_main(void)
{
    SDK_Input_t in;
    uint32_t next_frame = 0;

    build_sprites();
    load_best();
    s_lives = 3;
    show_title();
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
                    new_game(now);
                }
                break;

            case ST_PLAY:
                play_frame(&in, now);
                break;

            case ST_HIT:
                if ((now - s_state_tick) >= 1200U)
                {
                    sys_lcd_fill_rect((uint16_t)s_px, PLAYER_Y, SPR_W, SPR_H, BG);
                    for (uint8_t k = 0; k < MAX_BOMBS; k++)
                    {
                        if (s_bombs[k].on) erase_shot(&s_bombs[k], 5);
                        s_bombs[k].on = false;
                    }
                    if (--s_lives == 0U)
                    {
                        draw_hud();
                        game_over(now);
                    }
                    else
                    {
                        s_px = (SDK_SCREEN_W - SPR_W) / 2;
                        draw_player(s_spr_player);
                        draw_hud();
                        s_next_bomb = now + 1500U;
                        s_state = ST_PLAY;
                    }
                }
                break;

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    s_state = ST_PLAY;
                    draw_hud();
                    s_next_step = now;
                }
                break;

            case ST_OVER:
                if ((in.pressed & (KEY_A | KEY_UP)) && ((now - s_state_tick) > 800U))
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
