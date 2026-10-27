#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

/*
 * Single source of truth for the console RAM layout.
 * Internal SRAM split must match the RAM / APP_RAM regions in STM32F407xx_FLASH.ld
 * and APP_RAM in sdk_apps/app_sram.ld.
 */

/* ---- Internal SRAM: OS 0x20000000..0x20017FFF, App sandbox 0x20018000..0x2001FFFF ---- */
#define MM_APP_RAM_BASE              0x20018000UL
#define MM_APP_RAM_SIZE              (32UL * 1024UL)
#define MM_APP_STACK_SIZE            (4UL * 1024UL)
#define MM_APP_IMAGE_MAX_SIZE        (MM_APP_RAM_SIZE - MM_APP_STACK_SIZE)
#define MM_APP_STACK_BASE            (MM_APP_RAM_BASE + MM_APP_IMAGE_MAX_SIZE)

/* ---- External FSMC SRAM (IS62WV51216, 1 MB, Bank1 NE3) ---- */
#define MM_EXT_BASE                  0x68000000UL
#define MM_EXT_SIZE                  (1024UL * 1024UL)

/* OS buffers: 0x68000000..0x6803FFFF */
#define MM_EXT_LVGL_DRAW_BUF         0x68000000UL
#define MM_EXT_LVGL_DRAW_BUF_SIZE    (32UL * 1024UL)
#define MM_EXT_TEXT_PREVIEW          0x68008000UL
#define MM_EXT_TEXT_PREVIEW_SIZE     (2UL * 1024UL)
#define MM_EXT_BMP_LINE_BUF          0x68009000UL
#define MM_EXT_BMP_LINE_BUF_SIZE     (8UL * 1024UL)
#define MM_EXT_IMAGE_BUF             0x68010000UL
#define MM_EXT_IMAGE_BUF_SIZE        (192UL * 1024UL)

/* OS heap (FreeRTOS heap_5, region 2): 0x68040000..0x6807FFFF */
#define MM_EXT_OS_HEAP_BASE          0x68040000UL
#define MM_EXT_OS_HEAP_SIZE          (256UL * 1024UL)

/* App data sandbox (.ext_bss): 0x68080000..0x680FFFFF */
#define MM_EXT_APP_BASE              0x68080000UL
#define MM_EXT_APP_SIZE              (512UL * 1024UL)

/* MPU regions require base aligned to region size */
_Static_assert((MM_APP_RAM_BASE % MM_APP_RAM_SIZE) == 0UL, "App RAM base must be aligned to its size");
_Static_assert((MM_EXT_APP_BASE % MM_EXT_APP_SIZE) == 0UL, "Ext app base must be aligned to its size");
_Static_assert(MM_APP_RAM_BASE + MM_APP_RAM_SIZE == 0x20020000UL, "App RAM must end at the top of SRAM2");
_Static_assert(MM_EXT_TEXT_PREVIEW >= MM_EXT_LVGL_DRAW_BUF + MM_EXT_LVGL_DRAW_BUF_SIZE, "Ext overlap: LVGL buf / text");
_Static_assert(MM_EXT_BMP_LINE_BUF >= MM_EXT_TEXT_PREVIEW + MM_EXT_TEXT_PREVIEW_SIZE, "Ext overlap: text / BMP line");
_Static_assert(MM_EXT_IMAGE_BUF >= MM_EXT_BMP_LINE_BUF + MM_EXT_BMP_LINE_BUF_SIZE, "Ext overlap: BMP line / image");
_Static_assert(MM_EXT_OS_HEAP_BASE >= MM_EXT_IMAGE_BUF + MM_EXT_IMAGE_BUF_SIZE, "Ext overlap: image / OS heap");
_Static_assert(MM_EXT_APP_BASE >= MM_EXT_OS_HEAP_BASE + MM_EXT_OS_HEAP_SIZE, "Ext overlap: OS heap / app");
_Static_assert(MM_EXT_APP_BASE + MM_EXT_APP_SIZE <= MM_EXT_BASE + MM_EXT_SIZE, "Ext app region exceeds SRAM");

#endif /* MEMORY_MAP_H */
