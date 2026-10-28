/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/epd_reader_session.h"
#include "reader_service.h"
#include "reader_text.h"
#include "reader_view_ui.h"
#include "app_service.h"
#include "platform/epd_memory.h"
#include "../../../modules/common/reader/compute/include/reader_compute_client.h"
#include <string.h>

/* Bounded integration configuration. The final mixed-load budget is recorded
 * with the device results before the common reader UI enables this service. */
#define READER_SOURCE_BLOCK (16u * 1024u)
#define READER_SOURCE_BUDGET (4u * READER_SOURCE_BLOCK)

typedef struct {
    reader_core_t core;
    reader_providers_t providers;
    reader_provider_slot_t provider_slots[1];
    struct rt_mutex mutex;
    epd_service_t *service;
} reader_service_state_t;
static reader_service_state_t *state;
/* Session and completion IDs must not repeat after the last reader closes. */
static uint64_t sequence;
static const reader_provider_registration_t registrations[] = {
    {"text/plain", &reader_text_provider, NULL}
};
static void *allocate(size_t bytes) { return epd_app_alloc(bytes, EPD_APP_PSRAM); }
static void enter(void *user)
{
    reader_service_state_t *s = user;
    rt_mutex_take(&s->mutex, RT_WAITING_FOREVER);
}
static void leave(void *user)
{
    reader_service_state_t *s = user;
    rt_mutex_release(&s->mutex);
}
static void wake_worker(void *user)
{
    reader_service_state_t *s = user;
    epd_service_wake(s->service);
}
static void wake_ui(void *user)
{
    reader_service_state_t *s = user;
    epd_service_ui_request(s->service, 0);
}
static void run(epd_service_t *service)
{
    reader_service_state_t *s = state;
    while (!epd_service_cancelled(service))
    {
        bool work = reader_compute_worker_step(service);
        work = reader_core_step(&s->core) || work;
        if (!work) epd_service_wait(service, RT_WAITING_FOREVER);
    }
    reader_compute_shutdown();
    reader_pipeline_close(&s->core.pipeline);
    /* This worker owns the parser. Its last step closes files; borrowed UI
     * batches are returned later, after this worker has joined. */
    reader_core_step(&s->core);
}
static bool process(epd_service_t *service, bool stopping)
{
    reader_service_state_t *s = state;
    if (!s) return stopping;
    if (stopping) reader_core_stop(&s->core);
    if (reader_view_ui_process(&s->core, stopping)) epd_service_ui_request(service, 1);
    reader_core_process(&s->core);
    if (!stopping || !reader_core_empty(&s->core))
    {
        /* Completion notifications advance normal work without periodic
         * maintenance wakeups. The service framework retries stopping UI. */
        (void)service;
        return false;
    }
    rt_mutex_detach(&s->mutex);
    sequence = s->core.sessions.sequence;
    state = NULL; epd_app_free(s);
    return true;
}
static bool flush(void)
{ return !state || reader_sessions_flush(&state->core.sessions); }
static const epd_background_t definition = {
    .run = run, .stack_size = 8192, .priority = 23, .ui_process = process, .flush = flush
};
bool reader_service_supports(const char *format, uint32_t version)
{
    if (!format) return false;
    for (size_t i = 0; i < sizeof(registrations) / sizeof(registrations[0]); ++i)
        if (!strcmp(registrations[i].format, format))
            return !version || version == registrations[i].builtin->content_version;
    return false;
}

reader_core_t *reader_service_core(void) { return state ? &state->core : NULL; }
void reader_service_stop(void)
{
    if (!state) return;
    reader_core_stop(&state->core);
    app_service_stop("reader");
}
void reader_service_notify(void)
{
    if (state) { epd_service_wake(state->service); epd_service_ui_request(state->service, 0); }
}
int reader_service_open(const reader_open_t *request, reader_session_id_t *id)
{
    if (id) *id = 0;
    if (!request || !id) return EPD_READ_INVALID;
    if (state && epd_service_cancelled(state->service)) return EPD_READ_BUSY;
    if (!state)
    {
        reader_service_state_t *s = allocate(sizeof(*s));
        if (!s) return EPD_READ_NO_MEMORY;
        memset(s, 0, sizeof(*s));
        if (rt_mutex_init(&s->mutex, "rdsrc", RT_IPC_FLAG_PRIO) != RT_EOK)
        { epd_app_free(s); return EPD_READ_NO_MEMORY; }
        book_allocator_t memory = {allocate, NULL, epd_app_free};
        reader_pipeline_sync_t sync = {s, enter, leave, wake_worker, wake_ui};
        int result = reader_providers_init(&s->providers, s->provider_slots, registrations,
                                           sizeof(registrations) / sizeof(registrations[0]), NULL);
        if (!result) result = reader_core_init(&s->core, &s->providers, &sync, &memory,
                                               READER_SOURCE_BUDGET, READER_SOURCE_BLOCK);
        if (result) { rt_mutex_detach(&s->mutex); epd_app_free(s); return result; }
        s->core.sessions.sequence = sequence;
        state = s;
        s->service = app_service_start("reader", &definition, NULL);
        if (!s->service)
        { state = NULL; rt_mutex_detach(&s->mutex); epd_app_free(s); return EPD_READ_NO_MEMORY; }
    }
    int result = reader_core_open(&state->core, request, id);
    if (result && reader_core_empty(&state->core)) app_service_stop("reader");
    reader_service_notify();
    return result;
}
