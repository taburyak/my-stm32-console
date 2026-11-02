#ifndef LV_PORT_CONSOLE_H
#define LV_PORT_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/**
 * @brief Set to 1 to use non-blocking DMA interrupt flush (`HAL_DMA_Start_IT` + `lv_disp_flush_ready`
 *        in DMA TC IRQ with a single 320x40 draw buffer inside `MM_EXT_LVGL_DRAW_BUF`).
 *        Set to 0 to revert to legacy blocking poll mode (`lcdFlushFrameBufferDMA`).
 */
#ifndef LVGL_PORT_USE_DMA_IT
#define LVGL_PORT_USE_DMA_IT 1
#endif

/**
 * @brief Set to 1 only if the draw buffers reside in a memory bus separate from the LCD (e.g. internal SRAM).
 *        Keep at 0 when draw buffers live in external FSMC SRAM (Bank 3) because LCD is also on FSMC (Bank 4):
 *        simultaneous CPU rendering to Bank 3 while DMA copies Bank 3 -> Bank 4 causes FSMC bus contention.
 */
#ifndef LVGL_PORT_USE_DOUBLE_BUF
#define LVGL_PORT_USE_DOUBLE_BUF 0
#endif

/**
 * @brief Initialization of the LVGL graphics engine and rendering buffers
 *        and registration of the display and touchscreen drivers.
 */
void LVGL_Port_Init(void);

/**
 * @brief  Safely notify LVGL that the DMA transfer is finished.
 *         Must be called from the GUI task thread before lv_timer_handler().
 */
void LVGL_Port_HandleFlushConfirmation(void);

/**
 * @brief  Wait until any active LVGL DMA flush completes (called before handing the display to a sandbox app).
 */
void LVGL_Port_WaitFlushIdle(void);

#ifdef __cplusplus

}
#endif

#endif /* LV_PORT_CONSOLE_H */
