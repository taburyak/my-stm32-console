#include "sram_ext.h"

void SRAM_EXT_Init(void)
{
    /* External SRAM is hardware initialized via CubeMX MX_FSMC_Init() */
}

bool SRAM_EXT_IsAddressInSRAM(const void *ptr)
{
    uint32_t addr = (uint32_t)ptr;
    return (addr >= SRAM_EXT_BASE_ADDR && addr <= SRAM_EXT_END_ADDR);
}

void* SRAM_EXT_GetFrameBuffer1(void)
{
    return (void *)(SRAM_EXT_BASE_ADDR + SRAM_FB1_OFFSET);
}

void* SRAM_EXT_GetFrameBuffer2(void)
{
    return (void *)(SRAM_EXT_BASE_ADDR + SRAM_FB2_OFFSET);
}

void SRAM_EXT_CopyFast32(uint32_t *dst, const uint32_t *src, uint32_t words_count)
{
    while (words_count--)
    {
        *dst++ = *src++;
    }
}

void SRAM_EXT_FillFast32(uint32_t *dst, uint32_t value, uint32_t words_count)
{
    while (words_count--)
    {
        *dst++ = value;
    }
}
