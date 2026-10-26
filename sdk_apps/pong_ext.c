#include "console_sdk.h"

#define BRICK_ROWS      4
#define BRICK_COLS      8
#define BRICK_W         36
#define BRICK_H         10
#define BRICK_GAP_X     3
#define BRICK_GAP_Y     3
#define BRICK_START_X   5
#define BRICK_START_Y   35

#define SAVE_FILE_NAME  "pong.sav"

static void update_score_display(uint32_t current_score, uint32_t best_score,
                                 uint16_t fg_score, uint16_t fg_best, uint16_t bg)
{
    sys_draw_number_padded(80,  8, current_score, fg_score, bg, 2, 4);
    sys_draw_number_padded(240, 8, best_score,    fg_best,  bg, 2, 4);
}

__attribute__((section(".text.entry"))) void _start(void)
{
    const uint16_t COL_BG       = COLOR_CYBER_BG;
    const uint16_t COL_BORDER   = COLOR_MAGENTA;
    const uint16_t COL_PADDLE   = COLOR_MINT;
    const uint16_t COL_BALL     = COLOR_GOLD;
    const uint16_t COL_SCORE_FG = RGB565(0, 255, 128);
    const uint16_t COL_BEST_FG  = COLOR_GOLD;
    const uint16_t COL_GAMEOVER = COLOR_RED;

    const uint16_t brick_colors[BRICK_ROWS] = {
        RGB565(255, 0, 100),
        RGB565(255, 120, 0),
        RGB565(255, 220, 0),
        RGB565(0, 230, 255)
    };

    uint32_t best_score    = 0;
    uint32_t current_score = 0;

    /* Load persisted high score from SD card (/saves/pong.sav) */
    (void)sys_load_state(SAVE_FILE_NAME, &best_score, sizeof(best_score));

    uint8_t bricks[BRICK_ROWS][BRICK_COLS];
    uint8_t active_bricks_count = 0;

    int16_t paddle_x = 125, prev_paddle_x = 125;
    int16_t ball_x   = 154, ball_y   = 110;
    int16_t prev_bx  = 154, prev_by  = 110;
    int16_t vx       = 3,   vy       = 3;

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

    sys_clear_screen(COL_BG);
    sys_lcd_fill_rect(0, 0, 320, 4, COL_BORDER);
    sys_lcd_fill_rect(0, 26, 320, 2, COL_BORDER);

    sys_draw_text(8,   8, "SCORE:", COL_SCORE_FG, COL_BG, 2);
    sys_draw_text(168, 8, "BEST:",  COL_BEST_FG,  COL_BG, 2);
    update_score_display(current_score, best_score, COL_SCORE_FG, COL_BEST_FG, COL_BG);

    RESPAWN_BRICKS();
    sys_lcd_fill_rect((uint16_t)paddle_x, 220, 70, 10, COL_PADDLE);
    sys_beep(30);

    bool new_record_dirty = false;

    for (;;)
    {
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

        if (keys & KEY_LEFT)  paddle_x -= 6;
        if (keys & KEY_RIGHT) paddle_x += 6;
        if (paddle_x < 4)     paddle_x = 4;
        if (paddle_x > 246)   paddle_x = 246;

        if (paddle_x != prev_paddle_x)
        {
            sys_lcd_fill_rect((uint16_t)prev_paddle_x, 220, 70, 10, COL_BG);
            sys_lcd_fill_rect((uint16_t)paddle_x,      220, 70, 10, COL_PADDLE);
            prev_paddle_x = paddle_x;
        }

        sys_lcd_fill_rect((uint16_t)prev_bx, (uint16_t)prev_by, 10, 10, COL_BG);

        ball_x += vx;
        ball_y += vy;

        if (ball_x <= 4)        { ball_x = 4;   vx = -vx; sys_beep(10); }
        else if (ball_x >= 306) { ball_x = 306; vx = -vx; sys_beep(10); }
        if (ball_y <= 30)       { ball_y = 30;  vy = -vy; sys_beep(10); }

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
                                bricks[r][c] = 0;
                                active_bricks_count--;
                                sys_lcd_fill_rect(bx, by, BRICK_W, BRICK_H, COL_BG);
                                vy = -vy;

                                current_score += (uint32_t)(BRICK_ROWS - r);
                                if (current_score > best_score)
                                {
                                    best_score = current_score;
                                    new_record_dirty = true;
                                }
                                update_score_display(current_score, best_score, COL_SCORE_FG, COL_BEST_FG, COL_BG);

                                sys_beep(15);
                                sys_led_toggle(0);

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
                    new_record_dirty = true;
                }
                update_score_display(current_score, best_score, COL_SCORE_FG, COL_BEST_FG, COL_BG);
                sys_beep(20);
                sys_led_toggle(1);
            }
        }

        if (ball_y > 228)
        {
            sys_beep(120);
            if (new_record_dirty)
            {
                (void)sys_save_state(SAVE_FILE_NAME, &best_score, sizeof(best_score));
                new_record_dirty = false;
            }

            sys_draw_text(100, 120, "GAME OVER", COL_GAMEOVER, COL_BG, 2);
            sys_delay_ms(1200);
            sys_lcd_fill_rect(90, 115, 140, 30, COL_BG);

            current_score = 0;
            update_score_display(current_score, best_score, COL_SCORE_FG, COL_BEST_FG, COL_BG);
            RESPAWN_BRICKS();

            ball_x = 154;
            ball_y = 110;
            vx     = 3;
            vy     = 3;
        }

        sys_lcd_fill_rect((uint16_t)ball_x, (uint16_t)ball_y, 10, 10, COL_BALL);
        prev_bx = ball_x;
        prev_by = ball_y;

        sys_delay_ms(16);
    }
}
