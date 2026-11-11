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
 * @brief Outdoor Weather Telemetry from ESP32-C6 (Open-Meteo)
 */
typedef struct {
    bool     valid;
    int16_t  temp_deci;      /* Outdoor temperature in 0.1 C (e.g. 215 = 21.5 C) */
    int16_t  feels_deci;     /* Apparent temperature in 0.1 C */
    uint8_t  humidity;       /* Relative humidity 0..100 % */
    uint16_t wind_deci;      /* Wind speed in 0.1 km/h */
    uint8_t  wmo_code;       /* WMO weather condition code (0..99) */
    int16_t  day_hi_deci[3]; /* 3-day max temp in 0.1 C */
    int16_t  day_lo_deci[3]; /* 3-day min temp in 0.1 C */
    uint8_t  day_wmo[3];     /* 3-day WMO codes */
    char     city[24];       /* Resolved city name */
    uint32_t updated_tick;   /* HAL_GetTick() of last update */
} ESP_WeatherInfo_t;

/**
 * @brief Get cached outdoor weather telemetry
 */
const ESP_WeatherInfo_t *ESPCompanion_GetWeather(void);

/**
 * @brief Request immediate weather refresh from ESP32-C6
 */
bool ESPCompanion_RequestWeatherUpdate(void);

/**
 * @brief Diagnostic packet counters
 */
uint32_t ESPCompanion_GetRxPackets(void);
uint32_t ESPCompanion_GetTxPackets(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_COMPANION_H */
