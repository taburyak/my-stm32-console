#ifndef VS1053_H
#define VS1053_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

/* SCI (control) registers */
#define VS1053_SCI_MODE         0x00U
#define VS1053_SCI_STATUS       0x01U
#define VS1053_SCI_BASS         0x02U
#define VS1053_SCI_CLOCKF       0x03U
#define VS1053_SCI_DECODE_TIME  0x04U
#define VS1053_SCI_AUDATA       0x05U
#define VS1053_SCI_HDAT0        0x08U
#define VS1053_SCI_HDAT1        0x09U
#define VS1053_SCI_WRAM         0x06U
#define VS1053_SCI_WRAMADDR     0x07U
#define VS1053_SCI_VOL          0x0BU

/* SCI_MODE bits */
#define VS1053_SM_DIFF          (1U << 0)
#define VS1053_SM_RESET         (1U << 2)
#define VS1053_SM_TESTS         (1U << 5)
#define VS1053_SM_SDINEW        (1U << 11)

/**
 * @brief Reset and configure the VS1053 on the given SPI bus (XRESET, XCS, XDCS, DREQ pins from main.h).
 * @return true if a VLSI decoder answered on SCI; false if the module is absent or not responding.
 */
bool VS1053_Init(SPI_HandleTypeDef *hspi);

bool     VS1053_IsPresent(void);
/** @brief Chip version from SCI_STATUS[7:4]: 4 = VS1053, 3 = VS1003, 5 = VS1033, 6 = VS1063 */
uint8_t  VS1053_GetVersion(void);
bool     VS1053_IsReady(void);

uint16_t VS1053_ReadReg(uint8_t addr);
void     VS1053_WriteReg(uint8_t addr, uint16_t value);

/** @brief Output attenuation per channel in 0.5 dB steps: 0 = loudest, 254 = silent */
void     VS1053_SetVolume(uint8_t left, uint8_t right);

/**
 * @brief SM_DIFF: invert the left channel. A 4-pole (TRRS) headset in the 3-pole jack hears only L-R,
 *        which cancels mono audio; with SM_DIFF it hears the full mono mix instead.
 */
void     VS1053_SetDifferential(bool enable);
bool     VS1053_IsDifferential(void);

/**
 * @brief Sine test code for VS1053_SineStart() closest to hz (Fsin = Fs * S / 128).
 * @param actual_hz Optional: receives the frequency that will actually be generated.
 */
uint8_t  VS1053_SineCode(uint32_t hz, uint32_t *actual_hz);
bool     VS1053_SineStart(uint8_t code);
void     VS1053_SineStop(void);

#ifdef __cplusplus
}
#endif

#endif /* VS1053_H */
