#ifndef CCMRAM_H
#define CCMRAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

/* CCM RAM Memory Region Specifications (STM32F407ZET6) */
#define CCMRAM_BASE_ADDR   0x10000000U
#define CCMRAM_SIZE_BYTES  (64U * 1024U) /* 64 KB */
#define CCMRAM_END_ADDR    (CCMRAM_BASE_ADDR + CCMRAM_SIZE_BYTES - 1U)

/**
 * @brief Macro attribute to place variables into CCM RAM section (.ccmram)
 * @note  Variables in CCM RAM CANNOT be accessed by DMA controllers!
 */
#define CCMRAM_BUF __attribute__((section(".ccmram")))

/* --- Public Function Prototypes --- */

/**
 * @brief Check if a pointer address resides inside the CCM RAM memory region
 * @param ptr Pointer address to verify
 * @return true if address is in CCM RAM, false otherwise
 */
bool CCMRAM_IsAddressInCCMRAM(const void *ptr);

/**
 * @brief Safety check for DMA operations
 * @param ptr Pointer address intended for DMA transfer
 * @return true if address is SAFE for DMA (not in CCM RAM), false if unsafe
 */
bool CCMRAM_IsAddressValidForDMA(const void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* CCMRAM_H */
