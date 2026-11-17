#include "stm32f4xx_hal.h"
#include "ff.h"
#include "ff_gen_drv.h"
#include "fw_update.h"
#include "ili9341_simple.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Hardware pins matching Core/Inc/main.h */
#define BOOT_LD0_PORT   GPIOE
#define BOOT_LD0_PIN    GPIO_PIN_3
#define BOOT_KEY0_PORT  GPIOF
#define BOOT_KEY0_PIN   GPIO_PIN_6
#define BOOT_BL_PORT    GPIOF
#define BOOT_BL_PIN     GPIO_PIN_10

#define EXT_SRAM_BUF    ((uint8_t *)0x68000000UL)

extern const Diskio_drvTypeDef BootSD_Driver;

SD_HandleTypeDef   hsd;
UART_HandleTypeDef huart3;
SRAM_HandleTypeDef hsram1;
SRAM_HandleTypeDef hsram3;

static FATFS s_fs;
static char  s_sd_path[4];
static bool  s_lcd_ready = false;

void SysTick_Handler(void)
{
    HAL_IncTick();
}

static void uart3_puts(const char *s)
{
    if (s == NULL) return;
    size_t len = strlen(s);
    if (len > 0U)
    {
        (void)HAL_UART_Transmit(&huart3, (const uint8_t *)s, (uint16_t)len, 200U);
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= buf[i];
        for (uint8_t b = 0U; b < 8U; b++)
        {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return ~crc;
}

static uint16_t crc16_ccitt(const uint8_t *buf, uint32_t len)
{
    uint16_t crc = 0U;
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= (uint16_t)buf[i] << 8;
        for (uint8_t b = 0U; b < 8U; b++)
        {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Minimal 5x7 font for Bootloader status screen */
static const uint8_t s_font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},{0x00,0x00,0x00,0x00,0x00}
};

static void boot_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= 320U || y >= 240U || w == 0U || h == 0U) return;
    if (x + w > 320U) w = 320U - x;
    if (y + h > 240U) h = 240U - y;
    lcdSetWindow(x, y, x + w - 1U, y + h - 1U);
    uint32_t total = (uint32_t)w * (uint32_t)h;
    while (total--)
    {
        LCD_DataWrite(color);
    }
}

static void boot_draw_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (!s_lcd_ready || s == NULL) return;
    while (*s != '\0' && x < 320U)
    {
        char c = *s++;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = s_font5x7[(uint8_t)(c - 0x20)];
        for (uint8_t col = 0U; col < 5U; col++)
        {
            for (uint8_t row = 0U; row < 7U; row++)
            {
                uint16_t col_val = ((g[col] >> row) & 1U) ? fg : bg;
                if (scale == 1U)
                {
                    lcdSetWindow(x + col, y + row, x + col, y + row);
                    LCD_DataWrite(col_val);
                }
                else
                {
                    boot_fill_rect(x + col * scale, y + row * scale, scale, scale, col_val);
                }
            }
        }
        boot_fill_rect(x + 5U * scale, y, scale, 7U * scale, bg);
        x += 6U * scale;
    }
}

static void boot_ui_init(const char *subtitle)
{
    if (!s_lcd_ready)
    {
        lcdInit();
        lcdSetOrientation(LCD_ORIENTATION_LANDSCAPE);
        HAL_GPIO_WritePin(BOOT_BL_PORT, BOOT_BL_PIN, GPIO_PIN_SET);
        s_lcd_ready = true;
    }
    boot_fill_rect(0, 0, 320, 240, 0x0862U);
    boot_fill_rect(0, 0, 320, 32, 0x10A4U);
    boot_fill_rect(0, 32, 320, 2, 0x07FFU);
    boot_draw_text(12, 9, "STM32F407 BOOTLOADER", 0x07FFU, 0x10A4U, 2);
    boot_draw_text(12, 54, subtitle, 0xFFFFU, 0x0862U, 1);
    boot_fill_rect(18, 96, 284, 22, 0x10A4U);
    boot_fill_rect(20, 98, 280, 18, 0x0862U);
}

static void boot_ui_progress(uint32_t pct, const char *status)
{
    if (!s_lcd_ready) return;
    if (pct > 100U) pct = 100U;
    uint16_t w = (uint16_t)((280UL * pct) / 100UL);
    if (w > 0U) boot_fill_rect(20, 98, w, 18, 0x07E0U);
    boot_fill_rect(12, 130, 296, 16, 0x0862U);
    boot_draw_text(12, 130, status, 0xFFE0U, 0x0862U, 1);
}

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 4;
    RCC_OscInitStruct.PLL.PLLN = 168;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 7;
    (void)HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    (void)HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
}

static void Boot_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    HAL_GPIO_WritePin(BOOT_BL_PORT, BOOT_BL_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BOOT_LD0_PORT, BOOT_LD0_PIN, GPIO_PIN_SET);

    g.Pin = BOOT_LD0_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BOOT_LD0_PORT, &g);

    g.Pin = BOOT_BL_PIN;
    HAL_GPIO_Init(BOOT_BL_PORT, &g);

    g.Pin = BOOT_KEY0_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(BOOT_KEY0_PORT, &g);
}

static void Boot_FSMC_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_FSMC_CLK_ENABLE();

    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF12_FSMC;

    g.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
    HAL_GPIO_Init(GPIOF, &g);
    g.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_10|GPIO_PIN_12;
    HAL_GPIO_Init(GPIOG, &g);
    g.Pin = GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15|GPIO_PIN_0|GPIO_PIN_1;
    HAL_GPIO_Init(GPIOE, &g);
    g.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15|GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5;
    HAL_GPIO_Init(GPIOD, &g);

    FSMC_NORSRAM_TimingTypeDef Timing = {0};
    Timing.AddressSetupTime = 2;
    Timing.AddressHoldTime = 15;
    Timing.DataSetupTime = 6;
    Timing.BusTurnAroundDuration = 3;
    Timing.CLKDivision = 16;
    Timing.DataLatency = 17;
    Timing.AccessMode = FSMC_ACCESS_MODE_A;

    hsram1.Instance = FSMC_NORSRAM_DEVICE;
    hsram1.Extended = FSMC_NORSRAM_EXTENDED_DEVICE;
    hsram1.Init.NSBank = FSMC_NORSRAM_BANK4;
    hsram1.Init.DataAddressMux = FSMC_DATA_ADDRESS_MUX_DISABLE;
    hsram1.Init.MemoryType = FSMC_MEMORY_TYPE_SRAM;
    hsram1.Init.MemoryDataWidth = FSMC_NORSRAM_MEM_BUS_WIDTH_16;
    hsram1.Init.BurstAccessMode = FSMC_BURST_ACCESS_MODE_DISABLE;
    hsram1.Init.WaitSignalPolarity = FSMC_WAIT_SIGNAL_POLARITY_LOW;
    hsram1.Init.WrapMode = FSMC_WRAP_MODE_DISABLE;
    hsram1.Init.WaitSignalActive = FSMC_WAIT_TIMING_BEFORE_WS;
    hsram1.Init.WriteOperation = FSMC_WRITE_OPERATION_ENABLE;
    hsram1.Init.WaitSignal = FSMC_WAIT_SIGNAL_DISABLE;
    hsram1.Init.ExtendedMode = FSMC_EXTENDED_MODE_DISABLE;
    hsram1.Init.AsynchronousWait = FSMC_ASYNCHRONOUS_WAIT_DISABLE;
    hsram1.Init.WriteBurst = FSMC_WRITE_BURST_DISABLE;
    hsram1.Init.PageSize = FSMC_PAGE_SIZE_NONE;
    (void)HAL_SRAM_Init(&hsram1, &Timing, NULL);

    FSMC_NORSRAM_TimingTypeDef SramTiming = {0};
    SramTiming.AddressSetupTime = 0;
    SramTiming.AddressHoldTime = 15;
    SramTiming.DataSetupTime = 3;
    SramTiming.BusTurnAroundDuration = 1;
    SramTiming.CLKDivision = 16;
    SramTiming.DataLatency = 17;
    SramTiming.AccessMode = FSMC_ACCESS_MODE_A;

    hsram3.Instance = FSMC_NORSRAM_DEVICE;
    hsram3.Extended = FSMC_NORSRAM_EXTENDED_DEVICE;
    hsram3.Init = hsram1.Init;
    hsram3.Init.NSBank = FSMC_NORSRAM_BANK3;
    (void)HAL_SRAM_Init(&hsram3, &SramTiming, NULL);
}

static void Boot_USART3_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_USART3_CLK_ENABLE();
    g.Pin = GPIO_PIN_10 | GPIO_PIN_11;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOB, &g);

    huart3.Instance = USART3;
    huart3.Init.BaudRate = 115200;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    (void)HAL_UART_Init(&huart3);
}

static void Boot_SDIO_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_SDIO_CLK_ENABLE();
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF12_SDIO;

    g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOD, &g);

    hsd.Instance = SDIO;
    hsd.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
    hsd.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
    hsd.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
    hsd.Init.BusWide = SDIO_BUS_WIDE_1B;
    hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd.Init.ClockDiv = 4;
}

static bool Boot_IsAppValid(void)
{
    uint32_t sp = *(const volatile uint32_t *)FW_UPDATE_APP_ADDR;
    uint32_t pc = *(const volatile uint32_t *)(FW_UPDATE_APP_ADDR + 4U);
    if ((sp & 0x2FFE0000UL) != 0x20000000UL) return false;
    if (pc < FW_UPDATE_APP_ADDR || pc >= 0x08100000UL) return false;
    return true;
}

static uint32_t sector_from_addr(uint32_t addr)
{
    if (addr < 0x08020000UL) return FLASH_SECTOR_4;
    if (addr < 0x08040000UL) return FLASH_SECTOR_5;
    if (addr < 0x08060000UL) return FLASH_SECTOR_6;
    if (addr < 0x08080000UL) return FLASH_SECTOR_7;
    if (addr < 0x080A0000UL) return FLASH_SECTOR_8;
    if (addr < 0x080C0000UL) return FLASH_SECTOR_9;
    if (addr < 0x080E0000UL) return FLASH_SECTOR_10;
    return FLASH_SECTOR_11;
}

static bool Boot_EraseAppSectors(uint32_t image_size)
{
    uint32_t last_sector = sector_from_addr(FW_UPDATE_APP_ADDR + image_size - 1U);
    uint32_t nb_sectors = (last_sector - FLASH_SECTOR_4) + 1U;

    FLASH_EraseInitTypeDef er = {0};
    er.TypeErase    = FLASH_TYPEERASE_SECTORS;
    er.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    er.Sector       = FLASH_SECTOR_4;
    er.NbSectors    = nb_sectors;

    uint32_t sector_err = 0U;
    return (HAL_FLASHEx_Erase(&er, &sector_err) == HAL_OK);
}

static bool Boot_ValidateHeader(const FwUpdate_Header_t *hdr)
{
    if (hdr->magic != FW_UPDATE_MAGIC || hdr->header_size != FW_UPDATE_HDR_SIZE)
    {
        return false;
    }
    if (hdr->image_size == 0U || hdr->image_size > FW_UPDATE_MAX_SIZE)
    {
        return false;
    }
    if (hdr->target_addr != FW_UPDATE_APP_ADDR)
    {
        return false;
    }
    return true;
}

static bool Boot_TrySDUpdate(void)
{
    Boot_SDIO_Init();
    if (FATFS_LinkDriver(&BootSD_Driver, s_sd_path) != 0) return false;
    if (f_mount(&s_fs, s_sd_path, 1) != FR_OK)
    {
        (void)FATFS_UnLinkDriver(s_sd_path);
        return false;
    }

    static FIL f;
    if (f_open(&f, FW_UPDATE_SD_PATH, FA_READ) != FR_OK)
    {
        (void)f_mount(NULL, s_sd_path, 0);
        (void)FATFS_UnLinkDriver(s_sd_path);
        return false;
    }

    FwUpdate_Header_t hdr;
    UINT br = 0U;
    if (f_read(&f, &hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr) || !Boot_ValidateHeader(&hdr))
    {
        uart3_puts("[BOOT] Invalid fw_update.bin header, skipping.\r\n");
        (void)f_close(&f);
        (void)f_unlink(FW_UPDATE_SD_PATH);
        (void)f_mount(NULL, s_sd_path, 0);
        (void)FATFS_UnLinkDriver(s_sd_path);
        return false;
    }

    boot_ui_init("Checking SD:/system/fw_update.bin...");
    uart3_puts("[BOOT] Verifying fw_update.bin CRC32...\r\n");

    __attribute__((aligned(4))) static uint8_t io_buf[2048];
    uint32_t crc = 0U;
    uint32_t rem = hdr.image_size;
    while (rem > 0U)
    {
        UINT chunk = (rem > sizeof(io_buf)) ? (UINT)sizeof(io_buf) : (UINT)rem;
        if (f_read(&f, io_buf, chunk, &br) != FR_OK || br != chunk)
        {
            boot_ui_progress(0, "SD read error! Keeping old OS.");
            uart3_puts("[BOOT] SD read error!\r\n");
            HAL_Delay(1500);
            (void)f_close(&f);
            (void)f_mount(NULL, s_sd_path, 0);
            (void)FATFS_UnLinkDriver(s_sd_path);
            return false;
        }
        crc = crc32_update(crc, io_buf, br);
        rem -= br;
    }

    if (crc != hdr.crc32)
    {
        boot_ui_progress(0, "CRC mismatch! Aborting update.");
        uart3_puts("[BOOT] CRC mismatch! Aborting.\r\n");
        HAL_Delay(1500);
        (void)f_close(&f);
        (void)f_unlink(FW_UPDATE_SD_PATH);
        (void)f_mount(NULL, s_sd_path, 0);
        (void)FATFS_UnLinkDriver(s_sd_path);
        return false;
    }

    /* Erase & Flash */
    boot_ui_progress(5, "Erasing Flash sectors...");
    uart3_puts("[BOOT] Erasing Flash...\r\n");
    HAL_FLASH_Unlock();
    if (!Boot_EraseAppSectors(hdr.image_size))
    {
        HAL_FLASH_Lock();
        boot_ui_progress(0, "Flash erase failed!");
        uart3_puts("[BOOT] Flash erase failed!\r\n");
        HAL_Delay(2000);
        (void)f_close(&f);
        (void)f_mount(NULL, s_sd_path, 0);
        (void)FATFS_UnLinkDriver(s_sd_path);
        return false;
    }

    (void)f_lseek(&f, sizeof(hdr));
    uint32_t written = 0U;
    uint32_t addr = FW_UPDATE_APP_ADDR;
    boot_ui_progress(10, "Flashing new firmware...");
    uart3_puts("[BOOT] Flashing...\r\n");

    while (written < hdr.image_size)
    {
        UINT req = ((hdr.image_size - written) > sizeof(io_buf)) ? (UINT)sizeof(io_buf) : (UINT)(hdr.image_size - written);
        if (f_read(&f, io_buf, req, &br) != FR_OK || br == 0U) break;

        for (uint32_t off = 0U; off < br; off += 4U)
        {
            uint32_t w = 0xFFFFFFFFUL;
            uint32_t copy = ((br - off) >= 4U) ? 4U : (br - off);
            memcpy(&w, io_buf + off, copy);
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, w) != HAL_OK)
            {
                HAL_FLASH_Lock();
                boot_ui_progress(0, "Flash write error!");
                HAL_Delay(2000);
                (void)f_close(&f);
                (void)f_mount(NULL, s_sd_path, 0);
                (void)FATFS_UnLinkDriver(s_sd_path);
                return false;
            }
            addr += 4U;
        }
        written += br;
        uint32_t pct = 10U + (written * 85UL) / hdr.image_size;
        boot_ui_progress(pct, "Flashing STM32F407 Flash...");
        HAL_GPIO_TogglePin(BOOT_LD0_PORT, BOOT_LD0_PIN);
    }
    HAL_FLASH_Lock();
    (void)f_close(&f);

    uint32_t flash_crc = crc32_update(0U, (const uint8_t *)FW_UPDATE_APP_ADDR, hdr.image_size);
    if (flash_crc == hdr.crc32)
    {
        boot_ui_progress(100, "Update OK! Booting Console OS...");
        uart3_puts("[BOOT] Flash verified! Deleting fw_update.bin & booting.\r\n");
        (void)f_unlink(FW_UPDATE_SD_PATH);
        HAL_Delay(400);
    }
    else
    {
        boot_ui_progress(0, "Verify failed! Enter Rescue DFU.");
        uart3_puts("[BOOT] Flash verify failed!\r\n");
        HAL_Delay(2000);
    }

    (void)f_mount(NULL, s_sd_path, 0);
    (void)FATFS_UnLinkDriver(s_sd_path);
    return true;
}

static bool uart3_getc_timeout(uint8_t *out, uint32_t timeout_ms)
{
    return (HAL_UART_Receive(&huart3, out, 1U, timeout_ms) == HAL_OK);
}

static void Boot_RunUartRescueDFU(void)
{
    boot_ui_init("UART3 Rescue DFU (Send fw_update.bin via XMODEM)");
    boot_ui_progress(0, "Waiting XMODEM-1K/CRC on USART3 (115200)...");
    uart3_puts("\r\n[BOOT] UART3 Rescue DFU. Send fw_update.bin via XMODEM-CRC/1K.\r\n");

    for (;;)
    {
        uint32_t rx_size = 0U;
        uint8_t  expected_blk = 1U;
        uint8_t  first = 0U;
        bool     started = false;

        for (uint32_t tries = 0U; tries < 60U; tries++)
        {
            uint8_t c_poll = 'C';
            (void)HAL_UART_Transmit(&huart3, &c_poll, 1U, 50U);
            if (uart3_getc_timeout(&first, 1000U))
            {
                started = true;
                break;
            }
            HAL_GPIO_TogglePin(BOOT_LD0_PORT, BOOT_LD0_PIN);
        }
        if (!started) continue;

        bool ok = true;
        uint8_t pkt[1024U + 4U];
        while (ok)
        {
            if (first == 0x04U) /* EOT */
            {
                uint8_t ack = 0x06U;
                (void)HAL_UART_Transmit(&huart3, &ack, 1U, 50U);
                break;
            }
            if (first == 0x18U) /* CAN */
            {
                ok = false;
                break;
            }
            uint32_t blk_sz = (first == 0x02U) ? 1024U : ((first == 0x01U) ? 128U : 0U);
            if (blk_sz == 0U)
            {
                ok = false;
                break;
            }

            if (HAL_UART_Receive(&huart3, pkt, (uint16_t)(blk_sz + 4U), 2000U) != HAL_OK)
            {
                uint8_t nak = 0x15U;
                (void)HAL_UART_Transmit(&huart3, &nak, 1U, 50U);
                if (!uart3_getc_timeout(&first, 2000U)) ok = false;
                continue;
            }

            uint8_t blk = pkt[0], inv = pkt[1];
            uint16_t rx_crc = ((uint16_t)pkt[blk_sz + 2U] << 8) | pkt[blk_sz + 3U];
            if ((uint8_t)(blk + inv) != 0xFFU || crc16_ccitt(&pkt[2], blk_sz) != rx_crc)
            {
                uint8_t nak = 0x15U;
                (void)HAL_UART_Transmit(&huart3, &nak, 1U, 50U);
                if (!uart3_getc_timeout(&first, 2000U)) ok = false;
                continue;
            }

            if (blk == expected_blk)
            {
                if ((rx_size + blk_sz) <= (FW_UPDATE_MAX_SIZE + 1024U))
                {
                    memcpy(EXT_SRAM_BUF + rx_size, &pkt[2], blk_sz);
                    rx_size += blk_sz;
                }
                expected_blk++;
            }

            uint8_t ack = 0x06U;
            (void)HAL_UART_Transmit(&huart3, &ack, 1U, 50U);
            if ((expected_blk & 7U) == 0U)
            {
                boot_ui_progress((rx_size * 50UL) / (450UL * 1024UL), "Receiving XMODEM...");
            }
            if (!uart3_getc_timeout(&first, 3000U)) ok = false;
        }

        if (!ok || rx_size < sizeof(FwUpdate_Header_t))
        {
            boot_ui_progress(0, "Transfer failed! Retrying...");
            continue;
        }

        FwUpdate_Header_t hdr;
        memcpy(&hdr, EXT_SRAM_BUF, sizeof(hdr));
        if (!Boot_ValidateHeader(&hdr) ||
            (rx_size < sizeof(hdr) + hdr.image_size) ||
            crc32_update(0U, EXT_SRAM_BUF + sizeof(hdr), hdr.image_size) != hdr.crc32)
        {
            boot_ui_progress(0, "Bad 4WFU header or CRC! Retry.");
            uart3_puts("\r\n[BOOT] Invalid fw_update.bin!\r\n");
            continue;
        }

        boot_ui_progress(55, "Erasing Flash sectors...");
        HAL_FLASH_Unlock();
        if (!Boot_EraseAppSectors(hdr.image_size))
        {
            HAL_FLASH_Lock();
            boot_ui_progress(0, "Erase failed!");
            continue;
        }

        const uint8_t *payload = EXT_SRAM_BUF + sizeof(hdr);
        uint32_t addr = FW_UPDATE_APP_ADDR;
        for (uint32_t off = 0U; off < hdr.image_size; off += 4U)
        {
            uint32_t w = 0xFFFFFFFFUL;
            uint32_t copy = ((hdr.image_size - off) >= 4U) ? 4U : (hdr.image_size - off);
            memcpy(&w, payload + off, copy);
            (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, w);
            addr += 4U;
            if ((off & 0x3FFFU) == 0U)
            {
                boot_ui_progress(60U + (off * 38UL) / hdr.image_size, "Flashing from RAM...");
            }
        }
        HAL_FLASH_Lock();

        if (crc32_update(0U, (const uint8_t *)FW_UPDATE_APP_ADDR, hdr.image_size) == hdr.crc32)
        {
            boot_ui_progress(100, "DFU Complete! Booting...");
            uart3_puts("\r\n[BOOT] DFU Complete!\r\n");
            HAL_Delay(400);
            return;
        }
    }
}

static void Boot_JumpToApp(void)
{
    uint32_t app_sp = *(const volatile uint32_t *)FW_UPDATE_APP_ADDR;
    void (*app_reset)(void) = (void (*)(void))(*(const volatile uint32_t *)(FW_UPDATE_APP_ADDR + 4U));

    __disable_irq();
    if (hsd.Instance != NULL) (void)HAL_SD_DeInit(&hsd);
    (void)HAL_UART_DeInit(&huart3);
    HAL_RCC_DeInit();
    HAL_DeInit();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    for (uint32_t i = 0U; i < 8U; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->VTOR = FW_UPDATE_APP_ADDR;
    __DSB();
    __ISB();
    __set_MSP(app_sp);
    __enable_irq();

    app_reset();
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    Boot_GPIO_Init();
    Boot_FSMC_Init();
    Boot_USART3_Init();

    bool rescue_key = (HAL_GPIO_ReadPin(BOOT_KEY0_PORT, BOOT_KEY0_PIN) == GPIO_PIN_RESET);
    if (!rescue_key)
    {
        (void)Boot_TrySDUpdate();
    }

    if (rescue_key || !Boot_IsAppValid())
    {
        Boot_RunUartRescueDFU();
    }

    Boot_JumpToApp();
    for (;;) {}
}
