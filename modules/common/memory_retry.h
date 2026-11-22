/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_MEMORY_RETRY_H
#define EPD_MEMORY_RETRY_H

#include "epd_memory.h"
#include "epd_memory_manager.h"
#include <rtthread.h>

typedef epd_memory_recovery_t epd_memory_retry_t;

typedef enum {
    EPD_MEMORY_RETRY_DONE,
    EPD_MEMORY_RETRY_PENDING,
    EPD_MEMORY_RETRY_READY
} epd_memory_retry_result_t;

/* One recovery round for failed fresh application-PSRAM allocations. The byte
 * demand does not guarantee a contiguous block or a group of allocations. */
static inline bool epd_memory_retry_begin(epd_memory_retry_t *retry, size_t bytes)
{
    epd_memory_backend_t backend;
    if (!retry || retry->used) return false;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend)) {
        retry->used = true;
        return false;
    }
    return epd_memory_recovery_begin(retry, backend, epd_app_allocation_peak(bytes), true) !=
           EPD_MEMORY_RECOVERY_DENIED;
}

static inline epd_memory_retry_result_t epd_memory_retry_poll(epd_memory_retry_t *retry,
                                                            bool cancelled)
{
    epd_memory_recovery_result_t result = epd_memory_recovery_poll(retry, cancelled);
    return result == EPD_MEMORY_RECOVERY_READY ? EPD_MEMORY_RETRY_READY :
           result == EPD_MEMORY_RECOVERY_PENDING ? EPD_MEMORY_RETRY_PENDING : EPD_MEMORY_RETRY_DONE;
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
