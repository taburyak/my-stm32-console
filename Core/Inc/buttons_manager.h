#ifndef BUTTONS_MANAGER_H
#define BUTTONS_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>

/**
 * @brief System Button Identifiers (KEY_0 .. KEY_3 from main.h)
 */
typedef enum {
    BUTTON_KEY0 = 0, /* KEY_0_Pin (GPIOF, Pin 6) */
    BUTTON_KEY1 = 1, /* KEY_1_Pin (GPIOF, Pin 7) */
    BUTTON_KEY2 = 2, /* KEY_2_Pin (GPIOF, Pin 8) */
    BUTTON_KEY3 = 3, /* KEY_3_Pin (GPIOF, Pin 9) */
    BUTTON_COUNT
} Button_ID_t;

/**
 * @brief Button Event Types
 */
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESSED,        /* Physical press (after debounce) */
    BUTTON_EVENT_SHORT_RELEASE,  /* Short release (< 1000 ms) */
    BUTTON_EVENT_LONG_PRESS      /* Long press (>= 1000 ms) */
} Button_Event_t;

/**
 * @brief Button event message structure
 */
typedef struct {
    Button_ID_t    id;
    Button_Event_t event;
} Button_Message_t;

/* --- Function Prototypes --- */

/**
 * @brief Initial initialization of the button module
 */
void Buttons_Init(void);

/**
 * @brief EXTI interrupt handler (called from HAL_GPIO_EXTI_Callback)
 * @param GPIO_Pin Pin that generated the interrupt
 */
void Buttons_EXTI_Callback(uint16_t GPIO_Pin);

/**
 * @brief Non-blocking periodic processing of hold and debounce timers
 */
void Buttons_Process(void);

/**
 * @brief Get the last event from the event buffer
 * @param msg Pointer to the structure for saving the event
 * @return true - event read, false - no new events
 */
bool Buttons_GetEvent(Button_Message_t *msg);


#ifdef __cplusplus
}
#endif

#endif /* BUTTONS_MANAGER_H */
