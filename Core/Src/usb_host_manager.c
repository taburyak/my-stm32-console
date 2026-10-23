#include "usb_host_manager.h"
#include "usbh_hid_keybd.h"
#include "usbh_hid_mouse.h"
#include "status_led.h"
#include "beeper.h"
#include <stdio.h>

#define KBD_QUEUE_SIZE 16

static volatile bool is_kbd_ready = false;
static volatile bool is_mouse_ready = false;

/* Global debug info for Watch window & LCD HUD */
volatile USB_DebugInfo_t g_usb_dbg = {0};

/* Keyboard Queue */
static USB_KeyboardEvent_t kbd_queue[KBD_QUEUE_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;

/* Current Mouse State */
static USB_MouseEvent_t current_mouse_state = {
    .x = 160, .y = 120,
    .delta_x = 0, .delta_y = 0,
    .btn_left = false, .btn_right = false, .btn_middle = false
};
static volatile bool mouse_updated = false;

void USBHostManager_Init(void)
{
    is_kbd_ready = false;
    is_mouse_ready = false;
    kbd_head = 0;
    kbd_tail = 0;
    current_mouse_state.x = 160;
    current_mouse_state.y = 120;
    mouse_updated = false;

    g_usb_dbg.last_host_id = 0;
    g_usb_dbg.detected_dev_type = 0;
    g_usb_dbg.last_scancode = 0;
    g_usb_dbg.last_ascii = 0;
    g_usb_dbg.irq_event_count = 0;
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
        if (event.is_pressed && event.ascii != 0)
        {
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

/**
 * @brief Надійне визначення типу HID-пристрою (навіть якщо Report Descriptor нестандартний)
 */
static HID_TypeTypeDef Get_Reliable_HID_Type(USBH_HandleTypeDef *phost)
{
    /* 1. Спочатку питаємо стандартну функцію ST */
    HID_TypeTypeDef dev_type = USBH_HID_GetDeviceType(phost);
    if (dev_type == HID_KEYBOARD || dev_type == HID_MOUSE)
    {
        return dev_type;
    }

    /* 2. Якщо вона повернула 255 (HID_UNKNOWN), дивимось на покажчик Init у HID_Handle */
    if (phost->pActiveClass != NULL && phost->pActiveClass->pData != NULL)
    {
        HID_HandleTypeDef *HID_Handle = (HID_HandleTypeDef *)phost->pActiveClass->pData;
        if (HID_Handle->Init == USBH_HID_KeybdInit)
        {
            return HID_KEYBOARD;
        }
        if (HID_Handle->Init == USBH_HID_MouseInit)
        {
            return HID_MOUSE;
        }
    }

    /* 3. Резервна перевірка за протоколом першого інтерфейсу (1 = KBD, 2 = MOUSE) */
    uint8_t proto = phost->device.CfgDesc.Itf_Desc[0].bInterfaceProtocol;
    if (proto == HID_KEYBRD_BOOT_CODE) return HID_KEYBOARD;
    if (proto == HID_MOUSE_BOOT_CODE)  return HID_MOUSE;

    return HID_UNKNOWN;
}

void USBHostManager_ProcessUserCallback(USBH_HandleTypeDef *phost, uint8_t id)
{
    g_usb_dbg.last_host_id = id;

    switch (id)
    {
    case HOST_USER_DISCONNECTION:
        is_kbd_ready = false;
        is_mouse_ready = false;
        g_usb_dbg.detected_dev_type = 0;
        Beeper_Click();
        break;

    case HOST_USER_CLASS_ACTIVE:
        {
            HID_TypeTypeDef dev_type = Get_Reliable_HID_Type(phost);
            g_usb_dbg.detected_dev_type = (uint8_t)dev_type;

            if (dev_type == HID_KEYBOARD)
            {
                is_kbd_ready = true;
                is_mouse_ready = false;
                Beeper_BootMelody();
            }
            else if (dev_type == HID_MOUSE)
            {
                is_mouse_ready = true;
                is_kbd_ready = false;
                Beeper_Click();
            }
            else
            {
                Beeper_ErrorAlert();
            }
        }
        break;

    default:
        break;
    }
}

void USBHostManager_ProcessHIDCallback(USBH_HandleTypeDef *phost)
{
    g_usb_dbg.irq_event_count++;
    HID_TypeTypeDef dev_type = Get_Reliable_HID_Type(phost);
    g_usb_dbg.detected_dev_type = (uint8_t)dev_type;

    if (dev_type == HID_KEYBOARD)
    {
        is_kbd_ready = true;
        HID_KEYBD_Info_TypeDef *k_info = USBH_HID_GetKeybdInfo(phost);
        if (k_info != NULL)
        {
            uint8_t scancode = k_info->keys[0];
            if (scancode != 0)
            {
                uint8_t ascii = USBH_HID_GetASCIICode(k_info);

                g_usb_dbg.last_scancode = scancode;
                g_usb_dbg.last_ascii    = ascii;

                enqueue_key(scancode, ascii, true);
            }
        }
    }
}
