#ifndef ESP_COMPANION_H
#define ESP_COMPANION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize UART4 communication with ESP32-C6 companion co-processor
 */
void ESPCompanion_Init(void);

/**
 * @brief Periodic poll routine (called from GUI/main loop)
 *        Handles DMA circular reception, heartbeat detection and periodic telemetry transmission.
 */
void ESPCompanion_Poll(void);

/**
 * @brief Returns true if ESP32-C6 heartbeat was received recently (< 1500 ms)
 */
bool ESPCompanion_IsConnected(void);

/**
 * @brief Diagnostic packet counters
 */
uint32_t ESPCompanion_GetRxPackets(void);
uint32_t ESPCompanion_GetTxPackets(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_COMPANION_H */
