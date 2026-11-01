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
#include "epd_compute_pm.h"
#include "epd_compute_context.h"
#include "sleep_guard.h"
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif

#ifndef EPD_COMPUTE_TRACE
#define EPD_COMPUTE_TRACE 0
#endif

enum { SESSION_IDLE, SESSION_QUEUED, SESSION_RUNNING, SESSION_DONE };
typedef struct
{
    uint32_t remote_id, admission_ticks, queued_at;
    uint32_t queue_calls, queue_ticks, queue_max;
    uint32_t rtt_calls, rtt_ticks, rtt_max, errors, saturated;
    uint32_t cache_pre_ticks, cache_post_ticks;
    bool failure_reported;
} compute_diagnostics_t;
struct epd_compute_session
{
    struct epd_compute_session *active_next, *queue_next;
    struct rt_semaphore done;
    epd_compute_wire_t wire;
    volatile uint32_t signals;
    uint32_t purpose, max_regions;
    unsigned state, bypasses;
    epd_compute_buffer_t *owned_buffers;
    uint32_t owned_count;
    bool reclaiming;
    compute_diagnostics_t diagnostics;
    epd_compute_region_t regions[];
};

struct epd_compute_wait
{
    struct epd_compute_wait *next;
    struct rt_semaphore changed;
    epd_compute_session_t session;
    bool cancelled, in_use, notified;
};

static struct rt_thread dispatcher;
static struct rt_semaphore pending;
static epd_compute_wire_t transfer;
ALIGN(RT_ALIGN_SIZE)
static uint8_t dispatcher_stack[2048];
static epd_compute_session_t active, queue_head, queue_tail, running;
static epd_compute_wait_t admission_head, admission_tail;
static bool ready, pending_notified;

static void diagnostic_add(compute_diagnostics_t *d, uint32_t *value, uint32_t amount)
{
    if (amount > UINT32_MAX - *value) { *value = UINT32_MAX; d->saturated = 1; }
    else *value += amount;
}

#ifdef RT_USING_FINSH
/* Completed dispatcher batches across session lifetimes, in HCPU ticks. */
static compute_diagnostics_t profile_total;
static int compute_profile(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_base_t level = rt_hw_interrupt_disable();
    compute_diagnostics_t d = profile_total;
    rt_hw_interrupt_enable(level);
    rt_kprintf("cmp H total n=%u queue=%u pre=%u rtt=%u post=%u hz=%u sat=%u\n",
        d.rtt_calls, d.queue_ticks, d.cache_pre_ticks, d.rtt_ticks,
        d.cache_post_ticks, RT_TICK_PER_SECOND, d.saturated);
    return RT_EOK;
}
MSH_CMD_EXPORT(compute_profile, Show cumulative HCPU compute timing);
#endif

/* Call with the interrupt lock held, for dequeue AND queued cancellation. */
static void diagnostic_queue_exit(epd_compute_session_t s)
{
    compute_diagnostics_t *d = &s->diagnostics;
    uint32_t elapsed = (uint32_t)(rt_tick_get() - d->queued_at);
    diagnostic_add(d, &d->queue_calls, 1);
    diagnostic_add(d, &d->queue_ticks, elapsed);
    if (elapsed > d->queue_max) d->queue_max = elapsed;
#ifdef RT_USING_FINSH
    diagnostic_add(&profile_total, &profile_total.queue_ticks, elapsed);
#endif
}

static void diagnostic_result(epd_compute_session_t s, uint32_t operation, int status, bool closing)
{
    if (!s) return;
    rt_base_t level = rt_hw_interrupt_disable();
    compute_diagnostics_t *d = &s->diagnostics;
    if (status < 0) diagnostic_add(d, &d->errors, 1);
    bool first_failure = status < 0 && status != EPD_COMPUTE_BUSY && !d->failure_reported;
    if (first_failure) d->failure_reported = true;
    compute_diagnostics_t snapshot = *d;
    uint32_t capability = s->wire.capability;
    rt_hw_interrupt_enable(level);
    if (!closing && !first_failure) return;
    if (!EPD_COMPUTE_TRACE && closing && !snapshot.errors)
    {
        rt_kprintf("cmp H id=%u cap=%u end=close status=%d admission=%u queue=%u rtt=%u max=%u hz=%u sat=%u pre=%u post=%u n=%u\n",
            snapshot.remote_id, capability, status, snapshot.admission_ticks,
            snapshot.queue_ticks, snapshot.rtt_ticks, snapshot.rtt_max,
            RT_TICK_PER_SECOND, snapshot.saturated, snapshot.cache_pre_ticks,
            snapshot.cache_post_ticks, snapshot.rtt_calls);
        return;
    }
    rt_kprintf("cmp H id=%u cap=%u end=%s op=%u status=%d errors=%u hz=%u sat=%u\n",
        snapshot.remote_id, capability, closing ? "close" : "failed", operation,
        status, snapshot.errors, RT_TICK_PER_SECOND, snapshot.saturated);
    rt_kprintf("cmp H id=%u admission_ticks=%u queue_n=%u ticks=%u max=%u\n",
        snapshot.remote_id, snapshot.admission_ticks, snapshot.queue_calls,
        snapshot.queue_ticks, snapshot.queue_max);
    rt_kprintf("cmp H id=%u transport_n=%u rtt_ticks=%u max=%u pre=%u post=%u\n",
        snapshot.remote_id, snapshot.rtt_calls, snapshot.rtt_ticks, snapshot.rtt_max,
        snapshot.cache_pre_ticks, snapshot.cache_post_ticks);
}

/* Called by the submitting owner while its session is still DONE. The
 * dispatcher may serve another session; this response remains private. */
static void diagnostic_remote_result(epd_compute_session_t s)
{
    const epd_compute_diag_wire_t *d = &s->wire.diagnostics;
    if (!(d->flags & EPD_COMPUTE_DIAG_VALID)) return;
    uint32_t flags = d->flags;
    if (!EPD_COMPUTE_TRACE && (flags & EPD_COMPUTE_DIAG_CLOSE) && !d->errors && d->status >= 0)
    {
        rt_kprintf("cmp A id=%u cap=%u end=close status=%d steps=%u ticks=%u step_max=%u charged_peak=%u hz=%u sat=%u\n",
            d->session, s->wire.capability, d->status, d->calls[1], d->ticks[1],
            d->step_max, d->charged_peak, d->tick_hz,
            !!(flags & (EPD_COMPUTE_DIAG_CALL_SATURATED | EPD_COMPUTE_DIAG_MEMORY_SATURATED)));
        s->wire.diagnostics.flags = 0;
        return;
    }
    rt_kprintf("cmp A id=%u cap=%u end=%s status=%d errors=%u hz=%u sat=%u\n",
        d->session, s->wire.capability, (flags & EPD_COMPUTE_DIAG_CLOSE) ? "close" : "failed",
        d->status, d->errors, d->tick_hz, !!(flags & EPD_COMPUTE_DIAG_CALL_SATURATED));
    for (unsigned i = 0; i < 4; ++i)
        if (d->calls[i])
            rt_kprintf("cmp A id=%u op=%u calls=%u ticks=%u step_max=%u\n",
                d->session, i + 1, d->calls[i], d->ticks[i], i == 1 ? d->step_max : 0);
    rt_kprintf("cmp A heap-global payload=%u peak=%u charged=%u peak=%u\n",
        d->payload_live, d->payload_peak, d->charged_live, d->charged_peak);
    rt_kprintf("cmp A heap-global attempts=%u calls=%u bytes=%u fail=%u sat=%u\n",
        d->attempts, d->heap_calls, d->requested_bytes, d->failures,
        !!(flags & EPD_COMPUTE_DIAG_MEMORY_SATURATED));
    rt_kprintf("cmp A heap-global blocks=%u peak=%u frees=%u rejects=%u\n",
        d->live_blocks, d->peak_blocks, d->frees, d->rejects);
    rt_kprintf("cmp A psram present=%u size=%u used=%u peak=%u\n",
        !!(flags & EPD_COMPUTE_DIAG_PSRAM_PRESENT), d->heap_size, d->heap_used, d->heap_peak);
    rt_kprintf("cmp A system present=%u size=%u used=%u peak=%u\n",
        !!(flags & EPD_COMPUTE_DIAG_SYSTEM_PRESENT), d->system_size, d->system_used, d->system_peak);
    s->wire.diagnostics.flags = 0;
}

static bool cleanup_blocks(uint32_t capability)
{
    for (epd_compute_session_t s = active; s; s = s->active_next)
        if (s->owned_buffers && s->wire.capability == capability) return true;
    return false;
}

static bool admissible(const epd_compute_registration_t *candidate)
{
    for (epd_compute_session_t s = active; s; s = s->active_next)
    {
        const epd_compute_registration_t *other = &epd_compute_capabilities[s->wire.capability];
        if (s->owned_buffers && candidate == other) return false;
        if (!(candidate->flags & other->flags & EPD_COMPUTE_STEP_SHARED))
            return false;
    }
    return true;
}

static unsigned priority(epd_compute_session_t session)
{
    return session->wire.operation == EPD_COMPUTE_CLOSE ||
           session->bypasses >= EPD_COMPUTE_MAX_BYPASSES ? 0 : session->purpose + 1;
}

static void notify_dispatcher(void)
{
    /* Coalesce work notifications, including requests cancelled before they
     * run, so a blocked transport cannot accumulate stale semaphore tokens. */
    if (!pending_notified)
    {
        pending_notified = true;
        rt_sem_release(&pending);
    }
}

/* All list, signal and completion transitions are protected by the HCPU
 * interrupt lock. Notification stays inside the lock so a woken owner cannot
 * free a session before the notifier finishes using it. */
static void unlink_queued(epd_compute_session_t s, epd_compute_session_t previous)
{
    diagnostic_queue_exit(s);
    if (previous) previous->queue_next = s->queue_next;
    else queue_head = s->queue_next;
    if (queue_tail == s) queue_tail = previous;
    s->queue_next = NULL;
}

static void update_yield(void)
{
    if (!running) return;
    running->signals &= ~EPD_COMPUTE_YIELD;
    for (epd_compute_session_t s = queue_head; s; s = s->queue_next)
        if (priority(s) < priority(running))
        {
            running->signals |= EPD_COMPUTE_YIELD;
            break;
        }
    epd_compute_barrier();
}

static void cancel_session(epd_compute_session_t s)
{
    s->signals |= EPD_COMPUTE_CANCEL;
    if (s->state == SESSION_QUEUED && s->wire.operation != EPD_COMPUTE_CLOSE)
    {
        epd_compute_session_t previous = NULL;
        for (epd_compute_session_t p = queue_head; p != s; p = p->queue_next) previous = p;
        unlink_queued(s, previous);
        s->wire.status = EPD_COMPUTE_CANCELLED;
        s->state = SESSION_DONE;
        rt_sem_release(&s->done);
        update_yield();
    }
    epd_compute_barrier();
}

static void notify_admission(void)
{
    for (epd_compute_wait_t w = admission_head; w; w = w->next)
        if (!w->notified)
        {
            w->notified = true;
            rt_sem_release(&w->changed);
        }
}

static void unlink_admission(epd_compute_wait_t w)
{
    epd_compute_wait_t previous = NULL, p = admission_head;
    while (p && p != w) { previous = p; p = p->next; }
    if (!p) return;
    if (previous) previous->next = w->next;
    else admission_head = w->next;
    if (admission_tail == w) admission_tail = previous;
    w->next = NULL;
    notify_admission();
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
    unlink_queued(best, best_previous);
    /* Keep an aged request's service class for its entire batch. Otherwise a
     * queued continuous request would immediately make it yield without work. */
    for (epd_compute_session_t s = queue_head; s; s = s->queue_next)
        if (s->bypasses < EPD_COMPUTE_MAX_BYPASSES) ++s->bypasses;
    return best;
}

static void dispatch_entry(void *unused)
{
    (void)unused;
    sleep_guard_owner_t power = { .id = "compute" };
    epd_compute_transport_start();
    for (;;)
    {
        rt_sem_take(&pending, RT_WAITING_FOREVER);
        rt_base_t level = rt_hw_interrupt_disable();
        pending_notified = false;
        /* Commit and dispatch share the sleep guard's interrupt boundary. */
        if (!queue_head || !sleep_guard_acquire_drain(&power))
        {
            rt_hw_interrupt_enable(level);
            continue;
        }
        epd_compute_session_t s = dequeue();
        if (queue_head) notify_dispatcher();
        s->state = SESSION_RUNNING;
        running = s;
        update_yield();
        bool cancel = (s->signals & EPD_COMPUTE_CANCEL) && s->wire.operation != EPD_COMPUTE_CLOSE;
        rt_hw_interrupt_enable(level);

        uint32_t rtt = 0, cache_pre = 0, cache_post = 0;
        if (cancel)
            s->wire.status = EPD_COMPUTE_CANCELLED;
        else
        {
            memcpy(&transfer, &s->wire, EPD_COMPUTE_WIRE_PREFIX_SIZE);
            transfer.diagnostics.flags = 0;
            rt_tick_t cache_start = rt_tick_get();
            for (uint32_t i = 0; i < transfer.region_count; ++i)
            {
                const epd_compute_region_t *r = &s->regions[i];
                /* HCPU relinquishes every line before remote execution. */
                epd_compute_cache_clean_invalidate(r->address, r->capacity);
            }
            epd_compute_barrier();
            cache_pre = (uint32_t)(rt_tick_get() - cache_start);
            uint8_t error;
            rt_tick_t rtt_start = rt_tick_get();
            int submitted = epd_compute_transport_run(&transfer, sizeof(transfer), &error);
            rtt = (uint32_t)(rt_tick_get() - rtt_start);
            epd_compute_barrier();
            if (submitted == RT_EOK)
            {
                RT_ASSERT(error == ACPU_ERR_OK);
                RT_ASSERT(transfer.completed_sequence == s->wire.sequence);
                if (transfer.diagnostics.flags & EPD_COMPUTE_DIAG_VALID)
                    s->wire.diagnostics = transfer.diagnostics;
            }
            else
                transfer.status = EPD_COMPUTE_UNAVAILABLE;
            cache_start = rt_tick_get();
            for (uint32_t i = 0; i < transfer.region_count; ++i)
            {
                const epd_compute_region_t *r = &s->regions[i];
                if (r->access & EPD_COMPUTE_WRITE)
                    epd_compute_cache_invalidate(r->address, r->capacity);
            }
            cache_post = (uint32_t)(rt_tick_get() - cache_start);
            s->wire.session = transfer.session;
            s->wire.status = transfer.status;
            s->wire.completed_sequence = transfer.completed_sequence;
        }
        level = rt_hw_interrupt_disable();
        if (!cancel)
        {
            compute_diagnostics_t *d = &s->diagnostics;
            diagnostic_add(d, &d->rtt_calls, 1);
            diagnostic_add(d, &d->rtt_ticks, rtt);
            diagnostic_add(d, &d->cache_pre_ticks, cache_pre);
            diagnostic_add(d, &d->cache_post_ticks, cache_post);
#ifdef RT_USING_FINSH
            diagnostic_add(&profile_total, &profile_total.rtt_calls, 1);
            diagnostic_add(&profile_total, &profile_total.rtt_ticks, rtt);
            diagnostic_add(&profile_total, &profile_total.cache_pre_ticks, cache_pre);
            diagnostic_add(&profile_total, &profile_total.cache_post_ticks, cache_post);
#endif
            if (rtt > d->rtt_max) d->rtt_max = rtt;
            if (s->wire.session) d->remote_id = s->wire.session;
        }
        running = NULL;
        s->state = SESSION_DONE;
        sleep_guard_release(&power);
        rt_sem_release(&s->done);
        rt_hw_interrupt_enable(level);
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
    if (!epd_compute_can_wait() || rt_thread_self() == &dispatcher)
        return EPD_COMPUTE_INVALID;
    if (!s || !request || request->region_count > s->max_regions ||
        epd_compute_validate_regions(request->regions, request->region_count) != EPD_COMPUTE_OK)
    {
        diagnostic_result(s, operation, EPD_COMPUTE_INVALID, false);
        return EPD_COMPUTE_INVALID;
    }
    bool cancelled = operation != EPD_COMPUTE_CLOSE && request->cancelled &&
                     request->cancelled(request->context);
    rt_base_t level = rt_hw_interrupt_disable();
    if (s->state != SESSION_IDLE)
    {
        rt_hw_interrupt_enable(level);
        diagnostic_result(s, operation, EPD_COMPUTE_BUSY, false);
        return EPD_COMPUTE_BUSY;
    }
    if (cancelled) s->signals |= EPD_COMPUTE_CANCEL;
    if (operation != EPD_COMPUTE_CLOSE && (s->signals & EPD_COMPUTE_CANCEL))
    {
        rt_hw_interrupt_enable(level);
        diagnostic_result(s, operation, EPD_COMPUTE_CANCELLED, false);
        return EPD_COMPUTE_CANCELLED;
    }
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
    s->wire.diagnostics.flags = 0;
    s->state = SESSION_QUEUED;
    s->diagnostics.queued_at = rt_tick_get();
    s->bypasses = 0;
    if (queue_tail) queue_tail->queue_next = s;
    else queue_head = s;
    queue_tail = s;
    update_yield();
    epd_compute_barrier();
    notify_dispatcher();
    rt_hw_interrupt_enable(level);
    rt_sem_take(&s->done, RT_WAITING_FOREVER);
    level = rt_hw_interrupt_disable();
    int result = s->wire.status;
    rt_hw_interrupt_enable(level);
    diagnostic_remote_result(s);
    diagnostic_result(s, operation, result, operation == EPD_COMPUTE_CLOSE && result == EPD_COMPUTE_OK);
    level = rt_hw_interrupt_disable();
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
    notify_admission();
    rt_hw_interrupt_enable(level);
    rt_sem_detach(&s->done);
    for (uint32_t i = 0; i < s->owned_count; ++i) epd_compute_buffer_free(&s->owned_buffers[i]);
    epd_app_free(s->owned_buffers);
    epd_app_free(s);
}

static int admit_session(epd_compute_session_t s, epd_compute_wait_t wait)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (wait)
    {
        if (wait->in_use) { rt_hw_interrupt_enable(level); return EPD_COMPUTE_BUSY; }
        if (wait->cancelled) { rt_hw_interrupt_enable(level); return EPD_COMPUTE_CANCELLED; }
        wait->in_use = true;
        wait->session = s;
        if (admission_tail) admission_tail->next = wait;
        else admission_head = wait;
        admission_tail = wait;
    }
    int status;
    for (;;)
    {
        if (wait && wait->cancelled) { status = EPD_COMPUTE_CANCELLED; break; }
        /* A quarantined component cannot accumulate another retired payload
         * or hold unrelated shared components behind its admission waiter. */
        epd_compute_wait_t first = admission_head;
        while (first && cleanup_blocks(first->session->wire.capability)) first = first->next;
        if (first == wait && admissible(&epd_compute_capabilities[s->wire.capability]))
        {
            s->active_next = active;
            active = s;
            status = EPD_COMPUTE_OK;
            break;
        }
        if (!wait) { status = EPD_COMPUTE_BUSY; break; }
        rt_hw_interrupt_enable(level);
        rt_sem_take(&wait->changed, RT_WAITING_FOREVER);
        level = rt_hw_interrupt_disable();
        wait->notified = false;
    }
    if (wait)
    {
        unlink_admission(wait);
        if (wait->notified)
        {
            rt_sem_take(&wait->changed, 0);
            wait->notified = false;
        }
        if (status != EPD_COMPUTE_OK)
        {
            wait->session = NULL;
            wait->in_use = false;
        }
    }
    rt_hw_interrupt_enable(level);
    return status;
}
#endif

void epd_compute_resume_dispatch(void)
{
#ifdef EPD_COMPUTE_ACPU
    epd_compute_pm_report();
    rt_base_t level = rt_hw_interrupt_disable();
    if (ready && queue_head) notify_dispatcher();
    rt_hw_interrupt_enable(level);
#endif
}

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

static int open_session(const epd_compute_capability_t *capability,
                     const epd_compute_requirements_t *requirements,
                     const epd_compute_request_t *request, epd_compute_wait_t wait,
                     epd_compute_session_t *session)
{
    if (!session) return EPD_COMPUTE_INVALID;
    *session = NULL;
#ifdef EPD_COMPUTE_ACPU
    if (!epd_compute_can_wait() || rt_thread_self() == &dispatcher)
        return EPD_COMPUTE_INVALID;
    if (!capability || !requirements || !request || capability->index >= epd_compute_capability_count ||
        (unsigned)requirements->purpose >= EPD_COMPUTE_PURPOSE_COUNT ||
        requirements->max_regions > (SIZE_MAX - sizeof(struct epd_compute_session)) / sizeof(epd_compute_region_t))
        return EPD_COMPUTE_INVALID;
    if (request->region_count > requirements->max_regions ||
        epd_compute_validate_regions(request->regions, request->region_count) != EPD_COMPUTE_OK)
        return EPD_COMPUTE_INVALID;
    const epd_compute_registration_t *c = &epd_compute_capabilities[capability->index];
    if (!ready || !(c->flags & (EPD_COMPUTE_VALIDATED | EPD_COMPUTE_ALLOW_UNVALIDATED)))
        return EPD_COMPUTE_UNAVAILABLE;
    if (!(c->purposes & EPD_COMPUTE_PURPOSE(requirements->purpose))) return EPD_COMPUTE_UNSUPPORTED;
    if (request->cancelled && request->cancelled(request->context)) return EPD_COMPUTE_CANCELLED;
    /* One event-driven cleanup attempt; a failed CLOSE stays owned by the
     * resident scheduler. Never poll or prevent cancellation of admission. */
    (void)epd_compute_retry_cleanup();
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
    rt_tick_t admission_start = rt_tick_get();
    int status = admit_session(s, wait);
    s->diagnostics.admission_ticks = (uint32_t)(rt_tick_get() - admission_start);
    if (status != EPD_COMPUTE_OK)
    {
        diagnostic_result(s, EPD_COMPUTE_OPEN, status, false);
        rt_sem_detach(&s->done);
        epd_app_free(s);
        return status;
    }
    status = submit(s, EPD_COMPUTE_OPEN, request);
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancelled = (s->signals & EPD_COMPUTE_CANCEL) != 0;
    if (wait) wait->session = NULL;
    rt_hw_interrupt_enable(level);
    if (status != EPD_COMPUTE_OK || cancelled)
    {
        if (status == EPD_COMPUTE_OK) status = EPD_COMPUTE_CANCELLED;
        if (s->wire.session)
        {
            /* A failed OPEN may still own remote state and borrowed buffers.
             * Make the returned handle close-only if rollback cannot finish. */
            epd_compute_cancel(s);
            epd_compute_request_t close_request = {0};
            int closed = submit(s, EPD_COMPUTE_CLOSE, &close_request);
            if (closed != EPD_COMPUTE_OK) *session = s;
        }
        if (!*session) release_session(s);
        if (wait)
        {
            level = rt_hw_interrupt_disable();
            wait->in_use = false;
            rt_hw_interrupt_enable(level);
        }
        return status;
    }
    *session = s;
    if (wait)
    {
        level = rt_hw_interrupt_disable();
        wait->in_use = false;
        rt_hw_interrupt_enable(level);
    }
    return EPD_COMPUTE_OK;
#else
    (void)capability; (void)requirements; (void)request; (void)wait;
    return EPD_COMPUTE_UNAVAILABLE;
#endif
}

int epd_compute_open(const epd_compute_capability_t *capability,
                     const epd_compute_requirements_t *requirements,
                     const epd_compute_request_t *request, epd_compute_session_t *session)
{
    return open_session(capability, requirements, request, NULL, session);
}

int epd_compute_open_wait(const epd_compute_capability_t *capability,
                         const epd_compute_requirements_t *requirements,
                         const epd_compute_request_t *request,
                         epd_compute_wait_t wait, epd_compute_session_t *session)
{
    if (!wait) { if (session) *session = NULL; return EPD_COMPUTE_INVALID; }
    return open_session(capability, requirements, request, wait, session);
}

int epd_compute_wait_create(epd_compute_wait_t *wait)
{
    if (!wait) return EPD_COMPUTE_INVALID;
    *wait = NULL;
#ifdef EPD_COMPUTE_ACPU
    epd_compute_wait_t w = epd_app_alloc(sizeof(*w), EPD_APP_SRAM);
    if (!w) return EPD_COMPUTE_NO_MEMORY;
    memset(w, 0, sizeof(*w));
    if (rt_sem_init(&w->changed, "cmpa", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    { epd_app_free(w); return EPD_COMPUTE_NO_MEMORY; }
    *wait = w;
    return EPD_COMPUTE_OK;
#else
    return EPD_COMPUTE_UNAVAILABLE;
#endif
}

void epd_compute_wait_cancel(epd_compute_wait_t wait)
{
#ifdef EPD_COMPUTE_ACPU
    if (!wait) return;
    rt_base_t level = rt_hw_interrupt_disable();
    wait->cancelled = true;
    if (wait->session) cancel_session(wait->session);
    /* Wake the admission owner so it can unlink itself and notify its successor. */
    for (epd_compute_wait_t w = admission_head; w; w = w->next)
        if (w == wait && !w->notified)
        {
            w->notified = true;
            rt_sem_release(&w->changed);
            break;
        }
    rt_hw_interrupt_enable(level);
#else
    (void)wait;
#endif
}

int epd_compute_wait_destroy(epd_compute_wait_t wait)
{
#ifdef EPD_COMPUTE_ACPU
    if (!wait) return EPD_COMPUTE_INVALID;
    rt_base_t level = rt_hw_interrupt_disable();
    if (wait->in_use) { rt_hw_interrupt_enable(level); return EPD_COMPUTE_BUSY; }
    rt_hw_interrupt_enable(level);
    rt_sem_detach(&wait->changed);
    epd_app_free(wait);
    return EPD_COMPUTE_OK;
#else
    (void)wait; return EPD_COMPUTE_UNAVAILABLE;
#endif
}

int epd_compute_set_purpose(epd_compute_session_t session, epd_compute_purpose_t purpose)
{
#ifdef EPD_COMPUTE_ACPU
    if (!session || (unsigned)purpose >= EPD_COMPUTE_PURPOSE_COUNT) return EPD_COMPUTE_INVALID;
    const epd_compute_registration_t *c = &epd_compute_capabilities[session->wire.capability];
    if (!(c->purposes & EPD_COMPUTE_PURPOSE(purpose))) return EPD_COMPUTE_UNSUPPORTED;
    rt_base_t level = rt_hw_interrupt_disable();
    session->purpose = purpose;
    update_yield();
    rt_hw_interrupt_enable(level);
    return EPD_COMPUTE_OK;
#else
    (void)session; (void)purpose; return EPD_COMPUTE_UNAVAILABLE;
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
        cancel_session(session);
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

void epd_compute_close_later(epd_compute_session_t session, epd_compute_buffer_t *buffers, uint32_t count)
{
#ifdef EPD_COMPUTE_ACPU
    /* The sole owner calls only after synchronous OPEN/STEP/CLOSE returned.
     * Ownership transfer allocates nothing and has no failure path. */
    rt_base_t level = rt_hw_interrupt_disable();
    session->signals |= EPD_COMPUTE_CANCEL;
    session->owned_buffers = buffers;
    session->owned_count = count;
    notify_admission();
    rt_hw_interrupt_enable(level);
#else
    (void)session; (void)buffers; (void)count;
#endif
}

int epd_compute_retry_cleanup(void)
{
#ifdef EPD_COMPUTE_ACPU
    if (!epd_compute_can_wait() || rt_thread_self() == &dispatcher)
        return EPD_COMPUTE_INVALID;
    rt_base_t level = rt_hw_interrupt_disable();
    epd_compute_session_t s = active;
    while (s && (!s->owned_buffers || s->reclaiming)) s = s->active_next;
    if (s) {
        s->reclaiming = true;
        /* Rotate attempts so one permanent failure cannot starve another
         * retired session. Active-list order does not determine admission. */
        if (s->active_next) {
            epd_compute_session_t *link = &active;
            while (*link != s) link = &(*link)->active_next;
            *link = s->active_next;
            epd_compute_session_t last = active;
            while (last->active_next) last = last->active_next;
            last->active_next = s; s->active_next = NULL;
        }
    }
    rt_hw_interrupt_enable(level);
    if (!s) return EPD_COMPUTE_OK;
    int status = epd_compute_close(s);
    if (status != EPD_COMPUTE_OK) {
        level = rt_hw_interrupt_disable();
        s->reclaiming = false;
        rt_hw_interrupt_enable(level);
    }
    return status;
#else
    return EPD_COMPUTE_UNAVAILABLE;
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
RTM_EXPORT(epd_compute_open_wait);
RTM_EXPORT(epd_compute_wait_create);
RTM_EXPORT(epd_compute_wait_cancel);
RTM_EXPORT(epd_compute_wait_destroy);
RTM_EXPORT(epd_compute_set_purpose);
RTM_EXPORT(epd_compute_step);
RTM_EXPORT(epd_compute_reset);
RTM_EXPORT(epd_compute_cancel);
RTM_EXPORT(epd_compute_close);
RTM_EXPORT(epd_compute_close_later);
RTM_EXPORT(epd_compute_retry_cleanup);
RTM_EXPORT(epd_compute_buffer_alloc);
RTM_EXPORT(epd_compute_buffer_free);
RTM_EXPORT(epd_compute_buffer_region);
