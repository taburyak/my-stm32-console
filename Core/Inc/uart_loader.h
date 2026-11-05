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

#ifdef __cplusplus
}
#endif

#endif /* UART_LOADER_H */
