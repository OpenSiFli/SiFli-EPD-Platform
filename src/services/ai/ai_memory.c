/* SPDX-License-Identifier: Apache-2.0 */
#include "ai_internal.h"
#include "epd_memory.h"
#include <rthw.h>
#include <string.h>

typedef union {
    struct { size_t size, charge; } allocation;
    uint64_t alignment;
    void *pointer_alignment;
} ai_allocation_t;
static size_t memory_used, memory_peak;

void *ai_alloc(size_t size)
{
    /* Reserve alignment and the SDK heap header, including an unsplittable
     * tail. Conservative charging never hides allocator overhead. */
    const size_t overhead = sizeof(ai_allocation_t) +
                            2u * sizeof(struct rt_memheap_item) + 128u;
    if (!size || size > AI_MEMORY_LIMIT - overhead) return NULL;
    size_t charge = (size + overhead + 63u) & ~(size_t)63u;
    rt_base_t level = rt_hw_interrupt_disable();
    if (charge > AI_MEMORY_LIMIT - memory_used)
    { rt_hw_interrupt_enable(level); return NULL; }
    memory_used += charge;
    if (memory_used > memory_peak) memory_peak = memory_used;
    rt_hw_interrupt_enable(level);
    ai_allocation_t *allocation = epd_app_alloc(sizeof(*allocation) + size, EPD_APP_PSRAM);
    if (!allocation)
    {
        level = rt_hw_interrupt_disable();
        memory_used -= charge;
        rt_hw_interrupt_enable(level);
        return NULL;
    }
    allocation->allocation.size = size;
    allocation->allocation.charge = charge;
    return allocation + 1;
}

void *ai_calloc(size_t count, size_t size)
{
    if (!size || !count || count > AI_MEMORY_LIMIT / size) return NULL;
    void *memory = ai_alloc(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}

void ai_free(void *memory)
{
    if (!memory) return;
    ai_allocation_t *allocation = (ai_allocation_t *)memory - 1;
    size_t charge = allocation->allocation.charge;
    epd_app_free(allocation);
    rt_base_t level = rt_hw_interrupt_disable();
    memory_used -= charge;
    rt_hw_interrupt_enable(level);
}

void *ai_realloc(void *old, size_t size)
{
    if (!size) { ai_free(old); return NULL; }
    if (!old) return ai_alloc(size);
    ai_allocation_t *allocation = (ai_allocation_t *)old - 1;
    if (size == allocation->allocation.size) return old;
    void *next = ai_alloc(size);
    if (!next) return NULL;
    memcpy(next, old, size < allocation->allocation.size ? size : allocation->allocation.size);
    ai_free(old);
    return next;
}

char *ai_strndup(const char *text, size_t size)
{
    if (!text || size >= AI_MEMORY_LIMIT) return NULL;
    char *copy = ai_alloc(size + 1);
    if (copy) { memcpy(copy, text, size); copy[size] = 0; }
    return copy;
}

void ai_memory_usage(size_t *used, size_t *peak)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (used) *used = memory_used;
    if (peak) *peak = memory_peak;
    rt_hw_interrupt_enable(level);
}
