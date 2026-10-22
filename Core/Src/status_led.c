#include "status_led.h"

/**
 * @brief Helper function to get the port and pin by LED identifier
 */
static void Get_LED_GPIO(StatusLED_t led, GPIO_TypeDef **port, uint16_t *pin) {
    switch (led) {
        case STATUS_LED_HEARTBEAT:
            *port = LD0_GPIO_Port;
            *pin  = LD0_Pin;
            break;
        case STATUS_LED_DISK:
            *port = LD1_GPIO_Port;
            *pin  = LD1_Pin;
            break;
        case STATUS_LED_ERROR:
            *port = LD2_GPIO_Port;
            *pin  = LD2_Pin;
            break;
        default:
            *port = NULL;
            *pin  = 0;
            break;
    }
}

void StatusLED_Init(void) {
    /* Turn off all LEDs by default (Active-Low -> SET = OFF) */
    StatusLED_Set(STATUS_LED_HEARTBEAT, STATUS_LED_OFF);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    StatusLED_Set(STATUS_LED_ERROR, STATUS_LED_OFF);
}


void StatusLED_Set(StatusLED_t led, StatusLED_State_t state) {
    GPIO_TypeDef *port = NULL;
    uint16_t pin = 0;

    Get_LED_GPIO(led, &port, &pin);

    if (port != NULL) {
        /* Active-Low: ON -> RESET (0V), OFF -> SET (3.3V) */
        GPIO_PinState pinState = (state == STATUS_LED_ON) ? GPIO_PIN_RESET : GPIO_PIN_SET;
        HAL_GPIO_WritePin(port, pin, pinState);
    }
}

void StatusLED_Toggle(StatusLED_t led) {
    GPIO_TypeDef *port = NULL;
    uint16_t pin = 0;

    Get_LED_GPIO(led, &port, &pin);

    if (port != NULL) {
        HAL_GPIO_TogglePin(port, pin);
    }
}