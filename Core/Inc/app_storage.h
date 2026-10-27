#ifndef APP_STORAGE_H
#define APP_STORAGE_H

#include <stdint.h>

/* EEPROM window and W25Q partition for the sandboxed app (SDK_SVC_EEPROM_*, SDK_SVC_FLASH_*); called from the syscall gate */
int32_t AppStorage_EepromRead(uint32_t offset, void *buf, uint32_t len);
int32_t AppStorage_EepromWrite(uint32_t offset, const void *buf, uint32_t len);
int32_t AppStorage_FlashRead(uint32_t offset, void *buf, uint32_t len);
int32_t AppStorage_FlashErase(uint32_t offset);
int32_t AppStorage_FlashWrite(uint32_t offset, const void *buf, uint32_t len);

#endif /* APP_STORAGE_H */
