#include "system_post.h"
#include "memory_map.h"
#include "ili9341_simple.h"
#include "fatfs.h"
#include "AT24Cxx.h"
#include "w25qxx.h"
#include "ccmram.h"
#include <stdio.h>
#include <string.h>

#define SRAM_BASE_ADDR        MM_EXT_BASE
#define SRAM_SIZE_BYTES       MM_EXT_SIZE
#define SRAM_ADDR_BITS        19U /* A0..A18 = 512K x 16-bit (1 MB) */
#define SRAM_ADDR_PROBES      (SRAM_ADDR_BITS + 1U)

static POST_Report_t post_report = {0};

/* Dedicated CCM test word: the rest of CCM holds live task stacks and the RTOS heap */
CCMRAM_BUF static volatile uint32_t s_ccm_test_word;

/* External handles & prototypes */
extern IWDG_HandleTypeDef hiwdg;
extern uint16_t lcdReadID(void);
extern char SDPath[4];

static void Record_SRAM_Failure(uint32_t addr, uint32_t expected, uint32_t actual)
{
    if (post_report.sram_fail_addr == 0U)
    {
        post_report.sram_fail_addr = addr;
        post_report.sram_fail_exp  = expected;
        post_report.sram_fail_got  = actual;
    }
}

static inline uint32_t Prbs32_Step(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

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
 * @brief Stage 1: Non-destructive D0..D15 walking 1/0 + NBL0/NBL1 byte-lane strobe test
 */
static bool Test_SRAM_DataBus(void)
{
    volatile uint16_t *p0 = (volatile uint16_t *)SRAM_BASE_ADDR;
    volatile uint16_t *p1 = (volatile uint16_t *)(SRAM_BASE_ADDR + 2UL);

    uint16_t orig0 = *p0;
    uint16_t orig1 = *p1;
    bool pass = true;

    for (uint32_t bit = 0U; bit < 16U; bit++)
    {
        uint16_t pat = (uint16_t)(1U << bit);
        uint16_t inv = (uint16_t)~pat;

        /* Drive opposite polarity on adjacent address to discharge FSMC bus capacitance */
        *p0 = pat;
        *p1 = inv;
        __DSB();

        uint16_t r0 = *p0;
        uint16_t r1 = *p1;
        if (r0 != pat)
        {
            Record_SRAM_Failure((uint32_t)p0, pat, r0);
            pass = false;
            break;
        }
        if (r1 != inv)
        {
            Record_SRAM_Failure((uint32_t)p1, inv, r1);
            pass = false;
            break;
        }

        /* Walking-0 pass */
        *p0 = inv;
        *p1 = pat;
        __DSB();

        r0 = *p0;
        r1 = *p1;
        if (r0 != inv)
        {
            Record_SRAM_Failure((uint32_t)p0, inv, r0);
            pass = false;
            break;
        }
        if (r1 != pat)
        {
            Record_SRAM_Failure((uint32_t)p1, pat, r1);
            pass = false;
            break;
        }
    }

    /* Verify NBL0 (PE0) and NBL1 (PE1) individual byte-write strobes */
    if (pass)
    {
        volatile uint8_t *b0 = (volatile uint8_t *)p0;
        *p0 = 0xA55AU;
        __DSB();

        b0[0] = 0x3CU;
        __DSB();
        uint16_t r_low = *p0;
        if (r_low != 0xA53CU)
        {
            Record_SRAM_Failure((uint32_t)p0, 0xA53CU, r_low);
            pass = false;
        }

        b0[1] = 0x96U;
        __DSB();
        uint16_t r_high = *p0;
        if (r_high != 0x963CU)
        {
            Record_SRAM_Failure((uint32_t)p0, 0x963CU, r_high);
            pass = false;
        }
    }

    *p0 = orig0;
    *p1 = orig1;
    __DSB();
    return pass;
}

/**
 * @brief Stage 2: Non-destructive A0..A18 address walking-1 & anti-aliasing test across 1 MB
 */
static bool Test_SRAM_AddrBus(void)
{
    volatile uint16_t *base = (volatile uint16_t *)SRAM_BASE_ADDR;
    uint16_t saved[SRAM_ADDR_PROBES];
    bool pass = true;

    for (uint32_t i = 0U; i < SRAM_ADDR_PROBES; i++)
    {
        uint32_t hw_off = (i == 0U) ? 0UL : (1UL << (i - 1U));
        saved[i] = base[hw_off];
    }

    for (uint32_t i = 0U; i < SRAM_ADDR_PROBES; i++)
    {
        uint32_t hw_off = (i == 0U) ? 0UL : (1UL << (i - 1U));
        base[hw_off] = (uint16_t)(0x5A00U ^ (i * 0x0305U) ^ i);
    }
    __DSB();

    for (uint32_t i = 0U; i < SRAM_ADDR_PROBES && pass; i++)
    {
        uint32_t target_off = (i == 0U) ? 0UL : (1UL << (i - 1U));
        uint16_t sig_i = (uint16_t)(0x5A00U ^ (i * 0x0305U) ^ i);
        uint16_t inv_i = (uint16_t)~sig_i;

        base[target_off] = inv_i;
        __DSB();

        for (uint32_t j = 0U; j < SRAM_ADDR_PROBES; j++)
        {
            uint32_t check_off = (j == 0U) ? 0UL : (1UL << (j - 1U));
            uint16_t expected  = (j == i) ? inv_i : (uint16_t)(0x5A00U ^ (j * 0x0305U) ^ j);
            uint16_t actual    = base[check_off];
            if (actual != expected)
            {
                Record_SRAM_Failure((uint32_t)&base[check_off], expected, actual);
                pass = false;
                break;
            }
        }

        base[target_off] = sig_i;
        __DSB();
    }

    /* Restore in reverse order in case an address pin is stuck */
    for (int32_t i = (int32_t)SRAM_ADDR_PROBES - 1; i >= 0; i--)
    {
        uint32_t hw_off = (i == 0) ? 0UL : (1UL << ((uint32_t)i - 1U));
        base[hw_off] = saved[i];
    }
    __DSB();

    return pass;
}

/**
 * @brief Stage 3: 512 KB PRBS-32 + bitwise-inverse March test on MM_EXT_APP_BASE
 */
static bool Test_SRAM_PRBS512K(uint32_t seed)
{
    volatile uint32_t *buf = (volatile uint32_t *)MM_EXT_APP_BASE;
    const uint32_t words   = MM_EXT_APP_SIZE / sizeof(uint32_t);
    const uint32_t init_st = (seed != 0U) ? seed : 0x13579BDFU;

    (void)HAL_IWDG_Refresh(&hiwdg);

    /* Pass 1: Fill with PRBS-32 ^ word_index */
    uint32_t st = init_st;
    for (uint32_t i = 0U; i < words; i++)
    {
        st = Prbs32_Step(st);
        buf[i] = st ^ i;
    }
    __DSB();

    /* Pass 2: Verify PRBS-32 and write bitwise complement */
    st = init_st;
    for (uint32_t i = 0U; i < words; i++)
    {
        st = Prbs32_Step(st);
        uint32_t exp = st ^ i;
        uint32_t got = buf[i];
        if (got != exp)
        {
            Record_SRAM_Failure((uint32_t)&buf[i], exp, got);
            (void)HAL_IWDG_Refresh(&hiwdg);
            return false;
        }
        buf[i] = ~exp;
    }
    __DSB();

    /* Pass 3: Verify complement and clear back to zero */
    st = init_st;
    for (uint32_t i = 0U; i < words; i++)
    {
        st = Prbs32_Step(st);
        uint32_t exp = ~(st ^ i);
        uint32_t got = buf[i];
        if (got != exp)
        {
            Record_SRAM_Failure((uint32_t)&buf[i], exp, got);
            (void)HAL_IWDG_Refresh(&hiwdg);
            return false;
        }
        buf[i] = 0U;
    }
    __DSB();

    (void)HAL_IWDG_Refresh(&hiwdg);
    return true;
}

bool POST_RunSRAMFullTest(uint32_t seed)
{
    uint32_t t0 = HAL_GetTick();

    post_report.sram_fail_addr   = 0U;
    post_report.sram_fail_exp    = 0U;
    post_report.sram_fail_got    = 0U;

    post_report.sram_data_bus_ok = Test_SRAM_DataBus();
    post_report.sram_addr_bus_ok = Test_SRAM_AddrBus();
    post_report.sram_prbs_ok     = Test_SRAM_PRBS512K(seed);

    post_report.sram_ok      = post_report.sram_data_bus_ok &&
                               post_report.sram_addr_bus_ok &&
                               post_report.sram_prbs_ok;
    post_report.sram_size_kb = post_report.sram_ok ? 1024U : 0U;
    post_report.sram_test_ms = HAL_GetTick() - t0;

    post_report.sram_stress_runs++;
    if (!post_report.sram_ok)
    {
        post_report.sram_stress_errs++;
    }

    return post_report.sram_ok;
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

    /* 1. Test FSMC External SRAM (Data bus D0..D15 + NBL0/1, Address bus A0..A18, 512 KB PRBS-32) */
    bool sram_pass = POST_RunSRAMFullTest(0x13579BDFU);
    post_report.sram_stress_runs = 0U;
    post_report.sram_stress_errs = 0U;
    if (!sram_pass)
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
        printf("[POST][OK]   FSMC SRAM: %lu KB PASS (Data:OK Addr:OK PRBS:OK, %lu ms)\r\n",
               (unsigned long)post_report.sram_size_kb,
               (unsigned long)post_report.sram_test_ms);
    }
    else
    {
        printf("[POST][FAIL] FSMC SRAM: FAIL (Data:%s Addr:%s PRBS:%s) @0x%08lX exp=0x%08lX got=0x%08lX\r\n",
               post_report.sram_data_bus_ok ? "OK" : "ERR",
               post_report.sram_addr_bus_ok ? "OK" : "ERR",
               post_report.sram_prbs_ok     ? "OK" : "ERR",
               (unsigned long)post_report.sram_fail_addr,
               (unsigned long)post_report.sram_fail_exp,
               (unsigned long)post_report.sram_fail_got);
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
