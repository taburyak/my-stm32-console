#include "console_sdk.h"

/* Grid Dimensions: 10 columns x 20 rows, 10x10 px per cell */
#define GRID_COLS       10
#define GRID_ROWS       20
#define CELL_SIZE       10
#define BOARD_X         60
#define BOARD_Y         28

/* Store game state in External 512 KB SRAM Sandbox (0x68080000) */
typedef struct {
    uint8_t  board[GRID_ROWS][GRID_COLS];
    uint32_t score;
    uint32_t lines;
    uint32_t best_score;
    uint32_t rng_state;
} Tetris_State_t;

#define g_tetris ((Tetris_State_t *)SDK_EXT_RAM_BASE_ADDR)

/* 7 Tetrominoes x 4 Rotations (16-bit 4x4 bitmasks) */
static const uint16_t s_shapes[7][4] = {
    /* I */ { 0x0F00, 0x2222, 0x00F0, 0x4444 },
    /* O */ { 0x6600, 0x6600, 0x6600, 0x6600 },
    /* T */ { 0x0E40, 0x4C40, 0x4E00, 0x4640 },
    /* S */ { 0x06C0, 0x8C40, 0x6C00, 0x4620 },
    /* Z */ { 0x0C60, 0x4C80, 0xC600, 0x2640 },
    /* J */ { 0x0E20, 0x44C0, 0x8E00, 0x6440 },
    /* L */ { 0x0E80, 0xC440, 0x2E00, 0x4460 }
};

static const uint16_t s_colors[8] = {
    COLOR_DARK_BG,    /* 0: Empty */
    COLOR_CYAN,       /* 1: I */
    COLOR_YELLOW,     /* 2: O */
    COLOR_MAGENTA,    /* 3: T */
    COLOR_NEON_GREEN, /* 4: S */
    COLOR_RED,        /* 5: Z */
    COLOR_BLUE,       /* 6: J */
    COLOR_AMBER       /* 7: L */
};

/* Fast pseudo-random number 0..6 without hardware/software division */
static uint8_t next_random_piece(void)
{
    uint32_t x = g_tetris->rng_state;
    if (x == 0U) x = sys_get_tick() ^ 0xA5A55A5AU;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_tetris->rng_state = x;

    uint8_t v = (uint8_t)(x & 7U);
    if (v >= 7U) v -= 7U;
    return v;
}

static void draw_cell(int16_t gx, int16_t gy, uint8_t color_idx)
{
    if (gx < 0 || gx >= GRID_COLS || gy < 0 || gy >= GRID_ROWS) return;
    uint16_t px = (uint16_t)(BOARD_X + gx * CELL_SIZE);
    uint16_t py = (uint16_t)(BOARD_Y + gy * CELL_SIZE);
    uint16_t col = s_colors[color_idx & 7U];

    sys_lcd_fill_rect(px, py, CELL_SIZE - 1U, CELL_SIZE - 1U, col);
}

static void draw_piece(int16_t px, int16_t py, uint8_t type, uint8_t rot, uint8_t color_idx)
{
    uint16_t mask = s_shapes[type][rot & 3U];
    for (int16_t r = 0; r < 4; r++)
    {
        for (int16_t c = 0; c < 4; c++)
        {
            if (mask & (0x8000U >> (r * 4 + c)))
            {
                draw_cell(px + c, py + r, color_idx);
            }
        }
    }
}

static bool check_collision(int16_t px, int16_t py, uint8_t type, uint8_t rot)
{
    uint16_t mask = s_shapes[type][rot & 3U];
    for (int16_t r = 0; r < 4; r++)
    {
        for (int16_t c = 0; c < 4; c++)
        {
            if (mask & (0x8000U >> (r * 4 + c)))
            {
                int16_t gx = px + c;
                int16_t gy = py + r;
                if (gx < 0 || gx >= GRID_COLS || gy >= GRID_ROWS)
                {
                    return true;
                }
                if (gy >= 0 && g_tetris->board[gy][gx] != 0U)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static void redraw_board(void)
{
    for (int16_t r = 0; r < GRID_ROWS; r++)
    {
        for (int16_t c = 0; c < GRID_COLS; c++)
        {
            draw_cell(c, r, g_tetris->board[r][c]);
        }
    }
}

static void draw_next_preview(uint8_t next_type)
{
    sys_lcd_fill_rect(190, 50, 48, 48, COLOR_PANEL_BG);
    uint16_t mask = s_shapes[next_type][0];
    uint16_t col  = s_colors[next_type + 1U];
    for (uint8_t r = 0; r < 4U; r++)
    {
        for (uint8_t c = 0; c < 4U; c++)
        {
            if (mask & (0x8000U >> (r * 4U + c)))
            {
                sys_lcd_fill_rect((uint16_t)(194U + c * 10U),
                                  (uint16_t)(54U + r * 10U),
                                  9U, 9U, col);
            }
        }
    }
}

static void update_sidebar_stats(void)
{
    char buf[16];
    sdk_uint_to_str(g_tetris->score, buf, 5);
    sys_draw_text(190, 126, buf, COLOR_NEON_GREEN, COLOR_PANEL_BG, 1);

    sdk_uint_to_str(g_tetris->lines, buf, 3);
    sys_draw_text(190, 162, buf, COLOR_CYAN, COLOR_PANEL_BG, 1);

    sdk_uint_to_str(g_tetris->best_score, buf, 5);
    sys_draw_text(190, 198, buf, COLOR_AMBER, COLOR_PANEL_BG, 1);
}

static void draw_static_ui(void)
{
    sys_clear_screen(COLOR_DARK_BG);

    /* Top header */
    sys_lcd_fill_rect(0, 0, 320, 20, COLOR_PANEL_BG);
    sys_lcd_fill_rect(0, 20, 320, 2, COLOR_CYAN);
    sys_draw_text(10, 6, "CYBER TETRIS EXT", COLOR_CYAN, COLOR_PANEL_BG, 1);
    sys_draw_text(196, 6, "ESC/B: EXIT", COLOR_WHITE, COLOR_PANEL_BG, 1);

    /* Playfield border (100x200 px inside) */
    sys_lcd_fill_rect(BOARD_X - 2, BOARD_Y - 2, GRID_COLS * CELL_SIZE + 4, 2, COLOR_CYAN);
    sys_lcd_fill_rect(BOARD_X - 2, BOARD_Y + GRID_ROWS * CELL_SIZE, GRID_COLS * CELL_SIZE + 4, 2, COLOR_CYAN);
    sys_lcd_fill_rect(BOARD_X - 2, BOARD_Y, 2, GRID_ROWS * CELL_SIZE, COLOR_CYAN);
    sys_lcd_fill_rect(BOARD_X + GRID_COLS * CELL_SIZE, BOARD_Y, 2, GRID_ROWS * CELL_SIZE, COLOR_CYAN);

    /* Right Sidebar Panel */
    sys_lcd_fill_rect(180, 28, 124, 200, COLOR_PANEL_BG);
    sys_draw_text(190, 36, "NEXT:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sys_draw_text(190, 112, "SCORE:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sys_draw_text(190, 148, "LINES:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sys_draw_text(190, 184, "BEST:", COLOR_WHITE, COLOR_PANEL_BG, 1);
}

void app_main(void)
{
    sdk_memset(g_tetris, 0, sizeof(Tetris_State_t));
    g_tetris->rng_state = sys_get_tick() ^ 0xCAFEBABEUL;

    /* Load saved high score */
    (void)sys_load_state("tetris.sav", &g_tetris->best_score, sizeof(uint32_t));

    draw_static_ui();
    redraw_board();
    update_sidebar_stats();

    /* Wait for release of menu launch button */
    while (sys_get_keys() & (KEY_A | KEY_EXIT))
    {
        sys_delay_ms(10);
    }

    bool     game_started = false;
    uint32_t prev_keys    = 0;

    /* Show Start Prompt inside board */
    sys_draw_text(BOARD_X + 14, 100, "PRESS A", COLOR_AMBER, COLOR_DARK_BG, 1);
    sys_draw_text(BOARD_X + 11, 114, "OR SPACE", COLOR_AMBER, COLOR_DARK_BG, 1);
    sys_draw_text(BOARD_X + 11, 128, "TO START", COLOR_WHITE, COLOR_DARK_BG, 1);

    uint8_t cur_type  = next_random_piece();
    uint8_t next_type = next_random_piece();
    uint8_t cur_rot   = 0;
    int16_t cur_x     = 3;
    int16_t cur_y     = 0;

    draw_next_preview(next_type);

    uint32_t last_fall_tick = sys_get_tick();
    uint32_t last_move_tick = sys_get_tick();

    for (;;)
    {
        uint32_t now          = sys_get_tick();
        uint32_t keys         = sys_get_keys();
        uint32_t just_pressed = keys & ~prev_keys;
        prev_keys             = keys;

        /* Exit on ESC (Keyboard) or 'B' (Gamepad) */
        if (just_pressed & KEY_EXIT)
        {
            if (g_tetris->score >= g_tetris->best_score && g_tetris->best_score > 0U)
            {
                (void)sys_save_state("tetris.sav", &g_tetris->best_score, sizeof(uint32_t));
            }
            sys_exit();
            return;
        }

        /* Waiting for user to press A or SPACE to start */
        if (!game_started)
        {
            if (just_pressed & (KEY_A | KEY_UP))
            {
                sdk_memset(g_tetris->board, 0, sizeof(g_tetris->board));
                g_tetris->score = 0;
                g_tetris->lines = 0;
                redraw_board();
                update_sidebar_stats();

                cur_type  = next_random_piece();
                next_type = next_random_piece();
                cur_rot   = 0;
                cur_x     = 3;
                cur_y     = 0;
                draw_next_preview(next_type);
                draw_piece(cur_x, cur_y, cur_type, cur_rot, cur_type + 1U);

                game_started   = true;
                last_fall_tick = now;
                sys_beep(20);
            }
            sys_delay_ms(16);
            continue;
        }

        /* 1. Handle Horizontal Movement (with 110ms auto-repeat) */
        if ((just_pressed & (KEY_LEFT | KEY_RIGHT)) ||
            ((keys & (KEY_LEFT | KEY_RIGHT)) && (now - last_move_tick) >= 110U))
        {
            int16_t dx = (keys & KEY_LEFT) ? -1 : 1;
            if (!check_collision(cur_x + dx, cur_y, cur_type, cur_rot))
            {
                draw_piece(cur_x, cur_y, cur_type, cur_rot, 0U);
                cur_x += dx;
                draw_piece(cur_x, cur_y, cur_type, cur_rot, cur_type + 1U);
            }
            last_move_tick = now;
        }

        /* 2. Handle Rotation (Up Arrow, Spacebar, Enter, or Gamepad 'A') */
        if (just_pressed & (KEY_A | KEY_UP))
        {
            uint8_t next_rot = (cur_rot + 1U) & 3U;
            if (!check_collision(cur_x, cur_y, cur_type, next_rot))
            {
                draw_piece(cur_x, cur_y, cur_type, cur_rot, 0U);
                cur_rot = next_rot;
                draw_piece(cur_x, cur_y, cur_type, cur_rot, cur_type + 1U);
                sys_beep(8);
            }
        }

        /* 3. Handle Hard Drop (Gamepad 'X' or Keyboard 'X'/'Z') */
        if (just_pressed & KEY_B)
        {
            draw_piece(cur_x, cur_y, cur_type, cur_rot, 0U);
            while (!check_collision(cur_x, cur_y + 1, cur_type, cur_rot))
            {
                cur_y++;
            }
            draw_piece(cur_x, cur_y, cur_type, cur_rot, cur_type + 1U);
            last_fall_tick = now - 1000U; /* Trigger immediate lock */
        }

        /* 4. Gravity & Soft Drop (Down Arrow / D-Pad Down) */
        uint32_t fall_interval = (keys & KEY_DOWN) ? 55U : 450U;
        if ((now - last_fall_tick) >= fall_interval)
        {
            last_fall_tick = now;

            if (!check_collision(cur_x, cur_y + 1, cur_type, cur_rot))
            {
                draw_piece(cur_x, cur_y, cur_type, cur_rot, 0U);
                cur_y++;
                draw_piece(cur_x, cur_y, cur_type, cur_rot, cur_type + 1U);
            }
            else
            {
                /* Lock piece into board */
                uint16_t mask = s_shapes[cur_type][cur_rot & 3U];
                for (int16_t r = 0; r < 4; r++)
                {
                    for (int16_t c = 0; c < 4; c++)
                    {
                        if (mask & (0x8000U >> (r * 4 + c)))
                        {
                            int16_t gx = cur_x + c;
                            int16_t gy = cur_y + r;
                            if (gx >= 0 && gx < GRID_COLS && gy >= 0 && gy < GRID_ROWS)
                            {
                                g_tetris->board[gy][gx] = cur_type + 1U;
                            }
                        }
                    }
                }
                sys_beep(12);

                /* Check for completed lines */
                uint8_t cleared = 0;
                for (int16_t r = GRID_ROWS - 1; r >= 0; r--)
                {
                    bool full = true;
                    for (int16_t c = 0; c < GRID_COLS; c++)
                    {
                        if (g_tetris->board[r][c] == 0U)
                        {
                            full = false;
                            break;
                        }
                    }
                    if (full)
                    {
                        cleared++;
                        for (int16_t rr = r; rr > 0; rr--)
                        {
                            for (int16_t c = 0; c < GRID_COLS; c++)
                            {
                                g_tetris->board[rr][c] = g_tetris->board[rr - 1][c];
                            }
                        }
                        for (int16_t c = 0; c < GRID_COLS; c++)
                        {
                            g_tetris->board[0][c] = 0U;
                        }
                        r++; /* Re-check shifted row */
                    }
                }

                if (cleared > 0U)
                {
                    g_tetris->lines += cleared;
                    g_tetris->score += (uint32_t)cleared * (uint32_t)cleared * 100U;
                    if (g_tetris->score > g_tetris->best_score)
                    {
                        g_tetris->best_score = g_tetris->score;
                    }
                    redraw_board();
                    sys_beep(45);
                }
                else
                {
                    g_tetris->score += 10U;
                    if (g_tetris->score > g_tetris->best_score)
                    {
                        g_tetris->best_score = g_tetris->score;
                    }
                }
                update_sidebar_stats();

                /* Spawn next piece */
                cur_type  = next_type;
                next_type = next_random_piece();
                cur_rot   = 0;
                cur_x     = 3;
                cur_y     = 0;
                draw_next_preview(next_type);

                /* Check Game Over */
                if (check_collision(cur_x, cur_y, cur_type, cur_rot))
                {
                    sys_beep(120);
                    (void)sys_save_state("tetris.sav", &g_tetris->best_score, sizeof(uint32_t));

                    sys_lcd_fill_rect(BOARD_X + 6, 90, 88, 54, COLOR_PANEL_BG);
                    sys_draw_text(BOARD_X + 20, 98, "GAME OVER", COLOR_RED, COLOR_PANEL_BG, 1);
                    sys_draw_text(BOARD_X + 14, 114, "PRESS A OR", COLOR_AMBER, COLOR_PANEL_BG, 1);
                    sys_draw_text(BOARD_X + 11, 126, "SPACE AGAIN", COLOR_AMBER, COLOR_PANEL_BG, 1);
                    game_started = false;
                }
                else
                {
                    draw_piece(cur_x, cur_y, cur_type, cur_rot, cur_type + 1U);
                }
            }
        }

        sys_delay_ms(16);
    }
}
