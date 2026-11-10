#ifndef TEMP_SENSOR_H
#define TEMP_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* Overheat warning threshold in tenths of °C (750 = 75.0 °C) */
#define TEMP_MCU_OVERHEAT_DECIC   750

/**
 * @brief Initialize internal MCU temperature sensor (ADC1 IN16/IN17) and probe external DS18B20 on PA15.
 * @return true if external DS18B20 sensor was detected on the 1-Wire bus
 */
bool TempSensor_Init(void);

/**
 * @brief Read STM32F407 internal die temperature via ADC1 (calibrated against internal VREFINT 1.21V).
 * @return Temperature in tenths of a degree Celsius (e.g. 385 = 38.5 °C)
 */
int32_t TempSensor_ReadMcuDeciC(void);

/**
 * @brief Trigger an on-demand temperature conversion and read the external DS18B20 sensor on PA15.
 *        If called from a FreeRTOS task, yields with osDelay() while the sensor converts (~750 ms).
 * @param out_deci_c Receives temperature in tenths of °C (e.g. 246 = 24.6 °C)
 * @return true if a valid CRC-checked reading was obtained
 */
bool TempSensor_ReadExtDeciC(int32_t *out_deci_c);

/**
 * @brief Return the most recently cached MCU temperature in tenths of °C.
 */
int32_t TempSensor_GetCachedMcuDeciC(void);

/**
 * @brief Return the most recently cached external DS18B20 temperature in tenths of °C.
 * @param out_valid Optional pointer receiving true if an external sensor is present and valid
 */
int32_t TempSensor_GetCachedExtDeciC(bool *out_valid);

/**
 * @brief Returns true if the MCU die temperature is currently >= TEMP_MCU_OVERHEAT_DECIC.
 */
bool TempSensor_IsOverheated(void);

/**
 * @brief Periodic low-rate background monitor for the internal MCU temperature (call from GUI loop).
 *        Samples ADC1 every 5 seconds and raises an acoustic/visual warning if overheated.
 */
void TempSensor_PeriodicMcuCheck(void);

#ifdef __cplusplus
}
#endif

#endif /* TEMP_SENSOR_H */
