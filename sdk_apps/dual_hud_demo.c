// Dual-Screen Companion HUD + Live Bounce Demo (STM32F407 + ESP32-C6-LCD-1.47)
void main() {
    int px = 132, old_px = 132;
    int bx = 160, by = 90, vx = 3, vy = 2;
    int score = 0, best = load_int("best.dat", 0);

    clear_screen(COLOR_DARK_BG);
    fill_rect(0, 0, 320, 20, COLOR_PANEL_BG);
    draw_text(8, 6, "DUAL-SCREEN PONG   [ESC: EXIT]", COLOR_CYAN, COLOR_PANEL_BG, 1);

    esp_hud_clear();
    esp_hud_text(0, "=== DUAL-SCREEN TELEMETRY ===");
    esp_hud_num(1, "SCORE:       ", score);
    esp_hud_num(2, "HIGH BEST:   ", best);
    esp_hud_num(3, "MCU TEMP dC: ", temp_mcu());
    esp_hud_num(4, "OUTDOOR dC:  ", weather_temp());
    esp_hud_text(5, "STATUS:      READY (GREEN LED)");
    esp_led(0, 48, 16);

    fill_rect(px, 222, 56, 6, COLOR_CYAN);
    while (1) {
        int k = input_poll();
        if (input_pressed(KEY_EXIT)) break;

        old_px = px;
        if (k & KEY_LEFT)  px -= 5;
        if (k & KEY_RIGHT) px += 5;
        px = clamp(px + pad_lx() / 24, 4, 260);
        if (px != old_px) {
            fill_rect(old_px, 222, 56, 6, COLOR_DARK_BG);
            fill_rect(px, 222, 56, 6, COLOR_CYAN);
        }

        fill_rect(bx - 4, by - 4, 9, 9, COLOR_DARK_BG);
        bx += vx;
        by += vy;
        if (bx <= 8 || bx >= 311) { vx = -vx; bx = clamp(bx, 8, 311); beep(12); }
        if (by <= 26)             { vy = -vy; by = 26; beep(12); }

        if (vy > 0 && by >= 216 && by <= 224 && bx >= px - 4 && bx <= px + 60) {
            by = 216;
            vy = -vy;
            vx = clamp((bx - (px + 28)) / 6, -5, 5);
            if (vx == 0) vx = 2;
            score++;
            beep(25);
            esp_led(0, 64, 20);
            if (score > best) {
                best = score;
                save_int("best.dat", best);
                esp_led(0, 45, 72);
                esp_hud_text(5, "STATUS:      NEW RECORD!");
            } else {
                esp_hud_text(5, "STATUS:      RALLY ACTIVE");
            }
            esp_hud_num(1, "SCORE:       ", score);
            esp_hud_num(2, "HIGH BEST:   ", best);
        } else if (by > 234) {
            beep(120);
            score = 0;
            bx = 160; by = 80; vy = 2;
            esp_led(64, 0, 0);
            esp_hud_num(1, "SCORE:       ", score);
            esp_hud_text(5, "STATUS:      MISSED! BALL RESET");
        }

        fill_rect(bx - 4, by - 4, 9, 9, COLOR_AMBER);
        draw_num(276, 6, score, COLOR_NEON_GREEN, COLOR_PANEL_BG, 1, 3);
        frame_sync(16);
    }
}
