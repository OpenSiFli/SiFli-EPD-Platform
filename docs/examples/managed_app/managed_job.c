/* SPDX-License-Identifier: Apache-2.0 */
#include "managed_job.h"
#include "platform/epd_memory.h"
#include "platform/epd_operation_runtime.h"
#include <string.h>

enum { FIRST_BYTES = 1024, FINAL_BYTES = 4096, SLICE_BYTES = 128 };

static void scopes_release(managed_job_t *job)
{
    if (job->business) {
        epd_operation_scope_close(job->business);
        epd_operation_scope_release(job->business); job->business = 0;
    }
    if (job->cleanup) {
        epd_operation_scope_close(job->cleanup);
        epd_operation_scope_release(job->cleanup); job->cleanup = 0;
    }
    if (job->execution != EPD_MEMORY_OWNER_INVALID) {
        epd_memory_owner_close(job->execution);
        epd_memory_owner_release(job->execution); job->execution = EPD_MEMORY_OWNER_INVALID;
    }
}

/* All resource calls use a real accepted TASK, never an application identity
 * as evidence of priority. begin/update only register a request. */
static bool capacity_request(managed_job_t *job, bool resize)
{
    epd_resource_context_request_t request = {0};
    request.kind = EPD_RESOURCE_CONTINUOUS;
    epd_memory_backend_t backend;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend)) return false;
    request.backend = backend;
    size_t peak = epd_app_allocation_peak(job->target);
    if (resize) peak += epd_app_allocation_peak(job->size);
    request.minimum_bytes = request.target_bytes = peak;
    epd_memory_owner_t previous = epd_memory_owner_enter(job->task);
    if (previous == EPD_MEMORY_OWNER_INVALID) return false;
    int result = resize ? epd_resource_context_update(job->capacity, &request, &job->revision) :
                          epd_resource_context_begin(&request, NULL, &job->capacity);
    epd_memory_owner_leave(previous);
    if (!resize) job->revision = 1;
    return result == EPD_RESOURCE_ACCEPTED;
}

static void calculate(epd_operation_t operation, void *context)
{
    managed_job_t *job = context;
    epd_operation_snapshot_t snapshot;
    if (!epd_operation_get(operation, &snapshot)) return;
    if (snapshot.cancel_requested) {
        epd_operation_complete(operation, EPD_OPERATION_CANCELLED, 0, NULL); return;
    }
    if (!job->offset) {
        epd_memory_owner_t previous = epd_resource_context_enter(job->capacity);
        if (previous == EPD_MEMORY_OWNER_INVALID) {
            epd_operation_complete(operation, EPD_OPERATION_FAILED, EPD_RESOURCE_DENIED, NULL); return;
        }
        /* Replacement needs old + new physical peaks. Allocation failure
         * leaves the old buffer intact until terminal cleanup. */
        unsigned char *next = epd_app_alloc(job->target, EPD_APP_PSRAM);
        epd_resource_context_leave(previous);
        if (!next) {
            epd_operation_complete(operation, EPD_OPERATION_FAILED, EPD_RESOURCE_CAPACITY, NULL); return;
        }
        if (job->buffer) memcpy(next, job->buffer, job->size);
        epd_app_free(job->buffer); job->buffer = next; job->size = job->target;
        job->checksum = 0;
    }
    size_t end = job->size - job->offset > SLICE_BYTES ? job->offset + SLICE_BYTES : job->size;
    while (job->offset < end) {
        job->buffer[job->offset] = (unsigned char)job->offset;
        job->checksum += job->buffer[job->offset++];
    }
    if (job->offset < job->size) epd_operation_yield(operation);
    else epd_operation_complete(operation, EPD_OPERATION_OK, 0, NULL);
}

static bool received(epd_operation_t operation, const epd_operation_snapshot_t *state,
                     void *result, void *context)
{
    (void)operation; (void)state; (void)result; (void)context;
    /* No page pointer or heap result crosses delivery. poll reads the model
     * only after execution, delivery and cleanup have all finished. */
    return true;
}

static bool calculation_start(managed_job_t *job)
{
    epd_operation_spec_t spec = {0};
    if (!epd_operation_runtime_executors(&spec.execute, &spec.deliver, &spec.cleanup)) return false;
    spec.scope = job->business; spec.cleanup_scope = job->cleanup;
    spec.run = calculate; spec.receive = received; spec.user = job;
    job->offset = 0;
    return epd_operation_submit(&spec, &job->operation);
}

bool managed_job_active(const managed_job_t *job)
{
    return job && job->phase != DEMO_IDLE && job->phase != DEMO_DONE;
}

bool managed_job_start(managed_job_t *job, uint32_t generation)
{
    if (!job || !generation || managed_job_active(job)) return false;
    memset(job, 0, sizeof(*job)); job->generation = generation;
    job->task = job->execution = EPD_MEMORY_OWNER_INVALID;
    job->result = EPD_OPERATION_FAILED;
    if (epd_app_task_group_create(&job->group) != EPD_MEMORY_OK) return false;
    if (epd_app_task_begin(job->group, &job->task) != EPD_MEMORY_OK) goto failed;
    /* Sequential operation slices have their own executor lifetime. Their
     * completion must not withdraw the enclosing accepted task's capacity. */
    job->execution = epd_memory_scope_create(job->task, EPD_MEMORY_SCOPE_TASK);
    if (job->execution == EPD_MEMORY_OWNER_INVALID ||
        !epd_operation_scope_open(job->execution, generation, &job->business) ||
        !epd_operation_scope_open(job->execution, generation, &job->cleanup)) goto failed;
    epd_app_background_policy_t policy = {EPD_APP_BG_TASKS, job->group, EPD_APP_BG_STATE};
    if (epd_app_background_set(&policy, NULL) != EPD_MEMORY_OK) goto failed;
    job->target = FIRST_BYTES;
    if (!capacity_request(job, false)) goto failed;
    job->phase = DEMO_ACQUIRE; return true;
failed:
    scopes_release(job);
    if (job->task != EPD_MEMORY_OWNER_INVALID) epd_app_task_end(job->task);
    epd_app_task_group_release(job->group);
    memset(job, 0, sizeof(*job)); return false;
}

void managed_job_cancel(managed_job_t *job)
{
    if (!managed_job_active(job)) return;
    job->cancelled = true;
    if (job->operation) epd_operation_cancel(job->operation);
}

static void capacity_release(managed_job_t *job)
{
    /* Called only after the operation's FINISHED state: no worker or delivery
     * callback can still access the buffer. end itself is asynchronous. */
    epd_app_free(job->buffer); job->buffer = NULL; job->size = 0;
    epd_resource_activity_end(job->capacity);
    job->phase = DEMO_RELEASE;
}

void managed_job_poll(managed_job_t *job)
{
    if (!managed_job_active(job)) return;
    if (job->operation) {
        epd_operation_snapshot_t state;
        if (!epd_operation_get(job->operation, &state) || state.state != EPD_OPERATION_FINISHED) return;
        job->result = state.status;
        epd_operation_release(job->operation); job->operation = 0;
        if (!job->cancelled && state.status == EPD_OPERATION_OK && job->phase == DEMO_FIRST) {
            job->target = FINAL_BYTES;
            if (capacity_request(job, true)) { job->phase = DEMO_RESIZE; return; }
            job->result = EPD_OPERATION_FAILED;
        }
        if (job->cancelled) job->result = EPD_OPERATION_CANCELLED;
        capacity_release(job); return;
    }
    epd_resource_activity_snapshot_t state;
    if (epd_resource_activity_get(job->capacity, &state) != EPD_RESOURCE_OK) return;
    if (job->phase == DEMO_RELEASE) {
        if (state.state != EPD_RESOURCE_ENDED ||
            epd_resource_activity_forget(job->capacity) != EPD_RESOURCE_OK) return;
        job->capacity = 0; scopes_release(job);
        epd_app_task_end(job->task); job->task = EPD_MEMORY_OWNER_INVALID;
        epd_app_task_group_release(job->group); job->group = 0;
        job->phase = DEMO_DONE; return;
    }
    if (job->cancelled || state.state == EPD_RESOURCE_REJECTED || state.state == EPD_RESOURCE_ENDED) {
        job->result = job->cancelled ? EPD_OPERATION_CANCELLED : EPD_OPERATION_FAILED;
        capacity_release(job); return;
    }
    /* A resize may leave the old credit admitted. Only the new effective
     * revision permits the larger working set. No retry/resubmit on WAITING. */
    if (state.state != EPD_RESOURCE_ADMITTED || state.effective_revision != job->revision) return;
    if (!calculation_start(job)) {
        job->result = EPD_OPERATION_FAILED; capacity_release(job); return;
    }
    job->phase = job->phase == DEMO_ACQUIRE ? DEMO_FIRST : DEMO_SECOND;
}
