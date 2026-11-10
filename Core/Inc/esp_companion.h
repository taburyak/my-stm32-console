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
 * @brief Returns true if ESP32-C6 is connected to a Wi-Fi AP and has an IP
 */
bool ESPCompanion_IsWifiConnected(void);

/**
 * @brief Returns Wi-Fi connection status:
 *        1 = Connected (AP assigned IP)
 *        2 = Connecting / Reconnecting in progress
 *        0 = Disconnected / Idle
 *       -1 = Authentication / connection failed
 */
int8_t ESPCompanion_GetWifiStatus(void);

/**
 * @brief Returns the ESP32-C6 Wi-Fi IP address string (e.g. "192.168.1.45" or "0.0.0.0")
 */
const char *ESPCompanion_GetWifiIp(void);

/**
 * @brief Returns the connected Wi-Fi SSID
 */
const char *ESPCompanion_GetWifiSsid(void);

/**
 * @brief Returns RSSI signal strength in dBm (e.g. -65)
 */
int8_t ESPCompanion_GetWifiRssi(void);

/**
 * @brief Send Wi-Fi credentials to ESP32-C6 (which stores them in NVS and connects)
 */
bool ESPCompanion_SendWifiCredentials(const char *ssid, const char *pass);

/**
 * @brief Load Wi-Fi credentials from SD:/system/wifi.cfg and send to ESP32-C6
 */
bool ESPCompanion_LoadAndSendWifiFromSD(void);

/**
 * @brief Save Wi-Fi credentials to SD:/system/wifi.cfg
 */
bool ESPCompanion_SaveWifiToSD(const char *ssid, const char *pass);

/**
 * @brief Read Wi-Fi credentials from SD:/system/wifi.cfg into caller buffers
 */
bool ESPCompanion_ReadWifiFromSD(char *ssid_out, size_t ssid_max, char *pass_out, size_t pass_max);

/**
 * @brief Diagnostic packet counters
 */
uint32_t ESPCompanion_GetRxPackets(void);
uint32_t ESPCompanion_GetTxPackets(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_COMPANION_H */
