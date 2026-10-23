#include "ccmram.h"

bool CCMRAM_IsAddressInCCMRAM(const void *ptr)
{
    uint32_t addr = (uint32_t)ptr;
    return (addr >= CCMRAM_BASE_ADDR && addr <= CCMRAM_END_ADDR);
}

bool CCMRAM_IsAddressValidForDMA(const void *ptr)
{
    /* CCM RAM is connected strictly to CPU D-Bus and IS NOT accessible by DMA */
    return !CCMRAM_IsAddressInCCMRAM(ptr);
}
