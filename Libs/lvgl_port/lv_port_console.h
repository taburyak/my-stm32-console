#ifndef LV_PORT_CONSOLE_H
#define LV_PORT_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/**
 * @brief Initialization of the LVGL graphics engine and rendering buffers
 *        and registration of the display and touchscreen drivers.
 */
void LVGL_Port_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_CONSOLE_H */
