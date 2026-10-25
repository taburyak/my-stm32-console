#include "beeper.h"

/* Internal state of the beeper timers */
static uint32_t beep_end_tick = 0;
static bool     is_beeping    = false;
static bool     is_muted      = false;

/* Internal state for patterns (melodies/alarms) */
typedef enum {
    PATTERN_NONE = 0,
    PATTERN_BOOT,
    PATTERN_ERROR
} Beeper_Pattern_t;

static Beeper_Pattern_t current_pattern = PATTERN_NONE;
static uint8_t  pattern_step = 0;
static uint32_t pattern_next_tick = 0;

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
