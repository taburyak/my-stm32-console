#include "console_sdk.h"

/* =========================================================================
 * Cyber-Tetris (SRAM Application for STM32 Console)
 *
 * Uses Kernel SVC API (console_sdk.h):
 *  - SVC 10 (sys_draw_sprite): Single-pass 3D beveled block rendering
 *  - SVC 11 (sys_draw_text): Kernel 5x7 font rendering
 *  - SVC 12 (sys_clear_screen): Fast screen clear
 *  - SVC 15/16 (sys_save_state / sys_load_state): High score persistence
 * ========================================================================= */

#define BOARD_COLS      10
#define BOARD_ROWS      20
#define BLOCK_SIZE      10  /* 10x10 px per block -> 100x200 px playfield */

/* Playfield position on 320x240 screen */
#define BOARD_X_OFF     110
#define BOARD_Y_OFF     20

#define SAVE_FILE_NAME  "tetris.sav"

/* =========================================================================
 * Tetromino Definitions (7 Shapes x 4 Rotations x 4x4 Bitmask)
 * ========================================================================= */
static const uint16_t PIECES[7][4] = {
    /* 0: I (Cyan) */
    {0x0F00, 0x2222, 0x00F0, 0x4444},
    /* 1: J (Blue) */
    {0x8E00, 0x6440, 0x0E20, 0x44C0},
    /* 2: L (Orange) */
    {0x2E00, 0x4460, 0x0E80, 0xC440},
    /* 3: O (Yellow) */
    {0x6600, 0x6600, 0x6600, 0x6600},
    /* 4: S (Green) */
    {0x6C00, 0x4620, 0x06C0, 0x4620},
    /* 5: T (Purple) */
    {0x4E00, 0x4C40, 0x0E40, 0x4640},
    /* 6: Z (Red) */
    {0xC600, 0x2640, 0x0C60, 0x2640}
};

/* Board state placed in External SRAM (.bss): 0 = empty, 1..7 = piece color */
static uint8_t board[BOARD_ROWS][BOARD_COLS];

/* 10x10 Sprite buffer for fast single-syscall block rendering */
static uint16_t s_cell_sprite[BLOCK_SIZE * BLOCK_SIZE];

static const uint16_t PIECE_COLORS[7] = {
    RGB565(0, 240, 240),   /* 0: Cyan */
    RGB565(40, 110, 255),  /* 1: Blue */
    RGB565(255, 140, 0),   /* 2: Orange */
    RGB565(255, 220, 0),   /* 3: Yellow */
    RGB565(0, 235, 90),    /* 4: Green */
    RGB565(190, 40, 255),  /* 5: Purple */
    RGB565(255, 45, 70)    /* 6: Red */
};

static inline uint16_t get_piece_color(uint8_t type)
{
    return (type < 7U) ? PIECE_COLORS[type] : RGB565(200, 200, 200);
}

/* Pseudo-random generator */
static uint32_t rng_state = 0xCAFEBABEUL;
static uint8_t get_random_piece(void)
{
    rng_state = rng_state * 1664525UL + 1013904223UL;
    return (uint8_t)((rng_state >> 24) % 7U);
}

static bool test_piece_block(uint8_t type, uint8_t rot, int row, int col)
{
    uint16_t mask = PIECES[type][rot];
    int bit_idx = 15 - (row * 4 + col);
    return ((mask & (1U << bit_idx)) != 0U);
}

static bool check_collision(uint8_t type, uint8_t rot, int px, int py)
{
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            if (test_piece_block(type, rot, r, c))
            {
                int bx = px + c;
                int by = py + r;

                if (bx < 0 || bx >= BOARD_COLS || by >= BOARD_ROWS)
                {
                    return true;
                }
                if (by >= 0 && board[by][bx] != 0)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Draw a single 10x10 board cell in one SVC 10 sprite transfer
 */
static void draw_cell(int r, int c, uint8_t val, uint16_t bg_col)
{
    uint16_t scr_x = (uint16_t)(BOARD_X_OFF + c * BLOCK_SIZE);
    uint16_t scr_y = (uint16_t)(BOARD_Y_OFF + r * BLOCK_SIZE);

    if (val == 0)
    {
        for (int i = 0; i < BLOCK_SIZE * BLOCK_SIZE; i++)
        {
            s_cell_sprite[i] = bg_col;
        }
        /* Subtle cyber grid dot at center (4,4) */
        s_cell_sprite[4 * BLOCK_SIZE + 4] = RGB565(35, 25, 65);
    }
    else
    {
        uint16_t col  = get_piece_color((uint8_t)(val - 1U));
        uint16_t hi   = COLOR_WHITE;
        uint16_t dark = RGB565(10, 10, 20);

        for (int y = 0; y < BLOCK_SIZE; y++)
        {
            for (int x = 0; x < BLOCK_SIZE; x++)
            {
                uint16_t px = col;
                if (y == 0 || x == 0)
                {
                    px = hi; /* Top/Left 3D bevel highlight */
                }
                else if (y == BLOCK_SIZE - 1 || x == BLOCK_SIZE - 1)
                {
                    px = dark; /* Bottom/Right shadow */
                }
                s_cell_sprite[y * BLOCK_SIZE + x] = px;
            }
        }
    }

    sys_draw_sprite(scr_x, scr_y, BLOCK_SIZE, BLOCK_SIZE, s_cell_sprite);
}

static void render_active_piece(uint8_t type, uint8_t rot, int px, int py, bool erase, uint16_t bg_col)
{
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            if (test_piece_block(type, rot, r, c))
            {
                int bx = px + c;
                int by = py + r;
                if (bx >= 0 && bx < BOARD_COLS && by >= 0 && by < BOARD_ROWS)
                {
                    draw_cell(by, bx, erase ? board[by][bx] : (uint8_t)(type + 1U), bg_col);
                }
            }
        }
    }
}

static void draw_next_preview(uint8_t next_type, uint16_t bg_col)
{
    const uint16_t px_x = 230;
    const uint16_t px_y = 40;

    sys_lcd_fill_rect(px_x, px_y, 50, 42, bg_col);

    uint16_t col = get_piece_color(next_type);
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            if (test_piece_block(next_type, 0, r, c))
            {
                uint16_t bx = (uint16_t)(px_x + 7 + c * 9);
                uint16_t by = (uint16_t)(px_y + 4 + r * 9);
                sys_lcd_fill_rect(bx, by, 8, 8, col);
            }
        }
    }
}

static void redraw_board(uint16_t bg_col)
{
    for (int r = 0; r < BOARD_ROWS; r++)
    {
        for (int c = 0; c < BOARD_COLS; c++)
        {
            draw_cell(r, c, board[r][c], bg_col);
        }
    }
}

/* =========================================================================
 * Main Game Entry Point
 * ========================================================================= */

__attribute__((section(".text.entry"))) void _start(void)
{
    const uint16_t COL_BG       = COLOR_CYBER_BG;
    const uint16_t COL_BORDER   = COLOR_MAGENTA;
    const uint16_t COL_TEXT_FG  = COLOR_MINT;
    const uint16_t COL_BEST_FG  = COLOR_GOLD;
    const uint16_t COL_HINT_FG  = RGB565(140, 150, 190);
    const uint16_t COL_GAMEOVER = COLOR_RED;

    uint32_t best_score = 0;
    bool new_record_dirty = false;

    /* Load saved high score from /saves/tetris.sav on SD card */
    (void)sys_load_state(SAVE_FILE_NAME, &best_score, sizeof(best_score));

    /* Clear screen and draw static layout */
    sys_clear_screen(COL_BG);

    /* Playfield border frame */
    sys_lcd_fill_rect(BOARD_X_OFF - 2, BOARD_Y_OFF - 2, BOARD_COLS * BLOCK_SIZE + 4, 2, COL_BORDER);
    sys_lcd_fill_rect(BOARD_X_OFF - 2, BOARD_Y_OFF + BOARD_ROWS * BLOCK_SIZE, BOARD_COLS * BLOCK_SIZE + 4, 2, COL_BORDER);
    sys_lcd_fill_rect(BOARD_X_OFF - 2, BOARD_Y_OFF - 2, 2, BOARD_ROWS * BLOCK_SIZE + 4, COL_BORDER);
    sys_lcd_fill_rect(BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE, BOARD_Y_OFF - 2, 2, BOARD_ROWS * BLOCK_SIZE + 4, COL_BORDER);

    /* Left Panel: Labels */
    sys_draw_text(10, 20,  "SCORE", COL_TEXT_FG, COL_BG, 1);
    sys_draw_text(10, 70,  "BEST",  COL_BEST_FG, COL_BG, 1);
    sys_draw_text(10, 120, "LINES", COL_TEXT_FG, COL_BG, 1);
    sys_draw_text(10, 170, "LEVEL", COL_TEXT_FG, COL_BG, 1);

    /* Right Panel: Next Piece Box & Controls */
    sys_draw_text(230, 22, "NEXT", COL_TEXT_FG, COL_BG, 1);
    sys_lcd_fill_rect(226, 36, 58, 50, COL_BORDER);
    sys_lcd_fill_rect(228, 38, 54, 46, COL_BG);

    sys_draw_text(224, 110, "A/UP:ROT",  COL_HINT_FG, COL_BG, 1);
    sys_draw_text(224, 126, "DN:SOFT",   COL_HINT_FG, COL_BG, 1);
    sys_draw_text(224, 142, "X:DROP",    COL_HINT_FG, COL_BG, 1);
    sys_draw_text(224, 158, "B:EXIT",    COL_HINT_FG, COL_BG, 1);

    for (;;)
    {
        /* Clear Board Data */
        memset(board, 0, sizeof(board));
        redraw_board(COL_BG);

        uint32_t score = 0;
        uint32_t lines = 0;
        uint32_t level = 1;

        sys_draw_number_padded(10, 35,  score,      COL_TEXT_FG, COL_BG, 2, 6);
        sys_draw_number_padded(10, 85,  best_score, COL_BEST_FG, COL_BG, 2, 6);
        sys_draw_number_padded(10, 135, lines,      COL_TEXT_FG, COL_BG, 2, 4);
        sys_draw_number_padded(10, 185, level,      COL_TEXT_FG, COL_BG, 2, 2);

        rng_state ^= sys_get_tick();
        uint8_t cur_type  = get_random_piece();
        uint8_t next_type = get_random_piece();
        draw_next_preview(next_type, COL_BG);

        int cur_x = 3;
        int cur_y = 0;
        uint8_t cur_rot = 0;

        render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);

        uint32_t last_fall_tick = sys_get_tick();
        uint32_t fall_delay     = 500;

        SDK_InputTracker_t input_trk = {0};
        bool game_over = false;

        sys_beep(30);

        while (!game_over)
        {
            uint32_t now  = sys_get_tick();
            uint32_t keys = sys_get_keys();

            if (keys & KEY_EXIT)
            {
                if (new_record_dirty)
                {
                    (void)sys_save_state(SAVE_FILE_NAME, &best_score, sizeof(best_score));
                }
                sys_exit();
                return;
            }

            uint32_t pressed_edge = sdk_input_poll_edges(&input_trk, keys);

            bool move_left  = ((pressed_edge & KEY_LEFT) != 0U);
            bool move_right = ((pressed_edge & KEY_RIGHT) != 0U);
            bool rotate     = ((pressed_edge & (KEY_UP | KEY_A)) != 0U);
            bool soft_drop  = ((keys & KEY_DOWN) != 0U);
            bool hard_drop  = ((pressed_edge & KEY_B) != 0U);

            /* Auto-repeat when holding Left/Right */
            if (pressed_edge & (KEY_LEFT | KEY_RIGHT))
            {
                input_trk.last_repeat_tick = now + 180U; /* Initial delay before repeat */
            }
            else if ((keys & (KEY_LEFT | KEY_RIGHT)) && (now >= input_trk.last_repeat_tick))
            {
                if (keys & KEY_LEFT)  move_left  = true;
                if (keys & KEY_RIGHT) move_right = true;
                input_trk.last_repeat_tick = now + 85U;  /* Repeat rate */
            }

            /* 1. Horizontal Movement */
            if (move_left && !check_collision(cur_type, cur_rot, cur_x - 1, cur_y))
            {
                render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                cur_x--;
                render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                sys_beep(4);
            }
            if (move_right && !check_collision(cur_type, cur_rot, cur_x + 1, cur_y))
            {
                render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                cur_x++;
                render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                sys_beep(4);
            }

            /* 2. Rotation with Wall-Kick */
            if (rotate)
            {
                uint8_t new_rot = (cur_rot + 1U) & 3U;
                int kick_dx = 0;
                bool can_rot = false;

                if (!check_collision(cur_type, new_rot, cur_x, cur_y))
                {
                    can_rot = true;
                }
                else if (!check_collision(cur_type, new_rot, cur_x - 1, cur_y))
                {
                    kick_dx = -1;
                    can_rot = true;
                }
                else if (!check_collision(cur_type, new_rot, cur_x + 1, cur_y))
                {
                    kick_dx = 1;
                    can_rot = true;
                }

                if (can_rot)
                {
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                    cur_x  += kick_dx;
                    cur_rot = new_rot;
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    sys_beep(8);
                }
            }

            /* 3. Hard Drop (Button X on Gamepad) */
            if (hard_drop)
            {
                render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                while (!check_collision(cur_type, cur_rot, cur_x, cur_y + 1))
                {
                    cur_y++;
                    score += 2U;
                }
                render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                sys_beep(25);
                last_fall_tick = 0U; /* Force immediate lock on this frame */
            }

            uint32_t current_delay = soft_drop ? 45U : fall_delay;

            /* 4. Gravity Step */
            if ((now - last_fall_tick) >= current_delay)
            {
                last_fall_tick = now;

                if (!check_collision(cur_type, cur_rot, cur_x, cur_y + 1))
                {
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                    cur_y++;
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    if (soft_drop)
                    {
                        score += 1U;
                    }
                }
                else
                {
                    /* Lock Piece into board */
                    for (int r = 0; r < 4; r++)
                    {
                        for (int c = 0; c < 4; c++)
                        {
                            if (test_piece_block(cur_type, cur_rot, r, c))
                            {
                                int bx = cur_x + c;
                                int by = cur_y + r;
                                if (by >= 0 && by < BOARD_ROWS && bx >= 0 && bx < BOARD_COLS)
                                {
                                    board[by][bx] = (uint8_t)(cur_type + 1U);
                                }
                            }
                        }
                    }

                    sys_beep(12);

                    /* 5. Check Full Lines */
                    uint32_t cleared_in_step = 0;
                    for (int r = BOARD_ROWS - 1; r >= 0; r--)
                    {
                        bool full = true;
                        for (int c = 0; c < BOARD_COLS; c++)
                        {
                            if (board[r][c] == 0)
                            {
                                full = false;
                                break;
                            }
                        }

                        if (full)
                        {
                            cleared_in_step++;
                            /* White flash bar on cleared line */
                            sys_lcd_fill_rect(BOARD_X_OFF,
                                              (uint16_t)(BOARD_Y_OFF + r * BLOCK_SIZE),
                                              BOARD_COLS * BLOCK_SIZE,
                                              BLOCK_SIZE,
                                              COLOR_WHITE);
                        }
                    }

                    if (cleared_in_step > 0U)
                    {
                        sys_beep(65);
                        sys_led_toggle(0);
                        sys_delay_ms(90);

                        /* Collapse cleared rows */
                        for (int r = BOARD_ROWS - 1; r >= 0; r--)
                        {
                            bool full = true;
                            for (int c = 0; c < BOARD_COLS; c++)
                            {
                                if (board[r][c] == 0) { full = false; break; }
                            }

                            if (full)
                            {
                                for (int move_r = r; move_r > 0; move_r--)
                                {
                                    for (int c = 0; c < BOARD_COLS; c++)
                                    {
                                        board[move_r][c] = board[move_r - 1][c];
                                    }
                                }
                                for (int c = 0; c < BOARD_COLS; c++)
                                {
                                    board[0][c] = 0;
                                }
                                r++; /* Re-check shifted row */
                            }
                        }

                        redraw_board(COL_BG);

                        lines += cleared_in_step;
                        if (cleared_in_step == 1U)      score += 100U * level;
                        else if (cleared_in_step == 2U) score += 300U * level;
                        else if (cleared_in_step == 3U) score += 500U * level;
                        else                            score += 800U * level;

                        level = (lines / 10U) + 1U;
                        fall_delay = (level > 10U) ? 90U : (500U - (level - 1U) * 40U);
                    }

                    if (score > best_score)
                    {
                        best_score = score;
                        new_record_dirty = true;
                    }

                    /* Update HUD numbers */
                    sys_draw_number_padded(10, 35,  score,      COL_TEXT_FG, COL_BG, 2, 6);
                    sys_draw_number_padded(10, 85,  best_score, COL_BEST_FG, COL_BG, 2, 6);
                    sys_draw_number_padded(10, 135, lines,      COL_TEXT_FG, COL_BG, 2, 4);
                    sys_draw_number_padded(10, 185, level,      COL_TEXT_FG, COL_BG, 2, 2);

                    /* Spawn Next Piece */
                    cur_type  = next_type;
                    next_type = get_random_piece();
                    draw_next_preview(next_type, COL_BG);

                    cur_x   = 3;
                    cur_y   = 0;
                    cur_rot = 0;

                    if (check_collision(cur_type, cur_rot, cur_x, cur_y))
                    {
                        game_over = true;
                    }
                    else
                    {
                        render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    }
                }
            }

            sys_delay_ms(16);
        }

        /* Game Over Sequence */
        sys_beep(160);
        if (new_record_dirty)
        {
            (void)sys_save_state(SAVE_FILE_NAME, &best_score, sizeof(best_score));
            new_record_dirty = false;
        }

        sys_lcd_fill_rect(BOARD_X_OFF + 4, 102, 92, 28, COL_BG);
        sys_draw_text(BOARD_X_OFF + 20, 106, "GAME", COL_GAMEOVER, COL_BG, 2);
        sys_draw_text(BOARD_X_OFF + 20, 122, "OVER", COL_GAMEOVER, COL_BG, 2);
        sys_delay_ms(1500);
    }
}
