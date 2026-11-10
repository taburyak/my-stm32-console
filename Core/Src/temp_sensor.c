#include "temp_sensor.h"
#include "main.h"
#include "ds18b20.h"
#include "beeper.h"
#include "cmsis_os2.h"
#include <stdio.h>

extern ADC_HandleTypeDef  hadc1;
extern IWDG_HandleTypeDef hiwdg;
extern OneWire_t          OneWire;

static bool     s_ext_detected   = false;
static bool     s_ext_valid      = false;
static int32_t  s_cached_mcu_dc  = 320; /* 32.0 °C initial default */
static int32_t  s_cached_ext_dc  = 0;
static bool     s_overheated     = false;
static uint32_t s_last_mcu_tick  = 0U;

static uint16_t adc1_read_channel(uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel      = channel;
    sConfig.Rank         = 1U;
    sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;

    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        return 0U;
    }
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
        return 0U;
    }
    uint16_t val = 0U;
    if (HAL_ADC_PollForConversion(&hadc1, 10U) == HAL_OK)
    {
        val = (uint16_t)HAL_ADC_GetValue(&hadc1);
    }
    (void)HAL_ADC_Stop(&hadc1);
    return val;
}

int32_t TempSensor_ReadMcuDeciC(void)
{
    /* Ensure TSVREFE bit in ADC->CCR is enabled */
    ADC->CCR |= ADC_CCR_TSVREFE;

    /* Average 4 samples of VREFINT (1.21V typical on STM32F407) and TEMPSENSOR */
    uint32_t sum_vref = 0U;
    uint32_t sum_temp = 0U;
    for (uint8_t i = 0U; i < 4U; i++)
    {
        sum_vref += adc1_read_channel(ADC_CHANNEL_VREFINT);
        sum_temp += adc1_read_channel(ADC_CHANNEL_TEMPSENSOR);
    }

    uint32_t raw_vref = sum_vref >> 2;
    uint32_t raw_temp = sum_temp >> 2;

    /* Calculate true Vsense in microvolts:
     * If VREFINT is valid (~1490 counts at 3.3V), Vsense_uV = raw_temp * 1,210,000 / raw_vref.
     * Fallback to nominal 3300 mV if VREFINT reading is out of range. */
    int32_t vsense_uv;
    if (raw_vref >= 1000U && raw_vref <= 2200U)
    {
        vsense_uv = (int32_t)(((uint64_t)raw_temp * 1210000ULL) / raw_vref);
    }
    else
    {
        vsense_uv = (int32_t)(((uint64_t)raw_temp * 3300000ULL) / 4095ULL);
    }

    /* STM32F407 datasheet parameters:
     * V25 = 0.76 V = 760,000 uV at 25.0 °C
     * Avg_Slope = 2.5 mV/°C = 250 uV per 0.1 °C */
    int32_t temp_deci = 250 + ((vsense_uv - 760000) / 250);
    s_cached_mcu_dc   = temp_deci;
    s_overheated      = (temp_deci >= TEMP_MCU_OVERHEAT_DECIC);
    return temp_deci;
}

bool TempSensor_Init(void)
{
    ADC->CCR |= ADC_CCR_TSVREFE;
    (void)TempSensor_ReadMcuDeciC();

    s_ext_detected = Ds18b20_Init();
    if (s_ext_detected)
    {
        printf("[TEMP] DS18B20 detected on PA15 (ROM: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X), MCU=%ld.%ld C\r\n",
               ds18b20[0].Address[0], ds18b20[0].Address[1], ds18b20[0].Address[2], ds18b20[0].Address[3],
               ds18b20[0].Address[4], ds18b20[0].Address[5], ds18b20[0].Address[6], ds18b20[0].Address[7],
               (long)(s_cached_mcu_dc / 10), (long)((s_cached_mcu_dc >= 0 ? s_cached_mcu_dc : -s_cached_mcu_dc) % 10));

        /* Perform one initial conversion on boot so SysInfo has an immediate reading */
        int32_t dummy = 0;
        (void)TempSensor_ReadExtDeciC(&dummy);
    }
    else
    {
        printf("[TEMP] No DS18B20 detected on PA15, MCU=%ld.%ld C\r\n",
               (long)(s_cached_mcu_dc / 10), (long)((s_cached_mcu_dc >= 0 ? s_cached_mcu_dc : -s_cached_mcu_dc) % 10));
    }
    return s_ext_detected;
}

bool TempSensor_ReadExtDeciC(int32_t *out_deci_c)
{
    if (!s_ext_detected)
    {
        s_ext_detected = Ds18b20_Init();
        if (!s_ext_detected)
        {
            s_ext_valid = false;
            return false;
        }
    }

    DS18B20_StartAll(&OneWire);

    /* Wait up to 800 ms for DS18B20 conversion to complete, refreshing IWDG */
    uint32_t t0 = HAL_GetTick();
    while (!DS18B20_AllDone(&OneWire))
    {
        (void)HAL_IWDG_Refresh(&hiwdg);
        if ((HAL_GetTick() - t0) > 850U)
        {
            break;
        }
        if (osKernelGetState() == osKernelRunning)
        {
            (void)osDelay(15U);
        }
        else
        {
            HAL_Delay(15U);
        }
    }

    float temp_c = 0.0f;
    if (DS18B20_Read(&OneWire, ds18b20[0].Address, &temp_c))
    {
        ds18b20[0].Temperature = temp_c;
        ds18b20[0].DataIsValid = true;
        int32_t deci = (temp_c >= 0.0f) ? (int32_t)(temp_c * 10.0f + 0.5f)
                                        : (int32_t)(temp_c * 10.0f - 0.5f);
        s_cached_ext_dc = deci;
        s_ext_valid     = true;
        if (out_deci_c != NULL)
        {
            *out_deci_c = deci;
        }
        return true;
    }

    ds18b20[0].DataIsValid = false;
    s_ext_valid            = false;
    return false;
}

int32_t TempSensor_GetCachedMcuDeciC(void)
{
    return s_cached_mcu_dc;
}

int32_t TempSensor_GetCachedExtDeciC(bool *out_valid)
{
    if (out_valid != NULL)
    {
        *out_valid = s_ext_valid;
    }
    return s_cached_ext_dc;
}

bool TempSensor_IsOverheated(void)
{
    return s_overheated;
}

void TempSensor_PeriodicMcuCheck(void)
{
    uint32_t now = HAL_GetTick();
    if ((now - s_last_mcu_tick) < 5000U)
    {
        return;
    }
    s_last_mcu_tick = now;

    bool was_overheated = s_overheated;
    int32_t mcu_dc      = TempSensor_ReadMcuDeciC();
    if (s_overheated && !was_overheated)
    {
        printf("[TEMP] WARNING: MCU overheat detected (%ld.%ld C)!\r\n",
               (long)(mcu_dc / 10), (long)(mcu_dc % 10));
        Beeper_ErrorAlert();
    }
}
