#include "usb_host_manager.h"
#include "usbh_hid_keybd.h"
#include "usbh_hid_mouse.h"
#include "status_led.h"
#include <stdio.h>

#define KBD_QUEUE_SIZE 16

static volatile bool is_kbd_ready = false;
static volatile bool is_mouse_ready = false;

/* Keyboard Queue */
static USB_KeyboardEvent_t kbd_queue[KBD_QUEUE_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;

/* Current Mouse State & Absolute Coordinates */
static USB_MouseEvent_t current_mouse_state = {
    .x = 160, .y = 120, /* Centered at start */
    .delta_x = 0, .delta_y = 0,
    .btn_left = false, .btn_right = false, .btn_middle = false
};
static volatile bool mouse_updated = false;

void USBHostManager_Init(void) {
    is_kbd_ready = false;
    is_mouse_ready = false;
    kbd_head = 0;
    kbd_tail = 0;
    current_mouse_state.x = 160;
    current_mouse_state.y = 120;
    mouse_updated = false;
}

bool USBHostManager_IsKeyboardReady(void)
{
    return is_kbd_ready;
}

bool USBHostManager_IsMouseReady(void)
{
    return is_mouse_ready;
}

static void enqueue_key(uint8_t scancode, uint8_t ascii, bool is_pressed)
{
    uint8_t next_head = (kbd_head + 1) % KBD_QUEUE_SIZE;
    if (next_head != kbd_tail)
    {
        kbd_queue[kbd_head].scancode = scancode;
        kbd_queue[kbd_head].ascii = ascii;
        kbd_queue[kbd_head].is_pressed = is_pressed;
        kbd_head = next_head;
    }
}

bool USBHostManager_GetKey(char *ch)
{
    USB_KeyboardEvent_t event;
    while (USBHostManager_GetKeyEvent(&event))
    {
        if (event.is_pressed && event.ascii != 0) {
            if (ch) *ch = (char)event.ascii;
            return true;
        }
    }
    return false;
}

bool USBHostManager_GetKeyEvent(USB_KeyboardEvent_t *event)
{
    if (kbd_tail == kbd_head)
    {
        return false;
    }
    if (event)
    {
        *event = kbd_queue[kbd_tail];
    }
    kbd_tail = (kbd_tail + 1) % KBD_QUEUE_SIZE;
    return true;
}

bool USBHostManager_GetMouseEvent(USB_MouseEvent_t *event)
{
    if (!mouse_updated)
    {
        return false;
    }
    if (event)
    {
        *event = current_mouse_state;
    }
    mouse_updated = false;
    return true;
}

void USBHostManager_ProcessUserCallback(USBH_HandleTypeDef *phost, uint8_t id)
{
    switch (id)
    {
    case HOST_USER_DISCONNECTION:
        is_kbd_ready = false;
        is_mouse_ready = false;
        printf("[USB Host] Device Disconnected\r\n");
        break;

    case HOST_USER_CLASS_ACTIVE:
        if (USBH_HID_GetDeviceType(phost) == HID_KEYBOARD)
        {
            is_kbd_ready = true;
            printf("[USB Host] Keyboard Ready ⌨️\r\n");
        } else if (USBH_HID_GetDeviceType(phost) == HID_MOUSE)
        {
            is_mouse_ready = true;
            printf("[USB Host] Mouse Ready 🖱️\r\n");
        }
        break;

    default:
        break;
    }
}

void USBHostManager_ProcessHIDCallback(USBH_HandleTypeDef *phost)
{
    HID_TypeTypeDef dev_type = USBH_HID_GetDeviceType(phost);

    if (dev_type == HID_KEYBOARD)
    {
        HID_KEYBD_Info_TypeDef *k_info = USBH_HID_GetKeybdInfo(phost);
        if (k_info != NULL && k_info->keys[0] != 0)
        {
            uint8_t scancode = k_info->keys[0];
            uint8_t ascii    = USBH_HID_GetASCIICode(k_info);
            enqueue_key(scancode, ascii, true);
        }
    }
    else if (dev_type == HID_MOUSE)
    {
        HID_MOUSE_Info_TypeDef *m_info = USBH_HID_GetMouseInfo(phost);
        if (m_info != NULL)
        {
            current_mouse_state.delta_x = m_info->x;
            current_mouse_state.delta_y = m_info->y;

            /* Clamp absolute coordinates to 320x240 screen limits */
            int16_t new_x = current_mouse_state.x + m_info->x;
            int16_t new_y = current_mouse_state.y + m_info->y;

            if (new_x < 0) new_x = 0;
            if (new_x > 319) new_x = 319;
            if (new_y < 0) new_y = 0;
            if (new_y > 239) new_y = 239;

            current_mouse_state.x = new_x;
            current_mouse_state.y = new_y;

            current_mouse_state.btn_left   = (m_info->buttons[0] != 0);
            current_mouse_state.btn_right  = (m_info->buttons[1] != 0);
            current_mouse_state.btn_middle = (m_info->buttons[2] != 0);

            mouse_updated = true;
        }
    }
}
