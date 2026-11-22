#ifndef UI_FONT_MEMORY_H
#define UI_FONT_MEMORY_H

#include "epd_memory_manager.h"
#include "epd_resource_runtime.h"
#include <string.h>

#if !defined(BUILD_DLMODULE) && !defined(RTT_IN_MODULE)
/* Resident font caches accept completed buffers from their callers. Allocation
 * admission stays with the producer; only ownership settlement uses the cache
 * account, with its existing limits, before restoring the producer's context. */
static inline int ui_font_memory_cache_adopt(epd_resource_cache_t *cache, void *allocation)
{
    if (!cache || !cache->provider || !allocation) return EPD_RESOURCE_INVALID;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(cache->owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_STOPPED;
    int result = epd_resource_cache_adopt(cache, allocation);
    epd_memory_owner_leave_trusted(previous);
    return result;
}
#endif

/* Font callbacks never wait for providers while holding their cache locks.
 * A deferred request retains this state until allocation succeeds or ends. */
static inline bool ui_font_memory_begin(epd_memory_recovery_t *state, size_t physical)
{
    epd_memory_backend_t backend;
    if (!physical || physical == SIZE_MAX) return false;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend)) return true;
    if (state->used && state->owner != epd_memory_owner_current())
        memset(state, 0, sizeof(*state));
    if (state->used) {
        if (epd_memory_recovery_poll(state, false) != EPD_MEMORY_RECOVERY_READY) return false;
        if (state->physical == physical) return true;
        memset(state, 0, sizeof(*state));
    }
    return epd_memory_recovery_begin(state, backend, physical, false) == EPD_MEMORY_RECOVERY_READY;
}

static inline void ui_font_memory_result(epd_memory_recovery_t *state, bool allocated)
{
    if (allocated) memset(state, 0, sizeof(*state));
    else if (state->used) epd_memory_recovery_failed(state);
}

#endif
