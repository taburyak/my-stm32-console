#include "lv_port_console.h"
#include "main.h"
#include "ili9341_simple.h"
#include "XPT2046_touch.h"

/* External handles and addresses declared in main.c */
extern DMA_HandleTypeDef hdma_memtomem_dma2_stream0;

/* Buffer for rendering on 40 rows (320 * 40 * 2 bytes = 25.6 KB) */
#define MY_DISP_HOR_RES    320
#define MY_DISP_VER_RES    240
#define DISP_BUF_SIZE      (MY_DISP_HOR_RES * 40)

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf_1 = (lv_color_t *)0x68000000UL;

/**
 * @brief Pixel Transfer Buffer to the ILI9341 Controller
 */
static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t width  = area->x2 - area->x1 + 1;
    uint32_t height = area->y2 - area->y1 + 1;
    uint32_t pixels_count = width * height;

    /* 1. Set the active update window on the display */
    lcdSetWindow(area->x1, area->y1, area->x2, area->y2);

    /* 2. Transfer pixels via the universal ili9341_simple driver */
    lcdFlushFrameBufferDMA(&hdma_memtomem_dma2_stream0, (uint16_t *)color_p, pixels_count);

    /* 3. Notify LVGL that the rectangle update is complete */
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
            data->state   = LV_INDEV_STATE_PR; /* PR = Pressed (натиснуто) */
            return;
        }
    }

    data->state = LV_INDEV_STATE_REL; /* REL = Released (відпущено) */
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
}
