/* SPDX-License-Identifier: Apache-2.0 */
#include <board.h>
#include "epd_compute_cache.h"

void epd_compute_barrier(void) { __DMB(); }

void epd_compute_cache_clean(uint32_t address, uint32_t capacity)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (IS_DCACHED_RAM(address))
    {
        /* The SDK range wrapper may operate on the whole cache for a large
         * range. Keep maintenance confined to the independently owned lines. */
        while (capacity)
        {
            uint32_t bytes = capacity > 4096 ? 4096 : capacity;
            SCB_CleanDCache_by_Addr((void *)(uintptr_t)address, bytes);
            address += bytes;
            capacity -= bytes;
        }
    }
#else
    (void)address;
    (void)capacity;
#endif
    __DSB();
}

void epd_compute_cache_invalidate(uint32_t address, uint32_t capacity)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (IS_DCACHED_RAM(address))
    {
        while (capacity)
        {
            uint32_t bytes = capacity > 4096 ? 4096 : capacity;
            SCB_InvalidateDCache_by_Addr((void *)(uintptr_t)address, bytes);
            address += bytes;
            capacity -= bytes;
        }
    }
#else
    (void)address;
    (void)capacity;
#endif
    __DSB();
}

void epd_compute_cache_clean_invalidate(uint32_t address, uint32_t capacity)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (IS_DCACHED_RAM(address))
    {
        /* Transfer independently owned lines without a whole-cache operation. */
        while (capacity)
        {
            uint32_t bytes = capacity > 4096 ? 4096 : capacity;
            SCB_CleanInvalidateDCache_by_Addr((void *)(uintptr_t)address, bytes);
            address += bytes;
            capacity -= bytes;
        }
    }
#else
    (void)address;
    (void)capacity;
#endif
    __DSB();
}
