#include "app_storage.h"
#include "app_loader.h"
#include "AT24Cxx.h"
#include "w25qxx.h"
#include "status_led.h"
#include <string.h>

/* EEPROM 0x00..0x7F belongs to the OS; the app window is the upper half */
#define APP_EEPROM_BASE     0x80U
#define APP_FLASH_BASE      0x00F00000UL /* last 1 MB of the 16 MB W25Q128 */
#define FLASH_PAGE_SIZE     256U

_Static_assert((APP_EEPROM_BASE + SDK_EEPROM_SIZE) <= EEPROM_MAX_ADDR, "EEPROM app window out of range");

static uint8_t s_eeprom_shadow[SDK_EEPROM_SIZE];

static bool AppStorage_RangeOk(uint32_t offset, uint32_t len, uint32_t size)
{
    return (len != 0U) && (offset < size) && (len <= (size - offset));
}

static bool AppStorage_FlashPresent(void)
{
    return w25qxx.CapacityInKiloByte >= ((APP_FLASH_BASE + SDK_FLASH_SIZE) / 1024UL);
}

int32_t AppStorage_EepromRead(uint32_t offset, void *buf, uint32_t len)
{
    if (!AppStorage_RangeOk(offset, len, SDK_EEPROM_SIZE) || !AppLoader_IsUserBufferValid(buf, len))
    {
        return SDK_ERR_INVALID;
    }
    if (AT24Cxx_Read(APP_EEPROM_BASE + offset, buf, len) != HAL_OK)
    {
        return SDK_ERR_IO;
    }
    return (int32_t)len;
}

int32_t AppStorage_EepromWrite(uint32_t offset, const void *buf, uint32_t len)
{
    if (!AppStorage_RangeOk(offset, len, SDK_EEPROM_SIZE) || !AppLoader_IsUserBufferValid(buf, len))
    {
        return SDK_ERR_INVALID;
    }

    /* Saves write cycles: identical data is not rewritten */
    if (AT24Cxx_Read(APP_EEPROM_BASE + offset, s_eeprom_shadow, len) != HAL_OK)
    {
        return SDK_ERR_IO;
    }
    if (memcmp(s_eeprom_shadow, buf, len) == 0)
    {
        return (int32_t)len;
    }

    if (AT24Cxx_Write(APP_EEPROM_BASE + offset, buf, len) != HAL_OK)
    {
        return SDK_ERR_IO;
    }
    return (int32_t)len;
}

int32_t AppStorage_FlashRead(uint32_t offset, void *buf, uint32_t len)
{
    if (!AppStorage_RangeOk(offset, len, SDK_FLASH_SIZE) || !AppLoader_IsUserBufferValid(buf, len))
    {
        return SDK_ERR_INVALID;
    }
    if (!AppStorage_FlashPresent())
    {
        return SDK_ERR_NOT_SUPPORTED;
    }

    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);
    W25qxx_ReadBytes((uint8_t *)buf, APP_FLASH_BASE + offset, len);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    return (int32_t)len;
}

int32_t AppStorage_FlashErase(uint32_t offset)
{
    if ((offset >= SDK_FLASH_SIZE) || ((offset % SDK_FLASH_SECTOR) != 0U))
    {
        return SDK_ERR_INVALID;
    }
    if (!AppStorage_FlashPresent())
    {
        return SDK_ERR_NOT_SUPPORTED;
    }

    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);
    W25qxx_EraseSector((APP_FLASH_BASE + offset) / SDK_FLASH_SECTOR);
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    return SDK_OK;
}

int32_t AppStorage_FlashWrite(uint32_t offset, const void *buf, uint32_t len)
{
    if (!AppStorage_RangeOk(offset, len, SDK_FLASH_SIZE) || !AppLoader_IsUserBufferValid(buf, len))
    {
        return SDK_ERR_INVALID;
    }
    if (!AppStorage_FlashPresent())
    {
        return SDK_ERR_NOT_SUPPORTED;
    }

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t addr = APP_FLASH_BASE + offset;
    uint32_t left = len;

    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_ON);
    while (left > 0U)
    {
        uint32_t page_off = addr % FLASH_PAGE_SIZE;
        uint32_t chunk = FLASH_PAGE_SIZE - page_off;
        if (chunk > left)
        {
            chunk = left;
        }
        W25qxx_WritePage((uint8_t *)src, addr / FLASH_PAGE_SIZE, page_off, chunk);
        src  += chunk;
        addr += chunk;
        left -= chunk;
    }
    StatusLED_Set(STATUS_LED_DISK, STATUS_LED_OFF);
    return (int32_t)len;
}
