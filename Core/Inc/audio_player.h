#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_PLAYER_PATH_MAX  128U
#define AUDIO_PLAYER_TITLE_MAX 64U

typedef enum
{
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED
} AudioPlayer_State_t;

/* What happens at the end of a track (manual Next/Prev always wrap around the folder) */
typedef enum
{
    AUDIO_MODE_NORMAL = 0,    /* next track in the folder, stop after the last one */
    AUDIO_MODE_REPEAT_ALL,    /* loop the folder */
    AUDIO_MODE_REPEAT_ONE,    /* loop the current track */
    AUDIO_MODE_SHUFFLE,       /* random track from the folder */
    AUDIO_MODE_COUNT
} AudioPlayer_Mode_t;

typedef struct
{
    AudioPlayer_State_t state;
    AudioPlayer_Mode_t  mode;
    uint32_t            track_seq;
    char                path[AUDIO_PLAYER_PATH_MAX];
    char                title[AUDIO_PLAYER_TITLE_MAX];  /* ID3 title or file name without extension */
    char                artist[AUDIO_PLAYER_TITLE_MAX]; /* ID3 artist, may be empty */
    uint32_t            file_size;
    uint32_t            bytes_sent;
    uint32_t            decode_sec;
    uint32_t            duration_sec;                   /* estimate: file size / decoder byte rate */
    uint32_t            sample_rate;
    uint32_t            kbps;
    bool                stereo;
    const char         *format;
} AudioPlayer_Status_t;

/** @brief Spawn the SD -> VS1053 streaming task (static stack in CCM RAM, buffers in SRAM). */
void AudioPlayer_Init(void);

/**
 * @brief Start (or switch to) playing an SD file: MP3, Ogg Vorbis, AAC/M4A, WMA, WAV, MIDI.
 * @param path FatFs path, e.g. "Music/song.mp3".
 * @return false if the VS1053 is missing, the SD card is not mounted or the file does not exist.
 */
bool AudioPlayer_Play(const char *path);
void AudioPlayer_Stop(void);
void AudioPlayer_TogglePause(void);
/** @brief Next / previous audio file in the current track's folder (Prev restarts after 3 s of play). */
void AudioPlayer_Next(void);
void AudioPlayer_Prev(void);

void               AudioPlayer_SetMode(AudioPlayer_Mode_t mode);
AudioPlayer_Mode_t AudioPlayer_GetMode(void);
const char        *AudioPlayer_ModeName(AudioPlayer_Mode_t mode);

/** @brief Changes whenever the track, play state or mode changes: cheap UI refresh trigger (no SPI access). */
uint32_t AudioPlayer_GetTrackSeq(void);

/** @brief true for extensions the VS1053 can decode (mp3, ogg, wav, wma, aac, m4a, mid, midi, flac). */
bool AudioPlayer_IsAudioFile(const char *name);

/**
 * @brief Queue a short synthesized system sound (Beeper_SysSound_t) on the VS1053 when no track is loaded.
 * @return false if the decoder is busy or absent, so the caller can fall back to the buzzer.
 */
bool AudioPlayer_PlaySysSound(uint8_t kind);

AudioPlayer_State_t AudioPlayer_GetState(void);
/** @brief Snapshot of the current track; decoder fields are only valid while not idle. */
void AudioPlayer_GetStatus(AudioPlayer_Status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H */
