/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_compute_client.h"
#include "reader_wire.h"
#include "epd_memory.h"
#include "epd_compute.h"
#include <rthw.h>
#include <stdio.h>
#include <string.h>

#define READER_JOBS 4u
#ifndef READER_COMPUTE_BATCH_TRACE
#define READER_COMPUTE_BATCH_TRACE 0
#endif
struct reader_compute_job {
    struct reader_compute_job *next;
    unsigned references;
    bool submitted, done, cancelled, foreground;
    int result;
    epd_reader_batch_t batch;
};
typedef struct {
    epd_reader_batch_t *batch;
    uint32_t epoch, sequence, record;
    bool valid;
} cached_slot_t;

static reader_compute_job_t *queue, *tail, *active;
static unsigned jobs, victim;
static bool stopping, idle_requested;
/* Completion detaches active before cleanup; serialize all worker/shutdown
 * ownership separately until session cleanup and reference release finish. */
static bool worker_busy;
static epd_service_t *owner;
static epd_compute_session_t session;
static epd_compute_wait_t admission;
static bool closing;
static int close_status;
static reader_compute_wire_stats_t wire_stats;
static uint32_t session_limit, next_epoch;
static epd_compute_purpose_t session_purpose;
static cached_slot_t slots[READER_SLOTS];
static epd_compute_buffer_t input, output;

static bool cancelled(void *context)
{
    reader_compute_job_t *job = context;
    rt_base_t key = rt_hw_interrupt_disable();
    bool result = job->cancelled || stopping;
    epd_service_t *service = owner;
    rt_hw_interrupt_enable(key);
    return result || (service && epd_service_cancelled(service));
}

void reader_compute_release(reader_compute_job_t *job)
{
    if (!job) return;
    rt_base_t key = rt_hw_interrupt_disable();
    RT_ASSERT(job->references);
    bool last = --job->references == 0;
    if (last) --jobs;
    if (last && !jobs && owner) epd_service_wake(owner);
    rt_hw_interrupt_enable(key);
    if (last) epd_app_free(job);
}

reader_compute_job_t *reader_compute_create(int *status)
{
    int ignored;
    if (!status) status = &ignored;
    epd_compute_capability_t capability;
    *status = epd_compute_query(EPD_READER_CAPABILITY, EPD_READER_VERSION, &capability);
    if (*status != EPD_COMPUTE_OK) return NULL;
    if (!capability.ready || !capability.registration.step_limit) { *status = EPD_COMPUTE_UNAVAILABLE; return NULL; }
    rt_base_t key = rt_hw_interrupt_disable();
    if (stopping || jobs >= READER_JOBS) {
        *status = stopping ? EPD_COMPUTE_CANCELLED : EPD_COMPUTE_BUSY;
        rt_hw_interrupt_enable(key); return NULL;
    }
    ++jobs;
    rt_hw_interrupt_enable(key);
    reader_compute_job_t *job = epd_app_alloc(sizeof(*job), EPD_APP_PSRAM);
    if (job) { memset(job, 0, sizeof(*job)); job->references = 1; *status = EPD_COMPUTE_OK; return job; }
    key = rt_hw_interrupt_disable(); --jobs; rt_hw_interrupt_enable(key);
    *status = EPD_COMPUTE_NO_MEMORY;
    return NULL;
}

epd_reader_batch_t *reader_compute_data(reader_compute_job_t *job)
{ RT_ASSERT(!job->submitted || job->done); return &job->batch; }

void reader_compute_submit_target(reader_compute_job_t *job, bool foreground)
{
    rt_base_t key = rt_hw_interrupt_disable();
    RT_ASSERT(!job->submitted || job->done);
    job->submitted = true; job->foreground = foreground; job->done = false; job->next = NULL;
    if (stopping || job->cancelled) {
        job->result = EPD_COMPUTE_CANCELLED; job->done = true;
        rt_hw_interrupt_enable(key); return;
    }
    idle_requested = false; ++job->references;
    if (tail) tail->next = job; else queue = job;
    tail = job;
    if (owner) epd_service_wake(owner);
    rt_hw_interrupt_enable(key);
}
void reader_compute_submit(reader_compute_job_t *job) { reader_compute_submit_target(job, false); }
void reader_compute_idle(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    idle_requested = true;
    if (owner) epd_service_wake(owner);
    rt_hw_interrupt_enable(key);
}
bool reader_compute_poll(reader_compute_job_t *job, int *status)
{
    rt_base_t key = rt_hw_interrupt_disable();
    bool done = job->done;
    if (done && status) *status = job->result;
    rt_hw_interrupt_enable(key); return done;
}

void reader_compute_cancel(reader_compute_job_t *job)
{
    if (!job) return;
    bool removed = false;
    rt_base_t key = rt_hw_interrupt_disable();
    job->cancelled = true;
    if (job == active) {
        if (admission) epd_compute_wait_cancel(admission);
        if (session && !closing) epd_compute_cancel(session);
    }
    reader_compute_job_t **p = &queue, *previous = NULL;
    while (*p && *p != job) { previous = *p; p = &(*p)->next; }
    if (*p) {
        *p = job->next;
        if (tail == job) tail = previous;
        job->result = EPD_COMPUTE_CANCELLED; job->done = true; removed = true;
    }
    if (owner) epd_service_wake(owner);
    rt_hw_interrupt_enable(key);
    if (removed) reader_compute_release(job);
    reader_compute_release(job);
}

static int close_session(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    epd_compute_session_t old = session;
    closing = old != NULL;
    rt_hw_interrupt_enable(key);
    if (old) {
        epd_compute_cancel(old);
        int status = epd_compute_close(old);
        key = rt_hw_interrupt_disable();
        close_status = status;
        if (status == EPD_COMPUTE_OK) session = NULL;
        rt_hw_interrupt_enable(key);
        if (status != EPD_COMPUTE_OK) return status;
        if (wire_stats.calls) {
            char summary[192];
            snprintf(summary, sizeof(summary), "[reader-compute] backend=%s calls=%llu tx_bytes=%llu rx_bytes=%llu rx_payload=%llu\n",
                       wire_stats.verified_replies ? "ACPU" : "unconfirmed",
                       (unsigned long long)wire_stats.calls, (unsigned long long)wire_stats.tx_bytes,
                       (unsigned long long)wire_stats.rx_bytes, (unsigned long long)wire_stats.rx_payload);
            rt_kprintf("%s", summary);
        }
    }
    for (unsigned i = 0; i < READER_SLOTS; ++i) {
        epd_app_free(slots[i].batch); memset(&slots[i], 0, sizeof(slots[i]));
    }
    epd_compute_buffer_free(&input); epd_compute_buffer_free(&output);
    key = rt_hw_interrupt_disable();
    closing = false;
    rt_hw_interrupt_enable(key);
    return EPD_COMPUTE_OK;
}

static int ensure_session(reader_compute_job_t *job)
{
    if (closing) {
        int status = close_session();
        if (status != EPD_COMPUTE_OK) return status;
    }
    epd_compute_purpose_t purpose = job->foreground ? EPD_COMPUTE_FOREGROUND : EPD_COMPUTE_BACKGROUND;
    if (session) {
        if (session_purpose != purpose) {
            int status = epd_compute_set_purpose(session, purpose);
            if (status != EPD_COMPUTE_OK) return status;
            session_purpose = purpose;
        }
        return EPD_COMPUTE_OK;
    }
    epd_compute_capability_t capability;
    int status = epd_compute_query(EPD_READER_CAPABILITY, EPD_READER_VERSION, &capability);
    if (status != EPD_COMPUTE_OK) return status;
    if (!capability.ready || !capability.registration.step_limit) return EPD_COMPUTE_UNAVAILABLE;
    if (!epd_compute_buffer_alloc(&input, READER_TX_MAX) || !epd_compute_buffer_alloc(&output, READER_RX_MAX)) {
        epd_compute_buffer_free(&input); epd_compute_buffer_free(&output);
        return EPD_COMPUTE_NO_MEMORY;
    }
    epd_compute_requirements_t requirements = {purpose, 2};
    epd_compute_request_t request = {0};
    request.params.region = request.result.region = EPD_COMPUTE_REGION_NONE;
    request.cancelled = cancelled; request.context = job;
    epd_compute_session_t opened = NULL;
    epd_compute_wait_t wait = NULL;
    status = epd_compute_wait_create(&wait);
    if (status != EPD_COMPUTE_OK) {
        epd_compute_buffer_free(&input); epd_compute_buffer_free(&output); return status;
    }
    rt_base_t key = rt_hw_interrupt_disable();
    admission = wait;
    if (job->cancelled || stopping) epd_compute_wait_cancel(wait);
    rt_hw_interrupt_enable(key);
    status = epd_compute_open_wait(&capability, &requirements, &request, wait, &opened);
    key = rt_hw_interrupt_disable();
    admission = NULL;
    session = opened;
    if (session) {
        memset(&wire_stats, 0, sizeof(wire_stats));
        closing = status != EPD_COMPUTE_OK;
        close_status = status;
    }
    if (session && (job->cancelled || stopping)) epd_compute_cancel(session);
    rt_hw_interrupt_enable(key);
    /* The published handle is removed under the same guard used by cancel.
     * open_wait has returned, so no caller can still be using this wait. */
    epd_compute_wait_destroy(wait);
    if (status != EPD_COMPUTE_OK) {
        if (!opened) { epd_compute_buffer_free(&input); epd_compute_buffer_free(&output); }
        return status;
    }
    key = rt_hw_interrupt_disable();
    memset(&wire_stats, 0, sizeof(wire_stats)); close_status = EPD_COMPUTE_OK;
    rt_hw_interrupt_enable(key);
    session_limit = capability.registration.step_limit; session_purpose = purpose;
    return EPD_COMPUTE_OK;
}

static int execute(reader_compute_job_t *job)
{
    if (cancelled(job)) return EPD_COMPUTE_CANCELLED;
    int status = ensure_session(job);
    if (status != EPD_COMPUTE_OK) return status;
    if (cancelled(job)) { epd_compute_cancel(session); close_session(); return EPD_COMPUTE_CANCELLED; }
    epd_reader_batch_t *b = &job->batch;
    if (b->limit > session_limit) b->limit = session_limit;
    unsigned index = READER_SLOTS;
    for (unsigned i = 0; i < READER_SLOTS; ++i)
        if (slots[i].valid && slots[i].batch->generation == b->generation &&
            reader_page_equal(&slots[i].batch->page, &b->page)) { index = i; break; }
    uint32_t flags = 0;
    if (index == READER_SLOTS) {
        index = victim++ % READER_SLOTS;
        flags = READER_SEED_PAGE | READER_SEED_RECORD;
    }
    cached_slot_t *cache = &slots[index];
    if (!cache->batch) {
        cache->batch = epd_app_alloc(sizeof(*b), EPD_APP_PSRAM);
        if (!cache->batch) return EPD_COMPUTE_NO_MEMORY;
    }
    if (flags) {
        do { ++next_epoch; } while (!next_epoch);
        cache->epoch = next_epoch; cache->sequence = 0; cache->valid = false;
    } else if (cache->record != b->page.cursor.record || !reader_record_equal(cache->batch, b)) flags = READER_SEED_RECORD;
    uint32_t sequence = cache->sequence + 1;
    if (!sequence) ++sequence;
    size_t bytes = reader_wire_encode(input.data, input.capacity, b, index, cache->epoch, sequence, flags);
    if (!bytes) return EPD_COMPUTE_INVALID;
    size_t bound = reader_wire_reply_bound(b);
    if (bound > output.capacity) return EPD_COMPUTE_INVALID;
    epd_compute_region_t regions[2] = {
        epd_compute_buffer_region(&input, (uint32_t)bytes, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&output, 0, EPD_COMPUTE_WRITE)
    };
    /* Cache maintenance uses capacity, not length. Expose only this batch's
     * aligned input and bounded reply, even though buffers are reused. */
    regions[0].capacity = (uint32_t)RT_ALIGN(bytes, EPD_COMPUTE_CACHE_LINE);
    regions[1].capacity = (uint32_t)RT_ALIGN(bound, EPD_COMPUTE_CACHE_LINE);
    epd_compute_request_t request = {regions, 2, {0, 0, (uint32_t)bytes}, {1, 0, (uint32_t)bound}, cancelled, job};
    uint32_t record = b->page.cursor.record;
    status = epd_compute_step(session, &request);
    rt_base_t key = rt_hw_interrupt_disable();
    ++wire_stats.calls;
    wire_stats.tx_bytes += regions[0].capacity;
    wire_stats.rx_bytes += regions[1].capacity;
    rt_hw_interrupt_enable(key);
    if (cancelled(job)) status = EPD_COMPUTE_CANCELLED;
    if (status == EPD_COMPUTE_OK && !reader_wire_apply(b, input.data, output.data, bound)) status = EPD_COMPUTE_COMPONENT_ERROR;
    if (status == EPD_COMPUTE_OK && !epd_reader_batch_copy(cache->batch, b)) status = EPD_COMPUTE_COMPONENT_ERROR;
    if (status == EPD_COMPUTE_OK) {
        cache->record = record; cache->sequence = sequence; cache->valid = true;
        const reader_wire_reply_t *reply = output.data;
        key = rt_hw_interrupt_disable();
        ++wire_stats.verified_replies;
        wire_stats.rx_payload += reply->size;
        rt_hw_interrupt_enable(key);
#if READER_COMPUTE_BATCH_TRACE
        rt_kprintf("[reader-compute] backend=ACPU gen=%lu slot=%u seq=%lu tx_bytes=%lu rx_bytes=%lu rx_payload=%lu\n",
                   (unsigned long)b->generation, index, (unsigned long)sequence,
                   (unsigned long)regions[0].capacity, (unsigned long)regions[1].capacity, (unsigned long)reply->size);
#endif
    } else close_session();
    return status;
}

bool reader_compute_worker_step(epd_service_t *service)
{
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_busy || (stopping && (session || admission || active || jobs))) {
        rt_hw_interrupt_enable(key); return false;
    }
    worker_busy = true;
    owner = service; stopping = false;
    reader_compute_job_t *job = queue;
    if (job) { queue = job->next; if (!queue) tail = NULL; active = job; }
    bool idle = idle_requested || !jobs;
    rt_hw_interrupt_enable(key);
    if (!job) {
        bool progressed = (idle || closing) && session && close_session() == EPD_COMPUTE_OK;
        key = rt_hw_interrupt_disable(); worker_busy = false; rt_hw_interrupt_enable(key);
        return progressed;
    }
    epd_reader_cursor_t start = job->batch.page.cursor;
    int result = execute(job);
    /* done permits the UI to change the batch and resubmit this same object. */
    const struct {
        uint32_t generation, kind, limit, events, page_done, eof;
        epd_reader_cursor_t cursor;
    } completed = {
        job->batch.generation, job->batch.kind, job->batch.limit,
        job->batch.events, job->batch.page.done, job->batch.page.eof, job->batch.page.cursor
    };
    key = rt_hw_interrupt_disable();
    bool discard = job->cancelled, stopped = stopping;
    int status = discard || stopped ? EPD_COMPUTE_CANCELLED : result;
    /* Cancel must stop targeting this execution before completion is visible.
     * Keep the worker reference until cleanup ends, but never read the batch
     * or cancellation state again: a new submission may already own them. */
    RT_ASSERT(active == job);
    active = NULL;
    job->result = status; job->done = true;
    rt_hw_interrupt_enable(key);
    if (status != EPD_COMPUTE_OK) {
        char diagnostic[320];
        snprintf(diagnostic, sizeof(diagnostic),
                 "[reader-compute] error status=%d execute_status=%d generation=%lu kind=%lu cursor=%lu:%lu->%lu:%lu events=%lu limit=%lu page_done=%lu eof=%lu cancelled=%u stopping=%u\n",
                 status, result, (unsigned long)completed.generation, (unsigned long)completed.kind,
                 (unsigned long)start.record, (unsigned long)start.byte,
                 (unsigned long)completed.cursor.record, (unsigned long)completed.cursor.byte,
                 (unsigned long)completed.events, (unsigned long)completed.limit,
                 (unsigned long)completed.page_done, (unsigned long)completed.eof,
                 (unsigned)discard, (unsigned)stopped);
        rt_kprintf("%s", diagnostic);
    }
    if (discard && !closing && session) close_session();
    reader_compute_release(job);
    if (service) epd_service_ui_request(service, 0);
    key = rt_hw_interrupt_disable(); worker_busy = false; rt_hw_interrupt_enable(key);
    return true;
}

void reader_compute_stop(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    stopping = true;
    if (admission) epd_compute_wait_cancel(admission);
    if (session && !closing) epd_compute_cancel(session);
    if (owner) epd_service_wake(owner);
    rt_hw_interrupt_enable(key);
}

reader_compute_state_t reader_compute_state(int *status)
{
    rt_base_t key = rt_hw_interrupt_disable();
    reader_compute_state_t state = closing ? READER_COMPUTE_CLOSING :
        admission ? READER_COMPUTE_WAITING : active ? READER_COMPUTE_RUNNING :
        stopping ? (worker_busy || session || jobs ? READER_COMPUTE_STOPPING : READER_COMPUTE_STOPPED) :
        session ? READER_COMPUTE_READY : READER_COMPUTE_IDLE;
    if (status) *status = close_status;
    rt_hw_interrupt_enable(key);
    return state;
}

void reader_compute_wire_stats(reader_compute_wire_stats_t *stats)
{
    if (!stats) return;
    rt_base_t key = rt_hw_interrupt_disable(); *stats = wire_stats; rt_hw_interrupt_enable(key);
}

int reader_compute_shutdown(void)
{
    reader_compute_stop();
    rt_base_t key = rt_hw_interrupt_disable();
    bool busy = worker_busy || active != NULL || admission != NULL;
    if (!busy) worker_busy = true;
    rt_hw_interrupt_enable(key);
    if (busy) return EPD_COMPUTE_BUSY;
    for (;;) {
        key = rt_hw_interrupt_disable();
        reader_compute_job_t *job = queue;
        if (job) { queue = job->next; job->result = EPD_COMPUTE_CANCELLED; job->done = true; }
        if (!queue) tail = NULL;
        rt_hw_interrupt_enable(key);
        if (!job) break;
        reader_compute_release(job);
    }
    int status = close_session();
    key = rt_hw_interrupt_disable();
    if (status == EPD_COMPUTE_OK && jobs) status = EPD_COMPUTE_BUSY;
    if (status == EPD_COMPUTE_OK) owner = NULL;
    worker_busy = false;
    rt_hw_interrupt_enable(key);
    return status;
}
