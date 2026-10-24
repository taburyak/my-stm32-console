#ifndef STATUS_LED_H
#define STATUS_LED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/**
 * @brief Status LED Identifiers
 */
typedef enum {
    STATUS_LED_HEARTBEAT = 0,  /* LD0 (PE3) - System Alive */
    STATUS_LED_DISK      = 1,  /* LD1 (PE4) - SD Card Activity */
    STATUS_LED_ERROR     = 2   /* LD2 (PG9) - System Fault */
} StatusLED_t;

/**
 * @brief Logical state of the LED
 */
typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_ON  = 1
} StatusLED_State_t;

/* --- Function Prototypes --- */

/**
 * @brief Initialization and default disabling of all LEDs
 */
void StatusLED_Init(void);


/**
 * @brief Set LED state (ON / OFF)
 */
void StatusLED_Set(StatusLED_t led, StatusLED_State_t state);

/**
 * @brief Invert LED state
 */
void StatusLED_Toggle(StatusLED_t led);


#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_H */
