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
    bool     sram_ok;        /* FSMC External SRAM status */
    bool     ccmram_ok;      /* Core Coupled Memory status */
    bool     display_ok;     /* ILI9341 Display status */
    bool     sd_ok;          /* FatFs SD Card mount status */
    bool     eeprom_ok;      /* AT24C02 I2C EEPROM status */
    bool     flash_ok;       /* W25Q128 SPI Flash status */
    bool     usb_host_ok;    /* USB Host subsystem status */
    uint32_t sram_size_kb;   /* Verified SRAM size in KB */
    uint16_t display_id;     /* Display Chip ID (Expected: 0x9341) */
    uint32_t flash_id;       /* SPI Flash JEDEC ID (Expected: 0xEF4018) */
} POST_Report_t;

/* --- Public Function Prototypes --- */

/**
 * @brief Execute Power-On Self-Test diagnostics for all hardware subsystems
 * @return Overall POST status (POST_OK or error code)
 */
POST_Status_t POST_RunSelfTest(void);

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
