/*
 * star_catcher.c — Sample C-Script Game for STM32F407 Console
 *
 * Copy this raw .c file directly to /games/star_catcher.c on the SD card!
 * No PC compiler needed: the console's built-in sandboxed C-Script engine
 * parses and runs it directly from External SRAM.
 *
 * Controls:
 *   D-Pad Left/Right, Analog Stick, or A/D : Move catcher
 *   Button A / Space                       : Dash boost
 *   ESC / Button B                         : Exit to Games Hub
 */

#define NUM_STARS  6
#define CATCHER_Y  214
#define CATCHER_W  44
#define CATCHER_H  6

int star_x[NUM_STARS];
int star_y[NUM_STARS];
int star_vy[NUM_STARS];
int star_col[NUM_STARS];

int player_x = 138;
int score    = 0;
int best     = 0;
int lives    = 3;

void spawn_star(int i)
{
    star_x[i]  = rand_range(12, 308);
    star_y[i]  = rand_range(26, 60);
    star_vy[i] = rand_range(2, 5);
    int c = rand_range(0, 3);
    if (c == 0)      star_col[i] = COLOR_YELLOW;
    else if (c == 1) star_col[i] = COLOR_CYAN;
    else             star_col[i] = COLOR_NEON_GREEN;
}

void draw_hud()
{
    fill_rect(0, 0, 320, 20, COLOR_PANEL_BG);
    draw_text(8, 6, "STAR CATCHER (.C)", COLOR_CYAN, COLOR_PANEL_BG, 1);
    draw_text(132, 6, "SCORE:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(172, 6, score, COLOR_NEON_GREEN, COLOR_PANEL_BG, 1, 3);
    draw_text(210, 6, "BEST:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(244, 6, best, COLOR_AMBER, COLOR_PANEL_BG, 1, 3);
    draw_text(278, 6, "HP:", COLOR_RED, COLOR_PANEL_BG, 1);
    draw_num(300, 6, lives, COLOR_WHITE, COLOR_PANEL_BG, 1, 1);
}

void main()
{
    best = load_int("best.dat", 0);
    if (best <= 0)
    {
        int saved = eeprom_read_u8(120);
        if (saved > 0 && saved < 255) best = saved;
    }

    clear_screen(COLOR_DARK_BG);
    draw_hud();

    for (int i = 0; i < NUM_STARS; i++)
    {
        spawn_star(i);
    }

    while (1)
    {
        int k = input_poll();
        if (input_pressed(KEY_EXIT))
        {
            break;
        }

        int speed = (k & KEY_A) ? 8 : 5;
        int old_px = player_x;

        if (k & KEY_LEFT)  player_x -= speed;
        if (k & KEY_RIGHT) player_x += speed;
        player_x += pad_lx() / 22;
        player_x = clamp(player_x, 4, 316 - CATCHER_W);

        if (player_x != old_px)
        {
            fill_rect(old_px, CATCHER_Y, CATCHER_W, CATCHER_H, COLOR_DARK_BG);
        }
        fill_rect(player_x, CATCHER_Y, CATCHER_W, CATCHER_H, (k & KEY_A) ? COLOR_AMBER : COLOR_CYAN);

        for (int i = 0; i < NUM_STARS; i++)
        {
            fill_rect(star_x[i] - 3, star_y[i] - 3, 7, 7, COLOR_DARK_BG);
            star_y[i] += star_vy[i];

            if (star_y[i] >= CATCHER_Y - 4 && star_y[i] <= CATCHER_Y + CATCHER_H &&
                star_x[i] >= player_x - 4 && star_x[i] <= player_x + CATCHER_W + 4)
            {
                score++;
                if (score > best)
                {
                    best = score;
                    save_int("best.dat", best);
                    if (best < 250) eeprom_write_u8(120, best);
                }
                beep(25);
                draw_hud();
                spawn_star(i);
            }
            else if (star_y[i] > 232)
            {
                lives--;
                beep(90);
                draw_hud();
                spawn_star(i);

                if (lives <= 0)
                {
                    draw_text(76, 100, "GAME OVER", COLOR_RED, COLOR_DARK_BG, 2);
                    draw_text(64, 130, "PRESS A TO RESTART", COLOR_WHITE, COLOR_DARK_BG, 1);
                    while (1)
                    {
                        int gk = input_poll();
                        if (input_pressed(KEY_EXIT)) return;
                        if (input_pressed(KEY_A)) break;
                        frame_sync(20);
                    }
                    lives = 3;
                    score = 0;
                    clear_screen(COLOR_DARK_BG);
                    draw_hud();
                }
            }
            else
            {
                fill_rect(star_x[i] - 3, star_y[i] - 3, 7, 7, star_col[i]);
            }
        }

        frame_sync(16);
    }
}
