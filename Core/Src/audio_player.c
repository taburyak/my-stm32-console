#include "audio_player.h"
#include "app_services.h"
#include "beeper.h"
#include "vs1053.h"
#include "ccmram.h"
#include "memory_map.h"
#include "sdcard_fatfs.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>

#define AUDIO_TASK_STACK_WORDS  768U  /* 3 KB: LFN f_open/f_readdir keep 512 B on the stack + track name copies */
#define AUDIO_READ_BUF_SIZE     2048U
#define AUDIO_FIFO_WAIT_MS      3U    /* VS1053 FIFO (2 KB) lasts ~50 ms at 320 kbps */
#define AUDIO_FLAG_CMD          0x0001U
#define AUDIO_READ_RETRIES      3U
#define AUDIO_RETRY_DELAY_MS    20U
#define AUDIO_PREV_RESTART_SEC  3U    /* "Prev" after this many seconds restarts the current track */
#define AUDIO_NAME_MAX          96U

typedef enum
{
    AP_CMD_NONE = 0,
    AP_CMD_PLAY,
    AP_CMD_STOP,
    AP_CMD_PAUSE,
    AP_CMD_NEXT,
    AP_CMD_PREV
} ap_cmd_t;

CCMRAM_BUF static StackType_t  s_task_stack[AUDIO_TASK_STACK_WORDS];
CCMRAM_BUF static StaticTask_t s_task_tcb;
static osThreadId_t            s_task = NULL;

/* Read buffer, FIL and the folder-scan DIR/FILINFO live in external FSMC SRAM to save internal RAM;
 * sd_diskio bounces ext-SRAM buffers through its internal scratch sector, so SDIO DMA never targets FSMC */
#define AP_ALIGN4(x)     (((x) + 3U) & ~3U)
#define AP_FIL_OFFSET    AUDIO_READ_BUF_SIZE
#define AP_DIR_OFFSET    (AP_FIL_OFFSET + AP_ALIGN4(sizeof(FIL)))
#define AP_FNO_OFFSET    (AP_DIR_OFFSET + AP_ALIGN4(sizeof(DIR)))
static uint8_t *const s_buf  = (uint8_t *)MM_EXT_AUDIO_BUF;
static FIL *const     s_file = (FIL *)(MM_EXT_AUDIO_BUF + AP_FIL_OFFSET);
static DIR *const     s_dir  = (DIR *)(MM_EXT_AUDIO_BUF + AP_DIR_OFFSET);
static FILINFO *const s_fno  = (FILINFO *)(MM_EXT_AUDIO_BUF + AP_FNO_OFFSET);
_Static_assert(AP_FNO_OFFSET + sizeof(FILINFO) <= MM_EXT_AUDIO_BUF_SIZE, "Audio buffers exceed MM_EXT_AUDIO_BUF");
static uint32_t s_buf_len = 0U;
static uint32_t s_buf_pos = 0U;

static volatile AudioPlayer_State_t s_state      = AUDIO_STATE_IDLE;
static volatile AudioPlayer_Mode_t  s_mode       = AUDIO_MODE_NORMAL;
static volatile ap_cmd_t            s_cmd        = AP_CMD_NONE;
static volatile uint8_t             s_sfx_req    = 0U;
static volatile uint32_t            s_file_size  = 0U;
static volatile uint32_t            s_bytes_sent = 0U;
static volatile uint32_t            s_track_seq  = 0U;
static char                         s_req_path[AUDIO_PLAYER_PATH_MAX];
static char                         s_cur_path[AUDIO_PLAYER_PATH_MAX];
static char                         s_title[AUDIO_PLAYER_TITLE_MAX];
static char                         s_artist[AUDIO_PLAYER_TITLE_MAX];

static void ap_post(ap_cmd_t cmd, const char *path)
{
    if (s_task == NULL)
    {
        return;
    }
    taskENTER_CRITICAL();
    if (path != NULL)
    {
        strncpy(s_req_path, path, AUDIO_PLAYER_PATH_MAX - 1U);
        s_req_path[AUDIO_PLAYER_PATH_MAX - 1U] = '\0';
    }
    s_cmd = cmd;
    taskEXIT_CRITICAL();
    (void)osThreadFlagsSet(s_task, AUDIO_FLAG_CMD);
}

static void ap_close(bool cancel)
{
    (void)f_close(s_file);
    (void)VS1053_EndPlayback(cancel);
    s_state = AUDIO_STATE_IDLE;
    s_track_seq++;
}

/* ---- ID3v2 title/artist (VS1053 skips the tag itself, we only peek at the text frames) ---- */

static uint32_t ap_be32(const uint8_t *p, bool synchsafe)
{
    if (synchsafe)
    {
        return ((uint32_t)(p[0] & 0x7FU) << 21) | ((uint32_t)(p[1] & 0x7FU) << 14) |
               ((uint32_t)(p[2] & 0x7FU) << 7) | (uint32_t)(p[3] & 0x7FU);
    }
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void ap_utf8_put(char *dst, uint32_t size, uint32_t *n, uint32_t cp)
{
    uint8_t  enc[3];
    uint32_t len;
    if (cp < 0x80U)       { enc[0] = (uint8_t)cp; len = 1U; }
    else if (cp < 0x800U) { enc[0] = (uint8_t)(0xC0U | (cp >> 6)); enc[1] = (uint8_t)(0x80U | (cp & 0x3FU)); len = 2U; }
    else                  { enc[0] = (uint8_t)(0xE0U | (cp >> 12)); enc[1] = (uint8_t)(0x80U | ((cp >> 6) & 0x3FU));
                            enc[2] = (uint8_t)(0x80U | (cp & 0x3FU)); len = 3U; }
    if (*n + len < size)
    {
        memcpy(&dst[*n], enc, len);
        *n += len;
    }
}

/* ID3 text frame (encoding byte + payload) -> NUL-terminated UTF-8 */
static void ap_id3_text(const uint8_t *src, uint32_t len, char *dst, uint32_t size)
{
    uint32_t n = 0U;
    if (len > 1U)
    {
        uint8_t enc = src[0];
        src++;
        len--;
        if (enc == 1U || enc == 2U)
        {
            bool le = false;
            if (enc == 1U && len >= 2U && src[0] == 0xFFU && src[1] == 0xFEU) { le = true; src += 2; len -= 2U; }
            else if (enc == 1U && len >= 2U && src[0] == 0xFEU && src[1] == 0xFFU) { src += 2; len -= 2U; }
            for (uint32_t i = 0U; i + 1U < len; i += 2U)
            {
                uint32_t cu = le ? ((uint32_t)src[i] | ((uint32_t)src[i + 1U] << 8))
                                 : (((uint32_t)src[i] << 8) | (uint32_t)src[i + 1U]);
                if (cu == 0U) break;
                ap_utf8_put(dst, size, &n, (cu >= 0xD800U && cu <= 0xDFFFU) ? (uint32_t)'?' : cu);
            }
        }
        else
        {
            for (uint32_t i = 0U; i < len && src[i] != 0U; i++)
            {
                /* enc 3 = UTF-8 copied as is, enc 0 = Latin-1 widened to UTF-8 */
                if (enc == 3U && n + 1U < size) dst[n++] = (char)src[i];
                else if (enc != 3U)             ap_utf8_put(dst, size, &n, src[i]);
            }
        }
    }
    dst[n] = '\0';
}

static void ap_read_id3(void)
{
    uint8_t h[10];
    UINT    br = 0U;
    s_title[0]  = '\0';
    s_artist[0] = '\0';
    if (f_read(s_file, h, sizeof(h), &br) != FR_OK || br != sizeof(h) || memcmp(h, "ID3", 3) != 0 ||
        h[3] < 2U || h[3] > 4U)
    {
        (void)f_lseek(s_file, 0U);
        return;
    }
    uint8_t  ver  = h[3];
    uint32_t end  = 10U + ap_be32(&h[6], true);
    uint32_t pos  = 10U;
    uint32_t hlen = (ver == 2U) ? 6U : 10U;
    if ((h[5] & 0x40U) != 0U && ver >= 3U && f_read(s_file, h, 4U, &br) == FR_OK && br == 4U)
    {
        uint32_t ext = ap_be32(h, ver == 4U);
        pos += (ver == 4U) ? ext : (ext + 4U);
    }

    uint8_t txt[128];
    for (uint32_t frames = 0U; frames < 48U && pos + hlen <= end; frames++)
    {
        if (f_lseek(s_file, pos) != FR_OK || f_read(s_file, h, hlen, &br) != FR_OK || br != hlen || h[0] == 0U)
        {
            break;
        }
        uint32_t fsize = (ver == 2U) ? (((uint32_t)h[3] << 16) | ((uint32_t)h[4] << 8) | h[5])
                                     : ap_be32(&h[4], ver == 4U);
        bool is_title  = (ver == 2U) ? (memcmp(h, "TT2", 3) == 0) : (memcmp(h, "TIT2", 4) == 0);
        bool is_artist = (ver == 2U) ? (memcmp(h, "TP1", 3) == 0) : (memcmp(h, "TPE1", 4) == 0);
        if ((is_title || is_artist) && fsize > 1U)
        {
            uint32_t take = (fsize < sizeof(txt)) ? fsize : (uint32_t)sizeof(txt);
            if (f_read(s_file, txt, take, &br) == FR_OK)
            {
                ap_id3_text(txt, br, is_title ? s_title : s_artist, AUDIO_PLAYER_TITLE_MAX);
            }
        }
        if (s_title[0] != '\0' && s_artist[0] != '\0')
        {
            break;
        }
        pos += hlen + fsize;
    }
    (void)f_lseek(s_file, 0U);
}

static void ap_open(const char *path)
{
    if (f_open(s_file, path, FA_READ) != FR_OK)
    {
        printf("[AUDIO] Cannot open '/%s'\r\n", path);
        return;
    }
    snprintf(s_cur_path, sizeof(s_cur_path), "%s", path);
    ap_read_id3();
    s_file_size  = (uint32_t)f_size(s_file);
    s_bytes_sent = 0U;
    s_buf_len    = 0U;
    s_buf_pos    = 0U;
    /* Datasheet: SCI_DECODE_TIME must be written twice to reset it */
    VS1053_WriteReg(VS1053_SCI_DECODE_TIME, 0U);
    VS1053_WriteReg(VS1053_SCI_DECODE_TIME, 0U);
    s_state = AUDIO_STATE_PLAYING;
    s_track_seq++;
    printf("[AUDIO] Playing '/%s' (%lu bytes)\r\n", path, (unsigned long)s_file_size);
}

/* ---- Folder playlist: tracks are the audio files of the current track's folder, in directory order ---- */

static const char *ap_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? (slash + 1) : path;
}

static void ap_dirname(const char *path, char *dir, uint32_t size)
{
    const char *base = ap_basename(path);
    uint32_t    len  = (base > path) ? (uint32_t)(base - path - 1) : 0U;
    if (len >= size) len = size - 1U;
    memcpy(dir, path, len);
    dir[len] = '\0';
}

static bool ap_next_entry(void)
{
    while (f_readdir(s_dir, s_fno) == FR_OK && s_fno->fname[0] != '\0')
    {
        if ((s_fno->fattrib & AM_DIR) == 0U && strlen(s_fno->fname) < AUDIO_NAME_MAX &&
            AudioPlayer_IsAudioFile(s_fno->fname))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Pick the neighbour of the current track (step +1/-1) or a random one (shuffle).
 * @param wrap Continue from the other end of the folder instead of failing at the first/last track.
 */
static bool ap_pick_track(int32_t step, bool shuffle, bool wrap, char *out, uint32_t out_size)
{
    char dir[AUDIO_PLAYER_PATH_MAX];
    char cur[AUDIO_NAME_MAX];
    char edge[AUDIO_NAME_MAX] = "";  /* first (step +1) or last (step -1) track */
    char pick[AUDIO_NAME_MAX] = "";
    ap_dirname(s_cur_path, dir, sizeof(dir));
    snprintf(cur, sizeof(cur), "%s", ap_basename(s_cur_path));

    uint32_t count = 0U;
    uint32_t cur_idx = 0xFFFFFFFFU;
    bool     prev_was_cur = false;
    bool     found = false;
    if (f_opendir(s_dir, dir) != FR_OK)
    {
        return false;
    }
    while (ap_next_entry())
    {
        bool is_cur = (strcmp(s_fno->fname, cur) == 0);
        if (!shuffle && step > 0)
        {
            if (count == 0U) snprintf(edge, sizeof(edge), "%s", s_fno->fname);
            if (prev_was_cur && pick[0] == '\0') snprintf(pick, sizeof(pick), "%s", s_fno->fname);
        }
        else if (!shuffle)
        {
            /* Until the current track shows up, pick trails one entry behind it ("" if it is the first) */
            if (is_cur)      found = true;
            else if (!found) snprintf(pick, sizeof(pick), "%s", s_fno->fname);
            snprintf(edge, sizeof(edge), "%s", s_fno->fname);
        }
        if (is_cur) cur_idx = count;
        prev_was_cur = is_cur;
        count++;
    }
    (void)f_closedir(s_dir);

    if (count == 0U)
    {
        return false;
    }
    if (shuffle)
    {
        uint32_t r = AppSvc_Random() % count;
        if (count > 1U && r == cur_idx) r = (r + 1U) % count;
        if (f_opendir(s_dir, dir) != FR_OK) return false;
        for (uint32_t i = 0U; i <= r && ap_next_entry(); i++)
        {
            if (i == r) snprintf(pick, sizeof(pick), "%s", s_fno->fname);
        }
        (void)f_closedir(s_dir);
    }
    if (pick[0] == '\0')
    {
        if (!wrap) return false;
        snprintf(pick, sizeof(pick), "%s", edge);
    }
    if (dir[0] != '\0') snprintf(out, out_size, "%s/%s", dir, pick);
    else                snprintf(out, out_size, "%s", pick);
    return pick[0] != '\0';
}

/* Next track after a manual skip (wrap) or at the end of a track (follow the play mode) */
static void ap_advance(bool manual)
{
    char next[AUDIO_PLAYER_PATH_MAX];
    AudioPlayer_Mode_t mode = s_mode;
    bool ok;
    if (!manual && mode == AUDIO_MODE_REPEAT_ONE)
    {
        snprintf(next, sizeof(next), "%s", s_cur_path);
        ok = true;
    }
    else
    {
        ok = ap_pick_track(1, mode == AUDIO_MODE_SHUFFLE, manual || mode != AUDIO_MODE_NORMAL, next, sizeof(next));
    }
    if (ok)
    {
        ap_open(next);
    }
}

static void ap_handle_cmd(void)
{
    char path[AUDIO_PLAYER_PATH_MAX];
    taskENTER_CRITICAL();
    ap_cmd_t cmd = s_cmd;
    s_cmd = AP_CMD_NONE;
    memcpy(path, s_req_path, sizeof(path));
    taskEXIT_CRITICAL();

    switch (cmd)
    {
        case AP_CMD_PLAY:
            if (s_state != AUDIO_STATE_IDLE)
            {
                ap_close(true);
            }
            ap_open(path);
            break;

        case AP_CMD_STOP:
            if (s_state != AUDIO_STATE_IDLE)
            {
                ap_close(true);
                printf("[AUDIO] Stopped\r\n");
            }
            break;

        case AP_CMD_PAUSE:
            if (s_state == AUDIO_STATE_PLAYING)
            {
                s_state = AUDIO_STATE_PAUSED;
            }
            else if (s_state == AUDIO_STATE_PAUSED)
            {
                s_state = AUDIO_STATE_PLAYING;
            }
            else if (s_cur_path[0] != '\0')
            {
                ap_open(s_cur_path); /* Play after Stop resumes the last track from the start */
            }
            else
            {
                /* Nothing played yet (e.g. keyboard Play key): first track of SD:/Music,
                 * else of its first album sub-folder that has tracks, else of the SD root */
                char first[AUDIO_PLAYER_PATH_MAX];
                bool ok = false;
                snprintf(s_cur_path, sizeof(s_cur_path), "Music/");
                ok = ap_pick_track(1, false, true, first, sizeof(first));
                for (uint32_t sub = 0U; !ok && sub < 16U; sub++)
                {
                    bool found = false;
                    if (f_opendir(s_dir, "Music") == FR_OK)
                    {
                        uint32_t idx = 0U;
                        while (!found && f_readdir(s_dir, s_fno) == FR_OK && s_fno->fname[0] != '\0')
                        {
                            if ((s_fno->fattrib & AM_DIR) != 0U && idx++ == sub)
                            {
                                snprintf(s_cur_path, sizeof(s_cur_path), "Music/%.100s/", s_fno->fname);
                                found = true;
                            }
                        }
                        (void)f_closedir(s_dir);
                    }
                    if (!found)
                    {
                        break;
                    }
                    ok = ap_pick_track(1, false, true, first, sizeof(first));
                }
                if (!ok)
                {
                    s_cur_path[0] = '\0';
                    ok = ap_pick_track(1, false, true, first, sizeof(first));
                }
                s_cur_path[0] = '\0';
                if (ok)
                {
                    ap_open(first);
                }
            }
            s_track_seq++;
            break;

        case AP_CMD_NEXT:
        case AP_CMD_PREV:
        {
            if (s_cur_path[0] == '\0')
            {
                break;
            }
            uint32_t sec = (s_state != AUDIO_STATE_IDLE) ? VS1053_ReadReg(VS1053_SCI_DECODE_TIME) : 0U;
            char target[AUDIO_PLAYER_PATH_MAX];
            bool ok;
            if (cmd == AP_CMD_PREV && sec >= AUDIO_PREV_RESTART_SEC)
            {
                snprintf(target, sizeof(target), "%s", s_cur_path);
                ok = true;
            }
            else
            {
                ok = ap_pick_track((cmd == AP_CMD_NEXT) ? 1 : -1, s_mode == AUDIO_MODE_SHUFFLE, true,
                                   target, sizeof(target));
            }
            if (ok)
            {
                if (s_state != AUDIO_STATE_IDLE)
                {
                    ap_close(true);
                }
                ap_open(target);
            }
            break;
        }

        default:
            break;
    }
}

/* A failed read leaves the FIL in a sticky error state: reopen and seek back to the unread offset */
static FRESULT ap_read_retry(UINT *br)
{
    FRESULT fr = f_read(s_file, s_buf, AUDIO_READ_BUF_SIZE, br);
    for (uint32_t attempt = 1U; fr != FR_OK && attempt <= AUDIO_READ_RETRIES; attempt++)
    {
        printf("[AUDIO] SD read error %d at %lu, retry %lu\r\n", (int)fr, (unsigned long)s_bytes_sent,
               (unsigned long)attempt);
        (void)f_close(s_file);
        osDelay(AUDIO_RETRY_DELAY_MS);
        fr = f_open(s_file, s_cur_path, FA_READ);
        if (fr == FR_OK)
        {
            fr = f_lseek(s_file, s_bytes_sent);
        }
        if (fr == FR_OK)
        {
            fr = f_read(s_file, s_buf, AUDIO_READ_BUF_SIZE, br);
        }
    }
    return fr;
}

/* @return true while the decoder keeps accepting data, false when its FIFO is full or the track ended */
static bool ap_feed(void)
{
    if (s_buf_pos >= s_buf_len)
    {
        UINT    br = 0U;
        FRESULT fr = ap_read_retry(&br);
        if (fr != FR_OK || br == 0U)
        {
            if (fr != FR_OK)
            {
                printf("[AUDIO] SD read failed (%d), stopping\r\n", (int)fr);
            }
            ap_close(fr != FR_OK);
            printf("[AUDIO] Finished '/%s'\r\n", s_cur_path);
            if (fr == FR_OK && s_cmd == AP_CMD_NONE)
            {
                ap_advance(false);
            }
            return false;
        }
        s_buf_len = br;
        s_buf_pos = 0U;
    }
    uint32_t n = VS1053_SdiFeed(&s_buf[s_buf_pos], s_buf_len - s_buf_pos);
    s_buf_pos    += n;
    s_bytes_sent += n;
    return n > 0U;
}

/* ---- System sounds: tone segments synthesized on the fly as a 16 kHz 16-bit mono WAV stream ---- */

#define SFX_RATE_HZ    16000U
#define SFX_AMPLITUDE  9000  /* about -11 dBFS, before the global volume */

typedef struct
{
    uint16_t hz;    /* 0 = rest */
    uint16_t ms;
    uint8_t  pluck; /* 1 = decay over the whole segment (tick), 0 = sustain with a short fade */
} sfx_seg_t;

static const sfx_seg_t s_sfx_click[]  = { { 2400U, 12U, 1U } };
static const sfx_seg_t s_sfx_error[]  = { { 392U, 90U, 0U }, { 0U, 30U, 0U }, { 262U, 160U, 0U } };
static const sfx_seg_t s_sfx_notify[] = { { 659U, 70U, 0U }, { 0U, 15U, 0U }, { 988U, 130U, 0U } };

static const int16_t s_sine64[64] = {
    0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
    23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
    32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329,
    23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
    0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
    -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212
};

/* @return false when a player command arrived and the sound must be cancelled */
static bool sfx_send(const uint8_t *data, uint32_t len)
{
    uint32_t off = 0U;
    while (off < len)
    {
        if (s_cmd != AP_CMD_NONE)
        {
            return false;
        }
        uint32_t n = VS1053_SdiFeed(&data[off], len - off);
        off += n;
        if (n == 0U)
        {
            osDelay(1U);
        }
    }
    return true;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void ap_play_sfx(uint8_t kind)
{
    const sfx_seg_t *segs;
    uint32_t         count;
    switch (kind)
    {
        case BEEPER_SND_ERROR:  segs = s_sfx_error;  count = sizeof(s_sfx_error) / sizeof(s_sfx_error[0]);   break;
        case BEEPER_SND_NOTIFY: segs = s_sfx_notify; count = sizeof(s_sfx_notify) / sizeof(s_sfx_notify[0]); break;
        default:                segs = s_sfx_click;  count = sizeof(s_sfx_click) / sizeof(s_sfx_click[0]);   break;
    }

    uint32_t samples = 0U;
    for (uint32_t s = 0U; s < count; s++)
    {
        samples += (SFX_RATE_HZ / 1000U) * segs[s].ms;
    }

    uint8_t hdr[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                        16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
                        'd', 'a', 't', 'a', 0, 0, 0, 0 };
    put_le32(&hdr[4], 36U + samples * 2U);
    put_le32(&hdr[24], SFX_RATE_HZ);
    put_le32(&hdr[28], SFX_RATE_HZ * 2U);
    put_le32(&hdr[40], samples * 2U);
    bool ok = sfx_send(hdr, sizeof(hdr));

    uint8_t  chunk[32];
    uint32_t ci = 0U;
    for (uint32_t s = 0U; s < count && ok; s++)
    {
        uint32_t n      = (SFX_RATE_HZ / 1000U) * segs[s].ms;
        uint32_t inc    = (uint32_t)(((uint64_t)segs[s].hz << 32) / SFX_RATE_HZ);
        uint32_t attack = (n / 8U < 32U) ? (n / 8U) : 32U;
        uint32_t fade   = segs[s].pluck ? (n - attack) : (n / 3U);
        uint32_t phase  = 0U;
        for (uint32_t i = 0U; i < n && ok; i++)
        {
            int32_t v = 0;
            if (segs[s].hz != 0U)
            {
                v = ((int32_t)s_sine64[phase >> 26] * SFX_AMPLITUDE) >> 15;
                if (i < attack)
                {
                    v = (v * (int32_t)i) / (int32_t)attack;
                }
                else if (i >= n - fade)
                {
                    v = (v * (int32_t)(n - i)) / (int32_t)fade;
                }
                phase += inc;
            }
            chunk[ci++] = (uint8_t)v;
            chunk[ci++] = (uint8_t)((uint32_t)v >> 8);
            if (ci == sizeof(chunk))
            {
                ok = sfx_send(chunk, ci);
                ci = 0U;
            }
        }
    }
    if (ok && ci > 0U)
    {
        ok = sfx_send(chunk, ci);
    }
    (void)VS1053_EndPlayback(!ok);
}

static void AudioPlayerTask(void *argument)
{
    (void)argument;
    bool wait = true;
    for (;;)
    {
        if (wait)
        {
            uint32_t timeout = (s_state == AUDIO_STATE_PLAYING) ? AUDIO_FIFO_WAIT_MS : osWaitForever;
            (void)osThreadFlagsWait(AUDIO_FLAG_CMD, osFlagsWaitAny, timeout);
        }
        if (s_cmd != AP_CMD_NONE)
        {
            ap_handle_cmd();
        }
        if (s_state == AUDIO_STATE_IDLE && s_sfx_req != 0U)
        {
            uint8_t kind = s_sfx_req;
            s_sfx_req = 0U;
            ap_play_sfx(kind);
        }
        wait = (s_state != AUDIO_STATE_PLAYING) || !ap_feed();
    }
}

void AudioPlayer_Init(void)
{
    if (s_task != NULL)
    {
        return;
    }
    /* Above the LVGL/GUI task so heavy redraws cannot starve the 2 KB decoder FIFO */
    const osThreadAttr_t attr = {
        .name       = "AudioPlayer",
        .cb_mem     = &s_task_tcb,
        .cb_size    = sizeof(s_task_tcb),
        .stack_mem  = s_task_stack,
        .stack_size = sizeof(s_task_stack),
        .priority   = (osPriority_t)osPriorityAboveNormal
    };
    s_task = osThreadNew(AudioPlayerTask, NULL, &attr);
}

bool AudioPlayer_Play(const char *path)
{
    if (path == NULL || path[0] == '\0' || !VS1053_IsPresent() || !SDCard_IsMounted())
    {
        return false;
    }
    static FILINFO fno;
    if (f_stat(path, &fno) != FR_OK || (fno.fattrib & AM_DIR) != 0U)
    {
        return false;
    }
    ap_post(AP_CMD_PLAY, path);
    return true;
}

void AudioPlayer_Stop(void)
{
    ap_post(AP_CMD_STOP, NULL);
}

void AudioPlayer_TogglePause(void)
{
    ap_post(AP_CMD_PAUSE, NULL);
}

void AudioPlayer_Next(void)
{
    ap_post(AP_CMD_NEXT, NULL);
}

void AudioPlayer_Prev(void)
{
    ap_post(AP_CMD_PREV, NULL);
}

void AudioPlayer_SetMode(AudioPlayer_Mode_t mode)
{
    if (mode < AUDIO_MODE_COUNT)
    {
        s_mode = mode;
        s_track_seq++;
    }
}

AudioPlayer_Mode_t AudioPlayer_GetMode(void)
{
    return s_mode;
}

const char *AudioPlayer_ModeName(AudioPlayer_Mode_t mode)
{
    static const char *const s_names[AUDIO_MODE_COUNT] = { "Normal", "Repeat all", "Repeat one", "Shuffle" };
    return (mode < AUDIO_MODE_COUNT) ? s_names[mode] : "?";
}

uint32_t AudioPlayer_GetTrackSeq(void)
{
    return s_track_seq;
}

bool AudioPlayer_IsAudioFile(const char *name)
{
    static const char *const s_exts[] = { ".mp3", ".ogg", ".wav", ".wma", ".aac", ".m4a", ".mid", ".midi", ".flac" };
    size_t nlen = (name != NULL) ? strlen(name) : 0U;
    for (uint32_t i = 0U; i < sizeof(s_exts) / sizeof(s_exts[0]); i++)
    {
        size_t elen = strlen(s_exts[i]);
        if (nlen > elen)
        {
            const char *tail = &name[nlen - elen];
            bool        eq   = true;
            for (size_t k = 0U; k < elen && eq; k++)
            {
                char c = tail[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
                eq = (c == s_exts[i][k]);
            }
            if (eq)
            {
                return true;
            }
        }
    }
    return false;
}

bool AudioPlayer_PlaySysSound(uint8_t kind)
{
    if (s_task == NULL || !VS1053_IsPresent() || s_state != AUDIO_STATE_IDLE)
    {
        return false;
    }
    s_sfx_req = kind;
    (void)osThreadFlagsSet(s_task, AUDIO_FLAG_CMD);
    return true;
}

AudioPlayer_State_t AudioPlayer_GetState(void)
{
    return s_state;
}

static const char *ap_format_name(uint16_t hdat1)
{
    switch (hdat1)
    {
        case 0x7665U: return "WAV";
        case 0x4154U:
        case 0x4144U:
        case 0x4D34U: return "AAC";
        case 0x574DU: return "WMA";
        case 0x4F67U: return "Ogg Vorbis";
        case 0x664CU: return "FLAC";
        case 0x4D54U: return "MIDI";
        default:      break;
    }
    if ((hdat1 & 0xFFE0U) == 0xFFE0U)
    {
        /* MPEG sync word: bits 2:1 = layer (1 = Layer III) */
        return (((hdat1 >> 1) & 0x3U) == 1U) ? "MP3" : "MPEG";
    }
    return "-";
}

static bool ap_is_ascii(const char *s)
{
    for (; *s != '\0'; s++)
    {
        if ((uint8_t)*s >= 0x80U)
        {
            return false;
        }
    }
    return true;
}

void AudioPlayer_GetStatus(AudioPlayer_Status_t *st)
{
    if (st == NULL)
    {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->state      = s_state;
    st->mode       = s_mode;
    st->track_seq  = s_track_seq;
    st->file_size  = s_file_size;
    st->bytes_sent = s_bytes_sent;
    st->format     = "-";
    snprintf(st->path, sizeof(st->path), "%s", s_cur_path);

    /* Title: ID3 TIT2, or the file name without extension (also when the UI font lacks the tag's glyphs) */
    const char *base = ap_basename(s_cur_path);
    if (s_title[0] != '\0' && (ap_is_ascii(s_title) || !ap_is_ascii(base)))
    {
        snprintf(st->title, sizeof(st->title), "%s", s_title);
        if (ap_is_ascii(s_artist) || !ap_is_ascii(base))
        {
            snprintf(st->artist, sizeof(st->artist), "%s", s_artist);
        }
    }
    else
    {
        snprintf(st->title, sizeof(st->title), "%s", base);
        char *dot = strrchr(st->title, '.');
        if (dot != NULL) *dot = '\0';
    }

    if (st->state == AUDIO_STATE_IDLE || !VS1053_IsPresent())
    {
        return;
    }
    uint16_t audata = VS1053_ReadReg(VS1053_SCI_AUDATA);
    uint32_t brate  = VS1053_GetByteRate();
    st->decode_sec  = VS1053_ReadReg(VS1053_SCI_DECODE_TIME);
    st->sample_rate = audata & 0xFFFEU;
    st->stereo      = (audata & 0x0001U) != 0U;
    st->kbps        = (brate * 8U) / 1000U;
    st->duration_sec = (brate > 0U) ? (s_file_size / brate) : 0U;
    st->format      = ap_format_name(VS1053_ReadReg(VS1053_SCI_HDAT1));
}
