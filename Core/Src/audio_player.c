#include "audio_player.h"
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
