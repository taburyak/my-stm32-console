#ifndef UART_CLI_H
#define UART_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Initialize the interactive PC terminal CLI on USART3.
 */
void UARTCli_Init(void);

/**
 * @brief Feed a received byte from USART3_IRQHandler (non-blocking, ISR-safe).
 */
void UARTCli_OnRxByte(uint8_t b);

/**
 * @brief Reset pending RX ring and line buffer (called when binary UPLD! triggers).
 */
void UARTCli_Reset(void);

/**
 * @brief Process queued CLI input and execute commands from StartDefaultTask.
 */
void UARTCli_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_CLI_H */
