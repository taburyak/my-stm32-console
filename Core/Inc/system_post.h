#ifndef SYSTEM_POST_H
#define SYSTEM_POST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief POST overall status return codes
 */
typedef enum {
    POST_OK = 0,             /* All hardware tests passed */
    POST_WARN_OPTIONAL,      /* Optional peripherals (SD, EEPROM, Flash) missing */
    POST_ERR_SRAM,           /* FSMC External SRAM test failed (fatal) */
    POST_ERR_CCMRAM,         /* CCM RAM test failed (fatal) */
    POST_ERR_DISPLAY,        /* ILI9341 display ID check failed */
    POST_ERR_FATAL           /* General system fault */
} POST_Status_t;


/**
 * @brief Diagnostic report structure holding test results
 */
typedef struct {
    bool     sram_ok;           /* FSMC External SRAM overall status */
    bool     sram_data_bus_ok;  /* D0..D15 walking 1/0 + NBL0/NBL1 byte lanes */
    bool     sram_addr_bus_ok;  /* A0..A18 address walking 1 / anti-aliasing */
    bool     sram_prbs_ok;      /* 512 KB PRBS-32 + inverse sweep */
    bool     ccmram_ok;         /* Core Coupled Memory status */
    bool     display_ok;        /* ILI9341 Display status */
    bool     sd_ok;             /* FatFs SD Card mount status */
    bool     eeprom_ok;         /* AT24C02 I2C EEPROM status */
    bool     flash_ok;          /* W25Q128 SPI Flash status */
    bool     usb_host_ok;       /* USB Host subsystem status */
    uint32_t sram_size_kb;      /* Verified SRAM size in KB */
    uint32_t sram_fail_addr;    /* First failing address (0 if none) */
    uint32_t sram_fail_exp;     /* Expected value at first failure */
    uint32_t sram_fail_got;     /* Actual value read at first failure */
    uint32_t sram_test_ms;      /* Duration of last SRAM full test in ms */
    uint32_t sram_stress_runs;  /* Completed on-demand stress passes */
    uint32_t sram_stress_errs;  /* Total errors across on-demand passes */
    uint16_t display_id;        /* Display Chip ID (Expected: 0x9341) */
    uint32_t flash_id;          /* SPI Flash JEDEC ID (Expected: 0xEF4018) */
} POST_Report_t;

/* --- Public Function Prototypes --- */

/**
 * @brief Execute Power-On Self-Test diagnostics for all hardware subsystems
 * @return Overall POST status (POST_OK or error code)
 */
POST_Status_t POST_RunSelfTest(void);

/**
 * @brief Run full 3-stage external SRAM test (Data bus, Address bus A0..A18, 512 KB PRBS-32).
 *        Safe to call at boot or from the GUI when no external app is running.
 * @param seed Initial PRBS-32 seed (non-zero)
 * @return true if all stages passed
 */
bool POST_RunSRAMFullTest(uint32_t seed);

/**
 * @brief Get read-only pointer to the latest POST diagnostic report
 * @return Pointer to POST_Report_t structure
 */
const POST_Report_t* POST_GetReport(void);

/**
 * @brief Print formatted POST diagnostic summary via UART3 (printf)
 */
void POST_PrintSummary(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_POST_H */
