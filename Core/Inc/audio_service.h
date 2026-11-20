#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_VOLUME_MAX   100U
#define AUDIO_BASS_MAX     15     /* dB boost below 100 Hz */
#define AUDIO_TREBLE_MIN   (-8)   /* x 1.5 dB above 3 kHz */
#define AUDIO_TREBLE_MAX   7

typedef enum
{
    AUDIO_SYS_OFF = 0,   /* system sounds muted */
    AUDIO_SYS_BUZZER,    /* always the on-board buzzer */
    AUDIO_SYS_SPEAKER,   /* VS1053 when idle, buzzer while music plays or without the module */
    AUDIO_SYS_COUNT
} AudioSvc_SysMode_t;

/**
 * @brief Load audio settings from EEPROM (0x20), apply them to the VS1053 and route system sounds.
 *        Call after UIManager_Init(): without a saved block the legacy "sound muted" flag picks the mode.
 */
void AudioSvc_Init(void);

/** @brief Re-push volume and tone to the decoder (after VS1053_Init or a soft reset). */
void AudioSvc_Apply(void);

/** @brief Persist the current settings; skips the EEPROM write when nothing changed. */
void AudioSvc_Save(void);

uint8_t            AudioSvc_GetVolume(void);
void               AudioSvc_SetVolume(uint8_t percent);
int8_t             AudioSvc_GetBass(void);
void               AudioSvc_SetBass(int8_t db);
int8_t             AudioSvc_GetTreble(void);
void               AudioSvc_SetTreble(int8_t steps);
AudioSvc_SysMode_t AudioSvc_GetSysMode(void);
void               AudioSvc_SetSysMode(AudioSvc_SysMode_t mode);
const char        *AudioSvc_SysModeName(AudioSvc_SysMode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_SERVICE_H */
