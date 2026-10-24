#include "usb_host_manager.h"
#include "usbh_hid_keybd.h"
#include "usbh_hid_mouse.h"
#include "status_led.h"
#include "beeper.h"
#include <stdio.h>
#include <string.h>

#define KBD_QUEUE_SIZE 16

extern USBH_HandleTypeDef hUsbHostFS;

static volatile bool is_kbd_ready     = false;
static volatile bool is_mouse_ready   = false;
static volatile bool is_gamepad_ready = false;

static uint32_t connect_start_tick  = 0;
static bool     cold_boot_recovered = false;

/* Custom Direct-Polling state for Non-Boot HID Gamepad (STK-7024X) */
static uint8_t  gpad_pipe      = 0U;
static uint8_t  gpad_ep_addr   = 0U;
static uint16_t gpad_ep_mps    = 8U;
static uint8_t  gpad_poll_ms   = 10U;
static uint32_t gpad_last_poll = 0U;
static bool     gpad_waiting_urb = false;
static uint8_t  gpad_rx_buf[64] __attribute__((aligned(4)));

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

/* Current Gamepad State */
static USB_GamepadState_t current_gamepad_state = {0};

void USBHostManager_Init(void)
{
    if (g_usb_dbg.last_host_id == 0)
    {
        is_kbd_ready = false;
        is_mouse_ready = false;
        is_gamepad_ready = false;
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

    memset(&current_gamepad_state, 0, sizeof(current_gamepad_state));
    current_gamepad_state.hat = 0x0F;

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

bool USBHostManager_IsGamepadReady(void)
{
    return is_gamepad_ready;
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

bool USBHostManager_GetGamepadState(USB_GamepadState_t *state)
{
    if (!is_gamepad_ready || state == NULL)
    {
        return false;
    }
    *state = current_gamepad_state;
    return true;
}

/**
 * @brief Parse raw D-Input Gamepad report (STK-7024X)
 */
static void Parse_Gamepad_Report(const uint8_t *buf, uint16_t len)
{
    if (len < 3U) return;

    uint8_t copy_len = (len < 8U) ? (uint8_t)len : 8U;
    for (uint8_t i = 0; i < copy_len; i++)
    {
        g_usb_dbg.raw_report[i] = buf[i];
    }

    /* --- 1. BUTTONS (Byte buf[0]) --- */
    bool btn_a = ((buf[0] & 0x04) != 0);  /* Button A */
    bool btn_b = ((buf[0] & 0x02) != 0);  /* Button B */
    bool btn_x = ((buf[0] & 0x08) != 0);  /* Button X */
    bool btn_y = ((buf[0] & 0x01) != 0);  /* Button Y */
    bool btn_l1 = ((buf[0] & 0x10) != 0); /* L1 / LB (біт 4) */
    bool btn_r1 = ((buf[0] & 0x20) != 0); /* R1 / RB (біт 5) */
    bool btn_l2 = ((buf[0] & 0x40) != 0); /* L2 / LT (біт 6) */
    bool btn_r2 = ((buf[0] & 0x80) != 0); /* R2 / RT (біт 7) */

    /* --- 2. D-PAD CROSS (Byte buf[2]) --- */
    uint8_t hat = buf[2] & 0x0F; /* The least significant 4 bits (0=Up, 2=Right, 4=Down, 6=Left, 0x0F=Neutral) */

    bool up    = (hat == 0 || hat == 1 || hat == 7);
    bool right = (hat == 1 || hat == 2 || hat == 3);
    bool down  = (hat == 3 || hat == 4 || hat == 5);
    bool left  = (hat == 5 || hat == 6 || hat == 7);

    /* Edge detection: push LVGL navigation keys on press! */
    if (up && !current_gamepad_state.dpad_up)       enqueue_key(0x52, 0, true); /* Up Arrow */
    if (down && !current_gamepad_state.dpad_down)   enqueue_key(0x51, 0, true); /* Down Arrow */
    if (left && !current_gamepad_state.dpad_left)   enqueue_key(0x50, 0, true); /* Left Arrow */
    if (right && !current_gamepad_state.dpad_right) enqueue_key(0x4F, 0, true); /* Right Arrow */
    if (btn_a && !current_gamepad_state.btn_a)      enqueue_key(0x28, '\r', true); /* Enter (LV_KEY_ENTER) */
    if (btn_b && !current_gamepad_state.btn_b)      enqueue_key(0x29, 0, true);    /* Escape (LV_KEY_ESC) */
    if (btn_x && !current_gamepad_state.btn_x)      enqueue_key(0x2A, 0, true);    /* Backspace (LV_KEY_BACKSPACE) */
    if (btn_y && !current_gamepad_state.btn_y)      enqueue_key(0x2B, 0, true);    /* Tab (LV_KEY_TAB) */
    if (btn_l1 && !current_gamepad_state.btn_l1)    enqueue_key(0x52, 0, true);    /* Up Arrow (LV_KEY_UP) */
    if (btn_r1 && !current_gamepad_state.btn_r1)    enqueue_key(0x4F, 0, true);    /* Right Arrow (LV_KEY_RIGHT) */
    // if (btn_l2 && !current_gamepad_state.btn_l2)    enqueue_key(0x2D, 0, true);    /* Custom: L2 */
    // if (btn_r2 && !current_gamepad_state.btn_r2)    enqueue_key(0x2E, 0, true);    /* Custom: R2 */

    /* --- 4. SAVING THE CURRENT STATE --- */
    // current_gamepad_state.left_x     = lx;
    // current_gamepad_state.left_y     = ly;
    // current_gamepad_state.buttons    = btns;
    current_gamepad_state.hat        = hat;
    current_gamepad_state.dpad_up    = up;
    current_gamepad_state.dpad_down  = down;
    current_gamepad_state.dpad_left  = left;
    current_gamepad_state.dpad_right = right;
    current_gamepad_state.btn_a      = btn_a;
    current_gamepad_state.btn_b      = btn_b;
    current_gamepad_state.btn_x      = btn_x;
    current_gamepad_state.btn_y      = btn_y;
    current_gamepad_state.btn_l1     = btn_l1;
    current_gamepad_state.btn_r1     = btn_r1;
    current_gamepad_state.btn_l2     = btn_l2;
    current_gamepad_state.btn_r2     = btn_r2;
}

/**
 * @brief Reliable determination of the HID device type
 */
static HID_TypeTypeDef Get_Reliable_HID_Type(USBH_HandleTypeDef *phost)
{
    if (is_gamepad_ready)
    {
        return HID_GAMEPAD;
    }

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
        if (is_gamepad_ready && gpad_pipe != 0U)
        {
            (void)USBH_ClosePipe(phost, gpad_pipe);
            (void)USBH_FreePipe(phost, gpad_pipe);
            gpad_pipe = 0U;
        }
        is_gamepad_ready = false;
        gpad_waiting_urb = false;
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
                is_gamepad_ready = false;
                Beeper_BootMelody();
            }
            else if (dev_type == HID_MOUSE)
            {
                is_mouse_ready = true;
                is_kbd_ready = false;
                is_gamepad_ready = false;
                current_mouse_state.x = 160;
                current_mouse_state.y = 120;
                g_usb_dbg.mouse_x = 160;
                g_usb_dbg.mouse_y = 120;
                Beeper_BootMelody();
            }
            else if (dev_type == HID_GAMEPAD)
            {
                is_gamepad_ready = true;
                is_kbd_ready = false;
                is_mouse_ready = false;
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
            HID_HandleTypeDef *HID_Handle = (HID_HandleTypeDef *)phost->pActiveClass->pData;
            uint8_t b0 = g_usb_dbg.raw_report[0];
            uint8_t b1 = g_usb_dbg.raw_report[1];
            uint8_t b2 = g_usb_dbg.raw_report[2];
            uint8_t b3 = g_usb_dbg.raw_report[3];

            int16_t dx = 0;
            int16_t dy = 0;

            /* 16-bit mouse vs 8-bit Boot mouse */
            if (HID_Handle->length > 4U)
            {
                dx = (int8_t)b1;
                dy = (int8_t)b3;
            }
            else
            {
                dx = (int8_t)b1;
                dy = (int8_t)b2;
            }

            current_mouse_state.delta_x = (int8_t)dx;
            current_mouse_state.delta_y = (int8_t)dy;

            /* Calculate absolute coordinates on 320x240 screen */
            int16_t new_x = current_mouse_state.x + dx;
            int16_t new_y = current_mouse_state.y + dy;

            if (new_x < 0)   new_x = 0;
            if (new_x > 319) new_x = 319;
            if (new_y < 0)   new_y = 0;
            if (new_y > 239) new_y = 239;

            current_mouse_state.x = new_x;
            current_mouse_state.y = new_y;

            /* Buttons: bit0 = Left, bit1 = Right, bit2 = Middle */
            current_mouse_state.btn_left   = ((b0 & 0x01) != 0);
            current_mouse_state.btn_right  = ((b0 & 0x02) != 0);
            current_mouse_state.btn_middle = ((b0 & 0x04) != 0);

            /* Update diagnostic variables for HUD and Debugger */
            g_usb_dbg.mouse_x  = new_x;
            g_usb_dbg.mouse_y  = new_y;
            g_usb_dbg.mouse_dx = (int8_t)dx;
            g_usb_dbg.mouse_dy = (int8_t)dy;
            g_usb_dbg.mouse_btns = b0 & 0x07;
        }
    }
}

void USBHostManager_TaskPoll(void)
{
    /* 1. Custom Non-Boot HID Gamepad Activation & Polling (STK-7024X):
     * When a D-Input Gamepad (Class 0x03, SubClass 0x00, Protocol 0x00) is plugged in,
     * ST's Boot-only USBH_HID_InterfaceInit rejects it and puts gState into HOST_ABORT_STATE.
     * We intercept this state and open the Interrupt IN pipe directly! */
    if (hUsbHostFS.device.is_connected &&
        hUsbHostFS.gState == HOST_ABORT_STATE &&
        !is_gamepad_ready &&
        hUsbHostFS.device.CfgDesc.Itf_Desc[0].bInterfaceClass == USB_HID_CLASS &&
        hUsbHostFS.device.CfgDesc.Itf_Desc[0].bInterfaceProtocol == 0x00U)
    {
        /* Find Interrupt IN endpoint on Interface 0 */
        USBH_InterfaceDescTypeDef *itf = &hUsbHostFS.device.CfgDesc.Itf_Desc[0];
        for (uint8_t i = 0; i < itf->bNumEndpoints; i++)
        {
            if ((itf->Ep_Desc[i].bEndpointAddress & 0x80U) != 0U)
            {
                gpad_ep_addr = itf->Ep_Desc[i].bEndpointAddress;
                gpad_ep_mps  = itf->Ep_Desc[i].wMaxPacketSize;
                if (gpad_ep_mps > sizeof(gpad_rx_buf)) gpad_ep_mps = sizeof(gpad_rx_buf);
                gpad_poll_ms = itf->Ep_Desc[i].bInterval;
                if (gpad_poll_ms < 10U) gpad_poll_ms = 10U;

                gpad_pipe = USBH_AllocPipe(&hUsbHostFS, gpad_ep_addr);
                (void)USBH_OpenPipe(&hUsbHostFS,
                                    gpad_pipe,
                                    gpad_ep_addr,
                                    hUsbHostFS.device.address,
                                    hUsbHostFS.device.speed,
                                    USB_EP_TYPE_INTR,
                                    gpad_ep_mps);
                (void)USBH_LL_SetToggle(&hUsbHostFS, gpad_pipe, 0U);

                is_gamepad_ready = true;
                is_kbd_ready     = false;
                is_mouse_ready   = false;
                gpad_waiting_urb = false;
                gpad_last_poll   = HAL_GetTick();

                g_usb_dbg.detected_dev_type = (uint8_t)HID_GAMEPAD;
                g_usb_dbg.last_host_id      = HOST_USER_CLASS_ACTIVE;
                g_usb_dbg.hid_state         = 6U; /* POLL */
                Beeper_BootMelody();
                break;
            }
        }
    }

    /* Direct Polling loop for Gamepad Interrupt IN pipe */
    if (is_gamepad_ready && gpad_pipe != 0U)
    {
        if (!gpad_waiting_urb)
        {
            if ((HAL_GetTick() - gpad_last_poll) >= gpad_poll_ms)
            {
                gpad_last_poll   = HAL_GetTick();
                gpad_waiting_urb = true;
                (void)USBH_InterruptReceiveData(&hUsbHostFS, gpad_rx_buf, (uint8_t)gpad_ep_mps, gpad_pipe);
            }
        }
        else
        {
            USBH_URBStateTypeDef urb = USBH_LL_GetURBState(&hUsbHostFS, gpad_pipe);
            if (urb == USBH_URB_DONE)
            {
                gpad_waiting_urb = false;
                g_usb_dbg.irq_event_count++;
                Parse_Gamepad_Report(gpad_rx_buf, gpad_ep_mps);
            }
            else if (urb == USBH_URB_NOTREADY || urb == USBH_URB_NYET ||
                     urb == USBH_URB_ERROR || urb == USBH_URB_STALL ||
                     (HAL_GetTick() - gpad_last_poll) >= 20U)
            {
                gpad_waiting_urb = false;
            }
        }
        return;
    }

    /* 2. Cold-Boot Recovery for Keyboard/Mouse */
    if (hUsbHostFS.device.is_connected &&
        hUsbHostFS.gState != HOST_CLASS &&
        hUsbHostFS.gState != HOST_ABORT_STATE)
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

    /* 3. State machine polling & USBH_HID_IDLE bypass for Keyboard/Mouse */
    if (hUsbHostFS.pActiveClass != NULL && hUsbHostFS.pActiveClass->pData != NULL)
    {
        HID_HandleTypeDef *HID_Handle = (HID_HandleTypeDef *)hUsbHostFS.pActiveClass->pData;

        if (hUsbHostFS.gState == HOST_CLASS)
        {
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
