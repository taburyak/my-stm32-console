#include "console_sdk.h"

/* Place _start at the very first byte of 0x68080000! */
__attribute__((section(".text.entry"))) void _start(void)
{
    const uint16_t COL_BG     = RGB565(12, 8, 28);    /* Deep Cyber Purple */
    const uint16_t COL_BORDER = RGB565(255, 0, 128);  /* Neon Magenta */
    const uint16_t COL_PADDLE = RGB565(0, 255, 180);  /* Cyber Mint */
    const uint16_t COL_BALL   = RGB565(255, 220, 0);  /* Gold */

    /* Clear screen & draw top neon border */
    sys_lcd_fill_rect(0, 0, 320, 240, COL_BG);
    sys_lcd_fill_rect(0, 0, 320, 6, COL_BORDER);

    int16_t paddle_x = 125, prev_paddle_x = 125;
    int16_t ball_x   = 154, ball_y   = 40;
    int16_t prev_bx  = 154, prev_by  = 40;
    int16_t vx       = 4,   vy       = 4;

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

        if (keys & KEY_LEFT)  paddle_x -= 6;
        if (keys & KEY_RIGHT) paddle_x += 6;
        if (paddle_x < 4)     paddle_x = 4;
        if (paddle_x > 246)   paddle_x = 246;

        /* Redraw paddle only if moved (flicker-free!) */
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

        if (ball_x <= 4)   { ball_x = 4;   vx = -vx; sys_beep(10); }
        if (ball_x >= 306) { ball_x = 306; vx = -vx; sys_beep(10); }
        if (ball_y <= 10)  { ball_y = 10;  vy = -vy; sys_beep(10); }

        /* Paddle bounce */
        if (vy > 0 && ball_y >= 210 && ball_y <= 222)
        {
            if (ball_x + 10 >= paddle_x && ball_x <= paddle_x + 70)
            {
                ball_y = 209;
                vy = -vy;
                sys_beep(20);
                sys_led_toggle(1);
            }
        }

        /* Missed ball -> reset to top */
        if (ball_y > 228)
        {
            sys_beep(80);
            ball_x = 154;
            ball_y = 30;
            vy     = 4;
        }

        /* Draw new ball */
        sys_lcd_fill_rect((uint16_t)ball_x, (uint16_t)ball_y, 10, 10, COL_BALL);
        prev_bx = ball_x;
        prev_by = ball_y;

        /* 60 FPS frame pacing (16 ms) */
        sys_delay_ms(16);
    }
}
