#include "lv_port_console.h"
#include "main.h"
#include "ili9341_simple.h"
#include "XPT2046_touch.h"
#include "usb_host_manager.h"
#include "beeper.h"

extern DMA_HandleTypeDef hdma_memtomem_dma2_stream0;

#define MY_DISP_HOR_RES    320
#define MY_DISP_VER_RES    240
#define DISP_BUF_SIZE      (MY_DISP_HOR_RES * 40)

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf_1 = (lv_color_t *)0x68000000UL;

/* Pointer to the mouse cursor object on lv_layer_sys() */
static lv_obj_t * mouse_cursor_obj = NULL;

/**
 * @brief Pixel Transfer Buffer to the ILI9341 Controller
 */
static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t width  = area->x2 - area->x1 + 1;
    uint32_t height = area->y2 - area->y1 + 1;
    uint32_t pixels_count = width * height;

    lcdSetWindow(area->x1, area->y1, area->x2, area->y2);
    lcdFlushFrameBufferDMA(&hdma_memtomem_dma2_stream0, (uint16_t *)color_p, pixels_count);
    lv_disp_flush_ready(disp_drv);
}

/**
 * @brief Touch Read Callback for the XPT2046 Touch Controller
 */
static void touch_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    uint16_t touch_x = 0;
    uint16_t touch_y = 0;

    if (XPT2046_TouchPressed())
    {
        if (XPT2046_TouchGetCoordinates(&touch_x, &touch_y))
        {
            data->point.x = touch_x;
            data->point.y = touch_y;
            data->state   = LV_INDEV_STATE_PR;
            return;
        }
    }

    data->state = LV_INDEV_STATE_REL;
}

/**
 * @brief Mouse Read Callback for USB HID Mouse
 */
static void mouse_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    static int16_t last_x = 160;
    static int16_t last_y = 120;
    USB_MouseEvent_t m_state;

    if (USBHostManager_IsMouseReady() && USBHostManager_GetMouseState(&m_state))
    {
        /* Show cursor when mouse is connected */
        if (mouse_cursor_obj != NULL && lv_obj_has_flag(mouse_cursor_obj, LV_OBJ_FLAG_HIDDEN))
        {
            lv_obj_clear_flag(mouse_cursor_obj, LV_OBJ_FLAG_HIDDEN);
        }

        last_x = m_state.x;
        last_y = m_state.y;
        data->state = m_state.btn_left ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    }
    else
    {
        /* Hide cursor when mouse is disconnected */
        if (mouse_cursor_obj != NULL && !lv_obj_has_flag(mouse_cursor_obj, LV_OBJ_FLAG_HIDDEN))
        {
            lv_obj_add_flag(mouse_cursor_obj, LV_OBJ_FLAG_HIDDEN);
        }

        data->state = LV_INDEV_STATE_REL;
    }

    data->point.x = last_x;
    data->point.y = last_y;
}

/**
 * @brief Keyboard Read Callback for USB HID Keyboard
 */
static void keyboard_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    static uint32_t last_key = 0;
    static bool send_release_next = false;

    if (send_release_next)
    {
        data->key = last_key;
        data->state = LV_INDEV_STATE_REL;
        send_release_next = false;
        return;
    }

    USB_KeyboardEvent_t kbd_evt;
    if (USBHostManager_GetKeyEvent(&kbd_evt) && kbd_evt.is_pressed)
    {
        uint32_t lv_key = 0;

        switch (kbd_evt.scancode)
        {
            case 0x52: /* Up Arrow */
            case 0x50: /* Left Arrow */
                lv_key = LV_KEY_PREV;
                break;

            case 0x51: /* Down Arrow */
            case 0x4F: /* Right Arrow */
                lv_key = LV_KEY_NEXT;
                break;

            case 0x28: /* Enter */
            case 0x58: /* Keypad Enter */
                lv_key = LV_KEY_ENTER;
                break;

            case 0x29: /* Escape */
                lv_key = LV_KEY_ESC;
                break;

            case 0x2A: /* Backspace */
                lv_key = LV_KEY_BACKSPACE;
                break;

            case 0x2B: /* Tab */
                lv_key = LV_KEY_NEXT;
                break;

            default:
                lv_key = kbd_evt.ascii;
                break;
        }

        if (lv_key != 0)
        {
            last_key = lv_key;
            data->key = lv_key;
            data->state = LV_INDEV_STATE_PR;
            send_release_next = true;
            return;
        }
    }

    data->state = LV_INDEV_STATE_REL;
}

void LVGL_Port_Init(void)
{
    /* 1. Initialization of the LVGL core */
    lv_init();

    /* 2. Initialization of the rendering buffer */
    lv_disp_draw_buf_init(&draw_buf, buf_1, NULL, DISP_BUF_SIZE);

    /* 3. Registration of the display */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = MY_DISP_HOR_RES;
    disp_drv.ver_res = MY_DISP_VER_RES;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* 4. Registration of the input device (touch screen) */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read;
    lv_indev_drv_register(&indev_drv);

    /* 5. Registration of USB Keyboard & Default Group */
    lv_group_t * def_group = lv_group_create();
    lv_group_set_default(def_group);

    static lv_indev_drv_t indev_kbd_drv;
    lv_indev_drv_init(&indev_kbd_drv);
    indev_kbd_drv.type = LV_INDEV_TYPE_KEYPAD;
    indev_kbd_drv.read_cb = keyboard_read;
    lv_indev_t * kbd_indev = lv_indev_drv_register(&indev_kbd_drv);
    lv_indev_set_group(kbd_indev, def_group);

    /* 6. Registration of USB Mouse with dynamic cursor */
    static lv_indev_drv_t indev_mouse_drv;
    lv_indev_drv_init(&indev_mouse_drv);
    indev_mouse_drv.type = LV_INDEV_TYPE_POINTER;
    indev_mouse_drv.read_cb = mouse_read;
    lv_indev_t * mouse_indev = lv_indev_drv_register(&indev_mouse_drv);

    /* Create cursor icon on the top system layer and hide it initially */
    mouse_cursor_obj = lv_label_create(lv_layer_sys());
    lv_label_set_text(mouse_cursor_obj, LV_SYMBOL_UP); /* Neat pointer arrow */
    lv_obj_set_style_text_color(mouse_cursor_obj, lv_color_hex(0xFF0000), 0); /* Bright Red Cursor */
    lv_obj_add_flag(mouse_cursor_obj, LV_OBJ_FLAG_HIDDEN);
    lv_indev_set_cursor(mouse_indev, mouse_cursor_obj);
}
