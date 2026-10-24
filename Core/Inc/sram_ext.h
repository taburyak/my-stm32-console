#ifndef SRAM_EXT_H
#define SRAM_EXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

/* FSMC External SRAM Memory Specifications (1 MB / 512K x 16-bit) */
#define SRAM_EXT_BASE_ADDR     0x68000000U
#define SRAM_EXT_SIZE_BYTES    (1024U * 1024U) /* 1 MB */
#define SRAM_EXT_END_ADDR      (SRAM_EXT_BASE_ADDR + SRAM_EXT_SIZE_BYTES - 1U)

/* Display Framebuffer Dimensions & Allocations (320x240 RGB565) */
#define DISPLAY_WIDTH          320U
#define DISPLAY_HEIGHT         240U
#define FRAMEBUFFER_SIZE_BYTES (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2U) /* 153,600 Bytes */

/* Dedicated Framebuffer Offsets in External SRAM */
#define SRAM_FB1_OFFSET        0x00000000U
#define SRAM_FB2_OFFSET        0x00030000U /* 196,608 Bytes offset (aligned) */
#define SRAM_HEAP_OFFSET       0x00060000U /* Remaining ~614 KB for dynamic memory */

/**
 * @brief Macro attribute to place global variables into external SRAM section (.sram_data)
 */
#define SRAM_EXT_BUF __attribute__((section(".sram_data")))

/* --- Public Function Prototypes --- */

/**
 * @brief Initialize external SRAM subsystem
 */
void SRAM_EXT_Init(void);

/**
 * @brief Check if a pointer address resides inside the external SRAM region
 * @param ptr Pointer address to verify
 * @return true if address is in external SRAM, false otherwise
 */
bool SRAM_EXT_IsAddressInSRAM(const void *ptr);

/**
 * @brief Get absolute pointer to Framebuffer 1 in external SRAM
 * @return Pointer to Framebuffer 1 (153.6 KB)
 */
void* SRAM_EXT_GetFrameBuffer1(void);

/**
 * @brief Get absolute pointer to Framebuffer 2 in external SRAM
 * @return Pointer to Framebuffer 2 (153.6 KB)
 */
void* SRAM_EXT_GetFrameBuffer2(void);

/**
 * @brief Fast 32-bit aligned memory copy for external SRAM
 * @param dst Destination address (must be 32-bit aligned)
 * @param src Source address (must be 32-bit aligned)
 * @param words_count Number of 32-bit words to copy
 */
void SRAM_EXT_CopyFast32(uint32_t *dst, const uint32_t *src, uint32_t words_count);

/**
 * @brief Fast 32-bit aligned memory fill for external SRAM
 * @param dst Destination address
 * @param value 32-bit pattern value to fill
 * @param words_count Number of 32-bit words to fill
 */
void SRAM_EXT_FillFast32(uint32_t *dst, uint32_t value, uint32_t words_count);

#ifdef __cplusplus
}
#endif

#endif /* SRAM_EXT_H */
