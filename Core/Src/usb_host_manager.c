#include "usb_host_manager.h"
#include "usbh_hid_keybd.h"
#include "usbh_hid_mouse.h"
#include "status_led.h"
#include "beeper.h"
#include <stdio.h>
#include <string.h>

#define KBD_QUEUE_SIZE 16

extern USBH_HandleTypeDef hUsbHostFS;

static volatile bool is_kbd_ready = false;
static volatile bool is_mouse_ready = false;
static uint32_t connect_start_tick = 0;
static bool cold_boot_recovered = false;

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

void USBHostManager_Init(void)
{
    /* Do NOT wipe state if USB IRQ already fired during early boot! */
    if (g_usb_dbg.last_host_id == 0)
    {
        is_kbd_ready = false;
        is_mouse_ready = false;
        g_usb_dbg.detected_dev_type = 0;
        g_usb_dbg.hid_state = 0;
    }

    kbd_head = 0;
    kbd_tail = 0;

    current_mouse_state.x = 160;
    current_mouse_state.y = 120;
    current_mouse_state.delta_x = 0;
    current_mouse_state.delta_y = 0;
    current_mouse_state.btn_left = false;
    current_mouse_state.btn_right = false;
    current_mouse_state.btn_middle = false;

    g_usb_dbg.mouse_x = 160;
    g_usb_dbg.mouse_y = 120;
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

bool USBHostManager_GetMouseState(USB_MouseEvent_t *state)
{
    if (!is_mouse_ready || state == NULL)
    {
        return false;
    }
    *state = current_mouse_state;
    return true;
}

/**
 * @brief Reliable determination of the HID device type
 */
static HID_TypeTypeDef Get_Reliable_HID_Type(USBH_HandleTypeDef *phost)
{
    HID_TypeTypeDef dev_type = USBH_HID_GetDeviceType(phost);
    if (dev_type == HID_KEYBOARD || dev_type == HID_MOUSE)
    {
        return dev_type;
    }

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

    uint8_t idx = phost->device.current_interface;
    if (idx < USBH_MAX_NUM_INTERFACES)
    {
        uint8_t proto = phost->device.CfgDesc.Itf_Desc[idx].bInterfaceProtocol;
        if (proto == HID_KEYBRD_BOOT_CODE) return HID_KEYBOARD;
        if (proto == HID_MOUSE_BOOT_CODE)  return HID_MOUSE;
    }

    return HID_UNKNOWN;
}

void USBHostManager_ProcessUserCallback(USBH_HandleTypeDef *phost, uint8_t id)
{
    g_usb_dbg.last_host_id = id;

    switch (id)
    {
    case HOST_USER_CONNECTION:
        connect_start_tick = HAL_GetTick();
        break;

    case HOST_USER_DISCONNECTION:
        is_kbd_ready = false;
        is_mouse_ready = false;
        g_usb_dbg.detected_dev_type = 0;
        g_usb_dbg.hid_state = 0;
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
                current_mouse_state.x = 160;
                current_mouse_state.y = 120;
                g_usb_dbg.mouse_x = 160;
                g_usb_dbg.mouse_y = 120;
                Beeper_BootMelody();
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

    /* Copy raw report bytes from FIFO/pData for inspection */
    if (phost->pActiveClass != NULL && phost->pActiveClass->pData != NULL)
    {
        HID_HandleTypeDef *HID_Handle = (HID_HandleTypeDef *)phost->pActiveClass->pData;
        if (HID_Handle->pData != NULL)
        {
            uint8_t copy_len = (HID_Handle->length < 8U) ? (uint8_t)HID_Handle->length : 8U;
            for (uint8_t i = 0; i < copy_len; i++)
            {
                g_usb_dbg.raw_report[i] = HID_Handle->pData[i];
            }
        }
    }

    if (dev_type == HID_KEYBOARD)
    {
        is_kbd_ready = true;
        is_mouse_ready = false;

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
    else if (dev_type == HID_MOUSE)
    {
        is_mouse_ready = true;
        is_kbd_ready = false;

        /* Call ST parser to flush internal FIFO */
        HID_MOUSE_Info_TypeDef *m_info = USBH_HID_GetMouseInfo(phost);
        if (m_info != NULL)
        {
            uint8_t b0 = g_usb_dbg.raw_report[0];
            uint8_t b1 = g_usb_dbg.raw_report[1];
            uint8_t b2 = g_usb_dbg.raw_report[2];
            uint8_t b3 = g_usb_dbg.raw_report[3];
            uint8_t b4 = g_usb_dbg.raw_report[4];

            int16_t dx = 0;
            int16_t dy = 0;
            uint8_t btns = b0;

            /* Detect 12-bit/16-bit Report format vs standard 8-bit Boot format:
             * In 12-bit mice: Byte 1 = X_low, Byte 2 low nibble = X_high,
             * Byte 2 high nibble + Byte 3 = Y_12bit!
             * In 16-bit mice: Byte 1..2 = X_16bit, Byte 3..4 = Y_16bit.
             */
            HID_HandleTypeDef *HID_Handle = (HID_HandleTypeDef *)phost->pActiveClass->pData;

            /* Check if Byte 2 low nibble is sign extension of X (0x0 or 0xF) while Byte 3 holds Y */
            if (HID_Handle->length >= 5 && ((b2 & 0x0F) == 0x00 || (b2 & 0x0F) == 0x0F) && (b3 != 0 || (b2 & 0xF0) != 0))
            {
                /* 12-bit packed X/Y format (very common in modern optical mice):
                 * X = 12-bit signed: b1 | ((b2 & 0x0F) << 8)
                 * Y = 12-bit signed: (b2 >> 4) | (b3 << 4)
                 */
                int16_t raw_x12 = (int16_t)(b1 | ((b2 & 0x0F) << 8));
                if (raw_x12 & 0x0800) raw_x12 |= 0xF000; /* Sign extend 12->16 bit */

                int16_t raw_y12 = (int16_t)((b2 >> 4) | (b3 << 4));
                if (raw_y12 & 0x0800) raw_y12 |= 0xF000; /* Sign extend 12->16 bit */

                dx = raw_x12;
                dy = raw_y12;
            }
            else if (HID_Handle->length >= 6 && (b2 == 0x00 || b2 == 0xFF) && (b4 == 0x00 || b4 == 0xFF))
            {
                /* 16-bit little-endian X/Y format (Byte 1-2 = X, Byte 3-4 = Y) */
                dx = (int16_t)((uint16_t)b1 | ((uint16_t)b2 << 8));
                dy = (int16_t)((uint16_t)b3 | ((uint16_t)b4 << 8));
            }
            else
            {
                /* Standard 8-bit Boot Protocol format (Byte 1 = X, Byte 2 = Y) */
                dx = (int8_t)b1;
                dy = (int8_t)b2;
            }

            /* Clamp deltas to int8_t range */
            if (dx > 127)  dx = 127;
            if (dx < -127) dx = -127;
            if (dy > 127)  dy = 127;
            if (dy < -127) dy = -127;

            current_mouse_state.delta_x = (int8_t)dx;
            current_mouse_state.delta_y = (int8_t)dy;

            int16_t new_x = current_mouse_state.x + dx;
            int16_t new_y = current_mouse_state.y + dy;

            if (new_x < 0)   new_x = 0;
            if (new_x > 319) new_x = 319;
            if (new_y < 0)   new_y = 0;
            if (new_y > 239) new_y = 239;

            current_mouse_state.x = new_x;
            current_mouse_state.y = new_y;

            current_mouse_state.btn_left   = ((btns & 0x01) != 0);
            current_mouse_state.btn_right  = ((btns & 0x02) != 0);
            current_mouse_state.btn_middle = ((btns & 0x04) != 0);

            g_usb_dbg.mouse_x  = new_x;
            g_usb_dbg.mouse_y  = new_y;
            g_usb_dbg.mouse_dx = (int8_t)dx;
            g_usb_dbg.mouse_dy = (int8_t)dy;
            g_usb_dbg.mouse_btns = btns & 0x07;
        }
    }
}

void USBHostManager_TaskPoll(void)
{
    /* 1. Cold-Boot Recovery:
     * If a device was plugged in before power-on, LCD/POST blocking delays
     * can stall the initial USB enumeration in State 4 (HOST_USER_CONNECTION).
     * If connected for > 1200ms without reaching HOST_CLASS, trigger ReEnumerate! */
    if (hUsbHostFS.device.is_connected && hUsbHostFS.gState != HOST_CLASS)
    {
        if (!cold_boot_recovered && (HAL_GetTick() - connect_start_tick) > 1200U)
        {
            cold_boot_recovered = true;
            USBH_ReEnumerate(&hUsbHostFS);
            return;
        }
    }
    else if (!hUsbHostFS.device.is_connected)
    {
        cold_boot_recovered = false;
    }

    /* 2. State machine polling & USBH_HID_IDLE bypass */
    if (hUsbHostFS.pActiveClass != NULL && hUsbHostFS.pActiveClass->pData != NULL)
    {
        HID_HandleTypeDef *HID_Handle = (HID_HandleTypeDef *)hUsbHostFS.pActiveClass->pData;

        if (hUsbHostFS.gState == HOST_CLASS)
        {
            /* Ensure we also mark device ready if callback happened before init */
            if (!is_kbd_ready && !is_mouse_ready)
            {
                USBHostManager_ProcessUserCallback(&hUsbHostFS, HOST_USER_CLASS_ACTIVE);
            }

            if (HID_Handle->state == USBH_HID_IDLE)
            {
                HID_Handle->state = USBH_HID_SYNC;
            }

            g_usb_dbg.hid_state = (uint8_t)HID_Handle->state;
            USBH_LL_NotifyURBChange(&hUsbHostFS);
        }
        else
        {
            g_usb_dbg.hid_state = (uint8_t)HID_Handle->state;
            USBH_LL_NotifyURBChange(&hUsbHostFS);
        }
    }
}
