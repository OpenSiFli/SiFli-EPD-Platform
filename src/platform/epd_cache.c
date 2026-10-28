#include "bf0_hal.h"

#if defined(SOC_BF0_HCPU) && defined(PSRAM_CACHE_WB)
int mpu_dcache_invalidate(void *data, uint32_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    /* DMA completion owns only this range. Invalidating the whole cache can
     * discard unrelated dirty PSRAM data, including suspended worker stacks. */
    if (IS_DCACHED_RAM(data))
        SCB_InvalidateDCache_by_Addr(data, size);
#endif
    return 0;
}
#endif
