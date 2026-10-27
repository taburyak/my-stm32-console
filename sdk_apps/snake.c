#include "console_sdk.h"

/* =========================================================================
 * Snake: 20x14 grid of 16 px cells under a 16 px HUD.
 * Modes: WALLS (edges kill) and WRAP (edges connect).
 * Controls: arrows / D-pad turn, A start, START (pad L2/R2, key P) pause, ESC exit.
 * Best scores: SD:/appdata/snake/best.dat
 * ========================================================================= */

#define CELL        16U
#define GRID_W      20U
#define GRID_H      14U
#define HUD_H       16U
#define MAX_LEN     (GRID_W * GRID_H)
#define START_LEN   3U

#define COLOR_HEAD  0x07E0U
#define COLOR_BODY  0x03E0U
#define COLOR_FOOD  0xF800U

enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };
enum { MODE_WALLS, MODE_WRAP, MODE_COUNT };
enum { ST_MENU, ST_PLAY, ST_PAUSE, ST_OVER };

static const int8_t s_dx[4] = { 0, 1, 0, -1 };
static const int8_t s_dy[4] = { -1, 0, 1, 0 };
static const char *const s_mode_name[MODE_COUNT] = { "WALLS", "WRAP " };

static const SDK_Note_t s_snd_eat[]   = { { 1, 25 } };
static const SDK_Note_t s_snd_die[]   = { { 1, 90 }, { 0, 60 }, { 1, 90 }, { 0, 60 }, { 1, 300 } };
static const SDK_Note_t s_snd_start[] = { { 1, 40 }, { 0, 40 }, { 1, 40 } };

static uint8_t  s_body_x[MAX_LEN];
static uint8_t  s_body_y[MAX_LEN];
static uint16_t s_head;        /* ring index of the head */
static uint16_t s_len;
static uint8_t  s_occ[GRID_H][GRID_W];

static uint8_t  s_dir;
static uint8_t  s_queue[2];
static uint8_t  s_queue_n;
static uint8_t  s_food_x, s_food_y;

static uint8_t  s_mode = MODE_WALLS;
static uint8_t  s_state = ST_MENU;
static uint32_t s_score;
static uint32_t s_best[MODE_COUNT];
static uint32_t s_best_saved[MODE_COUNT];
static uint32_t s_over_tick;

/* ---------------------------------------------------------------- drawing */

static void draw_cell(uint8_t x, uint8_t y, uint16_t color)
{
    sys_lcd_fill_rect((uint16_t)(x * CELL + 1U), (uint16_t)(HUD_H + y * CELL + 1U), CELL - 2U, CELL - 2U, color);
}

static void draw_num(uint16_t x, uint16_t y, uint32_t v, uint16_t color, uint8_t scale)
{
    char buf[12];
    sdk_uint_to_str(v, buf, 3);
    sys_draw_text(x, y, buf, color, COLOR_DARK_BG, scale);
}

static void draw_hud(void)
{
    sys_draw_text(4, 4, "SCORE", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_num(40, 4, s_score, COLOR_NEON_GREEN, 1);
    sys_draw_text(100, 4, "BEST", COLOR_WHITE, COLOR_DARK_BG, 1);
    draw_num(130, 4, s_best[s_mode], COLOR_AMBER, 1);
    sys_draw_text(190, 4, s_mode_name[s_mode], COLOR_CYAN, COLOR_DARK_BG, 1);
    sys_draw_text(250, 4, (s_state == ST_PAUSE) ? "PAUSED" : "      ", COLOR_YELLOW, COLOR_DARK_BG, 1);
}

static void draw_menu(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_draw_text(100, 40, "SNAKE", COLOR_NEON_GREEN, COLOR_DARK_BG, 4);
    sys_draw_text(70, 110, "MODE:", COLOR_WHITE, COLOR_DARK_BG, 2);
    sys_draw_text(140, 110, "<", COLOR_AMBER, COLOR_DARK_BG, 2);
    sys_draw_text(160, 110, s_mode_name[s_mode], COLOR_CYAN, COLOR_DARK_BG, 2);
    sys_draw_text(226, 110, ">", COLOR_AMBER, COLOR_DARK_BG, 2);
    sys_draw_text(100, 140, "BEST", COLOR_WHITE, COLOR_DARK_BG, 2);
    draw_num(160, 140, s_best[s_mode], COLOR_AMBER, 2);
    sys_draw_text(70, 190, "A: START   ESC: EXIT", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(70, 204, "START / P: PAUSE", COLOR_WHITE, COLOR_DARK_BG, 1);
}

static void draw_game_over(void)
{
    sys_lcd_fill_rect(70, 90, 180, 70, COLOR_PANEL_BG);
    sys_draw_text(88, 100, "GAME OVER", COLOR_RED, COLOR_PANEL_BG, 2);
    sys_draw_text(100, 126, (s_score > s_best_saved[s_mode]) ? "NEW BEST!" : "         ", COLOR_AMBER, COLOR_PANEL_BG, 1);
    sys_draw_text(82, 142, "A: MENU  ESC: EXIT", COLOR_WHITE, COLOR_PANEL_BG, 1);
}

/* ---------------------------------------------------------------- persistence */

static void load_best(void)
{
    int32_t fd = sys_file_open("best.dat", SDK_FILE_READ);
    if (fd >= 0)
    {
        if (sys_file_read(fd, s_best, sizeof(s_best)) != (int32_t)sizeof(s_best))
        {
            sdk_memset(s_best, 0, sizeof(s_best));
        }
        (void)sys_file_close(fd);
    }
    sdk_memcpy(s_best_saved, s_best, sizeof(s_best));
}

static void save_best(void)
{
    bool changed = false;
    for (uint32_t i = 0; i < MODE_COUNT; i++)
    {
        changed |= (s_best[i] != s_best_saved[i]);
    }
    if (!changed)
    {
        return;
    }
    int32_t fd = sys_file_open("best.dat", SDK_FILE_WRITE | SDK_FILE_TRUNC);
    if (fd >= 0)
    {
        if (sys_file_write(fd, s_best, sizeof(s_best)) == (int32_t)sizeof(s_best))
        {
            sdk_memcpy(s_best_saved, s_best, sizeof(s_best));
        }
        (void)sys_file_close(fd);
    }
}

/* ---------------------------------------------------------------- game logic */

static void place_food(void)
{
    uint32_t free_cells = MAX_LEN - s_len;
    if (free_cells == 0U)
    {
        return;
    }
    uint32_t n = sys_random_range(free_cells);
    for (uint8_t y = 0; y < GRID_H; y++)
    {
        for (uint8_t x = 0; x < GRID_W; x++)
        {
            if (s_occ[y][x] == 0U)
            {
                if (n == 0U)
                {
                    s_food_x = x;
                    s_food_y = y;
                    draw_cell(x, y, COLOR_FOOD);
                    return;
                }
                n--;
            }
        }
    }
}

static void start_game(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_lcd_fill_rect(0, HUD_H - 1U, SDK_SCREEN_W, 1, COLOR_GRID_LINE);
    sdk_memset(s_occ, 0, sizeof(s_occ));

    s_len = START_LEN;
    s_head = START_LEN - 1U;
    for (uint16_t i = 0; i < START_LEN; i++)
    {
        s_body_x[i] = (uint8_t)(GRID_W / 2U - (START_LEN - 1U) + i);
        s_body_y[i] = GRID_H / 2U;
        s_occ[s_body_y[i]][s_body_x[i]] = 1U;
        draw_cell(s_body_x[i], s_body_y[i], (i == s_head) ? COLOR_HEAD : COLOR_BODY);
    }
    s_dir = DIR_RIGHT;
    s_queue_n = 0;
    s_score = 0;
    s_state = ST_PLAY;
    place_food();
    draw_hud();
    (void)sys_audio_play(s_snd_start, 3, 0);
}

static void queue_turn(uint8_t dir)
{
    uint8_t last = (s_queue_n > 0U) ? s_queue[s_queue_n - 1U] : s_dir;
    if ((s_queue_n < 2U) && (dir != last) && (dir != ((last + 2U) & 3U)))
    {
        s_queue[s_queue_n++] = dir;
    }
}

static void game_over(void)
{
    s_state = ST_OVER;
    s_over_tick = sys_get_tick();
    if (s_score > s_best[s_mode])
    {
        s_best[s_mode] = s_score;
    }
    draw_game_over();
    (void)sys_audio_play(s_snd_die, sizeof(s_snd_die) / sizeof(s_snd_die[0]), 0);
    save_best();
}

static void step(void)
{
    if (s_queue_n > 0U)
    {
        s_dir = s_queue[0];
        s_queue[0] = s_queue[1];
        s_queue_n--;
    }

    int16_t nx = (int16_t)(s_body_x[s_head] + s_dx[s_dir]);
    int16_t ny = (int16_t)(s_body_y[s_head] + s_dy[s_dir]);
    if (s_mode == MODE_WRAP)
    {
        nx = (int16_t)((nx + (int16_t)GRID_W) % (int16_t)GRID_W);
        ny = (int16_t)((ny + (int16_t)GRID_H) % (int16_t)GRID_H);
    }
    else if ((nx < 0) || (ny < 0) || (nx >= (int16_t)GRID_W) || (ny >= (int16_t)GRID_H))
    {
        game_over();
        return;
    }

    bool eat = ((uint8_t)nx == s_food_x) && ((uint8_t)ny == s_food_y);
    uint16_t tail = (uint16_t)((s_head + MAX_LEN - (s_len - 1U)) % MAX_LEN);

    if (!eat)
    {
        /* The tail moves away in the same step, so the head may enter its cell */
        s_occ[s_body_y[tail]][s_body_x[tail]] = 0U;
    }
    if (s_occ[ny][nx] != 0U)
    {
        game_over();
        return;
    }
    if (!eat)
    {
        draw_cell(s_body_x[tail], s_body_y[tail], COLOR_DARK_BG);
    }

    draw_cell(s_body_x[s_head], s_body_y[s_head], COLOR_BODY);
    s_head = (uint16_t)((s_head + 1U) % MAX_LEN);
    s_body_x[s_head] = (uint8_t)nx;
    s_body_y[s_head] = (uint8_t)ny;
    s_occ[ny][nx] = 1U;
    draw_cell((uint8_t)nx, (uint8_t)ny, COLOR_HEAD);

    if (eat)
    {
        s_len++;
        s_score += 10U;
        (void)sys_audio_play(s_snd_eat, 1, 0);
        draw_hud();
        if (s_len == MAX_LEN)
        {
            game_over();
            return;
        }
        place_food();
    }
}

static uint32_t step_interval(void)
{
    uint32_t speedup = (uint32_t)(s_len - START_LEN) * 3U;
    return (speedup >= 100U) ? 60U : (160U - speedup);
}

/* ---------------------------------------------------------------- main loop */

void app_main(void)
{
    SDK_Input_t in;
    uint32_t next_frame = 0;
    uint32_t next_step = 0;

    load_best();
    draw_menu();
    sdk_input_init(&in);

    for (;;)
    {
        uint32_t now = sys_get_tick();
        sdk_input_update(&in, sys_get_keys(), now);

        if (in.pressed & KEY_EXIT)
        {
            return;
        }

        switch (s_state)
        {
            case ST_MENU:
                if (in.pressed & (KEY_LEFT | KEY_RIGHT))
                {
                    s_mode = (uint8_t)((s_mode + 1U) % MODE_COUNT);
                    draw_menu();
                }
                if (in.pressed & (KEY_A | KEY_UP))
                {
                    start_game();
                    next_step = now + step_interval();
                }
                break;

            case ST_PLAY:
                if (in.pressed & KEY_UP)    queue_turn(DIR_UP);
                if (in.pressed & KEY_DOWN)  queue_turn(DIR_DOWN);
                if (in.pressed & KEY_LEFT)  queue_turn(DIR_LEFT);
                if (in.pressed & KEY_RIGHT) queue_turn(DIR_RIGHT);
                if (in.pressed & KEY_START)
                {
                    s_state = ST_PAUSE;
                    draw_hud();
                    break;
                }
                if ((int32_t)(now - next_step) >= 0)
                {
                    step();
                    next_step = now + step_interval();
                }
                break;

            case ST_PAUSE:
                if (in.pressed & (KEY_START | KEY_A))
                {
                    s_state = ST_PLAY;
                    draw_hud();
                    next_step = now + step_interval();
                }
                break;

            case ST_OVER:
                if ((in.pressed & KEY_A) && ((now - s_over_tick) > 500U))
                {
                    s_state = ST_MENU;
                    draw_menu();
                }
                break;

            default:
                break;
        }

        sdk_frame_sync(&next_frame, 10U);
    }
}
