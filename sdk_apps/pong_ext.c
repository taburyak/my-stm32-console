#include "console_sdk.h"

/* =========================================================================
 * Cyber-Pong & Arkanoid Extended (SRAM Application for STM32 Console)
 * Features:
 *  - Real-time score counter for paddle hits and destroyed bricks
 *  - High score (BEST) session tracker
 *  - Multi-colored brick grid with collision physics and auto-respawn
 *  - Clean digit erasure on Score reset (fixed digit ghosting bug)
 *  - Strict Game Over score reset to 0
 *  - On-screen 5x7 font rendering
 *  - Audio & LED feedback
 * ========================================================================= */

#define BRICK_ROWS      4
#define BRICK_COLS      8
#define BRICK_W         36
#define BRICK_H         10
#define BRICK_GAP_X     3
#define BRICK_GAP_Y     3
#define BRICK_START_X   5
#define BRICK_START_Y   35

/* 5x7 Font Bitmaps for ASCII characters ' ' (32) through 'Z' (90) */
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

/**
 * @brief Render a single 5x7 ASCII character
 */
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

/**
 * @brief Render a text string
 */
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

/**
 * @brief Render an unsigned integer
 */
static void draw_number(uint16_t x, uint16_t y, uint32_t num, uint16_t fg, uint16_t bg, uint8_t scale, uint8_t min_digits)
{
    char buf[12];
    int idx = 10;
    buf[11] = '\0';

    if (num == 0)
    {
        buf[idx--] = '0';
    }
    else
    {
        while (num > 0 && idx >= 0)
        {
            buf[idx--] = (char)('0' + (num % 10));
            num /= 10;
        }
    }

    while ((10 - idx) < min_digits && idx >= 0)
    {
        buf[idx--] = '0';
    }

    draw_string(x, y, &buf[idx + 1], fg, bg, scale);
}

/**
 * @brief Helper to update the score display with clean background erasure
 */
static void update_score_display(uint32_t current_score, uint32_t best_score, uint16_t fg_score, uint16_t fg_best, uint16_t bg)
{
    /* Clear numeric regions to prevent ghost digits when score shrinks or resets */
    sys_lcd_fill_rect(80, 8, 70, 16, bg);
    draw_number(80, 8, current_score, fg_score, bg, 2, 1);

    sys_lcd_fill_rect(240, 8, 70, 16, bg);
    draw_number(240, 8, best_score, fg_best, bg, 2, 1);
}

/* =========================================================================
 * Main Game Entry Point
 * ========================================================================= */

__attribute__((section(".text.entry"))) void _start(void)
{
    /* Cyberpunk Palette */
    const uint16_t COL_BG        = RGB565(12, 8, 28);     /* Deep Cyber Purple */
    const uint16_t COL_BORDER    = RGB565(255, 0, 128);   /* Neon Magenta */
    const uint16_t COL_PADDLE    = RGB565(0, 255, 180);   /* Cyber Mint */
    const uint16_t COL_BALL      = RGB565(255, 220, 0);   /* Gold */
    const uint16_t COL_SCORE_FG  = RGB565(0, 255, 128);   /* Neon Green */
    const uint16_t COL_BEST_FG   = RGB565(255, 215, 0);   /* Gold Text */
    const uint16_t COL_GAMEOVER  = RGB565(255, 60, 60);    /* Bright Red */

    /* Brick Row Palette */
    const uint16_t brick_colors[BRICK_ROWS] = {
        RGB565(255, 0, 100),   /* Row 0: Neon Pink */
        RGB565(255, 120, 0),   /* Row 1: Bright Orange */
        RGB565(255, 220, 0),   /* Row 2: Cyber Gold */
        RGB565(0, 230, 255)    /* Row 3: Cyber Cyan */
    };

    /* Game State */
    static uint32_t best_score = 0;
    uint32_t current_score     = 0;

    uint8_t bricks[BRICK_ROWS][BRICK_COLS];
    uint8_t active_bricks_count = 0;

    int16_t paddle_x = 125, prev_paddle_x = 125;
    int16_t ball_x   = 154, ball_y   = 110;
    int16_t prev_bx  = 154, prev_by  = 110;
    int16_t vx       = 3,   vy       = 3;

    /* Helper lambda/closure for spawning bricks */
    #define RESPAWN_BRICKS() do { \
        active_bricks_count = 0; \
        for (uint8_t r = 0; r < BRICK_ROWS; r++) { \
            for (uint8_t c = 0; c < BRICK_COLS; c++) { \
                bricks[r][c] = 1; \
                active_bricks_count++; \
                uint16_t bx = BRICK_START_X + c * (BRICK_W + BRICK_GAP_X); \
                uint16_t by = BRICK_START_Y + r * (BRICK_H + BRICK_GAP_Y); \
                sys_lcd_fill_rect(bx, by, BRICK_W, BRICK_H, brick_colors[r]); \
            } \
        } \
    } while(0)

    /* Draw UI Header & Base Layout */
    sys_lcd_fill_rect(0, 0, 320, 240, COL_BG);
    sys_lcd_fill_rect(0, 0, 320, 4, COL_BORDER);   /* Top neon border */
    sys_lcd_fill_rect(0, 26, 320, 2, COL_BORDER);  /* Header divider line */

    /* Render initial UI labels */
    draw_string(8, 8, "SCORE:", COL_SCORE_FG, COL_BG, 2);
    draw_string(180, 8, "BEST:", COL_BEST_FG, COL_BG, 2);
    update_score_display(current_score, best_score, COL_SCORE_FG, COL_BEST_FG, COL_BG);

    /* Spawn brick grid */
    RESPAWN_BRICKS();

    /* Initial paddle draw */
    sys_lcd_fill_rect((uint16_t)paddle_x, 220, 70, 10, COL_PADDLE);
    sys_beep(30);

    for (;;)
    {
        uint32_t keys = sys_get_keys();
        if (keys & KEY_EXIT)
        {
            sys_exit();
            return;
        }

        /* Paddle Movement */
        if (keys & KEY_LEFT)  paddle_x -= 6;
        if (keys & KEY_RIGHT) paddle_x += 6;
        if (paddle_x < 4)     paddle_x = 4;
        if (paddle_x > 246)   paddle_x = 246;

        /* Redraw paddle only if moved */
        if (paddle_x != prev_paddle_x)
        {
            sys_lcd_fill_rect((uint16_t)prev_paddle_x, 220, 70, 10, COL_BG);
            sys_lcd_fill_rect((uint16_t)paddle_x,      220, 70, 10, COL_PADDLE);
            prev_paddle_x = paddle_x;
        }

        /* Erase old ball */
        sys_lcd_fill_rect((uint16_t)prev_bx, (uint16_t)prev_by, 10, 10, COL_BG);

        /* Update ball physics */
        ball_x += vx;
        ball_y += vy;

        /* Bounce Left / Right walls */
        if (ball_x <= 4)   
        { 
            ball_x = 4;   
            vx = -vx; 
            sys_beep(10); 
        }
        else if (ball_x >= 306) 
        { 
            ball_x = 306; 
            vx = -vx; 
            sys_beep(10); 
        }

        /* Bounce Top ceiling (below header divider at y=28) */
        if (ball_y <= 30)  
        { 
            ball_y = 30;  
            vy = -vy; 
            sys_beep(10); 
        }

        /* Brick Collision Check */
        if (ball_y >= (BRICK_START_Y - 10) && ball_y <= (BRICK_START_Y + BRICK_ROWS * (BRICK_H + BRICK_GAP_Y)))
        {
            for (uint8_t r = 0; r < BRICK_ROWS; r++)
            {
                uint16_t by = BRICK_START_Y + r * (BRICK_H + BRICK_GAP_Y);
                if (ball_y + 10 >= by && ball_y <= by + BRICK_H)
                {
                    for (uint8_t c = 0; c < BRICK_COLS; c++)
                    {
                        if (bricks[r][c] == 1)
                        {
                            uint16_t bx = BRICK_START_X + c * (BRICK_W + BRICK_GAP_X);
                            if (ball_x + 10 >= bx && ball_x <= bx + BRICK_W)
                            {
                                /* Destroy brick */
                                bricks[r][c] = 0;
                                active_bricks_count--;
                                sys_lcd_fill_rect(bx, by, BRICK_W, BRICK_H, COL_BG);

                                /* Bounce ball vertical direction */
                                vy = -vy;

                                /* Award points: higher rows give more points */
                                current_score += (uint32_t)(BRICK_ROWS - r);
                                if (current_score > best_score)
                                {
                                    best_score = current_score;
                                }
                                update_score_display(current_score, best_score, COL_SCORE_FG, COL_BEST_FG, COL_BG);

                                sys_beep(15);
                                sys_led_toggle(0);

                                /* If all bricks cleared, respawn fresh grid! */
                                if (active_bricks_count == 0)
                                {
                                    sys_beep(100);
                                    sys_delay_ms(150);
                                    RESPAWN_BRICKS();
                                }

                                goto brick_hit_done;
                            }
                        }
                    }
                }
            }
        }
        brick_hit_done: ;

        /* Check Paddle Collision */
        if (vy > 0 && ball_y >= 210 && ball_y <= 222)
        {
            if (ball_x + 10 >= paddle_x && ball_x <= paddle_x + 70)
            {
                ball_y = 209;
                vy = -vy;

                current_score++;
                if (current_score > best_score)
                {
                    best_score = current_score;
                }
                update_score_display(current_score, best_score, COL_SCORE_FG, COL_BEST_FG, COL_BG);

                sys_beep(20);
                sys_led_toggle(1);
            }
        }

        /* Missed Ball -> Game Over / Strict Reset */
        if (ball_y > 228)
        {
            sys_beep(120);

            /* Display "GAME OVER" message */
            draw_string(100, 120, "GAME OVER", COL_GAMEOVER, COL_BG, 2);
            sys_delay_ms(1200);

            /* Clear "GAME OVER" text region */
            sys_lcd_fill_rect(90, 115, 140, 30, COL_BG);

            /* Reset Score STRICTLY to 0 and refresh display cleanly */
            current_score = 0;
            update_score_display(current_score, best_score, COL_SCORE_FG, COL_BG, COL_BG);
            update_score_display(current_score, best_score, COL_SCORE_FG, COL_BEST_FG, COL_BG);

            /* Respawn bricks */
            RESPAWN_BRICKS();

            /* Reset ball position and velocity */
            ball_x = 154;
            ball_y = 110;
            vx     = 3;
            vy     = 3;
        }

        /* Draw new ball */
        sys_lcd_fill_rect((uint16_t)ball_x, (uint16_t)ball_y, 10, 10, COL_BALL);
        prev_bx = ball_x;
        prev_by = ball_y;

        /* 60 FPS Frame pacing (16 ms) */
        sys_delay_ms(16);
    }
}
