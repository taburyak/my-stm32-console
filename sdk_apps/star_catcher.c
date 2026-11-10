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

#define NUM_STARS  3
#define CATCHER_Y  214
#define CATCHER_W  52
#define CATCHER_H  6

int star_x[NUM_STARS];
int star_y[NUM_STARS];
int star_vy[NUM_STARS];
int star_col[NUM_STARS];
int star_wait[NUM_STARS];

int player_x = 134;
int score    = 0;
int best     = 0;
int lives    = 3;
int last_spawn_x = 160;

void spawn_star(int i, int initial_delay)
{
    /* Spawn within reachable horizontal distance from the previous star */
    int min_x = max(16, last_spawn_x - 110);
    int max_x = min(304, last_spawn_x + 110);
    star_x[i] = rand_range(min_x, max_x);
    last_spawn_x = star_x[i];

    star_y[i]    = 26;
    star_wait[i] = initial_delay;

    /* Gentle speed at start (1..2 px/frame), slightly faster after 15 points */
    int max_vy = (score >= 15) ? 4 : 3;
    star_vy[i] = rand_range(1, max_vy);

    int c = rand_range(0, 3);
    if (c == 0)      star_col[i] = COLOR_YELLOW;
    else if (c == 1) star_col[i] = COLOR_CYAN;
    else             star_col[i] = COLOR_NEON_GREEN;
}

void reset_all_stars()
{
    last_spawn_x = player_x + (CATCHER_W / 2);
    for (int i = 0; i < NUM_STARS; i++)
    {
        /* Stagger stars by 45 frames (~0.75 sec apart) so they fall one by one! */
        spawn_star(i, i * 45);
    }
}

void draw_hud()
{
    fill_rect(0, 0, 320, 20, COLOR_PANEL_BG);
    fill_rect(0, 20, 320, 1, COLOR_CYAN);
    draw_text(6, 6, "STAR CATCHER", COLOR_CYAN, COLOR_PANEL_BG, 1);
    draw_text(104, 6, "SCORE:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(142, 6, score, COLOR_NEON_GREEN, COLOR_PANEL_BG, 1, 3);
    draw_text(178, 6, "BEST:", COLOR_WHITE, COLOR_PANEL_BG, 1);
    draw_num(210, 6, best, COLOR_AMBER, COLOR_PANEL_BG, 1, 3);
    draw_text(252, 6, "LIVES:", COLOR_RED, COLOR_PANEL_BG, 1);
    draw_num(290, 6, lives, COLOR_WHITE, COLOR_PANEL_BG, 1, 1);
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
    reset_all_stars();

    /* Draw initial paddle */
    fill_rect(player_x, CATCHER_Y, CATCHER_W, CATCHER_H, COLOR_CYAN);

    while (1)
    {
        int k = input_poll();
        if (input_pressed(KEY_EXIT))
        {
            break;
        }

        int speed = (k & KEY_A) ? 7 : 4;
        int old_px = player_x;

        if (k & KEY_LEFT)  player_x -= speed;
        if (k & KEY_RIGHT) player_x += speed;
        player_x += pad_lx() / 24;
        player_x = clamp(player_x, 4, 316 - CATCHER_W);

        if (player_x != old_px)
        {
            fill_rect(old_px, CATCHER_Y, CATCHER_W, CATCHER_H, COLOR_DARK_BG);
        }
        fill_rect(player_x, CATCHER_Y, CATCHER_W, CATCHER_H, (k & KEY_A) ? COLOR_AMBER : COLOR_CYAN);

        for (int i = 0; i < NUM_STARS; i++)
        {
            /* Wait until this star's staggered launch timer expires */
            if (star_wait[i] > 0)
            {
                star_wait[i]--;
                continue;
            }

            /* Erase old star position */
            fill_rect(star_x[i] - 3, star_y[i] - 3, 7, 7, COLOR_DARK_BG);
            star_y[i] += star_vy[i];

            /* Check Catch */
            if (star_y[i] >= CATCHER_Y - 5 && star_y[i] <= CATCHER_Y + CATCHER_H + 2 &&
                star_x[i] >= player_x - 5 && star_x[i] <= player_x + CATCHER_W + 5)
            {
                score++;
                if (score > best)
                {
                    best = score;
                    save_int("best.dat", best);
                    if (best < 250) eeprom_write_u8(120, best);
                }
                beep(20);
                draw_hud();
                spawn_star(i, 15);
            }
            /* Check Miss */
            else if (star_y[i] > 232)
            {
                lives--;
                beep(80);
                draw_hud();
                spawn_star(i, 30);

                if (lives <= 0)
                {
                    draw_text(104, 100, "GAME OVER", COLOR_RED, COLOR_DARK_BG, 2);
                    draw_text(76, 130, "PRESS A TO PLAY AGAIN", COLOR_WHITE, COLOR_DARK_BG, 1);
                    while (1)
                    {
                        input_poll();
                        if (input_pressed(KEY_EXIT)) return;
                        if (input_pressed(KEY_A)) break;
                        frame_sync(20);
                    }
                    lives = 3;
                    score = 0;
                    player_x = 134;
                    clear_screen(COLOR_DARK_BG);
                    draw_hud();
                    reset_all_stars();
                    break;
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
