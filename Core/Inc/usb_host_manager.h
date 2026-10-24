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

/**
 * @brief Diagnostic structure for debugging without UART
 */
typedef struct {
    uint8_t  last_host_id;      /* Last HOST_USER_... state (2 = CLASS_ACTIVE) */
    uint8_t  detected_dev_type; /* 0=Unknown, 1=Mouse, 2=Keyboard */
    uint8_t  hid_state;         /* Internal HID_Handle->state */
    uint8_t  last_scancode;     /* Last raw HID key scancode */
    uint8_t  last_ascii;        /* Last decoded ASCII char */
    uint32_t irq_event_count;   /* Total HID packets received */
} USB_DebugInfo_t;

/* Global debug struct visible in Debugger Watch / Live Expressions */
extern volatile USB_DebugInfo_t g_usb_dbg;

/* --- Public Function Prototypes --- */
void USBHostManager_Init(void);
void USBHostManager_TaskPoll(void);

bool USBHostManager_IsKeyboardReady(void);
bool USBHostManager_IsMouseReady(void);

bool USBHostManager_GetKey(char *ch);
bool USBHostManager_GetKeyEvent(USB_KeyboardEvent_t *event);
bool USBHostManager_GetMouseEvent(USB_MouseEvent_t *event);

void USBHostManager_ProcessUserCallback(USBH_HandleTypeDef *phost, uint8_t id);
void USBHostManager_ProcessHIDCallback(USBH_HandleTypeDef *phost);

#ifdef __cplusplus
}
#endif

#endif /* USB_HOST_MANAGER_H */