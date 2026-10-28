/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
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

void *epd_compute_malloc(size_t size)
{
    const size_t overhead = sizeof(algorithm_allocation_t) + ALGORITHM_ALIGNMENT - 1u;
    if (!size || size > SIZE_MAX - overhead) return NULL;
    size_t bytes = size + overhead;
#ifdef ACPU_PSRAM_START_ADDR
    if (!heap_ready || bytes > ACPU_PSRAM_SIZE) return NULL;
    void *allocation = rt_memheap_alloc(&algorithm_heap, bytes);
#else
    void *allocation = rt_malloc(bytes);
#endif
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
    if (size && count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    void *memory = epd_compute_malloc(bytes);
    if (memory) memset(memory, 0, bytes);
    return memory;
}

void epd_compute_free(void *memory)
{
    if (!memory) return;
    algorithm_allocation_t *header = (algorithm_allocation_t *)memory - 1;
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
