#include "audio_player.h"
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

#define AUDIO_TASK_STACK_WORDS  512U  /* 2 KB: f_open with LFN keeps a 512 B name buffer on the stack */
#define AUDIO_READ_BUF_SIZE     2048U
#define AUDIO_FIFO_WAIT_MS      3U    /* VS1053 FIFO (2 KB) lasts ~50 ms at 320 kbps */
#define AUDIO_FLAG_CMD          0x0001U
#define AUDIO_READ_RETRIES      3U
#define AUDIO_RETRY_DELAY_MS    20U

typedef enum
{
    AP_CMD_NONE = 0,
    AP_CMD_PLAY,
    AP_CMD_STOP,
    AP_CMD_PAUSE
} ap_cmd_t;

CCMRAM_BUF static StackType_t  s_task_stack[AUDIO_TASK_STACK_WORDS];
CCMRAM_BUF static StaticTask_t s_task_tcb;
static osThreadId_t            s_task = NULL;

/* Read buffer and FIL live in external FSMC SRAM to save internal RAM; sd_diskio bounces
 * ext-SRAM buffers through its internal scratch sector, so SDIO DMA never targets FSMC */
static uint8_t *const s_buf  = (uint8_t *)MM_EXT_AUDIO_BUF;
static FIL *const     s_file = (FIL *)(MM_EXT_AUDIO_BUF + AUDIO_READ_BUF_SIZE);
_Static_assert(AUDIO_READ_BUF_SIZE + sizeof(FIL) <= MM_EXT_AUDIO_BUF_SIZE, "Audio buffers exceed MM_EXT_AUDIO_BUF");
static uint32_t s_buf_len = 0U;
static uint32_t s_buf_pos = 0U;

static volatile AudioPlayer_State_t s_state      = AUDIO_STATE_IDLE;
static volatile ap_cmd_t            s_cmd        = AP_CMD_NONE;
static volatile uint8_t             s_sfx_req    = 0U;
static volatile uint32_t            s_file_size  = 0U;
static volatile uint32_t            s_bytes_sent = 0U;
static char                         s_req_path[AUDIO_PLAYER_PATH_MAX];
static char                         s_cur_path[AUDIO_PLAYER_PATH_MAX];

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
}

static void ap_open(const char *path)
{
    if (f_open(s_file, path, FA_READ) != FR_OK)
    {
        printf("[AUDIO] Cannot open '/%s'\r\n", path);
        return;
    }
    snprintf(s_cur_path, sizeof(s_cur_path), "%s", path);
    s_file_size  = (uint32_t)f_size(s_file);
    s_bytes_sent = 0U;
    s_buf_len    = 0U;
    s_buf_pos    = 0U;
    /* Datasheet: SCI_DECODE_TIME must be written twice to reset it */
    VS1053_WriteReg(VS1053_SCI_DECODE_TIME, 0U);
    VS1053_WriteReg(VS1053_SCI_DECODE_TIME, 0U);
    s_state = AUDIO_STATE_PLAYING;
    printf("[AUDIO] Playing '/%s' (%lu bytes)\r\n", path, (unsigned long)s_file_size);
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
            break;

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

void AudioPlayer_GetStatus(AudioPlayer_Status_t *st)
{
    if (st == NULL)
    {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->state      = s_state;
    st->file_size  = s_file_size;
    st->bytes_sent = s_bytes_sent;
    st->format     = "-";
    snprintf(st->path, sizeof(st->path), "%s", s_cur_path);
    if (st->state == AUDIO_STATE_IDLE || !VS1053_IsPresent())
    {
        return;
    }
    uint16_t audata = VS1053_ReadReg(VS1053_SCI_AUDATA);
    st->decode_sec  = VS1053_ReadReg(VS1053_SCI_DECODE_TIME);
    st->sample_rate = audata & 0xFFFEU;
    st->stereo      = (audata & 0x0001U) != 0U;
    st->kbps        = (VS1053_GetByteRate() * 8U) / 1000U;
    st->format      = ap_format_name(VS1053_ReadReg(VS1053_SCI_HDAT1));
}
