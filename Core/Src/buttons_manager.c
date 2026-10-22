#include "buttons_manager.h"

#define DEBOUNCE_TIME_MS   50    /* Debounce time (ms) */
#define LONG_PRESS_TIME_MS 1000  /* Long press threshold (ms) */
#define EVENT_QUEUE_SIZE   16    /* Event buffer size */

/* Internal state structure for each button */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    bool          is_pressed;
    bool          long_press_reported;
    uint32_t      press_start_tick;
    uint32_t      last_exti_tick;
} Button_State_t;

/* State array for 4 buttons */
static Button_State_t buttons[BUTTON_COUNT] = {
    [BUTTON_KEY0] = {KEY_0_GPIO_Port, KEY_0_Pin, false, false, 0, 0},
    [BUTTON_KEY1] = {KEY_1_GPIO_Port, KEY_1_Pin, false, false, 0, 0},
    [BUTTON_KEY2] = {KEY_2_GPIO_Port, KEY_2_Pin, false, false, 0, 0},
    [BUTTON_KEY3] = {KEY_3_GPIO_Port, KEY_3_Pin, false, false, 0, 0}
};

/* Circular event buffer */
static Button_Message_t event_queue[EVENT_QUEUE_SIZE];
static uint8_t queue_head = 0;
static uint8_t queue_tail = 0;

/**
 * @brief Internal function to add event to buffer
 */
static void Push_Event(Button_ID_t id, Button_Event_t event) {
    uint8_t next_head = (queue_head + 1) % EVENT_QUEUE_SIZE;
    if (next_head != queue_tail) { /* Overflow check */
        event_queue[queue_head].id = id;
        event_queue[queue_head].event = event;
        queue_head = next_head;
    }
}

void Buttons_Init(void) {
    queue_head = 0;
    queue_tail = 0;

    for (int i = 0; i < BUTTON_COUNT; i++) {
        buttons[i].is_pressed = false;
        buttons[i].long_press_reported = false;
        buttons[i].press_start_tick = 0;
        buttons[i].last_exti_tick = 0;
    }
}

void Buttons_EXTI_Callback(uint16_t GPIO_Pin) {
    uint32_t current_tick = HAL_GetTick();

    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (buttons[i].pin == GPIO_Pin) {
            /* Simple debounce protection at EXTI level */
            if ((current_tick - buttons[i].last_exti_tick) > DEBOUNCE_TIME_MS) {
                buttons[i].last_exti_tick = current_tick;
            }
            break;
        }
    }
}

void Buttons_Process(void) {
    uint32_t current_tick = HAL_GetTick();

    for (int i = 0; i < BUTTON_COUNT; i++) {
        /* Read current physical pin state (Active-Low: RESET = pressed) */
        bool pin_active = (HAL_GPIO_ReadPin(buttons[i].port, buttons[i].pin) == GPIO_PIN_RESET);

        /* Capture press (if debounce passed) */
        if (pin_active && !buttons[i].is_pressed) {
            if ((current_tick - buttons[i].last_exti_tick) >= DEBOUNCE_TIME_MS) {
                buttons[i].is_pressed = true;
                buttons[i].press_start_tick = current_tick;
                buttons[i].long_press_reported = false;

                Push_Event((Button_ID_t)i, BUTTON_EVENT_PRESSED);
            }
        }
        /* Waiting for long press */
        else if (pin_active && buttons[i].is_pressed && !buttons[i].long_press_reported) {
            if ((current_tick - buttons[i].press_start_tick) >= LONG_PRESS_TIME_MS) {
                buttons[i].long_press_reported = true;

                Push_Event((Button_ID_t)i, BUTTON_EVENT_LONG_PRESS);
            }
        }
        /* Capture release */
        else if (!pin_active && buttons[i].is_pressed) {
            if (!buttons[i].long_press_reported) {
                Push_Event((Button_ID_t)i, BUTTON_EVENT_SHORT_RELEASE);
            }
            buttons[i].is_pressed = false;
            buttons[i].long_press_reported = false;
        }
    }
}

bool Buttons_GetEvent(Button_Message_t *msg) {
    if (queue_tail == queue_head) {
        return false; /* Buffer is empty */
    }

    *msg = event_queue[queue_tail];
    queue_tail = (queue_tail + 1) % EVENT_QUEUE_SIZE;
    return true;
}

