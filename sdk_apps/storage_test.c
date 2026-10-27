#include "console_sdk.h"

/* =========================================================================
 * EEPROM / flash API test: each line shows PASS or FAIL with the returned code.
 * Both run counters must grow by 1 on every launch, also after a power cycle.
 * ========================================================================= */

#define LINE_H          14U
#define EE_COUNTER_OFF  (SDK_EEPROM_SIZE - 4U)
#define FL_TEST_OFF     0U
#define FL_COUNTER_OFF  SDK_FLASH_SECTOR
#define FL_DATA_LEN     600U

static uint8_t  s_buf[FL_DATA_LEN];
static uint8_t  s_rd[FL_DATA_LEN];
static uint16_t s_line;
static uint32_t s_failed;

static void draw_int(uint16_t x, uint16_t y, int32_t v, uint16_t color)
{
    char buf[14];
    char *p = buf;
    uint32_t mag = (uint32_t)v;
    if (v < 0)
    {
        *p++ = '-';
        mag = (uint32_t)(-v);
    }
    sdk_uint_to_str(mag, p, 1);
    sys_draw_text(x, y, buf, color, COLOR_DARK_BG, 1);
}

static void report(const char *name, bool ok, int32_t code)
{
    uint16_t y = (uint16_t)(30U + s_line * LINE_H);
    sys_draw_text(10, y, name, COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(200, y, ok ? "PASS" : "FAIL", ok ? COLOR_NEON_GREEN : COLOR_RED, COLOR_DARK_BG, 1);
    draw_int(240, y, code, COLOR_AMBER);
    s_line++;
    if (!ok)
    {
        s_failed++;
    }
}

static void info(const char *name, int32_t value)
{
    uint16_t y = (uint16_t)(30U + s_line * LINE_H);
    sys_draw_text(10, y, name, COLOR_CYAN, COLOR_DARK_BG, 1);
    draw_int(200, y, value, COLOR_YELLOW);
    s_line++;
}

static bool same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool all_ff(const uint8_t *a, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        if (a[i] != 0xFFU) return false;
    }
    return true;
}

static void test_eeprom(void)
{
    int32_t r;
    uint8_t pattern[16];
    uint8_t back[16];

    for (uint32_t i = 0; i < sizeof(pattern); i++)
    {
        pattern[i] = (uint8_t)(0xA0U + i);
    }

    /* Crosses an 8-byte EEPROM page boundary */
    r = sys_eeprom_write(4U, pattern, sizeof(pattern));
    sdk_memset(back, 0, sizeof(back));
    int32_t r2 = sys_eeprom_read(4U, back, sizeof(back));
    report("EEPROM WRITE + READ", (r == 16) && (r2 == 16) && same(pattern, back, sizeof(back)), r2);

    r = sys_eeprom_read(SDK_EEPROM_SIZE - 2U, back, 4U);
    r2 = sys_eeprom_write(SDK_EEPROM_SIZE, back, 1U);
    report("EEPROM OUT OF RANGE", (r == SDK_ERR_INVALID) && (r2 == SDK_ERR_INVALID), r);

    uint32_t counter = 0;
    r = sys_eeprom_read(EE_COUNTER_OFF, &counter, sizeof(counter));
    if (counter == 0xFFFFFFFFUL) counter = 0;
    counter++;
    r2 = sys_eeprom_write(EE_COUNTER_OFF, &counter, sizeof(counter));
    report("EEPROM COUNTER SAVED", (r == 4) && (r2 == 4), r2);
    info("EEPROM RUN COUNT", (int32_t)counter);
}

static void test_flash(void)
{
    int32_t r;
    int32_t r2;

    r = sys_flash_erase(FL_TEST_OFF);
    r2 = sys_flash_read(FL_TEST_OFF, s_rd, FL_DATA_LEN);
    report("FLASH ERASE -> 0XFF", (r == SDK_OK) && (r2 == (int32_t)FL_DATA_LEN) && all_ff(s_rd, FL_DATA_LEN), r);
    if (r == SDK_ERR_NOT_SUPPORTED)
    {
        return;
    }

    for (uint32_t i = 0; i < FL_DATA_LEN; i++)
    {
        s_buf[i] = (uint8_t)(i * 13U + 1U);
    }
    /* Unaligned start, spans three 256-byte flash pages */
    r = sys_flash_write(FL_TEST_OFF + 100U, s_buf, FL_DATA_LEN - 100U);
    sdk_memset(s_rd, 0, sizeof(s_rd));
    r2 = sys_flash_read(FL_TEST_OFF + 100U, s_rd, FL_DATA_LEN - 100U);
    report("FLASH WRITE + READ", (r == (int32_t)(FL_DATA_LEN - 100U)) && (r2 == r) &&
                                 same(s_buf, s_rd, FL_DATA_LEN - 100U), r2);

    r = sys_flash_erase(100U);
    r2 = sys_flash_read(SDK_FLASH_SIZE - 4U, s_rd, 8U);
    report("FLASH BAD ARGS", (r == SDK_ERR_INVALID) && (r2 == SDK_ERR_INVALID), r);

    uint32_t counter = 0;
    r = sys_flash_read(FL_COUNTER_OFF, &counter, sizeof(counter));
    if (counter == 0xFFFFFFFFUL) counter = 0;
    counter++;
    (void)sys_flash_erase(FL_COUNTER_OFF);
    r2 = sys_flash_write(FL_COUNTER_OFF, &counter, sizeof(counter));
    report("FLASH COUNTER SAVED", (r == 4) && (r2 == 4), r2);
    info("FLASH RUN COUNT", (int32_t)counter);
}

void app_main(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_draw_text(10, 8, "STORAGE TEST", COLOR_CYAN, COLOR_DARK_BG, 2);

    test_eeprom();
    test_flash();

    sys_draw_text(10, 222, s_failed == 0U ? "ALL PASSED. ESC: EXIT" : "FAILED! ESC: EXIT",
                  s_failed == 0U ? COLOR_NEON_GREEN : COLOR_RED, COLOR_DARK_BG, 1);
    sys_beep(s_failed == 0U ? 50 : 400);

    while (sys_get_keys() & KEY_EXIT)
    {
        sys_delay_ms(10);
    }
    while ((sys_get_keys() & KEY_EXIT) == 0U)
    {
        sys_delay_ms(20);
    }
}
