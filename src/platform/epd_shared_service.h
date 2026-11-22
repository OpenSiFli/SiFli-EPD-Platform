/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_SHARED_SERVICE_H
#define EPD_SHARED_SERVICE_H

#include <rtthread.h>
#include "epd_memory_manager.h"

typedef struct {
    bool active;
#ifdef RT_USING_THREAD_MEM_CONTEXT
    epd_memory_owner_t previous;
#endif
#ifdef RT_USING_MODULE_OBJECT_CONTEXT
    struct rt_object_global_scope objects;
#endif
} epd_shared_service_guard_t;

/* Only for platform-resident services whose code and callbacks outlive apps.
 * Zero-initialize the guard; cover IPC, stack and TCB creation. Memory billing
 * and RT object lifetime are switched independently. Leave on the same thread
 * in reverse entry order; do not wrap application callbacks in this guard. */
static inline bool epd_shared_service_enter(epd_shared_service_guard_t *guard)
{
    if (!guard || guard->active) return false;
#ifdef RT_USING_THREAD_MEM_CONTEXT
    guard->previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    if (guard->previous == EPD_MEMORY_OWNER_INVALID) return false;
#endif
#ifdef RT_USING_MODULE_OBJECT_CONTEXT
    if (rt_object_global_scope_enter(&guard->objects) != RT_EOK) {
#ifdef RT_USING_THREAD_MEM_CONTEXT
        epd_memory_owner_leave_trusted(guard->previous);
#endif
        return false;
    }
#endif
    guard->active = true;
    return true;
}

static inline bool epd_shared_service_leave(epd_shared_service_guard_t *guard)
{
    if (!guard || !guard->active) return false;
#ifdef RT_USING_MODULE_OBJECT_CONTEXT
    if (rt_object_global_scope_leave(&guard->objects) != RT_EOK) return false;
#endif
#ifdef RT_USING_THREAD_MEM_CONTEXT
    epd_memory_owner_leave_trusted(guard->previous);
#endif
    guard->active = false;
    return true;
}

#endif
