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

/* Extended Device Type for Gamepad */
#define HID_GAMEPAD  ((HID_TypeTypeDef)3U)

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
    int8_t  delta_x;    /* Relative X move (-127..127) */
    int8_t  delta_y;    /* Relative Y move (-127..127) */
    bool    btn_left;   /* Left button state */
    bool    btn_right;  /* Right button state */
    bool    btn_middle; /* Middle button state */
} USB_MouseEvent_t;

/**
 * @brief USB HID Gamepad State Structure (D-Input / STK-7024X)
 */
typedef struct {
    int8_t   left_x;    /* Left stick X (-127..+127) */
    int8_t   left_y;    /* Left stick Y (-127..+127) */
    uint8_t  hat;       /* D-Pad Hat switch (0=Up, 2=Right, 4=Down, 6=Left, 0x0F=Center) */
    uint16_t buttons;   /* Bitmask of buttons 0..14 */
    bool     dpad_up;
    bool     dpad_down;
    bool     dpad_left;
    bool     dpad_right;
    bool     btn_a;     /* Button A (Cross / A) */
    bool     btn_b;     /* Button B (Circle / B) */
    bool     btn_x;     /* Button X (Square / X) */
    bool     btn_y;     /* Button Y (Triangle / Y) */
    bool     btn_l1;    /* Left shoulder button */
    bool     btn_r1;    /* Right shoulder button */
    bool     btn_l2;    /* Left trigger */
    bool     btn_r2;    /* Right trigger */
    bool     btn_start; /* Start button */
    bool     btn_select;/* Select button */
} USB_GamepadState_t;

/**
 * @brief Diagnostic structure for debugging without UART
 */
typedef struct {
    uint8_t  last_host_id;      /* Last HOST_USER_... state (2 = CLASS_ACTIVE) */
    uint8_t  detected_dev_type; /* 0=Unknown, 1=Mouse, 2=Keyboard, 3=Gamepad */
    uint8_t  hid_state;         /* Internal HID_Handle->state (6 = POLL) */
    uint8_t  last_scancode;     /* Last raw HID key scancode */
    uint8_t  last_ascii;        /* Last decoded ASCII char */
    int16_t  mouse_x;           /* Current mouse X coordinate */
    int16_t  mouse_y;           /* Current mouse Y coordinate */
    int8_t   mouse_dx;          /* Last relative X delta */
    int8_t   mouse_dy;          /* Last relative Y delta */
    uint8_t  mouse_btns;        /* Bitmask: bit0=L, bit1=R, bit2=M */
    uint8_t  raw_report[8];     /* Raw HID report bytes for debugging */
    uint32_t irq_event_count;   /* Total HID packets received */
} USB_DebugInfo_t;

/* Global debug struct visible in Debugger Watch / Live Expressions */
extern volatile USB_DebugInfo_t g_usb_dbg;

/* --- Public Function Prototypes --- */
void USBHostManager_Init(void);
void USBHostManager_TaskPoll(void);

bool USBHostManager_IsKeyboardReady(void);
bool USBHostManager_IsMouseReady(void);
bool USBHostManager_IsGamepadReady(void);

bool USBHostManager_GetKey(char *ch);
bool USBHostManager_GetKeyEvent(USB_KeyboardEvent_t *event);
bool USBHostManager_GetMouseState(USB_MouseEvent_t *state);
bool USBHostManager_GetGamepadState(USB_GamepadState_t *state);

void USBHostManager_ProcessUserCallback(USBH_HandleTypeDef *phost, uint8_t id);
void USBHostManager_ProcessHIDCallback(USBH_HandleTypeDef *phost);

#ifdef __cplusplus
}
#endif

#endif /* USB_HOST_MANAGER_H */
