/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <rthw.h>
#include <board.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "epd_compute_memory.h"

typedef struct
{
    void *allocation;
    size_t size;
} algorithm_allocation_t;

#define ALGORITHM_ALIGNMENT 8u
static epd_compute_memory_stats_t memory_stats;

static void add_counter(uint32_t *value, size_t increment)
{
    if (increment > UINT32_MAX - *value)
    { *value = UINT32_MAX; memory_stats.saturated = 1; }
    else *value += (uint32_t)increment;
}

static void rejected_allocation(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    add_counter(&memory_stats.attempts, 1);
    add_counter(&memory_stats.failures, 1);
    rt_hw_interrupt_enable(level);
}

#ifdef ACPU_PSRAM_START_ADDR
static struct rt_memheap algorithm_heap;
static bool heap_ready;

static int algorithm_heap_init(void)
{
    /* This range is excluded from HCPU's PSRAM_DATA partition. The allocator
     * and its metadata are owned by ACPU; only explicit request buffers cross
     * the shared cache ownership boundary. */
    rt_err_t result = rt_memheap_init(&algorithm_heap, "algheap",
        (void *)ACPU_PSRAM_START_ADDR, ACPU_PSRAM_SIZE);
    heap_ready = result == RT_EOK;
    return result;
}
INIT_PREV_EXPORT(algorithm_heap_init);
#endif

void epd_compute_memory_stats_get(epd_compute_memory_stats_t *stats)
{
    if (!stats) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *stats = memory_stats;
#ifdef ACPU_PSRAM_START_ADDR
    if (heap_ready)
    {
        stats->heap_present = 1;
        stats->heap_size = algorithm_heap.pool_size;
        stats->heap_used = algorithm_heap.pool_size - algorithm_heap.available_size;
        stats->heap_peak = algorithm_heap.max_used_size;
    }
#endif
    rt_hw_interrupt_enable(level);
#ifdef RT_USING_HEAP
    stats->system_present = 1;
    rt_memory_info(&stats->system_size, &stats->system_used, &stats->system_peak);
#endif
}

void *epd_compute_malloc(size_t size)
{
    const size_t overhead = sizeof(algorithm_allocation_t) + ALGORITHM_ALIGNMENT - 1u;
    if (!size || size > SIZE_MAX - overhead)
    { rejected_allocation(); return NULL; }
    size_t bytes = size + overhead;
#ifdef ACPU_PSRAM_START_ADDR
    if (!heap_ready || bytes > ACPU_PSRAM_SIZE)
    { rejected_allocation(); return NULL; }
#endif
    rt_base_t level = rt_hw_interrupt_disable();
    add_counter(&memory_stats.attempts, 1);
    add_counter(&memory_stats.heap_calls, 1);
    add_counter(&memory_stats.requested_bytes, bytes);
    rt_hw_interrupt_enable(level);
#ifdef ACPU_PSRAM_START_ADDR
    void *allocation = rt_memheap_alloc(&algorithm_heap, bytes);
#else
    void *allocation = rt_malloc(bytes);
#endif
    level = rt_hw_interrupt_disable();
    if (allocation)
    {
        memory_stats.payload_live += size;
        memory_stats.charged_live += bytes;
        add_counter(&memory_stats.live_blocks, 1);
        if (memory_stats.payload_live > memory_stats.payload_peak)
            memory_stats.payload_peak = memory_stats.payload_live;
        if (memory_stats.charged_live > memory_stats.charged_peak)
            memory_stats.charged_peak = memory_stats.charged_live;
        if (memory_stats.live_blocks > memory_stats.peak_blocks)
            memory_stats.peak_blocks = memory_stats.live_blocks;
    }
    else add_counter(&memory_stats.failures, 1);
    rt_hw_interrupt_enable(level);
    if (!allocation) return NULL;
    /* RT_ALIGN_SIZE is four on this target. libFLAC also stores uint64_t
     * fields, so align the returned pointer, not merely the requested size. */
    uintptr_t address = ((uintptr_t)allocation + overhead) &
                        ~(uintptr_t)(ALGORITHM_ALIGNMENT - 1u);
    algorithm_allocation_t *header = (algorithm_allocation_t *)address - 1;
    header->allocation = allocation;
    header->size = size;
    return (void *)address;
}

void *epd_compute_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size)
    { rejected_allocation(); return NULL; }
    size_t bytes = count * size;
    void *memory = epd_compute_malloc(bytes);
    if (memory) memset(memory, 0, bytes);
    return memory;
}

void epd_compute_free(void *memory)
{
    if (!memory) return;
    algorithm_allocation_t *header = (algorithm_allocation_t *)memory - 1;
    size_t size = header->size;
    /* Release the charge before returning storage to the heap, so reuse by
     * another thread cannot count the same underlying block twice. */
    rt_base_t level = rt_hw_interrupt_disable();
    memory_stats.payload_live -= size;
    memory_stats.charged_live -= size + sizeof(*header) + ALGORITHM_ALIGNMENT - 1u;
    --memory_stats.live_blocks;
    add_counter(&memory_stats.frees, 1);
    rt_hw_interrupt_enable(level);
#ifdef ACPU_PSRAM_START_ADDR
    rt_memheap_free(header->allocation);
#else
    rt_free(header->allocation);
#endif
}

void *epd_compute_realloc(void *memory, size_t size)
{
    if (!size) { epd_compute_free(memory); return NULL; }
    if (!memory) return epd_compute_malloc(size);
    algorithm_allocation_t *header = (algorithm_allocation_t *)memory - 1;
    void *replacement = epd_compute_malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, memory, size < header->size ? size : header->size);
    epd_compute_free(memory);
    return replacement;
}
