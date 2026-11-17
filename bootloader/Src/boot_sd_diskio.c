#include "ff_gen_drv.h"
#include "stm32f4xx_hal.h"
#include <string.h>

extern SD_HandleTypeDef hsd;

static volatile DSTATUS s_stat = STA_NOINIT;
__attribute__((aligned(4))) static uint8_t s_scratch[512];

static DSTATUS boot_sd_check(void)
{
    return (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER) ? 0 : STA_NOINIT;
}

static DSTATUS boot_sd_init(BYTE lun)
{
    (void)lun;
    s_stat = STA_NOINIT;

    if (HAL_SD_Init(&hsd) != HAL_OK)
    {
        return s_stat;
    }
    if (HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B) != HAL_OK)
    {
        /* Fallback to 1-bit mode if 4-bit fails */
    }
    s_stat = boot_sd_check();
    return s_stat;
}

static DSTATUS boot_sd_status(BYTE lun)
{
    (void)lun;
    return boot_sd_check();
}

static DRESULT boot_sd_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;
    if (((uintptr_t)buff & 3U) == 0U)
    {
        if (HAL_SD_ReadBlocks(&hsd, buff, (uint32_t)sector, (uint32_t)count, 2000U) != HAL_OK)
        {
            return RES_ERROR;
        }
        uint32_t t0 = HAL_GetTick();
        while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER)
        {
            if ((HAL_GetTick() - t0) > 2000U) return RES_ERROR;
        }
        return RES_OK;
    }

    for (UINT i = 0U; i < count; i++)
    {
        if (HAL_SD_ReadBlocks(&hsd, s_scratch, (uint32_t)(sector + i), 1U, 2000U) != HAL_OK)
        {
            return RES_ERROR;
        }
        uint32_t t0 = HAL_GetTick();
        while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER)
        {
            if ((HAL_GetTick() - t0) > 2000U) return RES_ERROR;
        }
        memcpy(buff + i * 512U, s_scratch, 512U);
    }
    return RES_OK;
}

static DRESULT boot_sd_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;
    for (UINT i = 0U; i < count; i++)
    {
        memcpy(s_scratch, buff + i * 512U, 512U);
        if (HAL_SD_WriteBlocks(&hsd, s_scratch, (uint32_t)(sector + i), 1U, 2000U) != HAL_OK)
        {
            return RES_ERROR;
        }
        uint32_t t0 = HAL_GetTick();
        while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER)
        {
            if ((HAL_GetTick() - t0) > 2000U) return RES_ERROR;
        }
    }
    return RES_OK;
}

static DRESULT boot_sd_ioctl(BYTE lun, BYTE cmd, void *buff)
{
    (void)lun;
    HAL_SD_CardInfoTypeDef info;

    if (s_stat & STA_NOINIT) return RES_NOTRDY;

    switch (cmd)
    {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            HAL_SD_GetCardInfo(&hsd, &info);
            *(DWORD *)buff = info.LogBlockNbr;
            return RES_OK;
        case GET_SECTOR_SIZE:
            HAL_SD_GetCardInfo(&hsd, &info);
            *(WORD *)buff = (WORD)info.LogBlockSize;
            return RES_OK;
        case GET_BLOCK_SIZE:
            HAL_SD_GetCardInfo(&hsd, &info);
            *(DWORD *)buff = info.LogBlockSize / 512U;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

const Diskio_drvTypeDef BootSD_Driver = {
    boot_sd_init,
    boot_sd_status,
    boot_sd_read,
    boot_sd_write,
    boot_sd_ioctl
};
