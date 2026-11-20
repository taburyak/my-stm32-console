#include "audio_service.h"
#include "audio_player.h"
#include "beeper.h"
#include "vs1053.h"
#include "AT24Cxx.h"
#include <stdio.h>
#include <string.h>

/* OS EEPROM area 0x00..0x7F: 0x00 UI config (ui_manager.c), 0x20 this block (one 8-byte page) */
#define AUDIO_EEPROM_ADDR   0x20U
#define AUDIO_EEPROM_MAGIC  0xC1U

#define AUDIO_DEFAULT_VOLUME  70U
#define AUDIO_BASS_FREQ_10HZ  10U  /* SB_FREQLIMIT: boost below 100 Hz */
#define AUDIO_TREBLE_FREQ_KHZ 3U   /* ST_FREQLIMIT: shelf above 3 kHz */

typedef struct
{
    uint8_t magic;
    uint8_t volume;
    uint8_t bass;
    uint8_t treble_biased; /* treble + 8 */
    uint8_t sys_mode;
    uint8_t play_mode;     /* AudioPlayer_Mode_t */
    uint8_t reserved;
    uint8_t check;
} AudioSvc_Config_t;

_Static_assert(sizeof(AudioSvc_Config_t) == 8U, "Audio EEPROM block must fit one AT24C02 page");

static AudioSvc_Config_t s_cfg;
static AudioSvc_Config_t s_saved;
static bool              s_muted = false;

static uint8_t cfg_check(const AudioSvc_Config_t *c)
{
    const uint8_t *p = (const uint8_t *)c;
    uint8_t x = 0x5AU;
    for (uint32_t i = 0U; i < sizeof(*c) - 1U; i++)
    {
        x ^= p[i];
    }
    return x;
}

static bool sys_sound_hook(Beeper_SysSound_t snd)
{
    if (s_cfg.sys_mode != (uint8_t)AUDIO_SYS_SPEAKER)
    {
        return false;
    }
    return AudioPlayer_PlaySysSound((uint8_t)snd);
}

void AudioSvc_Apply(void)
{
    if (!VS1053_IsPresent())
    {
        return;
    }
    /* 0.5 dB attenuation steps: 100% = 0 dB, 1% = -49.5 dB, 0% or mute = analog power-down level */
    uint8_t att = (s_cfg.volume == 0U || s_muted) ? 0xFEU : (uint8_t)(AUDIO_VOLUME_MAX - s_cfg.volume);
    VS1053_SetVolume(att, att);

    int8_t   treble = (int8_t)((int32_t)s_cfg.treble_biased - 8);
    uint16_t bass   = (uint16_t)((((uint16_t)treble & 0xFU) << 12) | (AUDIO_TREBLE_FREQ_KHZ << 8) |
                                 ((uint16_t)s_cfg.bass << 4) | AUDIO_BASS_FREQ_10HZ);
    VS1053_WriteReg(VS1053_SCI_BASS, bass);
}

void AudioSvc_Init(void)
{
    AudioSvc_Config_t c;
    bool valid = (AT24Cxx_Read(AUDIO_EEPROM_ADDR, &c, sizeof(c)) == HAL_OK) &&
                 (c.magic == AUDIO_EEPROM_MAGIC) && (c.check == cfg_check(&c)) &&
                 (c.volume <= AUDIO_VOLUME_MAX) && (c.bass <= (uint8_t)AUDIO_BASS_MAX) &&
                 (c.treble_biased <= (uint8_t)(AUDIO_TREBLE_MAX + 8)) && (c.sys_mode < (uint8_t)AUDIO_SYS_COUNT) &&
                 (c.play_mode < (uint8_t)AUDIO_MODE_COUNT);
    if (!valid)
    {
        memset(&c, 0, sizeof(c));
        c.magic         = AUDIO_EEPROM_MAGIC;
        c.volume        = AUDIO_DEFAULT_VOLUME;
        c.treble_biased = 8U;
        c.sys_mode      = Beeper_IsMuted() ? (uint8_t)AUDIO_SYS_OFF : (uint8_t)AUDIO_SYS_SPEAKER;
    }
    s_cfg   = c;
    s_saved = valid ? c : (AudioSvc_Config_t){0};

    Beeper_SetMuted(s_cfg.sys_mode == (uint8_t)AUDIO_SYS_OFF);
    Beeper_SetSysSoundHook(sys_sound_hook);
    AudioPlayer_SetMode((AudioPlayer_Mode_t)s_cfg.play_mode);
    AudioSvc_Apply();
    printf("[AUDIO] Settings %s: volume=%u%% bass=%u treble=%d sys=%s\r\n", valid ? "loaded" : "defaults",
           (unsigned int)s_cfg.volume, (unsigned int)s_cfg.bass, (int)AudioSvc_GetTreble(),
           AudioSvc_SysModeName(AudioSvc_GetSysMode()));
}

void AudioSvc_Save(void)
{
    s_cfg.check = cfg_check(&s_cfg);
    if (memcmp(&s_cfg, &s_saved, sizeof(s_cfg)) != 0 &&
        AT24Cxx_Write(AUDIO_EEPROM_ADDR, &s_cfg, sizeof(s_cfg)) == HAL_OK)
    {
        s_saved = s_cfg;
    }
}

uint8_t AudioSvc_GetVolume(void)
{
    return s_cfg.volume;
}

void AudioSvc_SetVolume(uint8_t percent)
{
    s_cfg.volume = (percent > AUDIO_VOLUME_MAX) ? (uint8_t)AUDIO_VOLUME_MAX : percent;
    s_muted      = false;
    AudioSvc_Apply();
}

bool AudioSvc_IsMuted(void)
{
    return s_muted;
}

void AudioSvc_SetMuted(bool muted)
{
    s_muted = muted;
    AudioSvc_Apply();
}

int8_t AudioSvc_GetBass(void)
{
    return (int8_t)s_cfg.bass;
}

void AudioSvc_SetBass(int8_t db)
{
    s_cfg.bass = (uint8_t)((db < 0) ? 0 : ((db > AUDIO_BASS_MAX) ? AUDIO_BASS_MAX : db));
    AudioSvc_Apply();
}

int8_t AudioSvc_GetTreble(void)
{
    return (int8_t)((int32_t)s_cfg.treble_biased - 8);
}

void AudioSvc_SetTreble(int8_t steps)
{
    if (steps < AUDIO_TREBLE_MIN) steps = AUDIO_TREBLE_MIN;
    if (steps > AUDIO_TREBLE_MAX) steps = AUDIO_TREBLE_MAX;
    s_cfg.treble_biased = (uint8_t)(steps + 8);
    AudioSvc_Apply();
}

AudioSvc_SysMode_t AudioSvc_GetSysMode(void)
{
    return (AudioSvc_SysMode_t)s_cfg.sys_mode;
}

void AudioSvc_SetSysMode(AudioSvc_SysMode_t mode)
{
    if (mode >= AUDIO_SYS_COUNT)
    {
        return;
    }
    s_cfg.sys_mode = (uint8_t)mode;
    Beeper_SetMuted(mode == AUDIO_SYS_OFF);
}

const char *AudioSvc_SysModeName(AudioSvc_SysMode_t mode)
{
    static const char *const s_names[AUDIO_SYS_COUNT] = { "Off", "Buzzer", "Speaker" };
    return (mode < AUDIO_SYS_COUNT) ? s_names[mode] : "?";
}

void AudioSvc_CyclePlayMode(void)
{
    s_cfg.play_mode = (uint8_t)((s_cfg.play_mode + 1U) % (uint8_t)AUDIO_MODE_COUNT);
    AudioPlayer_SetMode((AudioPlayer_Mode_t)s_cfg.play_mode);
    AudioSvc_Save();
}
