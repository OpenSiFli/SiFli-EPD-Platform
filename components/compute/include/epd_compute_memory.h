/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_MEMORY_H
#define EPD_COMPUTE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Lifetime totals for this core's algorithm allocator. Cumulative uint32_t
 * fields saturate (saturated=1); snapshots never reset shared accounting. */
typedef struct
{
    size_t payload_live, payload_peak;
    size_t charged_live, charged_peak;
    uint32_t attempts, heap_calls, requested_bytes, failures, frees;
    uint32_t live_blocks, peak_blocks, saturated;
    /* Native dedicated rt_memheap values, including its internal overhead.
     * heap_present=0 means malloc fallback; native values are unavailable. */
    uint32_t heap_present;
    size_t heap_size, heap_used, heap_peak;
    /* Default RT heap (SRAM on ACPU), separate from the algorithm PSRAM heap. */
    uint32_t system_present, system_size, system_used, system_peak;
} epd_compute_memory_stats_t;

/* charged bytes include this wrapper's header/alignment slack, but not
 * rt_memheap metadata/rounding. requested_bytes counts actual heap calls,
 * including failed calls. Guard rejections count in attempts/failures only. */
void epd_compute_memory_stats_get(epd_compute_memory_stats_t *stats);

/* ACPU-owned algorithm workspace. Never free these allocations on HCPU. */
void *epd_compute_malloc(size_t size);
void *epd_compute_calloc(size_t count, size_t size);
void *epd_compute_realloc(void *memory, size_t size);
void epd_compute_free(void *memory);

#endif
