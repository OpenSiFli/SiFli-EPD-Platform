/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <stdbool.h>
#include "epd_compute_protocol.h"
#include "epd_compute_cache.h"
#include "epd_compute_memory.h"

typedef struct remote_session
{
    struct remote_session *next;
    uint32_t id, capability;
    void *state;
    uint32_t calls[4], ticks[4], step_max, errors, saturated;
    bool failure_reported;
} remote_session_t;

static remote_session_t *sessions;
static uint32_t next_id;
static uint32_t rejected_requests;

static void accumulate(remote_session_t *s, uint32_t *value, uint32_t amount)
{
    if (amount > UINT32_MAX - *value) { *value = UINT32_MAX; s->saturated = 1; }
    else *value += amount;
}

static void record_call(remote_session_t *s, uint32_t operation, rt_tick_t start)
{
    uint32_t elapsed = (uint32_t)(rt_tick_get() - start);
    accumulate(s, &s->calls[operation - 1], 1);
    accumulate(s, &s->ticks[operation - 1], elapsed);
    if (operation == EPD_COMPUTE_STEP && elapsed > s->step_max) s->step_max = elapsed;
}

static void report(const remote_session_t *s, epd_compute_wire_t *request,
                   int32_t status, bool closed)
{
    epd_compute_memory_stats_t m;
    epd_compute_memory_stats_get(&m);
    epd_compute_diag_wire_t *d = &request->diagnostics;
    d->status = status;
    d->session = s ? s->id : request->session;
    d->tick_hz = RT_TICK_PER_SECOND;
    d->errors = s ? s->errors : 1;
    d->rejects = rejected_requests;
    for (unsigned i = 0; i < 4; ++i)
    {
        d->calls[i] = s ? s->calls[i] : 0;
        d->ticks[i] = s ? s->ticks[i] : 0;
    }
    d->step_max = s ? s->step_max : 0;
    d->payload_live = (uint32_t)m.payload_live;
    d->payload_peak = (uint32_t)m.payload_peak;
    d->charged_live = (uint32_t)m.charged_live;
    d->charged_peak = (uint32_t)m.charged_peak;
    d->attempts = m.attempts;
    d->heap_calls = m.heap_calls;
    d->requested_bytes = m.requested_bytes;
    d->failures = m.failures;
    d->frees = m.frees;
    d->live_blocks = m.live_blocks;
    d->peak_blocks = m.peak_blocks;
    d->heap_size = (uint32_t)m.heap_size;
    d->heap_used = (uint32_t)m.heap_used;
    d->heap_peak = (uint32_t)m.heap_peak;
    d->system_size = m.system_size;
    d->system_used = m.system_used;
    d->system_peak = m.system_peak;
    uint32_t flags = EPD_COMPUTE_DIAG_VALID;
    if (closed) flags |= EPD_COMPUTE_DIAG_CLOSE;
    if (s && s->saturated) flags |= EPD_COMPUTE_DIAG_CALL_SATURATED;
    if (m.saturated) flags |= EPD_COMPUTE_DIAG_MEMORY_SATURATED;
    if (m.heap_present) flags |= EPD_COMPUTE_DIAG_PSRAM_PRESENT;
    if (m.system_present) flags |= EPD_COMPUTE_DIAG_SYSTEM_PRESENT;
    epd_compute_barrier();
    d->flags = flags | (d->flags & EPD_COMPUTE_PHASE_MASK);
}

static int32_t reject(epd_compute_wire_t *request, int32_t status)
{
    /* Bad versions/short descriptors cannot carry the response extension. */
    static bool reported;
    if (rejected_requests != UINT32_MAX) ++rejected_requests;
    if (request && !reported)
    {
        reported = true;
        report(NULL, request, status, false);
    }
    return status;
}

static remote_session_t **lookup(uint32_t id)
{
    remote_session_t **p = &sessions;
    while (*p && (*p)->id != id) p = &(*p)->next;
    return p;
}

static int32_t execute(epd_compute_wire_t *request)
{
    if (request->capability >= epd_compute_remote_capability_count) return reject(request, EPD_COMPUTE_UNSUPPORTED);
    const epd_compute_remote_registration_t *registration = &epd_compute_remote_capabilities[request->capability];
    const epd_compute_ops_t *ops = registration->ops;
    if (!ops || !ops->open || !ops->step || !ops->close) return reject(request, EPD_COMPUTE_UNSUPPORTED);
    if (!request->step_limit || request->step_limit > registration->capability.step_limit)
        return reject(request, EPD_COMPUTE_INVALID);
    if (request->operation == EPD_COMPUTE_OPEN)
    {
        if (request->session) return reject(request, EPD_COMPUTE_INVALID);
        remote_session_t *s = rt_calloc(1, sizeof(*s));
        if (!s) return reject(request, EPD_COMPUTE_NO_MEMORY);
        s->capability = request->capability;
        rt_tick_t start = rt_tick_get();
        int32_t status = ops->open(&s->state, request);
        record_call(s, EPD_COMPUTE_OPEN, start);
        if (status != EPD_COMPUTE_OK)
        {
            if (s->state)
            {
                start = rt_tick_get();
                ops->close(s->state, request);
                record_call(s, EPD_COMPUTE_CLOSE, start);
            }
            s->errors = 1;
            report(s, request, status, false);
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
    if (!s || s->capability != request->capability) return reject(request, EPD_COMPUTE_INVALID);
    rt_tick_t start = rt_tick_get();
    int32_t status;
    switch (request->operation)
    {
    case EPD_COMPUTE_STEP: status = ops->step(s->state, request); break;
    case EPD_COMPUTE_RESET:
        if (!ops->reset) status = EPD_COMPUTE_UNSUPPORTED;
        else status = ops->reset(s->state, request);
        break;
    case EPD_COMPUTE_CLOSE:
        ops->close(s->state, request);
        record_call(s, EPD_COMPUTE_CLOSE, start);
        report(s, request, EPD_COMPUTE_OK, true);
        *p = s->next;
        rt_free(s);
        request->session = 0;
        return EPD_COMPUTE_OK;
    default: return reject(request, EPD_COMPUTE_UNSUPPORTED);
    }
    if (request->operation != EPD_COMPUTE_RESET || ops->reset)
        record_call(s, request->operation, start);
    if (status < 0)
    {
        accumulate(s, &s->errors, 1);
        if (!s->failure_reported) { s->failure_reported = true; report(s, request, status, false); }
    }
    return status;
}

void epd_compute_remote_dispatch(epd_compute_wire_t *request)
{
    if (!request) return;
    epd_compute_barrier();
    request->status = EPD_COMPUTE_INVALID;
    if (request->protocol == EPD_COMPUTE_PROTOCOL && request->size == sizeof(*request))
    {
        request->diagnostics.flags = 0;
        epd_compute_phase(request, EPD_COMPUTE_PHASE_DISPATCH);
        const epd_compute_region_t *regions = (const void *)(uintptr_t)request->regions;
        if (epd_compute_validate_regions(regions, request->region_count) == EPD_COMPUTE_OK)
        {
            epd_compute_phase(request, EPD_COMPUTE_PHASE_INPUT);
            for (uint32_t i = 0; i < request->region_count; ++i)
                epd_compute_cache_invalidate(regions[i].address, regions[i].capacity);
            epd_compute_phase(request, EPD_COMPUTE_PHASE_EXECUTE);
            request->status = execute(request);
            epd_compute_phase(request, EPD_COMPUTE_PHASE_OUTPUT);
            for (uint32_t i = 0; i < request->region_count; ++i)
                if (regions[i].access & EPD_COMPUTE_WRITE)
                    epd_compute_cache_clean(regions[i].address, regions[i].capacity);
        }
        else request->status = reject(request, EPD_COMPUTE_INVALID);
    }
    else request->status = reject(NULL, EPD_COMPUTE_INVALID);
    request->completed_sequence = request->sequence;
    if (request->protocol == EPD_COMPUTE_PROTOCOL && request->size == sizeof(*request))
        epd_compute_phase(request, EPD_COMPUTE_PHASE_DONE);
    epd_compute_barrier();
}
