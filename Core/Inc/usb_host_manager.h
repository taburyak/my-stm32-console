#ifndef USB_HOST_MANAGER_H
#define USB_HOST_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "usbh_core.h"
#include "usbh_hid.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief USB HID Keyboard Event Structure
 */
typedef struct {
    uint8_t scancode;   /* USB HID Scancode */
    uint8_t ascii;      /* Translated ASCII Character */
    bool    is_pressed; /* Key state */
} USB_KeyboardEvent_t;

/**
 * @brief USB HID Mouse Event Structure
 */
typedef struct {
    int16_t x;          /* Absolute Cursor X Position (0..319) */
    int16_t y;          /* Absolute Cursor Y Position (0..239) */
    int8_t  delta_x;    /* Relative X move */
    int8_t  delta_y;    /* Relative Y move */
    bool    btn_left;   /* Left button state */
    bool    btn_right;  /* Right button state */
    bool    btn_middle; /* Middle button state */
} USB_MouseEvent_t;

/* --- Public Function Prototypes --- */

/**
 * @brief Initialize USB Host Manager subsystem
 */
void USBHostManager_Init(void);

/**
 * @brief Dynamic status checks for LVGL and System UI
 */
bool USBHostManager_IsKeyboardReady(void);
bool USBHostManager_IsMouseReady(void);

/**
 * @brief Keyboard Event Readers
 */
bool USBHostManager_GetKey(char *ch);
bool USBHostManager_GetKeyEvent(USB_KeyboardEvent_t *event);

/**
 * @brief Mouse Event Reader
 */
bool USBHostManager_GetMouseEvent(USB_MouseEvent_t *event);

/* --- Callbacks from USB Host library --- */
void USBHostManager_ProcessUserCallback(USBH_HandleTypeDef *phost, uint8_t id);
void USBHostManager_ProcessHIDCallback(USBH_HandleTypeDef *phost);

#ifdef __cplusplus
}
#endif

#endif /* USB_HOST_MANAGER_H */
