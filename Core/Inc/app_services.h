#ifndef APP_SERVICES_H
#define APP_SERVICES_H

#include "console_abi.h"
#include <stdbool.h>
#include <stdint.h>

/* Random numbers, input snapshot, LCD blit, audio sequences and GPIO for the sandboxed app */
uint32_t AppSvc_Random(void);
bool     AppSvc_GetInputState(SDK_InputState_t *dst);
int32_t  AppSvc_LcdBlit(const SDK_BlitParams_t *user_params); /* syscall gate */
int32_t  AppSvc_AudioPlay(const SDK_Note_t *notes, uint32_t count, uint32_t flags);
void     AppSvc_AudioStop(void);
int32_t  AppSvc_GpioGetCaps(SDK_GpioCaps_t *dst);
int32_t  AppSvc_GpioConfig(uint32_t pin, uint32_t mode);
int32_t  AppSvc_GpioWrite(uint32_t pin, uint32_t value);
int32_t  AppSvc_GpioRead(uint32_t pin);

/* Returns sound and user pins to a safe state after the app ends */
void AppSvc_ResetHardware(void);

#endif /* APP_SERVICES_H */
