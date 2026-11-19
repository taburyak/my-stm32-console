#include "vs1053.h"
#include <stdio.h>

/* XRESET: use the CubeMX label when present, otherwise drive PA2 here (wired to P15 PA2) */
#ifndef VS_XRESET_Pin
#define VS_XRESET_Pin           GPIO_PIN_2
#define VS_XRESET_GPIO_Port     GPIOA
#define VS1053_INIT_XRESET_PIN  1
#endif

#define VS1053_CMD_WRITE        0x02U
#define VS1053_CMD_READ         0x03U
#define VS1053_DREQ_TIMEOUT_MS  100U
#define VS1053_SPI_TIMEOUT_MS   10U

/* SC_MULT = 3.0x: CLKI = 12.288 MHz * 3 = 36.864 MHz */
#define VS1053_CLOCKF_VALUE     0x6000U

static SPI_HandleTypeDef *s_spi     = NULL;
static bool               s_present = false;
static uint8_t            s_version = 0U;

static bool vs_dreq(void)
{
    return HAL_GPIO_ReadPin(VS_DREQ_GPIO_Port, VS_DREQ_Pin) == GPIO_PIN_SET;
}

static bool vs_wait_dreq(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (!vs_dreq())
    {
        if ((HAL_GetTick() - t0) >= timeout_ms)
        {
            return false;
        }
    }
    return true;
}

static void vs_set_spi_prescaler(uint32_t prescaler)
{
    __HAL_SPI_DISABLE(s_spi);
    MODIFY_REG(s_spi->Instance->CR1, SPI_CR1_BR, prescaler);
    s_spi->Init.BaudRatePrescaler = prescaler;
}

static void vs_sdi_write(const uint8_t *data, uint16_t len)
{
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XDCS_GPIO_Port, VS_XDCS_Pin, GPIO_PIN_RESET);
    (void)HAL_SPI_Transmit(s_spi, (uint8_t *)data, len, VS1053_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XDCS_GPIO_Port, VS_XDCS_Pin, GPIO_PIN_SET);
}

uint16_t VS1053_ReadReg(uint8_t addr)
{
    if (s_spi == NULL)
    {
        return 0xFFFFU;
    }
    uint8_t tx[4] = { VS1053_CMD_READ, addr, 0xFFU, 0xFFU };
    uint8_t rx[4] = { 0U, 0U, 0U, 0U };
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_RESET);
    (void)HAL_SPI_TransmitReceive(s_spi, tx, rx, 4U, VS1053_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_SET);
    return (uint16_t)(((uint16_t)rx[2] << 8) | rx[3]);
}

void VS1053_WriteReg(uint8_t addr, uint16_t value)
{
    if (s_spi == NULL)
    {
        return;
    }
    uint8_t tx[4] = { VS1053_CMD_WRITE, addr, (uint8_t)(value >> 8), (uint8_t)value };
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_RESET);
    (void)HAL_SPI_Transmit(s_spi, tx, 4U, VS1053_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_SET);
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
}

bool VS1053_Init(SPI_HandleTypeDef *hspi)
{
    s_spi     = hspi;
    s_present = false;
    s_version = 0U;

#ifdef VS1053_INIT_XRESET_PIN
    __HAL_RCC_GPIOA_CLK_ENABLE();
    HAL_GPIO_WritePin(VS_XRESET_GPIO_Port, VS_XRESET_Pin, GPIO_PIN_RESET);
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin   = VS_XRESET_Pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(VS_XRESET_GPIO_Port, &gpio);
#endif

    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(VS_XDCS_GPIO_Port, VS_XDCS_Pin, GPIO_PIN_SET);

    /* Before CLOCKF is set CLKI = 12.288 MHz: SCI reads must stay below CLKI/7 (~1.75 MHz) */
    vs_set_spi_prescaler(SPI_BAUDRATEPRESCALER_32);

    HAL_GPIO_WritePin(VS_XRESET_GPIO_Port, VS_XRESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(2);
    HAL_GPIO_WritePin(VS_XRESET_GPIO_Port, VS_XRESET_Pin, GPIO_PIN_SET);
    if (!vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS))
    {
        printf("[VS1053] Not detected (DREQ stays low after reset)\r\n");
        return false;
    }

    VS1053_WriteReg(VS1053_SCI_MODE, VS1053_SM_SDINEW | VS1053_SM_RESET);
    HAL_Delay(2);
    if (!vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS))
    {
        printf("[VS1053] Soft reset timeout\r\n");
        return false;
    }

    /* Cheap breakout boards leave GPIO0/1 so the chip boots into RT-MIDI mode (no sine/MP3):
     * drive GPIO0/1 low (DDR = 3, ODATA = 0) and soft-reset again. Harmless on boards booting normally. */
    VS1053_WriteReg(VS1053_SCI_WRAMADDR, 0xC017U);
    VS1053_WriteReg(VS1053_SCI_WRAM, 0x0003U);
    VS1053_WriteReg(VS1053_SCI_WRAMADDR, 0xC019U);
    VS1053_WriteReg(VS1053_SCI_WRAM, 0x0000U);
    HAL_Delay(5);
    VS1053_WriteReg(VS1053_SCI_MODE, VS1053_SM_SDINEW | VS1053_SM_RESET);
    HAL_Delay(2);
    if (!vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS))
    {
        printf("[VS1053] Soft reset timeout after MP3-mode switch\r\n");
        return false;
    }

    uint16_t mode   = VS1053_ReadReg(VS1053_SCI_MODE);
    uint16_t status = VS1053_ReadReg(VS1053_SCI_STATUS);
    s_version = (uint8_t)((status >> 4) & 0x0FU);
    if (mode == 0xFFFFU || (mode & VS1053_SM_SDINEW) == 0U || s_version == 0U || s_version == 0x0FU)
    {
        printf("[VS1053] No valid SCI answer (MODE=0x%04X STATUS=0x%04X)\r\n",
               (unsigned int)mode, (unsigned int)status);
        return false;
    }

    VS1053_WriteReg(VS1053_SCI_CLOCKF, VS1053_CLOCKF_VALUE);
    HAL_Delay(1);
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);

    /* CLKI = 36.864 MHz: SCI reads up to CLKI/7 = 5.27 MHz, so 42 MHz / 8 = 5.25 MHz */
    vs_set_spi_prescaler(SPI_BAUDRATEPRESCALER_8);
    uint16_t clockf = VS1053_ReadReg(VS1053_SCI_CLOCKF);
    if (clockf != VS1053_CLOCKF_VALUE)
    {
        printf("[VS1053] CLOCKF readback 0x%04X at 5.25 MHz (expected 0x%04X)\r\n",
               (unsigned int)clockf, (unsigned int)VS1053_CLOCKF_VALUE);
        vs_set_spi_prescaler(SPI_BAUDRATEPRESCALER_32);
        return false;
    }

    VS1053_SetVolume(0x30U, 0x30U);
    s_present = true;
    printf("[VS1053] Detected chip version %u (MODE=0x%04X STATUS=0x%04X CLOCKF=0x%04X)\r\n",
           (unsigned int)s_version, (unsigned int)mode, (unsigned int)status, (unsigned int)clockf);
    return true;
}

bool VS1053_IsPresent(void)
{
    return s_present;
}

uint8_t VS1053_GetVersion(void)
{
    return s_version;
}

bool VS1053_IsReady(void)
{
    return vs_dreq();
}

void VS1053_SetVolume(uint8_t left, uint8_t right)
{
    VS1053_WriteReg(VS1053_SCI_VOL, (uint16_t)(((uint16_t)left << 8) | right));
}

void VS1053_SetDifferential(bool enable)
{
    if (!s_present)
    {
        return;
    }
    uint16_t mode = VS1053_ReadReg(VS1053_SCI_MODE);
    mode = enable ? (uint16_t)(mode | VS1053_SM_DIFF) : (uint16_t)(mode & ~VS1053_SM_DIFF);
    VS1053_WriteReg(VS1053_SCI_MODE, mode);
}

bool VS1053_IsDifferential(void)
{
    return s_present && ((VS1053_ReadReg(VS1053_SCI_MODE) & VS1053_SM_DIFF) != 0U);
}

uint8_t VS1053_SineCode(uint32_t hz, uint32_t *actual_hz)
{
    static const uint32_t s_rates[8] = { 44100U, 48000U, 32000U, 22050U, 24000U, 16000U, 11025U, 12000U };
    uint8_t  best_code = 0x44U; /* 32000 * 4 / 128 = 1000 Hz */
    uint32_t best_hz   = 1000U;
    uint32_t best_err  = 0xFFFFFFFFU;
    for (uint32_t r = 0U; r < 8U; r++)
    {
        for (uint32_t s = 1U; s < 32U; s++)
        {
            uint32_t f   = (s_rates[r] * s) / 128U;
            uint32_t err = (f > hz) ? (f - hz) : (hz - f);
            if (err < best_err)
            {
                best_err  = err;
                best_hz   = f;
                best_code = (uint8_t)((r << 5) | s);
            }
        }
    }
    if (actual_hz != NULL)
    {
        *actual_hz = best_hz;
    }
    return best_code;
}

bool VS1053_SineStart(uint8_t code)
{
    if (!s_present)
    {
        return false;
    }
    uint16_t mode = VS1053_ReadReg(VS1053_SCI_MODE);
    VS1053_WriteReg(VS1053_SCI_MODE, (uint16_t)(mode | VS1053_SM_TESTS));
    const uint8_t seq[8] = { 0x53U, 0xEFU, 0x6EU, code, 0U, 0U, 0U, 0U };
    vs_sdi_write(seq, sizeof(seq));
    return true;
}

void VS1053_SineStop(void)
{
    if (!s_present)
    {
        return;
    }
    static const uint8_t seq[8] = { 0x45U, 0x78U, 0x69U, 0x74U, 0U, 0U, 0U, 0U };
    vs_sdi_write(seq, sizeof(seq));
    uint16_t mode = VS1053_ReadReg(VS1053_SCI_MODE);
    VS1053_WriteReg(VS1053_SCI_MODE, (uint16_t)(mode & ~VS1053_SM_TESTS));
}
