#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_PLAYER_PATH_MAX 128U

typedef enum
{
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED
} AudioPlayer_State_t;

typedef struct
{
    AudioPlayer_State_t state;
    char                path[AUDIO_PLAYER_PATH_MAX];
    uint32_t            file_size;
    uint32_t            bytes_sent;
    uint32_t            decode_sec;
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

AudioPlayer_State_t AudioPlayer_GetState(void);
/** @brief Snapshot of the current track; decoder fields are only valid while not idle. */
void AudioPlayer_GetStatus(AudioPlayer_Status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H */
