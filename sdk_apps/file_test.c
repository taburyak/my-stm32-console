#include "console_sdk.h"

/* =========================================================================
 * File API test: each line shows PASS or FAIL with the returned code.
 * Run it twice: the handle limit test also checks that the OS closed the
 * handle intentionally left open at exit.
 * ========================================================================= */

#define LINE_H      14U
#define DATA_LEN    1000U

static uint8_t  s_buf[DATA_LEN];
static uint8_t  s_rd[DATA_LEN];
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

static bool same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static void run_tests(void)
{
    int32_t r;
    int32_t fd;

    for (uint32_t i = 0; i < DATA_LEN; i++)
    {
        s_buf[i] = (uint8_t)(i * 7U + 3U);
    }

    /* 1. Create + write (creates appdata/file_test/test/) */
    fd = sys_file_open("test/data.bin", SDK_FILE_WRITE | SDK_FILE_TRUNC);
    r = (fd >= 0) ? sys_file_write(fd, s_buf, DATA_LEN) : fd;
    if (fd >= 0) (void)sys_file_close(fd);
    report("WRITE 1000 B", r == (int32_t)DATA_LEN, r);

    /* 2. Read back */
    sdk_memset(s_rd, 0, sizeof(s_rd));
    fd = sys_file_open("test/data.bin", SDK_FILE_READ);
    int32_t size = (fd >= 0) ? sys_file_size(fd) : fd;
    r = (fd >= 0) ? sys_file_read(fd, s_rd, DATA_LEN) : fd;
    report("SIZE", size == (int32_t)DATA_LEN, size);
    report("READ + COMPARE", (r == (int32_t)DATA_LEN) && same(s_buf, s_rd, DATA_LEN), r);

    /* 3. Seek */
    r = (fd >= 0) ? sys_file_seek(fd, 500U) : fd;
    int32_t r2 = (fd >= 0) ? sys_file_read(fd, s_rd, 10U) : fd;
    report("SEEK 500 + READ 10", (r == SDK_OK) && (r2 == 10) && same(&s_buf[500], s_rd, 10U), r2);
    if (fd >= 0) (void)sys_file_close(fd);

    /* 4. Append */
    fd = sys_file_open("test/data.bin", SDK_FILE_WRITE | SDK_FILE_APPEND);
    r = (fd >= 0) ? sys_file_write(fd, "XYZ", 3U) : fd;
    size = (fd >= 0) ? sys_file_size(fd) : fd;
    if (fd >= 0) (void)sys_file_close(fd);
    report("APPEND 3 B", (r == 3) && (size == (int32_t)DATA_LEN + 3), size);

    /* 5. Directory listing */
    SDK_DirEntry_t ent;
    int32_t count = 0;
    bool found = false;
    while ((r = sys_dir_read("test", (uint32_t)count, &ent)) == 1)
    {
        if ((ent.name[0] == 'd') && (ent.size == DATA_LEN + 3U)) found = true;
        count++;
    }
    report("DIR LIST", (r == 0) && found, count);

    /* 6. Rejected paths */
    r = sys_file_open("../x.bin", SDK_FILE_READ);
    r2 = sys_file_open("/x.bin", SDK_FILE_READ);
    report("BAD PATHS REJECTED", (r == SDK_ERR_INVALID) && (r2 == SDK_ERR_INVALID), r);
    r = sys_file_open("res:x.bin", SDK_FILE_WRITE | SDK_FILE_TRUNC);
    report("RES: IS READ-ONLY", r == SDK_ERR_DENIED, r);

    /* 7. Handle limit (also proves the handle left open by the previous run was closed) */
    int32_t fds[SDK_FILE_MAX_OPEN];
    uint32_t opened = 0;
    for (uint32_t i = 0; i < SDK_FILE_MAX_OPEN; i++)
    {
        fds[i] = sys_file_open("test/data.bin", SDK_FILE_READ);
        if (fds[i] >= 0) opened++;
    }
    r = sys_file_open("test/data.bin", SDK_FILE_READ);
    report("4 HANDLES, 5TH REFUSED", (opened == SDK_FILE_MAX_OPEN) && (r == SDK_ERR_NO_HANDLES), (int32_t)opened);
    for (uint32_t i = 0; i < SDK_FILE_MAX_OPEN; i++)
    {
        if (fds[i] >= 0) (void)sys_file_close(fds[i]);
    }

    /* 8. Remove */
    r = sys_file_remove("test/data.bin");
    r2 = sys_file_open("test/data.bin", SDK_FILE_READ);
    report("REMOVE", (r == SDK_OK) && (r2 == SDK_ERR_NOT_FOUND), r2);

    /* 9. Resources: SD:/games/file_test/ is optional */
    count = 0;
    while (sys_dir_read("res:", (uint32_t)count, &ent) == 1)
    {
        count++;
    }
    report("RES: ENTRIES (INFO)", true, count);

    /* Left open on purpose: the OS must close it at exit */
    (void)sys_file_open("test/leak.txt", SDK_FILE_WRITE | SDK_FILE_TRUNC);
}

void app_main(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_draw_text(10, 8, "FILE TEST", COLOR_CYAN, COLOR_DARK_BG, 2);

    run_tests();

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
