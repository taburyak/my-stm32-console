#include "console_sdk.h"

/* =========================================================================
 * External SRAM Diagnostics & Under-Load Data Integrity Stress Test
 *
 * Isolates pure FSMC SRAM timing stability from concurrent FSMC LCD DMA2
 * and SDIO DMA bus contention:
 *   - Alternates PURE passes (no LCD/SD traffic during sweep) and
 *     DMA-LOAD passes (interleaved FSMC LCD DMA2 blits + SDIO writes),
 *     or locks onto one mode when the user presses B/X.
 *   - Captures up to 32 detailed failure records in `g_sram_stats` (placed in
 *     internal SRAM so ST-LINK HotPlug or SD report can inspect every failure:
 *     pass, stage, load_mode, address, expected, got, xor).
 *
 * Controls:
 *   A / START : Pause / Resume continuous loop
 *   RIGHT     : Run single pass (when paused)
 *   B / X     : Cycle mode: AUTO (Pure+DMA) -> PURE ONLY -> DMA LOAD ONLY
 *   UP        : Clear counters & failure log
 *   DOWN      : Save detailed report to SD:/appdata/sram_test/report.txt
 *   EXIT      : Return to OS
 * ========================================================================= */

#define ARENA_WORDS     (120U * 1024U) /* 480 KB = 122,880 words */
#define ARENA_BYTES     (ARENA_WORDS * sizeof(uint32_t))
#define CHUNK_WORDS     (16U * 1024U)  /* Interleave DMA load every 64 KB */

#define STRIP_W         320U
#define STRIP_H         22U
#define STRIP_Y         188U

#define ROW(n)          ((uint16_t)(28U + (n) * 13U))

#define MAX_FAIL_LOG    32U

enum {
    MODE_AUTO_ALT  = 0, /* Even pass = PURE, Odd pass = DMA LOAD */
    MODE_PURE_ONLY = 1,
    MODE_LOAD_ONLY = 2
};

typedef struct {
    uint16_t pass;
    uint8_t  stage;       /* 1=Data, 2=Addr, 3=Checker, 4=PRBS */
    uint8_t  load_active; /* 0=Pure, 1=DMA Load */
    uint32_t addr;
    uint32_t exp;
    uint32_t got;
} FailRecord_t;

typedef struct {
    uint32_t     magic;         /* 0x5352414D ("SRAM") */
    uint32_t     passes_total;
    uint32_t     passes_pure;
    uint32_t     passes_load;
    uint32_t     errors_total;
    uint32_t     errors_pure;   /* Errors when NO FSMC LCD DMA was active */
    uint32_t     errors_load;   /* Errors when FSMC LCD DMA was interleaved */
    uint32_t     stage_errs[4]; /* Per-stage error counts [0..3] -> stage 1..4 */
    uint32_t     low_hw_errs;   /* Errors in lower 16-bit halfword (bits 0..15) */
    uint32_t     high_hw_errs;  /* Errors in upper 16-bit halfword (bits 16..31) */
    uint32_t     both_hw_errs;  /* Errors where both halfwords were corrupted */
    uint32_t     or_xor_mask;   /* Cumulative OR of (exp ^ got) across all errors */
    uint32_t     sd_ops;
    uint32_t     dma_blits;
    uint32_t     last_pass_ms;
    uint32_t     log_count;
    FailRecord_t log[MAX_FAIL_LOG];
} StressStats_t;

static SDK_EXT_BSS volatile uint32_t s_arena[ARENA_WORDS];
static SDK_EXT_BSS uint16_t          s_dma_strip[STRIP_W * STRIP_H];
static SDK_EXT_BSS uint32_t          s_sd_scratch[128];

/* Placed in internal SRAM (.bss) so SWD HotPlug can inspect it without touching FSMC */
volatile StressStats_t g_sram_stats;

static bool     s_running   = true;
static uint8_t  s_mode      = MODE_AUTO_ALT;
static bool     s_cur_load  = false;
static bool     s_quit_req  = false;
static uint32_t s_start_ms  = 0U;
static uint32_t s_prev_keys = 0U;

static inline uint32_t prbs32_next(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void fmt_hex32(uint32_t v, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for (uint8_t i = 0U; i < 8U; i++)
    {
        out[2U + i] = hex[(v >> ((7U - i) * 4U)) & 0xFU];
    }
    out[10] = '\0';
}

static void draw_hex(uint16_t x, uint16_t y, uint32_t v, uint16_t fg)
{
    char buf[12];
    fmt_hex32(v, buf);
    sys_draw_text(x, y, buf, fg, COLOR_DARK_BG, 1);
}

static void draw_u32(uint16_t x, uint16_t y, uint32_t v, uint8_t pad, uint16_t fg)
{
    char buf[12];
    sdk_uint_to_str(v, buf, pad);
    sys_draw_text(x, y, "       ", fg, COLOR_DARK_BG, 1);
    sys_draw_text(x, y, buf, fg, COLOR_DARK_BG, 1);
}

static void reset_stats(void)
{
    sdk_memset((void *)&g_sram_stats, 0, sizeof(g_sram_stats));
    g_sram_stats.magic = 0x5352414DUL;
    s_start_ms = sys_get_tick();
}

static void record_error(uint8_t stage, uint32_t addr, uint32_t exp, uint32_t got)
{
    uint32_t diff = exp ^ got;
    g_sram_stats.errors_total++;
    if (s_cur_load)
    {
        g_sram_stats.errors_load++;
    }
    else
    {
        g_sram_stats.errors_pure++;
    }

    if (stage >= 1U && stage <= 4U)
    {
        g_sram_stats.stage_errs[stage - 1U]++;
    }

    bool lo_bad = (diff & 0x0000FFFFUL) != 0U;
    bool hi_bad = (diff & 0xFFFF0000UL) != 0U;
    if (lo_bad && hi_bad)      g_sram_stats.both_hw_errs++;
    else if (lo_bad)           g_sram_stats.low_hw_errs++;
    else if (hi_bad)           g_sram_stats.high_hw_errs++;

    g_sram_stats.or_xor_mask |= diff;

    uint32_t idx = g_sram_stats.log_count;
    if (idx < MAX_FAIL_LOG)
    {
        g_sram_stats.log[idx].pass        = (uint16_t)g_sram_stats.passes_total;
        g_sram_stats.log[idx].stage       = stage;
        g_sram_stats.log[idx].load_active = s_cur_load ? 1U : 0U;
        g_sram_stats.log[idx].addr        = addr;
        g_sram_stats.log[idx].exp         = exp;
        g_sram_stats.log[idx].got         = got;
        g_sram_stats.log_count            = idx + 1U;
    }
}

static void append_str(char *dst, uint32_t *pos, const char *s)
{
    while (*s != '\0')
    {
        dst[(*pos)++] = *s++;
    }
}

static void append_u32(char *dst, uint32_t *pos, uint32_t v)
{
    char num[12];
    sdk_uint_to_str(v, num, 1U);
    append_str(dst, pos, num);
}

static void append_hex(char *dst, uint32_t *pos, uint32_t v)
{
    char h[12];
    fmt_hex32(v, h);
    append_str(dst, pos, h);
}

static bool save_report_to_sd(void)
{
    int32_t fd = sys_file_open("report.txt", SDK_FILE_WRITE | SDK_FILE_CREATE | SDK_FILE_TRUNC);
    if (fd < 0)
    {
        return false;
    }

    char line[128];
    uint32_t p = 0U;

    append_str(line, &p, "=== STM32F407 FSMC SRAM DIAGNOSTIC REPORT ===\r\n");
    (void)sys_file_write(fd, line, p);

    p = 0U;
    append_str(line, &p, "Passes Total: ");
    append_u32(line, &p, g_sram_stats.passes_total);
    append_str(line, &p, " (Pure=");
    append_u32(line, &p, g_sram_stats.passes_pure);
    append_str(line, &p, ", DMA_Load=");
    append_u32(line, &p, g_sram_stats.passes_load);
    append_str(line, &p, ")\r\n");
    (void)sys_file_write(fd, line, p);

    p = 0U;
    append_str(line, &p, "Errors Total: ");
    append_u32(line, &p, g_sram_stats.errors_total);
    append_str(line, &p, " (Pure=");
    append_u32(line, &p, g_sram_stats.errors_pure);
    append_str(line, &p, ", DMA_Load=");
    append_u32(line, &p, g_sram_stats.errors_load);
    append_str(line, &p, ")\r\n");
    (void)sys_file_write(fd, line, p);

    p = 0U;
    append_str(line, &p, "Halfwords: Low16=");
    append_u32(line, &p, g_sram_stats.low_hw_errs);
    append_str(line, &p, " High16=");
    append_u32(line, &p, g_sram_stats.high_hw_errs);
    append_str(line, &p, " Both32=");
    append_u32(line, &p, g_sram_stats.both_hw_errs);
    append_str(line, &p, " XOR_OR=");
    append_hex(line, &p, g_sram_stats.or_xor_mask);
    append_str(line, &p, "\r\n");
    (void)sys_file_write(fd, line, p);

    for (uint32_t i = 0U; i < g_sram_stats.log_count; i++)
    {
        const volatile FailRecord_t *r = &g_sram_stats.log[i];
        p = 0U;
        append_str(line, &p, "#");
        append_u32(line, &p, i);
        append_str(line, &p, " pass=");
        append_u32(line, &p, r->pass);
        append_str(line, &p, " stg=");
        append_u32(line, &p, r->stage);
        append_str(line, &p, r->load_active ? " DMA " : " PURE ");
        append_hex(line, &p, r->addr);
        append_str(line, &p, " exp=");
        append_hex(line, &p, r->exp);
        append_str(line, &p, " got=");
        append_hex(line, &p, r->got);
        append_str(line, &p, "\r\n");
        (void)sys_file_write(fd, line, p);
    }

    (void)sys_file_close(fd);
    g_sram_stats.sd_ops++;
    return true;
}

static void poll_controls(void)
{
    SDK_InputState_t in;
    (void)sys_input_get_state(&in);
    uint32_t pressed = in.keys & ~s_prev_keys;
    s_prev_keys = in.keys;

    if ((pressed & KEY_EXIT) != 0U)
    {
        s_quit_req = true;
        return;
    }
    if ((pressed & (KEY_A | KEY_START)) != 0U)
    {
        s_running = !s_running;
    }
    if ((pressed & (KEY_B | KEY_X)) != 0U)
    {
        s_mode = (uint8_t)((s_mode + 1U) % 3U);
    }
    if ((pressed & KEY_UP) != 0U)
    {
        reset_stats();
        sys_beep(15);
    }
    if ((pressed & KEY_DOWN) != 0U)
    {
        (void)save_report_to_sd();
        sys_beep(20);
    }
}

static void pump_bus_load_and_input(uint32_t phase)
{
    poll_controls();
    if (s_quit_req || !s_cur_load)
    {
        return;
    }

    /* Render animated bar in External SRAM and DMA-blit it to FSMC LCD */
    uint32_t shift = (g_sram_stats.passes_total * 12U + phase * 8U) & 0xFFU;
    for (uint32_t y = 0U; y < STRIP_H; y++)
    {
        uint16_t *row = &s_dma_strip[y * STRIP_W];
        for (uint32_t x = 0U; x < STRIP_W; x++)
        {
            uint32_t v = (x + y * 3U + shift) & 0x3FU;
            uint16_t c = (g_sram_stats.errors_total == 0U)
                       ? (uint16_t)((v << 5) | (v >> 1))
                       : (uint16_t)(((v & 0x1FU) << 11));
            row[x] = c;
        }
    }
    (void)sys_lcd_blit(0U, STRIP_Y, STRIP_W, STRIP_H, s_dma_strip, STRIP_W);
    g_sram_stats.dma_blits++;
}

static bool run_stage1_data_bus(void)
{
    volatile uint16_t *p0 = (volatile uint16_t *)&s_arena[0];
    volatile uint16_t *p1 = (volatile uint16_t *)&s_arena[1];
    bool ok = true;

    for (uint32_t bit = 0U; bit < 16U; bit++)
    {
        uint16_t pat = (uint16_t)(1U << bit);
        uint16_t inv = (uint16_t)~pat;

        *p0 = pat;
        *p1 = inv;
        uint16_t r0 = *p0;
        uint16_t r1 = *p1;
        if (r0 != pat) { record_error(1U, (uint32_t)p0, pat, r0); ok = false; }
        if (r1 != inv) { record_error(1U, (uint32_t)p1, inv, r1); ok = false; }

        *p0 = inv;
        *p1 = pat;
        r0 = *p0;
        r1 = *p1;
        if (r0 != inv) { record_error(1U, (uint32_t)p0, inv, r0); ok = false; }
        if (r1 != pat) { record_error(1U, (uint32_t)p1, pat, r1); ok = false; }
    }

    volatile uint8_t *b = (volatile uint8_t *)p0;
    *p0 = 0xA55AU;
    b[0] = 0x3CU;
    uint16_t rl = *p0;
    if (rl != 0xA53CU) { record_error(1U, (uint32_t)p0, 0xA53CU, rl); ok = false; }
    b[1] = 0x96U;
    uint16_t rh = *p0;
    if (rh != 0x963CU) { record_error(1U, (uint32_t)p0, 0x963CU, rh); ok = false; }

    return ok;
}

static bool run_stage2_addr_pattern(uint32_t salt)
{
    bool ok = true;
    for (uint32_t i = 0U; i < ARENA_WORDS; i++)
    {
        uint32_t addr = (uint32_t)&s_arena[i];
        s_arena[i] = addr ^ (addr << 13) ^ salt;
        if ((i & (CHUNK_WORDS - 1U)) == (CHUNK_WORDS - 1U))
        {
            pump_bus_load_and_input(i >> 14);
            if (s_quit_req) return ok;
        }
    }

    for (uint32_t i = 0U; i < ARENA_WORDS; i++)
    {
        uint32_t addr = (uint32_t)&s_arena[i];
        uint32_t exp  = addr ^ (addr << 13) ^ salt;
        uint32_t got  = s_arena[i];
        if (got != exp)
        {
            record_error(2U, addr, exp, got);
            ok = false;
        }
        if ((i & (CHUNK_WORDS - 1U)) == (CHUNK_WORDS - 1U))
        {
            pump_bus_load_and_input(i >> 14);
            if (s_quit_req) return ok;
        }
    }
    return ok;
}

static bool run_stage3_checkerboard(void)
{
    const uint32_t p1 = 0x5555AAAAUL;
    const uint32_t p2 = 0xAAAA5555UL;
    bool ok = true;

    for (uint32_t i = 0U; i < ARENA_WORDS; i++)
    {
        s_arena[i] = ((i & 1U) == 0U) ? p1 : p2;
    }
    pump_bus_load_and_input(1U);
    if (s_quit_req) return ok;

    for (uint32_t i = 0U; i < ARENA_WORDS; i++)
    {
        uint32_t exp = ((i & 1U) == 0U) ? p1 : p2;
        uint32_t got = s_arena[i];
        if (got != exp)
        {
            record_error(3U, (uint32_t)&s_arena[i], exp, got);
            ok = false;
        }
        s_arena[i] = ~exp;
    }
    pump_bus_load_and_input(2U);
    if (s_quit_req) return ok;

    for (uint32_t i = 0U; i < ARENA_WORDS; i++)
    {
        uint32_t exp = ~(((i & 1U) == 0U) ? p1 : p2);
        uint32_t got = s_arena[i];
        if (got != exp)
        {
            record_error(3U, (uint32_t)&s_arena[i], exp, got);
            ok = false;
        }
    }
    return ok;
}

static bool run_stage4_prbs(uint32_t seed)
{
    const uint32_t init_st = (seed != 0U) ? seed : 0xCAFEBABEUL;
    uint32_t st = init_st;
    bool ok = true;

    for (uint32_t i = 0U; i < ARENA_WORDS; i++)
    {
        st = prbs32_next(st);
        s_arena[i] = st ^ i;
        if ((i & (CHUNK_WORDS - 1U)) == (CHUNK_WORDS - 1U))
        {
            pump_bus_load_and_input(i >> 14);
            if (s_quit_req) return ok;
        }
    }

    st = init_st;
    for (uint32_t i = 0U; i < ARENA_WORDS; i++)
    {
        st = prbs32_next(st);
        uint32_t exp = st ^ i;
        uint32_t got = s_arena[i];
        if (got != exp)
        {
            record_error(4U, (uint32_t)&s_arena[i], exp, got);
            ok = false;
        }
        s_arena[i] = ~exp;
        if ((i & (CHUNK_WORDS - 1U)) == (CHUNK_WORDS - 1U))
        {
            pump_bus_load_and_input(i >> 14);
            if (s_quit_req) return ok;
        }
    }

    st = init_st;
    for (uint32_t i = 0U; i < ARENA_WORDS; i++)
    {
        st = prbs32_next(st);
        uint32_t exp = ~(st ^ i);
        uint32_t got = s_arena[i];
        if (got != exp)
        {
            record_error(4U, (uint32_t)&s_arena[i], exp, got);
            ok = false;
        }
        if ((i & (CHUNK_WORDS - 1U)) == (CHUNK_WORDS - 1U))
        {
            pump_bus_load_and_input(i >> 14);
            if (s_quit_req) return ok;
        }
    }
    return ok;
}

static void exercise_sd_dma_io(uint32_t seed)
{
    if (!s_cur_load) return;

    for (uint32_t i = 0U; i < 128U; i++)
    {
        s_sd_scratch[i] = seed ^ (i * 0x9E3779B9UL);
    }
    int32_t fd = sys_file_open("stress.bin", SDK_FILE_WRITE | SDK_FILE_CREATE | SDK_FILE_TRUNC);
    if (fd >= 0)
    {
        (void)sys_file_write(fd, (const void *)s_sd_scratch, sizeof(s_sd_scratch));
        (void)sys_file_close(fd);
        g_sram_stats.sd_ops++;
    }
}

static void draw_static_ui(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_lcd_fill_rect(0, 0, 320, 24, COLOR_PANEL_BG);
    sys_draw_text(8, 5, "FSMC SRAM STRESS & TIMING ANALYZER", COLOR_CYAN, COLOR_PANEL_BG, 1);

    sys_draw_text(8,   ROW(0), "STATE:",     COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(168, ROW(0), "MODE:",      COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(8,   ROW(1), "PASSES:",    COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(168, ROW(1), "PASS MS:",   COLOR_WHITE, COLOR_DARK_BG, 1);

    sys_draw_text(8,   ROW(2), "PURE ERRS:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(168, ROW(2), "DMA ERRS:",  COLOR_WHITE, COLOR_DARK_BG, 1);

    sys_draw_text(8,   ROW(3), "1.DATA ERR:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(168, ROW(3), "2.ADDR ERR:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(8,   ROW(4), "3.CHKR ERR:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(168, ROW(4), "4.PRBS ERR:", COLOR_WHITE, COLOR_DARK_BG, 1);

    sys_draw_text(8,   ROW(5), "LO16/HI16:",  COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(168, ROW(5), "XOR MASK:",   COLOR_WHITE, COLOR_DARK_BG, 1);

    sys_draw_text(8,   ROW(6), "FIRST FAIL:", COLOR_WHITE, COLOR_DARK_BG, 1);
    sys_draw_text(8,   ROW(7), "EXP / GOT:",  COLOR_WHITE, COLOR_DARK_BG, 1);

    sys_lcd_fill_rect(0, 214, 320, 26, COLOR_PANEL_BG);
    sys_draw_text(6, 217, "A:PAUSE B:MODE UP:CLEAR DOWN:SAVE", COLOR_AMBER, COLOR_PANEL_BG, 1);
    sys_draw_text(6, 228, "ARENA: 0x68080000 (480 KB)  ESC:EXIT", COLOR_CYAN, COLOR_PANEL_BG, 1);
}

static void update_telemetry_ui(void)
{
    static const char * const mode_names[3] = { "AUTO(P+D)", "PURE ONLY", "DMA ONLY " };

    sys_draw_text(56,  ROW(0), s_running ? "RUNNING" : "PAUSED ",
                  s_running ? COLOR_NEON_GREEN : COLOR_YELLOW, COLOR_DARK_BG, 1);
    sys_draw_text(210, ROW(0), mode_names[s_mode], COLOR_CYAN, COLOR_DARK_BG, 1);

    draw_u32(64,  ROW(1), g_sram_stats.passes_total, 1U, COLOR_WHITE);
    draw_u32(232, ROW(1), g_sram_stats.last_pass_ms, 1U, COLOR_WHITE);

    draw_u32(78,  ROW(2), g_sram_stats.errors_pure, 1U,
             (g_sram_stats.errors_pure == 0U) ? COLOR_NEON_GREEN : COLOR_RED);
    draw_u32(232, ROW(2), g_sram_stats.errors_load, 1U,
             (g_sram_stats.errors_load == 0U) ? COLOR_NEON_GREEN : COLOR_RED);

    draw_u32(84,  ROW(3), g_sram_stats.stage_errs[0], 1U,
             (g_sram_stats.stage_errs[0] == 0U) ? COLOR_NEON_GREEN : COLOR_RED);
    draw_u32(244, ROW(3), g_sram_stats.stage_errs[1], 1U,
             (g_sram_stats.stage_errs[1] == 0U) ? COLOR_NEON_GREEN : COLOR_RED);
    draw_u32(84,  ROW(4), g_sram_stats.stage_errs[2], 1U,
             (g_sram_stats.stage_errs[2] == 0U) ? COLOR_NEON_GREEN : COLOR_RED);
    draw_u32(244, ROW(4), g_sram_stats.stage_errs[3], 1U,
             (g_sram_stats.stage_errs[3] == 0U) ? COLOR_NEON_GREEN : COLOR_RED);

    draw_u32(78,  ROW(5), g_sram_stats.low_hw_errs,  1U, COLOR_YELLOW);
    draw_u32(124, ROW(5), g_sram_stats.high_hw_errs, 1U, COLOR_AMBER);
    draw_hex(232, ROW(5), g_sram_stats.or_xor_mask,  COLOR_CYAN);

    if (g_sram_stats.log_count == 0U)
    {
        sys_draw_text(84, ROW(6), "NONE (ZERO ERRORS)       ", COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
        sys_draw_text(84, ROW(7), "---                      ", COLOR_WHITE, COLOR_DARK_BG, 1);
    }
    else
    {
        const volatile FailRecord_t *r = &g_sram_stats.log[0];
        draw_hex(84,  ROW(6), r->addr, COLOR_RED);
        sys_draw_text(160, ROW(6), r->load_active ? "[DMA]" : "[PUR]", COLOR_AMBER, COLOR_DARK_BG, 1);
        draw_hex(84,  ROW(7), r->exp,  COLOR_YELLOW);
        draw_hex(172, ROW(7), r->got,  COLOR_RED);
    }
}

static void run_one_full_pass(void)
{
    uint32_t t0   = sys_get_tick();
    uint32_t pass = g_sram_stats.passes_total;
    uint32_t seed = sys_random() ^ (t0 * 0x045D9F3BUL) ^ (pass + 1U);

    if (s_mode == MODE_AUTO_ALT)
    {
        s_cur_load = ((pass & 1U) != 0U);
    }
    else
    {
        s_cur_load = (s_mode == MODE_LOAD_ONLY);
    }

    (void)run_stage1_data_bus();
    if (s_quit_req) return;

    (void)run_stage2_addr_pattern(seed);
    if (s_quit_req) return;

    (void)run_stage3_checkerboard();
    if (s_quit_req) return;

    (void)run_stage4_prbs(seed);
    if (s_quit_req) return;

    g_sram_stats.passes_total++;
    if (s_cur_load) g_sram_stats.passes_load++;
    else            g_sram_stats.passes_pure++;

    g_sram_stats.last_pass_ms = sys_get_tick() - t0;

    if (s_cur_load && (g_sram_stats.passes_load % 3U) == 0U)
    {
        exercise_sd_dma_io(seed);
    }
    if ((g_sram_stats.passes_total % 8U) == 0U)
    {
        (void)save_report_to_sd();
    }
}

void app_main(void)
{
    reset_stats();

    while ((sys_get_keys() & (KEY_A | KEY_EXIT)) != 0U)
    {
        sys_delay_ms(10);
    }

    s_prev_keys = sys_get_keys();
    draw_static_ui();
    update_telemetry_ui();

    while (!s_quit_req)
    {
        if (s_running)
        {
            run_one_full_pass();
            if (s_quit_req) break;
            update_telemetry_ui();
        }
        else
        {
            poll_controls();
            if (s_quit_req) break;
            if ((sys_get_keys() & KEY_RIGHT) != 0U)
            {
                run_one_full_pass();
            }
            update_telemetry_ui();
            sys_delay_ms(30);
        }
    }

    (void)save_report_to_sd();
}
