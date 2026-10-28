/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_compute_client.h"
#include "epd_memory.h"
#include "epd_compute.h"
#include <rthw.h>
#include <string.h>

struct reader_compute_job
{
    struct reader_compute_job *next;
    epd_compute_buffer_t buffer;
    unsigned references;
    bool submitted, done, cancelled, foreground;
    int result;
};

static reader_compute_job_t *queue, *tail;
static unsigned jobs;
static bool stopping, idle_requested;
static epd_service_t *owner;
static epd_compute_session_t session;
static uint32_t session_limit;
static epd_compute_purpose_t session_purpose;

static bool cancelled(void *context)
{
    reader_compute_job_t *job = context;
    rt_base_t key = rt_hw_interrupt_disable();
    bool result = job->cancelled || stopping;
    rt_hw_interrupt_enable(key);
    return result || (owner && epd_service_cancelled(owner));
}

void reader_compute_release(reader_compute_job_t *job)
{
    if (!job) return;
    rt_base_t key = rt_hw_interrupt_disable();
    bool last = --job->references == 0;
    if (last) --jobs;
    if (last && !jobs && owner) epd_service_wake(owner);
    rt_hw_interrupt_enable(key);
    if (last) { epd_compute_buffer_free(&job->buffer); epd_app_free(job); }
}

reader_compute_job_t *reader_compute_create(int *status)
{
    rt_base_t key = rt_hw_interrupt_disable();
    if (stopping || jobs >= 2)
    {
        *status = stopping ? EPD_COMPUTE_CANCELLED : EPD_COMPUTE_BUSY;
        rt_hw_interrupt_enable(key);
        return NULL;
    }
    ++jobs;
    rt_hw_interrupt_enable(key);
    reader_compute_job_t *job = epd_app_alloc(sizeof(*job), EPD_APP_PSRAM);
    if (job)
    {
        memset(job, 0, sizeof(*job));
        job->references = 1;
        if (epd_compute_buffer_alloc(&job->buffer, sizeof(epd_reader_batch_t)))
        { *status = EPD_COMPUTE_OK; return job; }
        epd_app_free(job);
    }
    key = rt_hw_interrupt_disable();
    --jobs;
    rt_hw_interrupt_enable(key);
    *status = EPD_COMPUTE_NO_MEMORY;
    return NULL;
}

epd_reader_batch_t *reader_compute_data(reader_compute_job_t *job)
{
    RT_ASSERT(!job->submitted || job->done);
    return job->buffer.data;
}

void reader_compute_submit_target(reader_compute_job_t *job, bool foreground)
{
    rt_base_t key = rt_hw_interrupt_disable();
    RT_ASSERT(!job->submitted || job->done);
    job->submitted = true;
    job->foreground = foreground;
    job->done = false;
    job->next = NULL;
    if (stopping)
    {
        job->result = EPD_COMPUTE_CANCELLED; job->done = true;
        rt_hw_interrupt_enable(key);
        return;
    }
    idle_requested = false;
    ++job->references;
    if (tail) tail->next = job; else queue = job;
    tail = job;
    if (owner) epd_service_wake(owner);
    rt_hw_interrupt_enable(key);
}

void reader_compute_submit(reader_compute_job_t *job)
{ reader_compute_submit_target(job, false); }

void reader_compute_idle(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    if (!idle_requested)
    {
        idle_requested = true;
        if (owner) epd_service_wake(owner);
    }
    rt_hw_interrupt_enable(key);
}

bool reader_compute_poll(reader_compute_job_t *job, int *status)
{
    rt_base_t key = rt_hw_interrupt_disable();
    bool done = job->done;
    if (done) *status = job->result;
    rt_hw_interrupt_enable(key);
    return done;
}

void reader_compute_cancel(reader_compute_job_t *job)
{
    if (!job) return;
    rt_base_t key = rt_hw_interrupt_disable();
    job->cancelled = true;
    if (owner) epd_service_wake(owner);
    rt_hw_interrupt_enable(key);
    reader_compute_release(job);
}

static void close_session(void)
{
    if (!session) return;
    int result = epd_compute_close(session);
    RT_ASSERT(result == EPD_COMPUTE_OK);
    session = NULL;
}

static int execute(reader_compute_job_t *job)
{
    if (cancelled(job)) return EPD_COMPUTE_CANCELLED;
    epd_reader_batch_t *batch = job->buffer.data;
    epd_compute_purpose_t purpose = job->foreground ? EPD_COMPUTE_FOREGROUND : EPD_COMPUTE_BACKGROUND;
    if (session && session_purpose != purpose) close_session();
    if (!session)
    {
        epd_compute_capability_t capability;
        if (epd_compute_query(EPD_READER_CAPABILITY, EPD_READER_VERSION, &capability) == EPD_COMPUTE_OK &&
            capability.ready && capability.registration.step_limit)
        {
            epd_compute_requirements_t requirements = {purpose, 1};
            epd_compute_request_t request = {0};
            request.params.region = request.result.region = EPD_COMPUTE_REGION_NONE;
            request.cancelled = cancelled;
            request.context = job;
            int status = epd_compute_open(&capability, &requirements, &request, &session);
            if (status == EPD_COMPUTE_OK)
            { session_limit = capability.registration.step_limit; session_purpose = purpose; }
        }
    }
    if (cancelled(job)) return EPD_COMPUTE_CANCELLED;
    if (!session) return epd_reader_layout(batch) ? EPD_COMPUTE_COMPONENT_ERROR : EPD_COMPUTE_OK;
    if (batch->limit > session_limit) batch->limit = session_limit;
    epd_compute_region_t region = epd_compute_buffer_region(&job->buffer, sizeof(*batch),
                                                          EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    epd_compute_request_t request = {&region, 1, {0, 0, sizeof(*batch)}, {0, 0, sizeof(*batch)}, cancelled, job};
    int status = epd_compute_step(session, &request);
    if (status == EPD_COMPUTE_BUSY || status == EPD_COMPUTE_UNAVAILABLE)
    {
        close_session();
        return epd_reader_layout(batch) ? EPD_COMPUTE_COMPONENT_ERROR : EPD_COMPUTE_OK;
    }
    if (cancelled(job)) status = EPD_COMPUTE_CANCELLED;
    if (status != EPD_COMPUTE_OK) close_session();
    return status;
}

bool reader_compute_worker_step(epd_service_t *service)
{
    rt_base_t key = rt_hw_interrupt_disable();
    owner = service;
    stopping = false;
    reader_compute_job_t *job = queue;
    bool idle = idle_requested || !jobs;
    rt_hw_interrupt_enable(key);
    if (!job)
    {
        /* Only this worker opens/closes sessions or executes STEP. A submit
         * after this snapshot remains queued and can reopen after CLOSE. */
        if (idle && session) { close_session(); return true; }
        return false;
    }
    int result = execute(job);
    key = rt_hw_interrupt_disable();
    queue = job->next;
    if (!queue) tail = NULL;
    job->result = result; job->done = true;
    rt_hw_interrupt_enable(key);
    reader_compute_release(job);
    if (service) epd_service_ui_request(service, 0);
    return true;
}

void reader_compute_shutdown(void)
{
    /* Called by the same service after its last STEP has returned. */
    rt_base_t key = rt_hw_interrupt_disable();
    stopping = true;
    owner = NULL;
    rt_hw_interrupt_enable(key);
    close_session();
    for (;;)
    {
        key = rt_hw_interrupt_disable();
        reader_compute_job_t *job = queue;
        if (job)
        {
            queue = job->next;
            job->result = EPD_COMPUTE_CANCELLED; job->done = true;
        }
        if (!queue) tail = NULL;
        rt_hw_interrupt_enable(key);
        if (!job) break;
        reader_compute_release(job);
    }
}
