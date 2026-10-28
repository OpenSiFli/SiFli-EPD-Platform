/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include "epd_compute_protocol.h"
#include "epd_compute_cache.h"

typedef struct remote_session
{
    struct remote_session *next;
    uint32_t id, capability;
    void *state;
} remote_session_t;

static remote_session_t *sessions;
static uint32_t next_id;

static remote_session_t **lookup(uint32_t id)
{
    remote_session_t **p = &sessions;
    while (*p && (*p)->id != id) p = &(*p)->next;
    return p;
}

static int32_t execute(epd_compute_wire_t *request)
{
    if (request->capability >= epd_compute_remote_capability_count) return EPD_COMPUTE_UNSUPPORTED;
    const epd_compute_remote_registration_t *registration = &epd_compute_remote_capabilities[request->capability];
    const epd_compute_ops_t *ops = registration->ops;
    if (!ops || !ops->open || !ops->step || !ops->close) return EPD_COMPUTE_UNSUPPORTED;
    if (!request->step_limit || request->step_limit > registration->capability.step_limit)
        return EPD_COMPUTE_INVALID;
    if (request->operation == EPD_COMPUTE_OPEN)
    {
        if (request->session) return EPD_COMPUTE_INVALID;
        remote_session_t *s = rt_calloc(1, sizeof(*s));
        if (!s) return EPD_COMPUTE_NO_MEMORY;
        int32_t status = ops->open(&s->state, request);
        if (status != EPD_COMPUTE_OK)
        {
            if (s->state) ops->close(s->state, request);
            rt_free(s);
            return status;
        }
        do { ++next_id; } while (!next_id || *lookup(next_id));
        s->id = next_id;
        s->capability = request->capability;
        s->next = sessions;
        sessions = s;
        request->session = s->id;
        return EPD_COMPUTE_OK;
    }
    remote_session_t **p = lookup(request->session);
    remote_session_t *s = *p;
    if (!s || s->capability != request->capability) return EPD_COMPUTE_INVALID;
    switch (request->operation)
    {
    case EPD_COMPUTE_STEP: return ops->step(s->state, request);
    case EPD_COMPUTE_RESET: return ops->reset ? ops->reset(s->state, request) : EPD_COMPUTE_UNSUPPORTED;
    case EPD_COMPUTE_CLOSE:
        ops->close(s->state, request);
        *p = s->next;
        rt_free(s);
        request->session = 0;
        return EPD_COMPUTE_OK;
    default: return EPD_COMPUTE_UNSUPPORTED;
    }
}

void epd_compute_remote_dispatch(epd_compute_wire_t *request)
{
    if (!request) return;
    epd_compute_barrier();
    request->status = EPD_COMPUTE_INVALID;
    if (request->protocol == EPD_COMPUTE_PROTOCOL && request->size == sizeof(*request) &&
        epd_compute_validate_regions((const void *)(uintptr_t)request->regions, request->region_count) == EPD_COMPUTE_OK)
    {
        const epd_compute_region_t *regions = (const void *)(uintptr_t)request->regions;
        for (uint32_t i = 0; i < request->region_count; ++i)
            epd_compute_cache_invalidate(regions[i].address, regions[i].capacity);
        request->status = execute(request);
        for (uint32_t i = 0; i < request->region_count; ++i)
            if (regions[i].access & EPD_COMPUTE_WRITE)
                epd_compute_cache_clean(regions[i].address, regions[i].capacity);
    }
    request->completed_sequence = request->sequence;
    epd_compute_barrier();
}
