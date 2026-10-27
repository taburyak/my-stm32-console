#include "usb_host_manager.h"
#include "status_led.h"
#include "beeper.h"
#include "app_loader.h"
#include <stdio.h>
#include <string.h>

#define KBD_QUEUE_SIZE 16

static volatile bool is_kbd_ready     = false;
static volatile bool is_mouse_ready   = false;
static volatile bool is_gamepad_ready = false;

/* Global debug info for Watch window & LCD HUD */
volatile USB_DebugInfo_t g_usb_dbg = {0};

/* Keyboard Queue */
static USB_KeyboardEvent_t kbd_queue[KBD_QUEUE_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;

/* Previous keyboard report for key-press edge detection */
static hid_keyboard_report_t prev_kbd_report = {0};

/* Current Mouse State */
static USB_MouseEvent_t current_mouse_state = {
    .x = 160, .y = 120,
    .delta_x = 0, .delta_y = 0,
    .btn_left = false, .btn_right = false, .btn_middle = false
};

/* Current Gamepad State */
static USB_GamepadState_t current_gamepad_state = {0};

/* Lookup table: HID Scancode to ASCII [unshifted, shifted] */
static uint8_t const keycode2ascii[128][2] = { HID_KEYCODE_TO_ASCII };

static void enqueue_key(uint8_t scancode, uint8_t ascii, bool is_pressed)
{
    /* Do not enqueue LVGL navigation keys while a native Sandbox app owns the screen */
    if (AppLoader_IsRunning())
    {
        return;
    }

    uint8_t next_head = (kbd_head + 1U) % KBD_QUEUE_SIZE;
    if (next_head != kbd_tail)
    {
        kbd_queue[kbd_head].scancode   = scancode;
        kbd_queue[kbd_head].ascii      = ascii;
        kbd_queue[kbd_head].is_pressed = is_pressed;
        kbd_head = next_head;
    }
}

void USBHostManager_FlushInputQueues(void)
{
    kbd_head = 0;
    kbd_tail = 0;
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
}

void USBHostManager_Init(void)
{
    is_kbd_ready     = false;
    is_mouse_ready   = false;
    is_gamepad_ready = false;

    kbd_head = 0;
    kbd_tail = 0;
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));

    current_mouse_state.x          = 160;
    current_mouse_state.y          = 120;
    current_mouse_state.delta_x    = 0;
    current_mouse_state.delta_y    = 0;
    current_mouse_state.btn_left   = false;
    current_mouse_state.btn_right  = false;
    current_mouse_state.btn_middle = false;

    memset(&current_gamepad_state, 0, sizeof(current_gamepad_state));
    current_gamepad_state.hat = 0x0F;

    g_usb_dbg.last_host_id      = 0U;
    g_usb_dbg.detected_dev_type = USB_DEV_TYPE_NONE;
    g_usb_dbg.hid_state         = 0U;
    g_usb_dbg.mouse_x           = 160;
    g_usb_dbg.mouse_y           = 120;

    /* STM32F407 OTG_FS Host hardware transceiver setup:
     * Enable internal FS transceiver power (PWRDWN) and bypass VBUS sensing
     * (NOVBUSSENS) since PA9 VBUS is not routed on this board. */
    USB_OTG_FS->GCCFG |= USB_OTG_GCCFG_PWRDWN | USB_OTG_GCCFG_NOVBUSSENS;
    USB_OTG_FS->GCCFG &= ~(USB_OTG_GCCFG_VBUSBSEN | USB_OTG_GCCFG_VBUSASEN);

    /* Initialize TinyUSB Host stack on Root Hub Port 0 */
    tusb_rhport_init_t host_init = {
        .role  = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL
    };
    tusb_init(0, &host_init);
}

void USBHostManager_TaskPoll(void)
{
    /* Non-blocking TinyUSB Host task dispatcher (called every 5ms from InputAudioTask) */
    tuh_task_ext(0, false);
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
    kbd_tail = (kbd_tail + 1U) % KBD_QUEUE_SIZE;
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

/* ==========================================================================
 * Report Parsers (Keyboard, 8/16-bit Mouse, STK-7024X Gamepad)
 * ========================================================================== */

static bool is_key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
    for (uint8_t i = 0; i < 6U; i++)
    {
        if (report->keycode[i] == keycode) return true;
    }
    return false;
}

uint32_t USBHostManager_GetKeyboardGameMask(void)
{
    if (!is_kbd_ready)
    {
        return 0U;
    }

    uint32_t mask = 0U;
    for (uint8_t i = 0; i < 6U; i++)
    {
        uint8_t kc = prev_kbd_report.keycode[i];
        if (kc == 0U) continue;

        if (kc == 0x50U || kc == 0x04U) mask |= APP_KEY_LEFT;  /* Left Arrow  or 'A' */
        if (kc == 0x4FU || kc == 0x07U) mask |= APP_KEY_RIGHT; /* Right Arrow or 'D' */
        if (kc == 0x52U || kc == 0x1AU) mask |= APP_KEY_UP;    /* Up Arrow    or 'W' */
        if (kc == 0x51U || kc == 0x16U) mask |= APP_KEY_DOWN;  /* Down Arrow  or 'S' */
        if (kc == 0x2CU || kc == 0x28U) mask |= APP_KEY_A;     /* Spacebar or Enter -> Start / Rotate */
        if (kc == 0x1BU || kc == 0x1DU) mask |= APP_KEY_B;     /* 'X' or 'Z' -> Secondary / Hard Drop */
        if (kc == 0x29U)                mask |= APP_KEY_EXIT;  /* ESC -> Exit game */
    }
    return mask;
}

static void Parse_Keyboard_Report(uint8_t const *buf, uint16_t len)
{
    if (len < sizeof(hid_keyboard_report_t)) return;

    hid_keyboard_report_t const *kbd_report = (hid_keyboard_report_t const *)buf;
    bool is_shift = (kbd_report->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

    for (uint8_t i = 0; i < 6U; i++)
    {
        uint8_t scancode = kbd_report->keycode[i];
        if (scancode != 0U && !is_key_in_report(&prev_kbd_report, scancode))
        {
            uint8_t ascii = 0U;
            if (scancode < 128U)
            {
                ascii = keycode2ascii[scancode][is_shift ? 1 : 0];
            }

            g_usb_dbg.last_scancode = scancode;
            g_usb_dbg.last_ascii    = ascii;

            enqueue_key(scancode, ascii, true);
        }
    }

    prev_kbd_report = *kbd_report;
}

static void Parse_Mouse_Report(uint8_t const *buf, uint16_t len)
{
    if (len < 3U) return;

    uint8_t b0 = buf[0];
    uint8_t b1 = buf[1];
    uint8_t b2 = buf[2];
    uint8_t b3 = (len > 3U) ? buf[3] : 0U;

    int16_t dx = 0;
    int16_t dy = 0;

    /* 16-bit mouse vs 8-bit Boot mouse */
    if (len > 4U)
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
    current_mouse_state.btn_left   = ((b0 & 0x01U) != 0U);
    current_mouse_state.btn_right  = ((b0 & 0x02U) != 0U);
    current_mouse_state.btn_middle = ((b0 & 0x04U) != 0U);

    /* Update diagnostic variables for HUD and Debugger */
    g_usb_dbg.mouse_x    = new_x;
    g_usb_dbg.mouse_y    = new_y;
    g_usb_dbg.mouse_dx   = (int8_t)dx;
    g_usb_dbg.mouse_dy   = (int8_t)dy;
    g_usb_dbg.mouse_btns = b0 & 0x07U;
}

static void Parse_Gamepad_Report(uint8_t const *buf, uint16_t len)
{
    if (len < 3U) return;

    /* 1. BUTTONS (Byte buf[0]) */
    bool btn_a  = ((buf[0] & 0x04U) != 0U); /* Button A */
    bool btn_b  = ((buf[0] & 0x02U) != 0U); /* Button B */
    bool btn_x  = ((buf[0] & 0x08U) != 0U); /* Button X */
    bool btn_y  = ((buf[0] & 0x01U) != 0U); /* Button Y */
    bool btn_l1 = ((buf[0] & 0x10U) != 0U); /* L1 / LB */
    bool btn_r1 = ((buf[0] & 0x20U) != 0U); /* R1 / RB */
    bool btn_l2 = ((buf[0] & 0x40U) != 0U); /* L2 / LT */
    bool btn_r2 = ((buf[0] & 0x80U) != 0U); /* R2 / RT */

    /* 2. D-PAD CROSS (Byte buf[2]) */
    uint8_t hat = buf[2] & 0x0FU; /* 0=Up, 2=Right, 4=Down, 6=Left, 0x0F=Neutral */

    bool up    = (hat == 0U || hat == 1U || hat == 7U);
    bool right = (hat == 1U || hat == 2U || hat == 3U);
    bool down  = (hat == 3U || hat == 4U || hat == 5U);
    bool left  = (hat == 5U || hat == 6U || hat == 7U);

    /* Edge detection: push LVGL navigation keys on press (ignored if Sandbox app is active) */
    if (up && !current_gamepad_state.dpad_up)       enqueue_key(0x52, 0, true);    /* Up Arrow */
    if (down && !current_gamepad_state.dpad_down)   enqueue_key(0x51, 0, true);    /* Down Arrow */
    if (left && !current_gamepad_state.dpad_left)   enqueue_key(0x50, 0, true);    /* Left Arrow */
    if (right && !current_gamepad_state.dpad_right) enqueue_key(0x4F, 0, true);    /* Right Arrow */
    if (btn_a && !current_gamepad_state.btn_a)      enqueue_key(0x28, '\r', true); /* Enter (LV_KEY_ENTER) */
    if (btn_b && !current_gamepad_state.btn_b)      enqueue_key(0x29, 0, true);    /* Escape (LV_KEY_ESC) */
    if (btn_x && !current_gamepad_state.btn_x)      enqueue_key(0x2A, 0, true);    /* Backspace (LV_KEY_BACKSPACE) */
    if (btn_y && !current_gamepad_state.btn_y)      enqueue_key(0x2B, 0, true);    /* Tab (LV_KEY_TAB) */
    if (btn_l1 && !current_gamepad_state.btn_l1)    enqueue_key(0x52, 0, true);    /* Up Arrow */
    if (btn_r1 && !current_gamepad_state.btn_r1)    enqueue_key(0x4F, 0, true);    /* Right Arrow */

    /* Save current state */
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

/* ==========================================================================
 * TinyUSB Host HID Callbacks
 * ========================================================================== */

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    g_usb_dbg.last_host_id = 2U; /* CLASS_ACTIVE */
    g_usb_dbg.hid_state    = 6U; /* POLL */

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
    {
        is_kbd_ready = true;
        g_usb_dbg.detected_dev_type = USB_DEV_TYPE_KEYBOARD;
        printf("[USB] HID Keyboard Mounted (addr=%u, inst=%u)\r\n", dev_addr, instance);
        Beeper_BootMelody();
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
    {
        is_mouse_ready = true;
        current_mouse_state.x = 160;
        current_mouse_state.y = 120;
        g_usb_dbg.mouse_x     = 160;
        g_usb_dbg.mouse_y     = 120;
        g_usb_dbg.detected_dev_type = USB_DEV_TYPE_MOUSE;
        printf("[USB] HID Mouse Mounted (addr=%u, inst=%u)\r\n", dev_addr, instance);
        Beeper_BootMelody();
    }
    else
    {
        /* Generic HID (Protocol 0) -> Gamepad STK-7024X */
        is_gamepad_ready = true;
        g_usb_dbg.detected_dev_type = USB_DEV_TYPE_GAMEPAD;
        printf("[USB] HID Gamepad Mounted (addr=%u, inst=%u)\r\n", dev_addr, instance);
        Beeper_BootMelody();
    }

    (void)tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
    {
        is_kbd_ready = false;
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
    {
        is_mouse_ready = false;
    }
    else
    {
        is_gamepad_ready = false;
    }

    if (!is_kbd_ready && !is_mouse_ready && !is_gamepad_ready)
    {
        g_usb_dbg.last_host_id      = 0U;
        g_usb_dbg.detected_dev_type = USB_DEV_TYPE_NONE;
        g_usb_dbg.hid_state         = 0U;
    }

    printf("[USB] HID Device Unmounted (addr=%u, inst=%u)\r\n", dev_addr, instance);
    Beeper_Click();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
    g_usb_dbg.irq_event_count++;

    uint8_t copy_len = (len < 8U) ? (uint8_t)len : 8U;
    for (uint8_t i = 0; i < copy_len; i++)
    {
        g_usb_dbg.raw_report[i] = report[i];
    }

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
    {
        Parse_Keyboard_Report(report, len);
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
    {
        Parse_Mouse_Report(report, len);
    }
    else
    {
        Parse_Gamepad_Report(report, len);
    }

    (void)tuh_hid_receive_report(dev_addr, instance);
}
