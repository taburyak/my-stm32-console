#include "beeper.h"

/* Internal state of the beeper timers */
static uint32_t beep_end_tick = 0;
static bool     is_beeping    = false;
static bool     is_muted      = false;

/* Internal state for patterns (melodies/alarms) */
typedef enum {
    PATTERN_NONE = 0,
    PATTERN_BOOT,
    PATTERN_ERROR,
    PATTERN_SEQUENCE
} Beeper_Pattern_t;

static volatile Beeper_Pattern_t current_pattern = PATTERN_NONE;
static uint8_t  pattern_step = 0;
static uint32_t pattern_next_tick = 0;

static Beeper_Note_t s_seq[BEEPER_SEQ_MAX_NOTES];
static uint8_t       s_seq_len  = 0;
static bool          s_seq_loop = false;

void Beeper_Init(void)
{
    Beeper_Off();
    is_beeping = false;
    current_pattern = PATTERN_NONE;
}

void Beeper_On(void)
{
    if (is_muted) return;
    HAL_GPIO_WritePin(BEEPER_GPIO_Port, BEEPER_Pin, GPIO_PIN_SET);
}

void Beeper_Off(void)
{
    HAL_GPIO_WritePin(BEEPER_GPIO_Port, BEEPER_Pin, GPIO_PIN_RESET);
}

void Beeper_Beep(uint16_t duration_ms)
{
    if (duration_ms == 0) return;

    Beeper_On();
    is_beeping = true;
    beep_end_tick = HAL_GetTick() + duration_ms;
}

void Beeper_Click(void)
{
    Beeper_Beep(15); /* Short 15 ms response time for buttons */
}

void Beeper_BootMelody(void)
{
    current_pattern = PATTERN_BOOT;
    pattern_step = 0;
    pattern_next_tick = HAL_GetTick();
}

void Beeper_ErrorAlert(void)
{
    current_pattern = PATTERN_ERROR;
    pattern_step = 0;
    pattern_next_tick = HAL_GetTick();
}

void Beeper_Process(void)
{
    uint32_t current_tick = HAL_GetTick();

    /* 1. Handling a single beep */
    if (is_beeping)
    {
        if (current_tick >= beep_end_tick)
        {
            Beeper_Off();
            is_beeping = false;
        }
    }

    /* 2. Handling sound patterns (if active) */
    if (current_pattern != PATTERN_NONE && current_tick >= pattern_next_tick)
    {
        switch (current_pattern)
        {
            case PATTERN_BOOT:
                /* Two-tone greeting tone: 40 ms on -> 40 ms pause -> 60 ms on */
                if (pattern_step == 0)
                {
                    Beeper_On();
                    pattern_next_tick = current_tick + 40;
                    pattern_step++;
                }
                else if (pattern_step == 1)
                {
                    Beeper_Off();
                    pattern_next_tick = current_tick + 40;
                    pattern_step++;
                }
                else if (pattern_step == 2)
                {
                    Beeper_On();
                    pattern_next_tick = current_tick + 60;
                    pattern_step++;
                }
                else
                {
                    Beeper_Off();
                    current_pattern = PATTERN_NONE;
                }
                break;

            case PATTERN_ERROR:
                /* Alert: 3 short pulses of 80ms */
                if (pattern_step < 5)
                {
                    if (pattern_step % 2 == 0)
                    {
                        Beeper_On();
                        pattern_next_tick = current_tick + 80;
                    }
                    else
                    {
                        Beeper_Off();
                        pattern_next_tick = current_tick + 80;
                    }
                    pattern_step++;
                }
                else
                {
                    Beeper_Off();
                    current_pattern = PATTERN_NONE;
                }
                break;

            case PATTERN_SEQUENCE:
                if (pattern_step >= s_seq_len)
                {
                    if (!s_seq_loop || (s_seq_len == 0U))
                    {
                        Beeper_Off();
                        current_pattern = PATTERN_NONE;
                        break;
                    }
                    pattern_step = 0;
                }
                if (s_seq[pattern_step].freq_hz != 0U)
                {
                    Beeper_On();
                }
                else
                {
                    Beeper_Off();
                }
                pattern_next_tick = current_tick + s_seq[pattern_step].duration_ms;
                pattern_step++;
                break;

            default:
                current_pattern = PATTERN_NONE;
                break;
        }
    }
}

void Beeper_SetMuted(bool muted)
{
    is_muted = muted;
    if (muted)
    {
        Beeper_Off();
        is_beeping = false;
        current_pattern = PATTERN_NONE;
    }
}

bool Beeper_IsMuted(void)
{
    return is_muted;
}

void Beeper_PlayTone(uint16_t freq_hz, uint16_t duration_ms)
{
    if (is_muted || duration_ms == 0U || freq_hz == 0U)
    {
        return;
    }
    /* Active transducer on PG7 is driven via non-blocking timer duration */
    Beeper_Beep(duration_ms);
}

void Beeper_PlaySequence(const Beeper_Note_t *notes, uint32_t count, bool loop)
{
    current_pattern = PATTERN_NONE;
    Beeper_Off();
    if (is_muted || (notes == NULL) || (count == 0U))
    {
        return;
    }
    if (count > BEEPER_SEQ_MAX_NOTES)
    {
        count = BEEPER_SEQ_MAX_NOTES;
    }
    for (uint32_t i = 0U; i < count; i++)
    {
        s_seq[i] = notes[i];
    }
    s_seq_len         = (uint8_t)count;
    s_seq_loop        = loop;
    pattern_step      = 0;
    pattern_next_tick = HAL_GetTick();
    current_pattern   = PATTERN_SEQUENCE;
}

void Beeper_StopSequence(void)
{
    if (current_pattern == PATTERN_SEQUENCE)
    {
        current_pattern = PATTERN_NONE;
        Beeper_Off();
    }
}

bool Beeper_IsSequencePlaying(void)
{
    return current_pattern == PATTERN_SEQUENCE;
}
