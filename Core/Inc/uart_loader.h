#ifndef UART_LOADER_H
#define UART_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize background USART3 RX wake detector for zero-touch uploads.
 */
void UARTLoader_Init(void);

/**
 * @brief Feed a received byte from USART3_IRQHandler when RXNE is set.
 *        Detects the "UPLD!\n" auto-upload wake sequence from tools/uart_upload.py.
 */
void UARTLoader_OnRxByte(uint8_t b);

/**
 * @brief Returns true once if a background auto-upload trigger was received on USART3.
 */
bool UARTLoader_TakeAutoTrigger(void);

/**
 * @brief Returns true while an active UART transfer session owns USART3.
 *        Used by _write() / __io_putchar() to suppress background printf noise.
 */
bool UARTLoader_IsSessionActive(void);

/**
 * @brief Run interactive or auto-triggered UART file receive session over USART3.
 *        Supports both the fast framed "UPLD" protocol (tools/uart_upload.py)
 *        and standard XMODEM-CRC / XMODEM-1K (Tera Term, ExtraPuTTY, minicom).
 * @param auto_triggered true if entered via "UPLD!\n" wake sequence
 * @return true if a file was received (and saved or launched) successfully
 */
bool UARTLoader_RunSession(bool auto_triggered);

/**
 * @brief Request an asynchronous screenshot capture (from button/gamepad hotkey).
 */
void Screenshot_Request(void);

/**
 * @brief Returns true once if a screenshot hotkey was triggered.
 */
bool Screenshot_TakeRequest(void);

/**
 * @brief Capture the current 320x240 display GRAM into a 16-bit RGB565 BMP file
 *        under SD:/screenshots/shot_NNN.bmp.
 * @param out_path Optional buffer to receive the saved relative path
 * @param out_max  Capacity of out_path
 * @return true if saved to SD card successfully
 */
bool Screenshot_CaptureToSD(char *out_path, uint32_t out_max);

#ifdef __cplusplus
}
#endif

#endif /* UART_LOADER_H */
