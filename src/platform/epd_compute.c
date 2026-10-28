/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute.h"
#include "epd_memory.h"
#include <rtthread.h>
#include <rtm.h>
#include <rthw.h>
#include <string.h>
#include <limits.h>

#ifdef EPD_COMPUTE_ACPU
#include <board.h>
#include "acpu_ctrl.h"
#include "epd_compute_cache.h"
#include "epd_compute_transport.h"

enum { SESSION_IDLE, SESSION_QUEUED, SESSION_RUNNING, SESSION_DONE };
struct epd_compute_session
{
    struct epd_compute_session *active_next, *queue_next;
    struct rt_semaphore done;
    epd_compute_wire_t wire;
    volatile uint32_t signals;
    uint32_t purpose, max_regions;
    unsigned state;
    epd_compute_region_t regions[];
};

static struct rt_thread dispatcher;
static struct rt_semaphore pending;
static epd_compute_wire_t transfer;
ALIGN(RT_ALIGN_SIZE)
static uint8_t dispatcher_stack[2048];
static epd_compute_session_t active, queue_head, queue_tail, running;
static bool ready;

static bool admissible(const epd_compute_registration_t *candidate, uint32_t purpose)
{
    for (epd_compute_session_t s = active; s; s = s->active_next)
    {
        const epd_compute_registration_t *other = &epd_compute_capabilities[s->wire.capability];
        if (purpose == EPD_COMPUTE_CONTINUOUS || s->purpose == EPD_COMPUTE_CONTINUOUS ||
            !(candidate->flags & other->flags & EPD_COMPUTE_STEP_SHARED))
            return false;
    }
    return true;
}

static unsigned priority(epd_compute_session_t session)
{
    return session->wire.operation == EPD_COMPUTE_CLOSE ? 0 : session->purpose + 1;
}

static epd_compute_session_t dequeue(void)
{
    epd_compute_session_t best = queue_head, previous = NULL, best_previous = NULL;
    for (epd_compute_session_t s = queue_head; s; s = s->queue_next)
    {
        if (priority(s) < priority(best)) { best = s; best_previous = previous; }
        previous = s;
    }
    if (!best) return NULL;
    if (best_previous) best_previous->queue_next = best->queue_next;
    else queue_head = best->queue_next;
    if (queue_tail == best) queue_tail = best_previous;
    best->queue_next = NULL;
    return best;
}

static void dispatch_entry(void *unused)
{
    (void)unused;
    for (;;)
    {
        rt_sem_take(&pending, RT_WAITING_FOREVER);
        rt_base_t level = rt_hw_interrupt_disable();
        epd_compute_session_t s = dequeue();
        if (!s) { rt_hw_interrupt_enable(level); continue; }
        s->state = SESSION_RUNNING;
        running = s;
        bool cancel = (s->signals & EPD_COMPUTE_CANCEL) && s->wire.operation != EPD_COMPUTE_CLOSE;
        rt_hw_interrupt_enable(level);

        if (cancel)
            s->wire.status = EPD_COMPUTE_CANCELLED;
        else
        {
            transfer = s->wire;
            for (uint32_t i = 0; i < transfer.region_count; ++i)
            {
                const epd_compute_region_t *r = &s->regions[i];
                epd_compute_cache_clean(r->address, r->capacity);
                /* HCPU relinquishes every line before remote execution. */
                epd_compute_cache_invalidate(r->address, r->capacity);
            }
            epd_compute_barrier();
            uint8_t error;
            int submitted = epd_compute_transport_run(&transfer, sizeof(transfer), &error);
            epd_compute_barrier();
            if (submitted == RT_EOK)
            {
                RT_ASSERT(error == ACPU_ERR_OK);
                RT_ASSERT(transfer.completed_sequence == s->wire.sequence);
            }
            else
                transfer.status = EPD_COMPUTE_UNAVAILABLE;
            for (uint32_t i = 0; i < transfer.region_count; ++i)
            {
                const epd_compute_region_t *r = &s->regions[i];
                if (r->access & EPD_COMPUTE_WRITE)
                    epd_compute_cache_invalidate(r->address, r->capacity);
            }
            s->wire.session = transfer.session;
            s->wire.status = transfer.status;
            s->wire.completed_sequence = transfer.completed_sequence;
        }
        level = rt_hw_interrupt_disable();
        running = NULL;
        s->state = SESSION_DONE;
        rt_hw_interrupt_enable(level);
        rt_sem_release(&s->done);
    }
}

static int compute_init(void)
{
    if (rt_sem_init(&pending, "cmpq", 0, RT_IPC_FLAG_FIFO) != RT_EOK) return -RT_ENOMEM;
    if (rt_thread_init(&dispatcher, "compute_dispatch", dispatch_entry, NULL,
                       dispatcher_stack, sizeof(dispatcher_stack),
                       RT_THREAD_PRIORITY_HIGH + 1, RT_THREAD_TICK_DEFAULT) != RT_EOK)
    {
        rt_sem_detach(&pending);
        return -RT_ENOMEM;
    }
    if (epd_compute_transport_init(&dispatcher) != RT_EOK)
    {
        rt_thread_detach(&dispatcher);
        rt_sem_detach(&pending);
        return -RT_ENOMEM;
    }
    if (rt_thread_startup(&dispatcher) != RT_EOK)
    {
        rt_thread_detach(&dispatcher);
        epd_compute_transport_detach();
        rt_sem_detach(&pending);
        return -RT_ERROR;
    }
    ready = true;
    return RT_EOK;
}
INIT_APP_EXPORT(compute_init);

static int submit(epd_compute_session_t s, uint32_t operation, const epd_compute_request_t *request)
{
    if (!s || !request || request->region_count > s->max_regions ||
        epd_compute_validate_regions(request->regions, request->region_count) != EPD_COMPUTE_OK)
        return EPD_COMPUTE_INVALID;
    rt_base_t level = rt_hw_interrupt_disable();
    if (s->state != SESSION_IDLE) { rt_hw_interrupt_enable(level); return EPD_COMPUTE_BUSY; }
    s->signals &= EPD_COMPUTE_CANCEL;
    s->wire.operation = operation;
    s->wire.region_count = request->region_count;
    if (request->region_count)
        memcpy(s->regions, request->regions, request->region_count * sizeof(*s->regions));
    s->wire.params = request->params;
    s->wire.result = request->result;
    ++s->wire.sequence;
    s->wire.completed_sequence = ~s->wire.sequence;
    s->wire.status = EPD_COMPUTE_INVALID;
    s->state = SESSION_QUEUED;
    if (queue_tail) queue_tail->queue_next = s;
    else queue_head = s;
    queue_tail = s;
    if (running && priority(s) < priority(running)) running->signals |= EPD_COMPUTE_YIELD;
    epd_compute_barrier();
    rt_hw_interrupt_enable(level);
    rt_sem_release(&pending);
    while (rt_sem_take(&s->done, rt_tick_from_millisecond(20)) != RT_EOK)
        if (operation != EPD_COMPUTE_CLOSE && request->cancelled && request->cancelled(request->context))
            epd_compute_cancel(s);
    level = rt_hw_interrupt_disable();
    int result = s->wire.status;
    s->state = SESSION_IDLE;
    rt_hw_interrupt_enable(level);
    return result;
}

static void release_session(epd_compute_session_t s)
{
    rt_base_t level = rt_hw_interrupt_disable();
    epd_compute_session_t *p = &active;
    while (*p && *p != s) p = &(*p)->active_next;
    if (*p) *p = s->active_next;
    rt_hw_interrupt_enable(level);
    rt_sem_detach(&s->done);
    epd_app_free(s);
}
#endif

int epd_compute_query(const char *name, uint32_t version, epd_compute_capability_t *capability)
{
    if (!name || !capability) return EPD_COMPUTE_INVALID;
    memset(capability, 0, sizeof(*capability));
    for (uint32_t i = 0; i < epd_compute_capability_count; ++i)
        if (epd_compute_capabilities[i].version == version && !strcmp(epd_compute_capabilities[i].name, name))
        {
            capability->index = i;
            capability->registration = epd_compute_capabilities[i];
#ifdef EPD_COMPUTE_ACPU
            capability->ready = ready && (capability->registration.flags &
                (EPD_COMPUTE_VALIDATED | EPD_COMPUTE_ALLOW_UNVALIDATED));
#endif
            return EPD_COMPUTE_OK;
        }
    return EPD_COMPUTE_UNSUPPORTED;
}

int epd_compute_open(const epd_compute_capability_t *capability,
                     const epd_compute_requirements_t *requirements,
                     const epd_compute_request_t *request, epd_compute_session_t *session)
{
    if (!session) return EPD_COMPUTE_INVALID;
    *session = NULL;
#ifdef EPD_COMPUTE_ACPU
    if (!capability || !requirements || !request || capability->index >= epd_compute_capability_count ||
        (unsigned)requirements->purpose >= EPD_COMPUTE_PURPOSE_COUNT ||
        requirements->max_regions > (SIZE_MAX - sizeof(struct epd_compute_session)) / sizeof(epd_compute_region_t))
        return EPD_COMPUTE_INVALID;
    const epd_compute_registration_t *c = &epd_compute_capabilities[capability->index];
    if (!ready || !(c->flags & (EPD_COMPUTE_VALIDATED | EPD_COMPUTE_ALLOW_UNVALIDATED)))
        return EPD_COMPUTE_UNAVAILABLE;
    if (!(c->purposes & EPD_COMPUTE_PURPOSE(requirements->purpose))) return EPD_COMPUTE_UNSUPPORTED;
    epd_compute_session_t s = epd_app_alloc(sizeof(*s) + requirements->max_regions * sizeof(*s->regions), EPD_APP_SRAM);
    if (!s) return EPD_COMPUTE_NO_MEMORY;
    /* rt_malloc may fall back to another registered heap. Shared controls
     * must still be inside this image's uncached SRAM allocation. */
    uintptr_t address = (uintptr_t)s;
    size_t allocation_size = sizeof(*s) + requirements->max_regions * sizeof(*s->regions);
    if (address < HCPU_RAM_DATA_START_ADDR || address >= HCPU_RAM_DATA_START_ADDR + HCPU_RAM_DATA_SIZE ||
        allocation_size > HCPU_RAM_DATA_START_ADDR + HCPU_RAM_DATA_SIZE - address)
    { epd_app_free(s); return EPD_COMPUTE_NO_MEMORY; }
    memset(s, 0, sizeof(*s));
    if (rt_sem_init(&s->done, "cmps", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    { epd_app_free(s); return EPD_COMPUTE_NO_MEMORY; }
    s->purpose = requirements->purpose;
    s->max_regions = requirements->max_regions;
    s->wire.protocol = EPD_COMPUTE_PROTOCOL;
    s->wire.size = sizeof(s->wire);
    s->wire.capability = capability->index;
    s->wire.regions = (uint32_t)(uintptr_t)s->regions;
    s->wire.signals = (uint32_t)(uintptr_t)&s->signals;
    s->wire.step_limit = c->step_limit;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!admissible(c, s->purpose))
    {
        rt_hw_interrupt_enable(level);
        rt_sem_detach(&s->done);
        epd_app_free(s);
        return EPD_COMPUTE_BUSY;
    }
    s->active_next = active;
    active = s;
    rt_hw_interrupt_enable(level);
    int status = submit(s, EPD_COMPUTE_OPEN, request);
    if (status != EPD_COMPUTE_OK || (s->signals & EPD_COMPUTE_CANCEL))
    {
        if (s->wire.session)
        {
            epd_compute_request_t close_request = {0};
            int closed = submit(s, EPD_COMPUTE_CLOSE, &close_request);
            RT_ASSERT(closed == EPD_COMPUTE_OK);
        }
        if (status == EPD_COMPUTE_OK) status = EPD_COMPUTE_CANCELLED;
        release_session(s);
        return status;
    }
    *session = s;
    return EPD_COMPUTE_OK;
#else
    (void)capability; (void)requirements; (void)request;
    return EPD_COMPUTE_UNAVAILABLE;
#endif
}

int epd_compute_step(epd_compute_session_t session, const epd_compute_request_t *request)
{
#ifdef EPD_COMPUTE_ACPU
    return submit(session, EPD_COMPUTE_STEP, request);
#else
    (void)session; (void)request; return EPD_COMPUTE_UNAVAILABLE;
#endif
}

int epd_compute_reset(epd_compute_session_t session, const epd_compute_request_t *request)
{
#ifdef EPD_COMPUTE_ACPU
    return submit(session, EPD_COMPUTE_RESET, request);
#else
    (void)session; (void)request; return EPD_COMPUTE_UNAVAILABLE;
#endif
}

void epd_compute_cancel(epd_compute_session_t session)
{
#ifdef EPD_COMPUTE_ACPU
    if (session)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        session->signals |= EPD_COMPUTE_CANCEL;
        epd_compute_barrier();
        rt_hw_interrupt_enable(level);
    }
#else
    (void)session;
#endif
}

int epd_compute_close(epd_compute_session_t session)
{
#ifdef EPD_COMPUTE_ACPU
    epd_compute_request_t request = {0};
    int status = submit(session, EPD_COMPUTE_CLOSE, &request);
    if (status == EPD_COMPUTE_OK) release_session(session);
    return status;
#else
    (void)session; return EPD_COMPUTE_UNAVAILABLE;
#endif
}

bool epd_compute_buffer_alloc(epd_compute_buffer_t *buffer, size_t size)
{
    if (!buffer) return false;
    memset(buffer, 0, sizeof(*buffer));
    if (!size || size > UINT32_MAX - 2 * EPD_COMPUTE_CACHE_LINE) return false;
    uint32_t capacity = (size + EPD_COMPUTE_CACHE_LINE - 1) & ~(EPD_COMPUTE_CACHE_LINE - 1);
    void *allocation = epd_app_alloc(capacity + EPD_COMPUTE_CACHE_LINE - 1, EPD_APP_PSRAM);
    if (!allocation) return false;
    buffer->allocation = allocation;
    buffer->data = (void *)(((uintptr_t)allocation + EPD_COMPUTE_CACHE_LINE - 1) & ~(uintptr_t)(EPD_COMPUTE_CACHE_LINE - 1));
    buffer->capacity = capacity;
    return true;
}

void epd_compute_buffer_free(epd_compute_buffer_t *buffer)
{
    if (!buffer) return;
    epd_app_free(buffer->allocation);
    memset(buffer, 0, sizeof(*buffer));
}

epd_compute_region_t epd_compute_buffer_region(const epd_compute_buffer_t *buffer, uint32_t length, uint32_t access)
{
    epd_compute_region_t region = {0};
    if (buffer)
    {
        region.address = (uint32_t)(uintptr_t)buffer->data;
        region.capacity = buffer->capacity;
        region.length = length;
        region.access = access;
    }
    return region;
}

RTM_EXPORT(epd_compute_query);
RTM_EXPORT(epd_compute_open);
RTM_EXPORT(epd_compute_step);
RTM_EXPORT(epd_compute_reset);
RTM_EXPORT(epd_compute_cancel);
RTM_EXPORT(epd_compute_close);
RTM_EXPORT(epd_compute_buffer_alloc);
RTM_EXPORT(epd_compute_buffer_free);
RTM_EXPORT(epd_compute_buffer_region);
