#include "console_sdk.h"

/* =========================================================================
 * Cyber-Pong & Arkanoid Extended (SRAM Application for STM32 Console)
 * ========================================================================= */

#define PADDLE_W        64
#define PADDLE_H        8
#define PADDLE_Y        218
#define BALL_SIZE       8

/* Brick Grid: 4 rows x 8 columns inside the arena (x: 6..313) */
#define BRICK_ROWS      4
#define BRICK_COLS      8
#define BRICK_W         35
#define BRICK_H         9
#define BRICK_GAP_X     3
#define BRICK_GAP_Y     3
#define BRICK_START_X   9
#define BRICK_START_Y   32

/* Store brick grid in External SRAM Sandbox (0x68080000) */
typedef struct {
    uint8_t  bricks[BRICK_ROWS][BRICK_COLS];
    uint8_t  active_bricks;
} Pong_State_t;

#define g_pong ((Pong_State_t *)SDK_EXT_RAM_BASE_ADDR)

static const uint16_t s_brick_colors[BRICK_ROWS] = {
    COLOR_MAGENTA,    /* Row 0 (Top):    4 pts */
    COLOR_RED,        /* Row 1:          3 pts */
    COLOR_AMBER,      /* Row 2:          2 pts */
    COLOR_CYAN        /* Row 3 (Bottom): 1 pt  */
};

static void draw_hud(uint32_t score, uint32_t best)
{
    char buf[16];
    sys_draw_text(10, 6, "ARKANOID", COLOR_CYAN, COLOR_PANEL_BG, 1);

    sys_draw_text(104, 6, "SCORE:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sdk_uint_to_str(score, buf, 4);
    sys_draw_text(146, 6, buf, COLOR_NEON_GREEN, COLOR_PANEL_BG, 1);

    sys_draw_text(204, 6, "BEST:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    sdk_uint_to_str(best, buf, 4);
    sys_draw_text(240, 6, buf, COLOR_AMBER, COLOR_PANEL_BG, 1);
}

static void respawn_bricks(void)
{
    g_pong->active_bricks = 0;
    for (uint8_t r = 0; r < BRICK_ROWS; r++)
    {
        uint16_t by  = (uint16_t)(BRICK_START_Y + r * (BRICK_H + BRICK_GAP_Y));
        uint16_t col = s_brick_colors[r];

        for (uint8_t c = 0; c < BRICK_COLS; c++)
        {
            g_pong->bricks[r][c] = 1U;
            g_pong->active_bricks++;
            uint16_t bx = (uint16_t)(BRICK_START_X + c * (BRICK_W + BRICK_GAP_X));
            sys_lcd_fill_rect(bx, by, BRICK_W, BRICK_H, col);
        }
    }
}

__attribute__((section(".text.entry"))) void _start(void)
{
    sdk_memset(g_pong, 0, sizeof(Pong_State_t));

    int16_t  paddle_x      = 128;
    int16_t  prev_paddle_x = 128;
    int16_t  ball_x        = 156;
    int16_t  ball_y        = PADDLE_Y - BALL_SIZE - 2;
    int16_t  prev_ball_x   = 156;
    int16_t  prev_ball_y   = PADDLE_Y - BALL_SIZE - 2;
    int16_t  vx            = 3;
    int16_t  vy            = -3;
    uint32_t score         = 0;
    uint32_t best          = 0;
    bool     ball_active   = false;

    /* Load saved high score from /saves/pong.sav if available */
    (void)sys_load_state("pong.sav", &best, sizeof(best));

    /* 1. Draw Static Arena */
    sys_clear_screen(COLOR_DARK_BG);
    sys_lcd_fill_rect(0, 0, 320, 20, COLOR_PANEL_BG);
    sys_lcd_fill_rect(0, 20, 320, 2, COLOR_CYAN);
    sys_lcd_fill_rect(0, 22, 4, 218, COLOR_CYAN);
    sys_lcd_fill_rect(316, 22, 4, 218, COLOR_CYAN);

    draw_hud(score, best);
    respawn_bricks();

    /* Show launch prompt in center (below bricks) */
    sys_draw_text(64, 130, "PRESS A OR SPACE TO START", COLOR_AMBER, COLOR_DARK_BG, 1);
    sys_draw_text(76, 146, "ARROWS / PAD TO MOVE", COLOR_WHITE, COLOR_DARK_BG, 1);

    /* Wait until user releases the 'A' / 'Enter' key that launched the app from menu */
    while (sys_get_keys() & (KEY_A | KEY_EXIT))
    {
        sys_delay_ms(10);
    }

    uint32_t prev_keys = 0;

    for (;;)
    {
        uint32_t keys = sys_get_keys();
        uint32_t just_pressed = keys & ~prev_keys;
        prev_keys = keys;

        /* Exit on ESC (Keyboard), 'B' (Gamepad), or KEY_3 (Board) */
        if (just_pressed & KEY_EXIT)
        {
            if (score >= best && best > 0U)
            {
                (void)sys_save_state("pong.sav", &best, sizeof(best));
            }
            sys_exit();
            return;
        }

        /* Move paddle left/right */
        if (keys & KEY_LEFT)  paddle_x -= 6;
        if (keys & KEY_RIGHT) paddle_x += 6;

        if (paddle_x < 6)                paddle_x = 6;
        if (paddle_x > 314 - PADDLE_W)   paddle_x = 314 - PADDLE_W;

        /* Launch ball when waiting and player presses 'A' or SPACE */
        if (!ball_active)
        {
            ball_x = paddle_x + (PADDLE_W / 2) - (BALL_SIZE / 2);
            ball_y = PADDLE_Y - BALL_SIZE - 2;

            if (just_pressed & KEY_A)
            {
                /* Erase start prompt */
                sys_lcd_fill_rect(50, 126, 220, 32, COLOR_DARK_BG);
                ball_active = true;
                vy = (vy > 0) ? -vy : vy;
                if (vy == 0) vy = -3;
                sys_beep(20);
            }
        }
        else
        {
            /* Erase old ball before moving so brick redraws stay clean */
            sys_lcd_fill_rect((uint16_t)prev_ball_x, (uint16_t)prev_ball_y, BALL_SIZE, BALL_SIZE, COLOR_DARK_BG);

            /* Update Ball Physics */
            ball_x += vx;
            ball_y += vy;

            /* Bounce Left / Right walls */
            if (ball_x <= 6)
            {
                ball_x = 6;
                vx = -vx;
                sys_beep(8);
            }
            else if (ball_x >= 314 - BALL_SIZE)
            {
                ball_x = 314 - BALL_SIZE;
                vx = -vx;
                sys_beep(8);
            }

            /* Bounce Top ceiling */
            if (ball_y <= 24)
            {
                ball_y = 24;
                vy = -vy;
                sys_beep(8);
            }

            /* Brick Collision Check */
            if (ball_y >= (BRICK_START_Y - BALL_SIZE) &&
                ball_y <= (BRICK_START_Y + BRICK_ROWS * (BRICK_H + BRICK_GAP_Y)))
            {
                for (uint8_t r = 0; r < BRICK_ROWS; r++)
                {
                    int16_t by = (int16_t)(BRICK_START_Y + r * (BRICK_H + BRICK_GAP_Y));
                    if ((ball_y + BALL_SIZE) >= by && ball_y <= (by + BRICK_H))
                    {
                        for (uint8_t c = 0; c < BRICK_COLS; c++)
                        {
                            if (g_pong->bricks[r][c] != 0U)
                            {
                                int16_t bx = (int16_t)(BRICK_START_X + c * (BRICK_W + BRICK_GAP_X));
                                if ((ball_x + BALL_SIZE) >= bx && ball_x <= (bx + BRICK_W))
                                {
                                    /* Destroy brick */
                                    g_pong->bricks[r][c] = 0U;
                                    g_pong->active_bricks--;
                                    sys_lcd_fill_rect((uint16_t)bx, (uint16_t)by, BRICK_W, BRICK_H, COLOR_DARK_BG);

                                    /* Reverse vertical velocity */
                                    vy = -vy;

                                    /* Award points (top rows worth more!) */
                                    score += (uint32_t)(BRICK_ROWS - r);
                                    if (score > best)
                                    {
                                        best = score;
                                    }
                                    draw_hud(score, best);
                                    sys_beep(15);
                                    sys_led_toggle(0);

                                    /* If all bricks cleared, respawn wave & speed up! */
                                    if (g_pong->active_bricks == 0U)
                                    {
                                        sys_beep(90);
                                        sys_delay_ms(120);
                                        respawn_bricks();
                                        /* Push ball below brick zone so it doesn't collide immediately */
                                        ball_y = BRICK_START_Y + BRICK_ROWS * (BRICK_H + BRICK_GAP_Y) + 4;
                                        vy = (vy < 0) ? -vy : vy;
                                        if (vy < 5)
                                        {
                                            vy++;
                                            vx = (vx > 0) ? (vx + 1) : (vx - 1);
                                        }
                                    }
                                    goto brick_check_done;
                                }
                            }
                        }
                    }
                }
            }
            brick_check_done: ;

            /* Check Paddle Collision */
            if (vy > 0 && (ball_y + BALL_SIZE) >= PADDLE_Y && ball_y <= (PADDLE_Y + PADDLE_H))
            {
                if ((ball_x + BALL_SIZE) >= paddle_x && ball_x <= (paddle_x + PADDLE_W))
                {
                    ball_y = PADDLE_Y - BALL_SIZE - 1;
                    vy = -vy;
                    score++;
                    if (score > best)
                    {
                        best = score;
                    }
                    sys_beep(15);
                    sys_led_toggle(1);
                    draw_hud(score, best);
                }
            }

            /* Ball missed paddle -> Game Over & Reset Bricks */
            if (ball_y > 230)
            {
                sys_beep(100);
                if (score >= best && best > 0U)
                {
                    (void)sys_save_state("pong.sav", &best, sizeof(best));
                }

                sys_draw_text(106, 130, "GAME OVER", COLOR_RED, COLOR_DARK_BG, 2);
                sys_delay_ms(1000);
                sys_lcd_fill_rect(90, 126, 140, 24, COLOR_DARK_BG);

                score = 0;
                vx    = 3;
                vy    = -3;
                draw_hud(score, best);
                respawn_bricks();

                ball_active = false;
                ball_x = paddle_x + (PADDLE_W / 2) - (BALL_SIZE / 2);
                ball_y = PADDLE_Y - BALL_SIZE - 2;
                prev_ball_x = ball_x;
                prev_ball_y = ball_y;

                sys_draw_text(64, 130, "PRESS A OR SPACE TO START", COLOR_AMBER, COLOR_DARK_BG, 1);
            }
        }

        /* Erase old positions & draw new positions */
        if (!ball_active && (prev_ball_x != ball_x || prev_ball_y != ball_y))
        {
            sys_lcd_fill_rect((uint16_t)prev_ball_x, (uint16_t)prev_ball_y, BALL_SIZE, BALL_SIZE, COLOR_DARK_BG);
        }
        if (prev_paddle_x != paddle_x)
        {
            sys_lcd_fill_rect((uint16_t)prev_paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H, COLOR_DARK_BG);
        }

        sys_lcd_fill_rect((uint16_t)paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H, COLOR_CYAN);
        sys_lcd_fill_rect((uint16_t)ball_x, (uint16_t)ball_y, BALL_SIZE, BALL_SIZE, COLOR_AMBER);

        prev_paddle_x = paddle_x;
        prev_ball_x   = ball_x;
        prev_ball_y   = ball_y;

        sys_delay_ms(16);
    }
}
