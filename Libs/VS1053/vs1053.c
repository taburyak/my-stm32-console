#include "vs1053.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdio.h>
#include <string.h>

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
#define VS1053_DMA_TIMEOUT_MS   5U
#define VS1053_SDI_CHUNK        32U
#define VS1053_ENDFILL_BYTES    2052U
#define VS1053_CANCEL_TRIES     64U    /* 64 * 32 B = 2048 B, the datasheet limit before a soft reset */

/* SC_MULT = 3.0x: CLKI = 12.288 MHz * 3 = 36.864 MHz */
#define VS1053_CLOCKF_VALUE     0x6000U

/* WRAM parameters (VS1053 datasheet, "Extra Parameters") */
#define VS1053_PARA_BYTERATE    0x1E05U
#define VS1053_PARA_ENDFILL     0x1E06U

static SPI_HandleTypeDef *s_spi     = NULL;
static bool               s_present = false;
static uint8_t            s_version = 0U;

/* SPI2 is shared by the player task (SDI stream) and the CLI/UI (SCI registers) */
static StaticSemaphore_t  s_bus_mutex_cb;
static osMutexId_t        s_bus_mutex = NULL;
static StaticSemaphore_t  s_dma_sem_cb;
static osSemaphoreId_t    s_dma_sem   = NULL;

/* DMA source buffers must live in SRAM: DMA1 cannot read CCM RAM, where task stacks are placed */
static uint8_t s_dma_buf[VS1053_SDI_CHUNK] __attribute__((aligned(4)));

static bool vs_rtos_running(void)
{
    return (osKernelGetState() == osKernelRunning) && (__get_IPSR() == 0U);
}

static void vs_lock(void)
{
    if (s_bus_mutex != NULL && vs_rtos_running())
    {
        (void)osMutexAcquire(s_bus_mutex, osWaitForever);
    }
}

static void vs_unlock(void)
{
    if (s_bus_mutex != NULL && vs_rtos_running())
    {
        (void)osMutexRelease(s_bus_mutex);
    }
}

static bool vs_dreq(void)
{
    return HAL_GPIO_ReadPin(VS_DREQ_GPIO_Port, VS_DREQ_Pin) == GPIO_PIN_SET;
}

/* Spin for the first millisecond (SCI ops finish in microseconds), then yield while the FIFO drains */
static bool vs_wait_dreq(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (!vs_dreq())
    {
        uint32_t el = HAL_GetTick() - t0;
        if (el >= timeout_ms)
        {
            return false;
        }
        if (el >= 1U && vs_rtos_running())
        {
            osDelay(1U);
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

/* One SDI transfer of up to 32 bytes; the caller holds the bus and has checked DREQ */
static void vs_sdi_chunk(const uint8_t *data, uint16_t len)
{
    HAL_GPIO_WritePin(VS_XDCS_GPIO_Port, VS_XDCS_Pin, GPIO_PIN_RESET);
    if (s_dma_sem != NULL && s_spi->hdmatx != NULL && vs_rtos_running())
    {
        memcpy(s_dma_buf, data, len);
        (void)osSemaphoreAcquire(s_dma_sem, 0U);
        if (HAL_SPI_Transmit_DMA(s_spi, s_dma_buf, len) == HAL_OK)
        {
            if (osSemaphoreAcquire(s_dma_sem, VS1053_DMA_TIMEOUT_MS) != osOK)
            {
                (void)HAL_SPI_Abort(s_spi);
            }
        }
    }
    else
    {
        (void)HAL_SPI_Transmit(s_spi, (uint8_t *)data, len, VS1053_SPI_TIMEOUT_MS);
    }
    HAL_GPIO_WritePin(VS_XDCS_GPIO_Port, VS_XDCS_Pin, GPIO_PIN_SET);
}

static void vs_sdi_write(const uint8_t *data, uint16_t len)
{
    vs_lock();
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XDCS_GPIO_Port, VS_XDCS_Pin, GPIO_PIN_RESET);
    (void)HAL_SPI_Transmit(s_spi, (uint8_t *)data, len, VS1053_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XDCS_GPIO_Port, VS_XDCS_Pin, GPIO_PIN_SET);
    vs_unlock();
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (s_spi != NULL && hspi->Instance == s_spi->Instance && s_dma_sem != NULL)
    {
        (void)osSemaphoreRelease(s_dma_sem);
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    HAL_SPI_TxCpltCallback(hspi);
}

uint16_t VS1053_ReadReg(uint8_t addr)
{
    if (s_spi == NULL)
    {
        return 0xFFFFU;
    }
    uint8_t tx[4] = { VS1053_CMD_READ, addr, 0xFFU, 0xFFU };
    uint8_t rx[4] = { 0U, 0U, 0U, 0U };
    vs_lock();
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_RESET);
    (void)HAL_SPI_TransmitReceive(s_spi, tx, rx, 4U, VS1053_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_SET);
    vs_unlock();
    return (uint16_t)(((uint16_t)rx[2] << 8) | rx[3]);
}

void VS1053_WriteReg(uint8_t addr, uint16_t value)
{
    if (s_spi == NULL)
    {
        return;
    }
    uint8_t tx[4] = { VS1053_CMD_WRITE, addr, (uint8_t)(value >> 8), (uint8_t)value };
    vs_lock();
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_RESET);
    (void)HAL_SPI_Transmit(s_spi, tx, 4U, VS1053_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(VS_XCS_GPIO_Port, VS_XCS_Pin, GPIO_PIN_SET);
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
    vs_unlock();
}

uint16_t VS1053_ReadWram(uint16_t addr)
{
    vs_lock();
    VS1053_WriteReg(VS1053_SCI_WRAMADDR, addr);
    uint16_t v = VS1053_ReadReg(VS1053_SCI_WRAM);
    vs_unlock();
    return v;
}

static void vs_create_rtos_objects(void)
{
    if (s_bus_mutex == NULL)
    {
        const osMutexAttr_t mattr = {
            .name    = "vs1053_bus",
            .attr_bits = osMutexRecursive | osMutexPrioInherit,
            .cb_mem  = &s_bus_mutex_cb,
            .cb_size = sizeof(s_bus_mutex_cb)
        };
        s_bus_mutex = osMutexNew(&mattr);
    }
    if (s_dma_sem == NULL)
    {
        const osSemaphoreAttr_t sattr = {
            .name    = "vs1053_dma",
            .cb_mem  = &s_dma_sem_cb,
            .cb_size = sizeof(s_dma_sem_cb)
        };
        s_dma_sem = osSemaphoreNew(1U, 0U, &sattr);
    }
}

/* SM_RESET with SPI dropped below CLKI/7, then CLOCKF and the fast SPI clock restored */
static bool vs_soft_reset(uint16_t extra_mode)
{
    vs_set_spi_prescaler(SPI_BAUDRATEPRESCALER_32);
    VS1053_WriteReg(VS1053_SCI_MODE, (uint16_t)(VS1053_SM_SDINEW | VS1053_SM_RESET | extra_mode));
    HAL_Delay(2);
    if (!vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS))
    {
        return false;
    }
    VS1053_WriteReg(VS1053_SCI_CLOCKF, VS1053_CLOCKF_VALUE);
    HAL_Delay(1);
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);
    /* CLKI = 36.864 MHz: SCI reads up to CLKI/7 = 5.27 MHz, so 42 MHz / 8 = 5.25 MHz */
    vs_set_spi_prescaler(SPI_BAUDRATEPRESCALER_8);
    return VS1053_ReadReg(VS1053_SCI_CLOCKF) == VS1053_CLOCKF_VALUE;
}

bool VS1053_Init(SPI_HandleTypeDef *hspi)
{
    s_spi     = hspi;
    s_present = false;
    s_version = 0U;
    vs_create_rtos_objects();
    vs_lock();

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

    bool ok = false;
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
        goto out;
    }

    VS1053_WriteReg(VS1053_SCI_MODE, VS1053_SM_SDINEW | VS1053_SM_RESET);
    HAL_Delay(2);
    if (!vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS))
    {
        printf("[VS1053] Soft reset timeout\r\n");
        goto out;
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
        goto out;
    }

    uint16_t mode   = VS1053_ReadReg(VS1053_SCI_MODE);
    uint16_t status = VS1053_ReadReg(VS1053_SCI_STATUS);
    s_version = (uint8_t)((status >> 4) & 0x0FU);
    if (mode == 0xFFFFU || (mode & VS1053_SM_SDINEW) == 0U || s_version == 0U || s_version == 0x0FU)
    {
        printf("[VS1053] No valid SCI answer (MODE=0x%04X STATUS=0x%04X)\r\n",
               (unsigned int)mode, (unsigned int)status);
        goto out;
    }

    VS1053_WriteReg(VS1053_SCI_CLOCKF, VS1053_CLOCKF_VALUE);
    HAL_Delay(1);
    (void)vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS);

    vs_set_spi_prescaler(SPI_BAUDRATEPRESCALER_8);
    uint16_t clockf = VS1053_ReadReg(VS1053_SCI_CLOCKF);
    if (clockf != VS1053_CLOCKF_VALUE)
    {
        printf("[VS1053] CLOCKF readback 0x%04X at 5.25 MHz (expected 0x%04X)\r\n",
               (unsigned int)clockf, (unsigned int)VS1053_CLOCKF_VALUE);
        vs_set_spi_prescaler(SPI_BAUDRATEPRESCALER_32);
        goto out;
    }

    VS1053_SetVolume(0x30U, 0x30U);
    s_present = true;
    ok = true;
    printf("[VS1053] Detected chip version %u (MODE=0x%04X STATUS=0x%04X CLOCKF=0x%04X)\r\n",
           (unsigned int)s_version, (unsigned int)mode, (unsigned int)status, (unsigned int)clockf);
out:
    vs_unlock();
    return ok;
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
    vs_lock();
    uint16_t mode = VS1053_ReadReg(VS1053_SCI_MODE);
    mode = enable ? (uint16_t)(mode | VS1053_SM_DIFF) : (uint16_t)(mode & ~VS1053_SM_DIFF);
    VS1053_WriteReg(VS1053_SCI_MODE, mode);
    vs_unlock();
}

bool VS1053_IsDifferential(void)
{
    return s_present && ((VS1053_ReadReg(VS1053_SCI_MODE) & VS1053_SM_DIFF) != 0U);
}

uint32_t VS1053_SdiFeed(const uint8_t *data, uint32_t len)
{
    if (!s_present || data == NULL)
    {
        return 0U;
    }
    uint32_t sent = 0U;
    vs_lock();
    while (sent < len && vs_dreq())
    {
        uint32_t n = len - sent;
        if (n > VS1053_SDI_CHUNK)
        {
            n = VS1053_SDI_CHUNK;
        }
        vs_sdi_chunk(&data[sent], (uint16_t)n);
        sent += n;
    }
    vs_unlock();
    return sent;
}

static bool vs_send_fill(uint8_t fill, uint32_t count)
{
    uint8_t chunk[VS1053_SDI_CHUNK];
    memset(chunk, fill, sizeof(chunk));
    while (count > 0U)
    {
        if (!vs_wait_dreq(VS1053_DREQ_TIMEOUT_MS))
        {
            return false;
        }
        uint32_t n = (count > VS1053_SDI_CHUNK) ? VS1053_SDI_CHUNK : count;
        vs_sdi_chunk(chunk, (uint16_t)n);
        count -= n;
    }
    return true;
}

bool VS1053_EndPlayback(bool cancel)
{
    if (!s_present)
    {
        return false;
    }
    vs_lock();
    uint8_t fill = (uint8_t)(VS1053_ReadWram(VS1053_PARA_ENDFILL) & 0xFFU);

    /* Datasheet 10.5.1 (end of file) flushes with endFillByte first; 10.5.2 (cancel) does it after */
    if (!cancel)
    {
        (void)vs_send_fill(fill, VS1053_ENDFILL_BYTES);
    }
    uint16_t mode = VS1053_ReadReg(VS1053_SCI_MODE);
    VS1053_WriteReg(VS1053_SCI_MODE, (uint16_t)(mode | VS1053_SM_CANCEL));

    bool cleared = false;
    for (uint32_t i = 0U; i < VS1053_CANCEL_TRIES && !cleared; i++)
    {
        (void)vs_send_fill(fill, VS1053_SDI_CHUNK);
        cleared = (VS1053_ReadReg(VS1053_SCI_MODE) & VS1053_SM_CANCEL) == 0U;
    }
    if (cleared && cancel)
    {
        (void)vs_send_fill(fill, VS1053_ENDFILL_BYTES);
    }
    if (!cleared)
    {
        uint16_t vol  = VS1053_ReadReg(VS1053_SCI_VOL);
        uint16_t bass = VS1053_ReadReg(VS1053_SCI_BASS);
        (void)vs_soft_reset((uint16_t)(mode & VS1053_SM_DIFF));
        VS1053_WriteReg(VS1053_SCI_VOL, vol);
        VS1053_WriteReg(VS1053_SCI_BASS, bass);
        printf("[VS1053] SM_CANCEL not acknowledged, decoder soft-reset\r\n");
    }
    vs_unlock();
    return cleared;
}

uint32_t VS1053_GetByteRate(void)
{
    return s_present ? (uint32_t)VS1053_ReadWram(VS1053_PARA_BYTERATE) : 0U;
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
    vs_lock();
    uint16_t mode = VS1053_ReadReg(VS1053_SCI_MODE);
    VS1053_WriteReg(VS1053_SCI_MODE, (uint16_t)(mode | VS1053_SM_TESTS));
    const uint8_t seq[8] = { 0x53U, 0xEFU, 0x6EU, code, 0U, 0U, 0U, 0U };
    vs_sdi_write(seq, sizeof(seq));
    vs_unlock();
    return true;
}

void VS1053_SineStop(void)
{
    if (!s_present)
    {
        return;
    }
    static const uint8_t seq[8] = { 0x45U, 0x78U, 0x69U, 0x74U, 0U, 0U, 0U, 0U };
    vs_lock();
    vs_sdi_write(seq, sizeof(seq));
    uint16_t mode = VS1053_ReadReg(VS1053_SCI_MODE);
    VS1053_WriteReg(VS1053_SCI_MODE, (uint16_t)(mode & ~VS1053_SM_TESTS));
    vs_unlock();
}
