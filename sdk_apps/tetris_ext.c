#include "console_sdk.h"

/* =========================================================================
 * Cyber-Tetris (SRAM Application for STM32 Console)
 *
 * Board: 10 columns x 20 rows
 * Screen: 320x240 RGB565
 * Features:
 *  - 7 Classic Tetromino shapes with 4-rotation states
 *  - Next Piece preview & Line counter & Level progression
 *  - Soft Drop & Hard Drop mechanics
 *  - Score tracking with Session Best Score
 *  - Full 5x7 ASCII Font renderer
 *  - Non-blocking input handling & Frame pacing (60 FPS)
 * ========================================================================= */

#define BOARD_COLS  10
#define BOARD_ROWS  20
#define BLOCK_SIZE  10  /* 10x10 px per block -> 100x200 px playfield */

/* Playfield position on 320x240 screen */
#define BOARD_X_OFF 110
#define BOARD_Y_OFF 20

/* 5x7 Font Bitmaps for ASCII ' ' (32) to 'Z' (90) */
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* '\'' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}  /* 'Z' */
};

static void draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (c < ' ' || c > 'Z') c = ' ';
    uint8_t idx = (uint8_t)(c - ' ');

    for (uint8_t col = 0; col < 5; col++)
    {
        uint8_t line = font5x7[idx][col];
        for (uint8_t row = 0; row < 7; row++)
        {
            uint16_t color = (line & (1U << row)) ? fg : bg;
            sys_lcd_fill_rect(x + (uint16_t)(col * scale), 
                              y + (uint16_t)(row * scale), 
                              scale, scale, color);
        }
    }
    sys_lcd_fill_rect(x + (uint16_t)(5 * scale), y, scale, (uint16_t)(7 * scale), bg);
}

static void draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale)
{
    uint16_t cur_x = x;
    while (*str)
    {
        draw_char(cur_x, y, *str, fg, bg, scale);
        cur_x += (uint16_t)(6 * scale);
        str++;
    }
}

static void draw_number_padded(uint16_t x, uint16_t y, uint32_t num, uint16_t fg, uint16_t bg, uint8_t scale, uint8_t digits)
{
    /* Clear bounding box to avoid overlapping text artifact */
    sys_lcd_fill_rect(x, y, (uint16_t)(digits * 6 * scale), (uint16_t)(7 * scale), bg);

    char buf[12];
    buf[digits] = '\0';
    for (int i = digits - 1; i >= 0; i--)
    {
        buf[i] = (char)('0' + (num % 10));
        num /= 10;
    }
    draw_string(x, y, buf, fg, bg, scale);
}

/* =========================================================================
 * Tetromino Definitions
 * ========================================================================= */

/* 7 Shapes x 4 Rotations x 4x4 Grid */
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

static uint16_t get_piece_color(uint8_t type)
{
    switch (type)
    {
        case 0: return RGB565(0, 240, 240);  /* Cyan */
        case 1: return RGB565(0, 100, 255);  /* Blue */
        case 2: return RGB565(255, 140, 0);  /* Orange */
        case 3: return RGB565(255, 220, 0);  /* Yellow */
        case 4: return RGB565(0, 230, 80);   /* Green */
        case 5: return RGB565(180, 0, 255);  /* Purple */
        case 6: return RGB565(255, 40, 60);  /* Red */
        default: return RGB565(200, 200, 200);
    }
}

/* Pseudo-random generator */
static uint32_t rng_state = 12345;
static uint8_t get_random_piece(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (uint8_t)((rng_state >> 16) % 7);
}

/* Board state: 0 = empty, 1..7 = piece color type + 1 */
static uint8_t board[BOARD_ROWS][BOARD_COLS];

/* Helper to test block in 4x4 matrix */
static bool test_piece_block(uint8_t type, uint8_t rot, int row, int col)
{
    uint16_t mask = PIECES[type][rot];
    int bit_idx = 15 - (row * 4 + col);
    return ((mask & (1U << bit_idx)) != 0);
}

/* Check collision */
static bool check_collision(uint8_t type, uint8_t rot, int px, int py)
{
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            if (test_piece_block(type, rot, r, c))
            {
                int board_x = px + c;
                int board_y = py + r;

                if (board_x < 0 || board_x >= BOARD_COLS || board_y >= BOARD_ROWS)
                {
                    return true;
                }
                if (board_y >= 0 && board[board_y][board_x] != 0)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Draw a single board cell on screen */
static void draw_cell(int r, int c, uint8_t val, uint16_t bg_col)
{
    uint16_t scr_x = (uint16_t)(BOARD_X_OFF + c * BLOCK_SIZE);
    uint16_t scr_y = (uint16_t)(BOARD_Y_OFF + r * BLOCK_SIZE);

    if (val == 0)
    {
        /* Grid cell background */
        sys_lcd_fill_rect(scr_x, scr_y, BLOCK_SIZE, BLOCK_SIZE, bg_col);
        /* Subtle grid dot */
        sys_lcd_fill_rect(scr_x + 4, scr_y + 4, 1, 1, RGB565(30, 20, 50));
    }
    else
    {
        uint16_t col = get_piece_color((uint8_t)(val - 1));
        /* Main block */
        sys_lcd_fill_rect(scr_x, scr_y, BLOCK_SIZE, BLOCK_SIZE, col);
        /* Inner bevel highlight */
        sys_lcd_fill_rect(scr_x, scr_y, BLOCK_SIZE - 1, 1, RGB565(255, 255, 255));
        sys_lcd_fill_rect(scr_x, scr_y, 1, BLOCK_SIZE - 1, RGB565(255, 255, 255));
        sys_lcd_fill_rect(scr_x + BLOCK_SIZE - 1, scr_y, 1, BLOCK_SIZE, RGB565(0, 0, 0));
        sys_lcd_fill_rect(scr_x, scr_y + BLOCK_SIZE - 1, BLOCK_SIZE, 1, RGB565(0, 0, 0));
    }
}

/* Render piece onto screen or erase it */
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
                    if (erase)
                    {
                        draw_cell(by, bx, board[by][bx], bg_col);
                    }
                    else
                    {
                        draw_cell(by, bx, (uint8_t)(type + 1), bg_col);
                    }
                }
            }
        }
    }
}

/* Render Next Piece Preview window */
static void draw_next_preview(uint8_t next_type, uint16_t bg_col)
{
    uint16_t px_x = 230;
    uint16_t px_y = 130;

    sys_lcd_fill_rect(px_x, px_y, 50, 40, bg_col);

    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            if (test_piece_block(next_type, 0, r, c))
            {
                uint16_t col = get_piece_color(next_type);
                uint16_t bx = (uint16_t)(px_x + 5 + c * 9);
                uint16_t by = (uint16_t)(px_y + 2 + r * 9);
                sys_lcd_fill_rect(bx, by, 8, 8, col);
            }
        }
    }
}

/* Draw entire board */
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
    const uint16_t COL_BG       = RGB565(12, 8, 28);     /* Deep Purple */
    const uint16_t COL_BORDER   = RGB565(255, 0, 128);   /* Neon Magenta */
    const uint16_t COL_TEXT_FG  = RGB565(0, 255, 180);   /* Cyber Mint */
    const uint16_t COL_BEST_FG  = RGB565(255, 215, 0);   /* Gold */
    const uint16_t COL_GAMEOVER = RGB565(255, 50, 50);    /* Red */

    static uint32_t best_score = 0;

    /* Initialize screen UI */
    sys_lcd_fill_rect(0, 0, 320, 240, COL_BG);

    /* Playfield border frame */
    sys_lcd_fill_rect(BOARD_X_OFF - 2, BOARD_Y_OFF - 2, BOARD_COLS * BLOCK_SIZE + 4, 2, COL_BORDER);
    sys_lcd_fill_rect(BOARD_X_OFF - 2, BOARD_Y_OFF + BOARD_ROWS * BLOCK_SIZE, BOARD_COLS * BLOCK_SIZE + 4, 2, COL_BORDER);
    sys_lcd_fill_rect(BOARD_X_OFF - 2, BOARD_Y_OFF - 2, 2, BOARD_ROWS * BLOCK_SIZE + 4, COL_BORDER);
    sys_lcd_fill_rect(BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE, BOARD_Y_OFF - 2, 2, BOARD_ROWS * BLOCK_SIZE + 4, COL_BORDER);

    /* Left Panel: Stats */
    draw_string(10, 20, "SCORE", COL_TEXT_FG, COL_BG, 1);
    draw_string(10, 70, "BEST", COL_BEST_FG, COL_BG, 1);
    draw_string(10, 120, "LINES", COL_TEXT_FG, COL_BG, 1);
    draw_string(10, 170, "LEVEL", COL_TEXT_FG, COL_BG, 1);

    /* Right Panel: Next & Controls */
    draw_string(230, 20, "NEXT", COL_TEXT_FG, COL_BG, 1);
    sys_lcd_fill_rect(226, 126, 58, 48, COL_BORDER);
    sys_lcd_fill_rect(228, 128, 54, 44, COL_BG);

    draw_string(225, 185, "A:ROTATE", RGB565(140, 140, 180), COL_BG, 1);
    draw_string(225, 200, "B:DROP",   RGB565(140, 140, 180), COL_BG, 1);

    /* Main Game Loop Reset */
    for (;;)
    {
        /* Clear Board Data */
        for (int r = 0; r < BOARD_ROWS; r++)
            for (int c = 0; c < BOARD_COLS; c++)
                board[r][c] = 0;

        redraw_board(COL_BG);

        uint32_t score = 0;
        uint32_t lines = 0;
        uint32_t level = 1;

        draw_number_padded(10, 35, score, COL_TEXT_FG, COL_BG, 2, 6);
        draw_number_padded(10, 85, best_score, COL_BEST_FG, COL_BG, 2, 6);
        draw_number_padded(10, 135, lines, COL_TEXT_FG, COL_BG, 2, 4);
        draw_number_padded(10, 185, level, COL_TEXT_FG, COL_BG, 2, 2);

        rng_state += sys_get_tick();
        uint8_t cur_type = get_random_piece();
        uint8_t next_type = get_random_piece();
        draw_next_preview(next_type, COL_BG);

        int cur_x = 3;
        int cur_y = 0;
        uint8_t cur_rot = 0;

        uint32_t last_fall_tick = sys_get_tick();
        uint32_t fall_delay = 500; /* ms per step */

        uint32_t last_key_tick = 0;
        uint32_t prev_keys = 0;

        bool game_over = false;

        sys_beep(30);

        while (!game_over)
        {
            uint32_t now = sys_get_tick();
            uint32_t keys = sys_get_keys();

            if (keys & KEY_EXIT)
            {
                sys_exit();
                return;
            }

            /* Key Edge & Repeat Detection */
            uint32_t pressed_edge = keys & (~prev_keys);
            prev_keys = keys;

            bool move_left  = (pressed_edge & KEY_LEFT);
            bool move_right = (pressed_edge & KEY_RIGHT);
            bool rotate     = (pressed_edge & (KEY_UP | KEY_A));
            bool soft_drop  = (keys & KEY_DOWN);
            bool hard_drop  = (pressed_edge & KEY_B);

            /* Repeat key holding for left/right */
            if ((keys & (KEY_LEFT | KEY_RIGHT)) && (now - last_key_tick > 120))
            {
                if (keys & KEY_LEFT)  move_left = true;
                if (keys & KEY_RIGHT) move_right = true;
                last_key_tick = now;
            }

            /* 1. Handle Movement */
            if (move_left)
            {
                if (!check_collision(cur_type, cur_rot, cur_x - 1, cur_y))
                {
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                    cur_x--;
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    sys_beep(5);
                }
            }
            if (move_right)
            {
                if (!check_collision(cur_type, cur_rot, cur_x + 1, cur_y))
                {
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                    cur_x++;
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    sys_beep(5);
                }
            }

            /* 2. Handle Rotation */
            if (rotate)
            {
                uint8_t new_rot = (cur_rot + 1) % 4;
                if (!check_collision(cur_type, new_rot, cur_x, cur_y))
                {
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                    cur_rot = new_rot;
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    sys_beep(10);
                }
                /* Wall kick try 1 space left/right */
                else if (!check_collision(cur_type, new_rot, cur_x - 1, cur_y))
                {
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                    cur_x--;
                    cur_rot = new_rot;
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    sys_beep(10);
                }
                else if (!check_collision(cur_type, new_rot, cur_x + 1, cur_y))
                {
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                    cur_x++;
                    cur_rot = new_rot;
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    sys_beep(10);
                }
            }

            /* 3. Handle Hard Drop */
            if (hard_drop)
            {
                render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                while (!check_collision(cur_type, cur_rot, cur_x, cur_y + 1))
                {
                    cur_y++;
                    score += 2;
                }
                render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                sys_beep(25);
                /* Trigger immediate lock */
                last_fall_tick = 0;
            }

            /* Effective fall speed */
            uint32_t current_delay = soft_drop ? 40 : fall_delay;

            /* 4. Gravity Step */
            if (now - last_fall_tick >= current_delay)
            {
                last_fall_tick = now;

                if (!check_collision(cur_type, cur_rot, cur_x, cur_y + 1))
                {
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, true, COL_BG);
                    cur_y++;
                    render_active_piece(cur_type, cur_rot, cur_x, cur_y, false, COL_BG);
                    if (soft_drop) score += 1;
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
                                    board[by][bx] = (uint8_t)(cur_type + 1);
                                }
                            }
                        }
                    }

                    sys_beep(15);

                    /* 5. Line Clear Check */
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
                            /* Flash line */
                            for (int c = 0; c < BOARD_COLS; c++)
                            {
                                sys_lcd_fill_rect(BOARD_X_OFF + c * BLOCK_SIZE, BOARD_Y_OFF + r * BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE, RGB565(255, 255, 255));
                            }
                        }
                    }

                    if (cleared_in_step > 0)
                    {
                        sys_beep(60);
                        sys_led_toggle(1);

                        /* Shift rows down */
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
                                for (int c = 0; c < BOARD_COLS; c++) board[0][c] = 0;
                                r++; /* Re-check same index after shift */
                            }
                        }

                        redraw_board(COL_BG);

                        /* Score system */
                        lines += cleared_in_step;
                        if (cleared_in_step == 1) score += 100 * level;
                        else if (cleared_in_step == 2) score += 300 * level;
                        else if (cleared_in_step == 3) score += 500 * level;
                        else if (cleared_in_step >= 4) score += 800 * level; /* Tetris! */

                        if (score > best_score) best_score = score;

                        level = (lines / 10) + 1;
                        fall_delay = (level > 10) ? 100 : (500 - (level - 1) * 40);

                        draw_number_padded(10, 35, score, COL_TEXT_FG, COL_BG, 2, 6);
                        draw_number_padded(10, 85, best_score, COL_BEST_FG, COL_BG, 2, 6);
                        draw_number_padded(10, 135, lines, COL_TEXT_FG, COL_BG, 2, 4);
                        draw_number_padded(10, 185, level, COL_TEXT_FG, COL_BG, 2, 2);
                    }

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
        sys_beep(150);
        draw_string(115, 110, "GAME OVER", COL_GAMEOVER, COL_BG, 2);
        sys_delay_ms(1500);

        sys_lcd_fill_rect(110, 105, 110, 30, COL_BG);
    }
}
