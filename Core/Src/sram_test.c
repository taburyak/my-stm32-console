/**
 * @file sram_test.c
 * @brief Memory testing suite implementation for IS62WV51216
 */

#include "sram_test.h"
#include <stdio.h>

/**
 * @brief Tests the data bus wiring for shorts or opens
 */
static bool SRAM_TestDataBus(volatile uint16_t *address)
{
    uint16_t pattern;
    
    /* Write walking 1s */
    for (pattern = 1; pattern != 0; pattern <<= 1) {
        *address = pattern;
        if (*address != pattern) {
            printf("[SRAM ERROR] Data bus fail at 0x%p. Wrote 0x%04X, Read 0x%04X\r\n", 
                    address, pattern, *address);
            return false;
        }
    }
    return true;
}

/**
 * @brief Tests the address bus wiring for aliasing or shorts
 * Uses power-of-two offsets to verify each address line is independent.
 */
static bool SRAM_TestAddressBus(volatile uint16_t *base_addr, uint32_t size_bytes)
{
    uint32_t address_mask = (size_bytes / sizeof(uint16_t)) - 1;
    uint32_t offset;
    uint32_t test_offset;
    
    const uint16_t pattern = 0xAAAA;
    const uint16_t antipattern = 0x5555;

    /* Write default pattern to all power-of-two addresses */
    for (offset = 1; (offset & address_mask) != 0; offset <<= 1) {
        base_addr[offset] = pattern;
    }

    /* Check for aliasing by modifying one address and checking others */
    test_offset = 0;
    base_addr[test_offset] = antipattern;

    for (offset = 1; (offset & address_mask) != 0; offset <<= 1) {
        if (base_addr[offset] != pattern) {
            printf("[SRAM ERROR] Address bus aliasing detected between 0x%p and 0x%p\r\n", 
                    &base_addr[test_offset], &base_addr[offset]);
            return false;
        }
    }

    base_addr[test_offset] = pattern;

    /* Now check the rest of the lines */
    for (test_offset = 1; (test_offset & address_mask) != 0; test_offset <<= 1) {
        base_addr[test_offset] = antipattern;

        if (base_addr[0] != pattern) {
            printf("[SRAM ERROR] Address bus aliasing detected between 0x%p and 0x%p\r\n", 
                    &base_addr[test_offset], &base_addr[0]);
            return false;
        }

        for (offset = 1; (offset & address_mask) != 0; offset <<= 1) {
            if ((base_addr[offset] != pattern) && (offset != test_offset)) {
                printf("[SRAM ERROR] Address bus aliasing detected between 0x%p and 0x%p\r\n", 
                        &base_addr[test_offset], &base_addr[offset]);
                return false;
            }
        }
        base_addr[test_offset] = pattern;
    }

    return true;
}

/**
 * @brief Tests the entire memory array for stuck bits using Pseudo-Random Data
 */
static bool SRAM_TestDeviceIntegrity(volatile uint16_t *base_addr, uint32_t size_bytes)
{
    uint32_t words = size_bytes / sizeof(uint16_t);
    uint32_t i;
    uint32_t errors = 0;
    
    /* Simple PRNG seed */
    uint16_t prng_state = 0x1234;

    /* Pass 1: Write */
    for (i = 0; i < words; i++) {
        /* Xorshift PRNG */
        prng_state ^= (prng_state << 7);
        prng_state ^= (prng_state >> 9);
        prng_state ^= (prng_state << 8);
        base_addr[i] = prng_state;
    }

    /* Reset seed */
    prng_state = 0x1234;

    /* Pass 2: Read and Verify */
    for (i = 0; i < words; i++) {
        prng_state ^= (prng_state << 7);
        prng_state ^= (prng_state >> 9);
        prng_state ^= (prng_state << 8);
        
        uint16_t read_val = base_addr[i];
        if (read_val != prng_state) {
            if (errors < 5) { // Only print first 5 errors to avoid spam
                printf("[SRAM ERROR] Data mismatch at 0x%p. Expected 0x%04X, Read 0x%04X\r\n", 
                        &base_addr[i], prng_state, read_val);
            }
            errors++;
        }
    }

    if (errors > 0) {
        printf("[SRAM ERROR] Total integrity errors found: %lu\r\n", errors);
        return false;
    }
    return true;
}

bool SRAM_RunFullDiagnostics(void)
{
    volatile uint16_t *base_addr = (volatile uint16_t *)EXTSRAM_BASE_ADDR;
    
    printf("\r\n--- Starting SRAM Diagnostics (Base: 0x%08lX, Size: %lu KB) ---\r\n", 
            (uint32_t)EXTSRAM_BASE_ADDR, EXTSRAM_SIZE_BYTES / 1024);

    /* 1. Data Bus Test */
    printf("[1/3] Testing Data Bus... ");
    if (SRAM_TestDataBus(base_addr)) {
        printf("PASSED\r\n");
    } else {
        printf("FAILED\r\n");
        return false;
    }

    /* 2. Address Bus Test */
    printf("[2/3] Testing Address Bus... ");
    if (SRAM_TestAddressBus(base_addr, EXTSRAM_SIZE_BYTES)) {
        printf("PASSED\r\n");
    } else {
        printf("FAILED\r\n");
        return false;
    }

    /* 3. Device Integrity Test */
    printf("[3/3] Testing Memory Integrity (Writing/Reading 1MB)... ");
    if (SRAM_TestDeviceIntegrity(base_addr, EXTSRAM_SIZE_BYTES)) {
        printf("PASSED\r\n");
    } else {
        printf("FAILED\r\n");
        return false;
    }

    printf("--- SRAM Diagnostics Completed Successfully ---\r\n\r\n");
    return true;
}
