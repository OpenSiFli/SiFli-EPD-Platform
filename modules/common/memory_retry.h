#ifndef EPD_MEMORY_RETRY_H
#define EPD_MEMORY_RETRY_H

#include "app_memory_pressure.h"
#include "epd_memory.h"
#include <rtthread.h>

typedef struct {
    uint32_t generation, started;
    bool used, pending;
} epd_memory_retry_t;

typedef enum {
    EPD_MEMORY_RETRY_DONE,
    EPD_MEMORY_RETRY_PENDING,
    EPD_MEMORY_RETRY_READY
} epd_memory_retry_result_t;

/* One recovery round per operation; available bytes are not a reservation. */
static inline bool epd_memory_retry_begin(epd_memory_retry_t *retry, size_t bytes)
{
    if (retry->used) return false;
    retry->used = true;
    size_t overhead = 2u * RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    size_t capacity = epd_app_psram_capacity();
    if (!bytes || capacity < overhead || bytes > capacity - overhead) return false;
    retry->started = (uint32_t)rt_tick_get();
    retry->generation = app_memory_pressure_allocation_failed(bytes + overhead);
    retry->pending = true;
    return true;
}

static inline epd_memory_retry_result_t epd_memory_retry_poll(epd_memory_retry_t *retry,
                                                            bool cancelled)
{
    if (!retry->pending) return EPD_MEMORY_RETRY_DONE;
    if (cancelled) { retry->pending = false; return EPD_MEMORY_RETRY_DONE; }
    app_memory_pressure_status_t status = app_memory_pressure_query();
    if (retry->generation && status.generation == retry->generation && status.pending &&
        (uint32_t)((uint32_t)rt_tick_get() - retry->started) <
        (uint32_t)(((uint64_t)APP_MEMORY_PRESSURE_TIMEOUT_MS * RT_TICK_PER_SECOND + 999u) / 1000u))
        return EPD_MEMORY_RETRY_PENDING;
    retry->pending = false;
    return EPD_MEMORY_RETRY_READY;
}

/* Dedicated non-realtime workers only, with no storage/application locks held.
 * Cancellation also rejects a replaced request. No callback frees live memory. */
static inline bool epd_memory_retry_wait(epd_memory_retry_t *retry, size_t bytes,
                                        bool (*cancelled)(void *), void *context)
{
    if (!cancelled || cancelled(context) || !epd_memory_retry_begin(retry, bytes)) return false;
    for (;;) {
        epd_memory_retry_result_t result = epd_memory_retry_poll(retry, cancelled(context));
        if (result != EPD_MEMORY_RETRY_PENDING) return result == EPD_MEMORY_RETRY_READY;
        rt_thread_mdelay(10);
    }
}

#endif
