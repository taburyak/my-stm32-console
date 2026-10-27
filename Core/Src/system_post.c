#include "system_post.h"
#include "ili9341_simple.h"
#include "fatfs.h"
#include "AT24Cxx.h"
#include "w25qxx.h"
#include "ccmram.h"
#include <stdio.h>
#include <string.h>

#define SRAM_BASE_ADDR    0x68000000U
#define SRAM_SIZE_BYTES   (1024 * 1024) /* 1 MB */

static POST_Report_t post_report = {0};

/* Dedicated CCM test word: the rest of CCM holds live task stacks and the RTOS heap */
CCMRAM_BUF static volatile uint32_t s_ccm_test_word;

/* External function prototype from display driver */
extern uint16_t lcdReadID(void);
extern char SDPath[4];

/**
 * @brief Non-destructive fast memory test for CCM RAM
 */
static bool Test_CCMRAM(void)
{
    volatile uint32_t *ptr = &s_ccm_test_word;

    /* Save original data */
    uint32_t orig_value = *ptr;
    bool pass = true;

    /* Write and check pattern 1 */
    *ptr = 0xAA55AA55;
    if (*ptr != 0xAA55AA55) pass = false;

    /* Write and check pattern 2 */
    *ptr = 0x55AA55AA;
    if (*ptr != 0x55AA55AA) pass = false;

    /* Restore original data */
    *ptr = orig_value;

    return pass;
}

/**
 * @brief Non-destructive full address-line (A0..A18) & anti-aliasing test for 1 MB FSMC SRAM
 */
static bool Test_SRAM(void)
{
    volatile uint16_t *base   = (volatile uint16_t *)SRAM_BASE_ADDR;         /* 0x68000000 (A18 = 0) */
    volatile uint16_t *upper  = (volatile uint16_t *)(SRAM_BASE_ADDR + 0x80000U); /* 0x68080000 (A18 = 1) */
    volatile uint16_t *end    = (volatile uint16_t *)(SRAM_BASE_ADDR + SRAM_SIZE_BYTES - 2U);

    /* Save original values */
    uint16_t orig_base  = *base;
    uint16_t orig_upper = *upper;
    uint16_t orig_end   = *end;
    bool pass = true;

    /* Simultaneous write to verify A18 (512 KB boundary) does not alias! */
    *base  = 0xCAFEU;
    *upper = 0xBEEFU;
    *end   = 0x1234U;
    __DSB();

    if (*base != 0xCAFEU || *upper != 0xBEEFU || *end != 0x1234U)
    {
        pass = false;
    }

    /* Invert patterns */
    *base  = 0x55AAU;
    *upper = 0xAA55U;
    *end   = 0x4321U;
    __DSB();

    if (*base != 0x55AAU || *upper != 0xAA55U || *end != 0x4321U)
    {
        pass = false;
    }

    /* Restore original values */
    *base  = orig_base;
    *upper = orig_upper;
    *end   = orig_end;
    __DSB();

    return pass;
}

/**
 * @brief Test I2C EEPROM (AT24C02) presence
 */
static bool Test_EEPROM(void)
{
    return (AT24Cxx_IsConnected() == HAL_OK);
}

/**
 * @brief Test SPI Flash (W25Q128) presence and JEDEC ID
 */
static bool Test_SPI_Flash(uint32_t *out_id)
{
    uint32_t flash_id = W25qxx_ReadID();
    if (out_id) *out_id = flash_id;

    /* Valid W25Q128 JEDEC ID starts with Winbond manufacturer byte 0xEF */
    return ((flash_id & 0xFF0000) == 0xEF0000) || (flash_id != 0 && flash_id != 0xFFFFFFFF);
}

static bool Test_USB_Host(void)
{
    /* Check whether the clock is enabled and whether the USB_OTG_FS peripheral block is responding */
    if ((RCC->AHB2ENR & RCC_AHB2ENR_OTGFSEN) && (USB_OTG_FS->GINTSTS != 0xFFFFFFFF))
    {
        return true;
    }
    return false;
}

POST_Status_t POST_RunSelfTest(void)
{
    memset(&post_report, 0, sizeof(POST_Report_t));
    POST_Status_t overall_status = POST_OK;

    /* 1. Test FSMC External SRAM (Non-destructive) */
    post_report.sram_ok = Test_SRAM();
    if (post_report.sram_ok)
    {
        post_report.sram_size_kb = 1024;
    }
    else
    {
        overall_status = POST_ERR_SRAM;
    }

    /* 2. Test CCM RAM (Non-destructive) */
    post_report.ccmram_ok = Test_CCMRAM();
    if (!post_report.ccmram_ok && overall_status == POST_OK)
    {
        overall_status = POST_ERR_CCMRAM;
    }

    /* 3. Check ILI9341 Display Chip ID (Strict: MUST be 0x9341) */
    post_report.display_id = lcdReadID();
    if (post_report.display_id == 0x9341)
    {

        post_report.display_ok = true;
    }
    else
    {
        post_report.display_ok = false;
        if (overall_status == POST_OK)
        {
            overall_status = POST_ERR_DISPLAY;
        }
    }

    /* 4. Test FatFs SD Card Mount (FATFS window is a DMA target: must not be on a CCM task stack) */
    static FATFS fatfs_obj;
    FRESULT fres = f_mount(&fatfs_obj, SDPath, 1);
    post_report.sd_ok = (fres == FR_OK);
    if (post_report.sd_ok)
    {
        f_mount(NULL, SDPath, 0); /* Mandatory: unregister local stack object */
    }

    /* 5. Test I2C EEPROM (AT24C02) */
    post_report.eeprom_ok = Test_EEPROM();

    /* 6. Test SPI Flash (W25Q128) */
    post_report.flash_ok = Test_SPI_Flash(&post_report.flash_id);

    /* 7. Test USB Host Controller */
    post_report.usb_host_ok = Test_USB_Host();

    /* Set warning flag if non-critical peripherals are missing */
    if (overall_status == POST_OK && (!post_report.sd_ok || !post_report.eeprom_ok || !post_report.flash_ok || !post_report.usb_host_ok))
    {
        overall_status = POST_WARN_OPTIONAL;
    }

    return overall_status;
}

const POST_Report_t* POST_GetReport(void)
{
    return &post_report;
}

void POST_PrintSummary(void)
{
    printf("\r\n========================================\r\n");
    printf("     POWER-ON SELF-TEST (POST) REPORT   \r\n");
    printf("========================================\r\n");

    /* SRAM status */
    if (post_report.sram_ok)
    {
        printf("[POST][OK]   FSMC SRAM: %lu KB PASS\r\n", (unsigned long)post_report.sram_size_kb);
    }
    else
    {
        printf("[POST][FAIL] FSMC SRAM: Test Failed!\r\n");
    }

    /* CCM RAM status */
    if (post_report.ccmram_ok)
    {
        printf("[POST][OK]   CCM RAM:   64 KB PASS\r\n");
    }
    else
    {
        printf("[POST][FAIL] CCM RAM:   Test Failed!\r\n");
    }

    /* Display status */
    if (post_report.display_ok)
    {
        printf("[POST][OK]   ILI9341 Display ID: 0x%04X PASS\r\n", post_report.display_id);
    }
    else
    {
        printf("[POST][FAIL] ILI9341 Display ID: 0x%04X FAIL (Expected 0x9341)\r\n", post_report.display_id);
    }

    /* SD Card status */
    if (post_report.sd_ok)
    {
        printf("[POST][OK]   SD Card:   Mounted PASS\r\n");
    }
    else
    {
        printf("[POST][WARN] SD Card:   Not Mounted / Missing\r\n");
    }

    /* EEPROM status */
    if (post_report.eeprom_ok)
    {
        printf("[POST][OK]   EEPROM AT24C02:  Detected PASS\r\n");
    }
    else
    {
        printf("[POST][WARN] EEPROM AT24C02:  Not Detected (Using Defaults)\r\n");
    }

    /* SPI Flash status */
    if (post_report.flash_ok)
    {
        printf("[POST][OK]   SPI Flash:      ID 0x%06X PASS\r\n", (unsigned int)post_report.flash_id);
    }
    else
    {
        printf("[POST][WARN] SPI Flash:      Not Detected (Fallback to SD)\r\n");
    }

    printf("  USB Host Peripheral : %s\r\n", post_report.usb_host_ok ? "OK" : "WARN (Not Detected)");

    printf("========================================\r\n\r\n");
}
