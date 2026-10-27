#ifndef BEEPER_H
#define BEEPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>

/* --- Адаптивне визначення піна зуммера з main.h --- */
#ifndef BEEPER_Pin
  #ifdef BEEP_Pin
    #define BEEPER_Pin       BEEP_Pin
    #define BEEPER_GPIO_Port BEEP_GPIO_Port
  #else
    #define BEEPER_Pin       GPIO_PIN_8
    #define BEEPER_GPIO_Port GPIOF
  #endif
#endif

/* --- Прототипи функцій --- */

/**
 * @brief Початкова ініціалізація модуля біпера (вимкнення звуку)
 */
void Beeper_Init(void);

/**
 * @brief Пряме увімкнення/вимкнення зуммера
 */
void Beeper_On(void);
void Beeper_Off(void);

/**
 * @brief Видати звуковий сигнал заданої тривалості (неблокуючий)
 * @param duration_ms Тривалість сигналу в мілісекундах
 */
void Beeper_Beep(uint16_t duration_ms);

/**
 * @brief Короткий звуковий клік (15 мс) для акустичного відгуку кнопок
 */
void Beeper_Click(void);

/**
 * @brief Звуковий сигнал успішного завантаження / старту
 */
void Beeper_BootMelody(void);

/**
 * @brief Звуковий сигнал помилки / системного збою
 */
void Beeper_ErrorAlert(void);

/**
 * @brief Неблокуюча обробка таймерів відключення звуку та паттернів
 *        (викликається періодично у StartDefaultTask)
 */
void Beeper_Process(void);

void Beeper_SetMuted(bool muted);
bool Beeper_IsMuted(void);
void Beeper_PlayTone(uint16_t freq_hz, uint16_t duration_ms);

#define BEEPER_SEQ_MAX_NOTES 64U

/* freq_hz > 0 sounds (fixed-pitch buzzer), 0 is a rest */
typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
} Beeper_Note_t;

/* Copies up to BEEPER_SEQ_MAX_NOTES notes and plays them from Beeper_Process() */
void Beeper_PlaySequence(const Beeper_Note_t *notes, uint32_t count, bool loop);
void Beeper_StopSequence(void);
bool Beeper_IsSequencePlaying(void);

#ifdef __cplusplus
}
#endif

#endif /* BEEPER_H */
