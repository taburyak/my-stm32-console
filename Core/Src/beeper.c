#include "beeper.h"

/* Внутрішній стан таймерів біпера */
static uint32_t beep_end_tick = 0;
static bool     is_beeping    = false;

/* Внутрішній стан для паттернів (мелодій/тривоги) */
typedef enum {
    PATTERN_NONE = 0,
    PATTERN_BOOT,
    PATTERN_ERROR
} Beeper_Pattern_t;

static Beeper_Pattern_t current_pattern = PATTERN_NONE;
static uint8_t  pattern_step = 0;
static uint32_t pattern_next_tick = 0;

void Beeper_Init(void) {
    Beeper_Off();
    is_beeping = false;
    current_pattern = PATTERN_NONE;
}

void Beeper_On(void) {
    HAL_GPIO_WritePin(BEEPER_GPIO_Port, BEEPER_Pin, GPIO_PIN_SET);
}

void Beeper_Off(void) {
    HAL_GPIO_WritePin(BEEPER_GPIO_Port, BEEPER_Pin, GPIO_PIN_RESET);
}

void Beeper_Beep(uint16_t duration_ms) {
    if (duration_ms == 0) return;

    Beeper_On();
    is_beeping = true;
    beep_end_tick = HAL_GetTick() + duration_ms;
}

void Beeper_Click(void) {
    Beeper_Beep(15); /* Короткий 15 мс відгук для кнопок */
}

void Beeper_BootMelody(void) {
    current_pattern = PATTERN_BOOT;
    pattern_step = 0;
    pattern_next_tick = HAL_GetTick();
}

void Beeper_ErrorAlert(void) {
    current_pattern = PATTERN_ERROR;
    pattern_step = 0;
    pattern_next_tick = HAL_GetTick();
}

void Beeper_Process(void) {
    uint32_t current_tick = HAL_GetTick();

    /* 1. Обробка одиночного біпу */
    if (is_beeping) {
        if (current_tick >= beep_end_tick) {
            Beeper_Off();
            is_beeping = false;
        }
    }

    /* 2. Обробка звукових паттернів (якщо задіяно) */
    if (current_pattern != PATTERN_NONE && current_tick >= pattern_next_tick) {
        switch (current_pattern) {
            case PATTERN_BOOT:
                /* Двотональний привітальний сигнал: 40мс вкл -> 40мс пауза -> 60мс вкл */
                if (pattern_step == 0) {
                    Beeper_On();
                    pattern_next_tick = current_tick + 40;
                    pattern_step++;
                } else if (pattern_step == 1) {
                    Beeper_Off();
                    pattern_next_tick = current_tick + 40;
                    pattern_step++;
                } else if (pattern_step == 2) {
                    Beeper_On();
                    pattern_next_tick = current_tick + 60;
                    pattern_step++;
                } else {
                    Beeper_Off();
                    current_pattern = PATTERN_NONE;
                }
                break;

            case PATTERN_ERROR:
                /* Тривога: 3 коротких імпульси по 80мс */
                if (pattern_step < 5) {
                    if (pattern_step % 2 == 0) {
                        Beeper_On();
                        pattern_next_tick = current_tick + 80;
                    } else {
                        Beeper_Off();
                        pattern_next_tick = current_tick + 80;
                    }
                    pattern_step++;
                } else {
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
