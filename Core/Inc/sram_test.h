/**
 * @file sram_test.h
 * @brief Memory testing suite for IS62WV51216 (1 MB External SRAM)
 */

#ifndef SRAM_TEST_H
#define SRAM_TEST_H

#include <stdint.h>
#include <stdbool.h>

/* Base address for FSMC Bank 1, Sub-bank 3 (NE3) */
#define EXTSRAM_BASE_ADDR   ((uint32_t)0x68000000)
#define EXTSRAM_SIZE_BYTES  ((uint32_t)(1024 * 1024)) /* 1 MB */

/**
 * @brief Runs a full diagnostic suite on the external SRAM
 *
 * Performs:
 * 1. Data Bus Test (Walking 1s)
 * 2. Address Bus Test (Aliasing check)
 * 3. Full Device Test (PRBS data integrity across whole chip)
 *
 * @return true if all tests pass, false if any error is detected.
 */
bool SRAM_RunFullDiagnostics(void);

#endif /* SRAM_TEST_H */
