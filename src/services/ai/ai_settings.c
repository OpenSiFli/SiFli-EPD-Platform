/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service_internal.h"
#include "storage_internal.h"
#include "ai_settings.h"
#include "app_service.h"
#include "storage.h"
#include "system_reset_guard.h"
#include "board_service.h"
#include <string.h>

typedef struct {
    ai_settings_job_t job;
    uint32_t id;
    bool cancelled;
} queued_job_t;

typedef struct {
    struct rt_mutex mutex;
    epd_service_t *service;
    bool stopping, quiescing, worker_done, initialized, save_failed;
    epd_ai_error_t initialization_error;
    uint32_t next_id, completed_id;
    size_t head, queued, history_next;
    queued_job_t queue[AI_SETTINGS_QUEUE_CAPACITY], active;
    ai_settings_result_t history[AI_SETTINGS_RESULT_CAPACITY], last;
    epd_ai_cancel_t cancel;
    uint32_t sync_id;
    bool sync_done;
    epd_ai_error_t sync_error;
    bool initializing, candidate_discarded, draft_ready, draft_dirty, draft_writing;
    ai_config_t *candidate;
    uint32_t candidate_id, candidate_session;
    ai_settings_draft_t draft, draft_io;
    rt_tick_t draft_due;
    epd_ai_error_t draft_error;
} settings_state_t;

static settings_state_t *state;

static bool state_lock(settings_state_t *s, bool wait)
{ return rt_mutex_take(&s->mutex, wait ? RT_WAITING_FOREVER : 0) == RT_EOK; }
static void state_unlock(settings_state_t *s) { rt_mutex_release(&s->mutex); }

static bool terminated(const char *text, size_t size)
{
    for (size_t i = 0; i < size; ++i) if (!text[i]) return true;
    return false;
}

static bool mutates(ai_settings_operation_t operation)
{ return operation != AI_SETTINGS_TEST && operation != AI_SETTINGS_EXPORT &&
         operation != AI_SETTINGS_RELOAD && operation != AI_SETTINGS_IMPORT_PREPARE; }

static void clear_candidate(settings_state_t *s)
{
    state_lock(s, true);
    ai_config_t *candidate = s->candidate;
    s->candidate = NULL; s->candidate_id = 0; s->candidate_discarded = false;
    state_unlock(s);
    if (candidate) { memset(candidate, 0, sizeof(*candidate)); ai_free(candidate); }
}

static void draft_clear_locked(settings_state_t *s)
{
    uint32_t revision = s->draft.revision == UINT32_MAX ? UINT32_MAX : s->draft.revision + 1;
    memset(&s->draft, 0, sizeof(s->draft)); s->draft.revision = revision;
    s->draft_dirty = true; s->draft_error = EPD_AI_OK; s->draft_due = rt_tick_get();
}

static void save_draft(settings_state_t *s)
{
    state_lock(s, true);
    s->draft_io = s->draft; s->draft_writing = true;
    state_unlock(s);
    epd_ai_error_t error = ai_settings_draft_store(&s->draft_io);
    state_lock(s, true);
    if (s->draft_io.revision == s->draft.revision)
    {
        s->draft_error = error;
        if (error == EPD_AI_OK) s->draft_dirty = false;
    }
    s->draft_writing = false;
    memset(&s->draft_io, 0, sizeof(s->draft_io));
    state_unlock(s);
}

static void cancel_tests_locked(settings_state_t *s, uint32_t id)
{
    if (s->active.id && s->active.job.operation == AI_SETTINGS_TEST && (!id || s->active.id == id))
    {
        s->active.cancelled = true;
        epd_ai_cancel(&s->cancel);
    }
    for (size_t i = 0; i < s->queued; ++i)
    {
        queued_job_t *q = &s->queue[(s->head + i) % AI_SETTINGS_QUEUE_CAPACITY];
        if (q->job.operation == AI_SETTINGS_TEST && (!id || q->id == id)) q->cancelled = true;
    }
}

static int find_profile(const ai_config_t *c, const char *id)
{
    for (size_t i = 0; i < c->profile_count; ++i)
        if (!strcmp(c->profiles[i].public.id, id)) return (int)i;
    return -1;
}

static epd_ai_error_t worker_snapshot(ai_config_t **out)
{
    epd_ai_error_t error;
    do
    {
        error = ai_config_snapshot(out);
        if (error == EPD_AI_BUSY) rt_thread_mdelay(1);
    } while (error == EPD_AI_BUSY);
    return error;
}

static epd_ai_error_t change_config(const ai_settings_job_t *job)
{
    if (job->operation == AI_SETTINGS_EXPORT) return ai_config_export(job->path, job->card_session);
    if (job->operation == AI_SETTINGS_REVOKE && !*job->profile_id)
        return ai_config_revoke_app(job->app_id);
    ai_config_t *next = NULL;
    epd_ai_error_t error = worker_snapshot(&next);
    if (error != EPD_AI_OK) return error;
    int index = find_profile(next, job->profile_id);
    switch (job->operation)
    {
    case AI_SETTINGS_SAVE_PROFILE:
        index = find_profile(next, job->profile.public.id);
        if (index < 0)
        {
            if (next->profile_count == EPD_AI_MAX_PROFILES) { error = EPD_AI_LIMIT; break; }
            if (job->keep_secret) { error = EPD_AI_NOT_FOUND; break; }
            index = (int)next->profile_count++;
        }
        if (job->keep_secret)
        {
            /* Copy around the credential so it never has a second stack copy. */
            memcpy(&next->profiles[index], &job->profile, offsetof(ai_profile_t, secret));
            size_t after = offsetof(ai_profile_t, secret) + sizeof(job->profile.secret);
            memcpy((char *)&next->profiles[index] + after, (const char *)&job->profile + after,
                   sizeof(job->profile) - after);
        }
        else next->profiles[index] = job->profile;
        if (job->profile.public.is_default) strcpy(next->default_id, job->profile.public.id);
        else if (!strcmp(next->default_id, job->profile.public.id)) next->default_id[0] = 0;
        break;
    case AI_SETTINGS_REMOVE:
        if (index < 0) { error = EPD_AI_NOT_FOUND; break; }
        if (!strcmp(next->default_id, job->profile_id)) next->default_id[0] = 0;
        memmove(&next->profiles[index], &next->profiles[index + 1],
                (next->profile_count - (size_t)index - 1) * sizeof(next->profiles[0]));
        --next->profile_count;
        for (size_t i = 0; i < next->grant_count; )
        {
            if (strcmp(next->grants[i].profile, job->profile_id)) { ++i; continue; }
            memmove(&next->grants[i], &next->grants[i + 1],
                    (--next->grant_count - i) * sizeof(next->grants[0]));
        }
        break;
    case AI_SETTINGS_DEFAULT:
        if (*job->profile_id && index < 0) { error = EPD_AI_NOT_FOUND; break; }
        strcpy(next->default_id, job->profile_id);
        break;
    case AI_SETTINGS_GRANT:
        if (index < 0) { error = EPD_AI_NOT_FOUND; break; }
        if (!next->profiles[index].public.enabled) { error = EPD_AI_UNAUTHORIZED; break; }
        {
            bool exists = false;
            for (size_t i = 0; i < next->grant_count; ++i)
                if (!strcmp(next->grants[i].app, job->app_id) &&
                    !strcmp(next->grants[i].profile, job->profile_id)) exists = true;
            if (exists) { ai_free(next); return EPD_AI_OK; }
            if (next->grant_count == AI_MAX_GRANTS) { error = EPD_AI_LIMIT; break; }
            ai_grant_t *g = &next->grants[next->grant_count++];
            strcpy(g->app, job->app_id); strcpy(g->profile, job->profile_id);
        }
        break;
    case AI_SETTINGS_REVOKE:
        for (size_t i = 0; i < next->grant_count; )
        {
            if (strcmp(next->grants[i].app, job->app_id) || strcmp(next->grants[i].profile, job->profile_id))
            { ++i; continue; }
            memmove(&next->grants[i], &next->grants[i + 1],
                    (--next->grant_count - i) * sizeof(next->grants[0]));
        }
        break;
    default: error = EPD_AI_INVALID; break;
    }
    if (error == EPD_AI_OK) error = ai_config_commit(next);
    ai_free(next);
    return error;
}

static epd_ai_error_t run_test(settings_state_t *s, const ai_settings_job_t *job,
                              ai_settings_result_t *completion)
{
    if (ai_cancelled(&s->cancel)) return EPD_AI_CANCELLED;
    /* Explicit testing authorizes the fixed resident owner through the same
     * durable grant table read by ordinary epd_ai_perform admission. */
    ai_config_t *next = NULL;
    epd_ai_error_t error = worker_snapshot(&next);
    if (error != EPD_AI_OK) return error;
    int index = find_profile(next, job->profile_id);
    const epd_ai_model_t *model = NULL;
    if (index >= 0)
        for (size_t i = 0; i < next->profiles[index].public.model_count; ++i)
            if (!strcmp(next->profiles[index].public.models[i].id, job->model_id))
                model = &next->profiles[index].public.models[i];
    if (!model || !next->profiles[index].public.enabled)
    { ai_free(next); return model ? EPD_AI_UNAUTHORIZED : EPD_AI_NOT_FOUND; }
    uint32_t tokens = model->max_output_tokens < 32 ? model->max_output_tokens : 32;
    bool stream = (model->capabilities & EPD_AI_CAP_STREAM) != 0;
    bool granted = false;
    for (size_t i = 0; i < next->grant_count; ++i)
        if (!strcmp(next->grants[i].app, AI_SETTINGS_OWNER) &&
            !strcmp(next->grants[i].profile, job->profile_id)) granted = true;
    if (!granted)
    {
        if (next->grant_count == AI_MAX_GRANTS) error = EPD_AI_LIMIT;
        else
        {
            ai_grant_t *g = &next->grants[next->grant_count++];
            strcpy(g->app, AI_SETTINGS_OWNER); strcpy(g->profile, job->profile_id);
            error = ai_config_commit(next);
        }
    }
    ai_free(next);
    if (error != EPD_AI_OK || ai_cancelled(&s->cancel))
        return error != EPD_AI_OK ? error : EPD_AI_CANCELLED;
    epd_ai_message_t message = {0};
    message.kind = EPD_AI_USER; message.text = "Reply OK."; message.text_size = 9;
    epd_ai_request_t request = {0};
    request.struct_size = sizeof(request); request.version = EPD_AI_VERSION;
    request.profile_id = job->profile_id; request.model = job->model_id;
    request.messages = &message; request.message_count = 1;
    request.max_output_tokens = tokens; request.stream = stream;
    epd_ai_result_t result = {0};
    result.struct_size = sizeof(result); result.version = EPD_AI_VERSION;
    error = epd_ai_perform(s->service, &request, &s->cancel, NULL, NULL, &result);
    completion->http_status = result.http_status; completion->finish = result.finish;
    completion->usage = result.usage; completion->complete = result.complete;
    memcpy(completion->error_code, result.error_code, sizeof(completion->error_code));
    memcpy(completion->stage, result.stage, sizeof(completion->stage));
    completion->error_code[sizeof(completion->error_code) - 1] = 0;
    completion->stage[sizeof(completion->stage) - 1] = 0;
    epd_ai_result_clear(&result);
    return error;
}

static epd_ai_error_t initialize(settings_state_t *s)
{
    epd_ai_error_t error = ai_config_init();
    state_lock(s, true);
    s->initialized = error == EPD_AI_OK;
    s->initialization_error = error;
    state_unlock(s);
    return error;
}

static epd_ai_error_t load_draft(settings_state_t *s)
{
    state_lock(s, true);
    bool needed = !s->draft_ready;
    state_unlock(s);
    if (!needed) return EPD_AI_OK;
    epd_ai_error_t error = ai_settings_draft_load(&s->draft_io);
    state_lock(s, true);
    if (error == EPD_AI_OK) { s->draft = s->draft_io; s->draft_ready = true; }
    memset(&s->draft_io, 0, sizeof(s->draft_io));
    s->draft_error = error;
    state_unlock(s);
    return error;
}

static void settings_run(epd_service_t *service)
{
    settings_state_t *s = state;
    state_lock(s, true);
    s->service = service;
    state_unlock(s);
    epd_service_sleep_acquire(service);
    initialize(s);
    load_draft(s);
    state_lock(s, true); s->initializing = false; state_unlock(s);
    for (;;)
    {
        state_lock(s, true);
        if (epd_service_cancelled(service))
        { s->stopping = true; cancel_tests_locked(s, 0); }
        if (s->candidate_discarded || (s->stopping && s->candidate))
        { state_unlock(s); clear_candidate(s); continue; }
        if (!s->queued && s->draft_dirty && s->draft_error == EPD_AI_OK)
        {
            rt_int32_t delay = (rt_int32_t)(s->draft_due - rt_tick_get());
            bool now = s->stopping || s->quiescing || delay <= 0;
            state_unlock(s);
            if (now) save_draft(s);
            else epd_service_wait(service, delay);
            continue;
        }
        if (!s->queued)
        {
            bool stop = s->stopping;
            if (stop) s->worker_done = true;
            epd_service_sleep_release(service);
            state_unlock(s);
            if (stop) break;
            epd_service_wait(service, RT_WAITING_FOREVER);
            continue;
        }
        s->active = s->queue[s->head];
        memset(&s->queue[s->head], 0, sizeof(s->queue[s->head]));
        s->head = (s->head + 1) % AI_SETTINGS_QUEUE_CAPACITY;
        --s->queued;
        epd_ai_cancel_init(&s->cancel);
        if (s->active.cancelled || s->stopping || s->quiescing) epd_ai_cancel(&s->cancel);
        bool ready = s->initialized;
        state_unlock(s);

        ai_settings_result_t completion = {0};
        completion.job_id = s->active.id;
        completion.operation = s->active.job.operation;
        ai_settings_operation_t operation = completion.operation;
        bool import_job = operation == AI_SETTINGS_IMPORT_PREPARE || operation == AI_SETTINGS_IMPORT;
        bool attempted_mutation = false;
        completion.error = ready || import_job || operation == AI_SETTINGS_RELOAD ? EPD_AI_OK : initialize(s);
        if (completion.error == EPD_AI_OK)
        {
            if (operation == AI_SETTINGS_IMPORT_PREPARE)
            {
                clear_candidate(s);
                ai_config_t *candidate = NULL;
                completion.error = ai_config_import_prepare(s->active.job.path,
                                      s->active.job.card_session, &candidate);
                state_lock(s, true);
                if (s->active.cancelled || s->stopping || s->quiescing)
                    completion.error = EPD_AI_CANCELLED;
                if (completion.error == EPD_AI_OK)
                {
                    s->candidate = candidate; candidate = NULL;
                    s->candidate_id = completion.candidate_id = s->active.id;
                    s->candidate_session = s->active.job.card_session;
                }
                state_unlock(s);
                if (candidate) { memset(candidate, 0, sizeof(*candidate)); ai_free(candidate); }
            }
            else if (operation == AI_SETTINGS_IMPORT)
            {
                state_lock(s, true);
                bool valid = s->candidate && !s->candidate_discarded && s->active.job.candidate_id &&
                    s->candidate_id == s->active.job.candidate_id &&
                    s->candidate_session == s->active.job.card_session;
                state_unlock(s);
                attempted_mutation = valid;
                completion.error = valid ? ai_config_import_commit(s->candidate, s->candidate_session) : EPD_AI_CANCELLED;
                clear_candidate(s);
                if (completion.error == EPD_AI_OK) initialize(s);
            }
            else if (operation == AI_SETTINGS_TEST)
                completion.error = run_test(s, &s->active.job, &completion);
            else if (completion.operation == AI_SETTINGS_RELOAD)
            {
                completion.error = initialize(s);
                epd_ai_error_t draft_error = load_draft(s);
                if (completion.error == EPD_AI_OK) completion.error = draft_error;
            }
            else { attempted_mutation = mutates(operation); completion.error = change_config(&s->active.job); }
        }
        state_lock(s, true);
        s->last = completion;
        s->history[s->history_next++ % AI_SETTINGS_RESULT_CAPACITY] = completion;
        s->completed_id = completion.job_id;
        /* Rejected input leaves the previous durable-save outcome unchanged. */
        if (attempted_mutation &&
            (completion.error == EPD_AI_OK || completion.error == EPD_AI_STORAGE))
            s->save_failed = completion.error == EPD_AI_STORAGE;
        if (operation == AI_SETTINGS_RELOAD && s->draft_dirty) s->draft_error = EPD_AI_OK;
        if (completion.error == EPD_AI_OK && s->active.job.draft_revision &&
            s->active.job.draft_revision == s->draft.revision &&
            (operation == AI_SETTINGS_SAVE_PROFILE || operation == AI_SETTINGS_REMOVE))
            draft_clear_locked(s);
        if (s->sync_id == completion.job_id)
        { s->sync_error = completion.error; s->sync_done = true; }
        memset(&s->active, 0, sizeof(s->active));
        state_unlock(s);
    }
}

static bool settings_ui(epd_service_t *service, bool stopping)
{
    settings_state_t *s = state;
    if (!stopping) return false;
    if (!s) return true;
    state_lock(s, true);
    bool done = s->worker_done && !s->queued && !s->active.id;
    if (done && s->service == service) s->service = NULL;
    state_unlock(s);
    return done;
}

static const epd_background_t definition = {
    .run = settings_run, .stack_size = 8192, .priority = 25,
    .network = true, .ui_process = settings_ui, .flush = ai_settings_flush
};

epd_ai_error_t ai_settings_start(void)
{
    settings_state_t *s = state;
    if (!s)
    {
        s = ai_calloc(1, sizeof(*s));
        if (!s) return EPD_AI_NO_MEMORY;
        if (rt_mutex_init(&s->mutex, "ai_set", RT_IPC_FLAG_PRIO) != RT_EOK)
        { ai_free(s); return EPD_AI_NO_MEMORY; }
        s->initialization_error = EPD_AI_STORAGE;
        state = s;
    }
    if (!state_lock(s, false)) return EPD_AI_BUSY;
    if (s->service)
    {
        epd_ai_error_t error = s->stopping ? EPD_AI_BUSY : EPD_AI_OK;
        state_unlock(s); return error;
    }
    if (s->sync_id) { state_unlock(s); return EPD_AI_BUSY; }
    s->stopping = s->quiescing = s->worker_done = false;
    s->initializing = true;
    state_unlock(s);
    epd_service_t *service = app_service_start(AI_SETTINGS_OWNER, &definition, NULL);
    if (!service) { state_lock(s, true); s->initializing = false; state_unlock(s); return EPD_AI_BUSY; }
    state_lock(s, true); s->service = service; state_unlock(s);
    return EPD_AI_OK;
}

void ai_settings_process(void)
{
    if (!storage_changing() && !storage_release_requested() && !board_power_pending() &&
        !system_reset_blocks_apps() && !app_service_stopping()) (void)ai_settings_start();
}

epd_ai_error_t ai_settings_snapshot(ai_settings_snapshot_t *out)
{
    if (!out) return EPD_AI_INVALID;
    memset(out, 0, sizeof(*out));
    settings_state_t *s = state;
    if (!s) { out->initialization_error = EPD_AI_STORAGE; return EPD_AI_STORAGE; }
    if (!state_lock(s, false)) return EPD_AI_BUSY;
    out->initialized = s->initialized; out->initialization_error = s->initialization_error;
    out->busy = s->active.id || s->queued || !s->initialized;
    out->stopping = s->stopping;
    out->active_job_id = s->active.id; out->completed_job_id = s->completed_id;
    out->queued = s->queued; out->last_result = s->last;
    out->draft_ready = s->draft_ready; out->draft_available = s->draft.present != 0;
    out->draft_error = s->draft_error;
    state_unlock(s);
    if (!out->initialized) return EPD_AI_OK;
    ai_config_t *copy = NULL;
    epd_ai_error_t error = ai_config_snapshot(&copy);
    if (error != EPD_AI_OK) return error;
    out->generation = copy->generation;
    out->profile_count = copy->profile_count; out->grant_count = copy->grant_count;
    memcpy(out->default_id, copy->default_id, sizeof(out->default_id));
    memcpy(out->grants, copy->grants, copy->grant_count * sizeof(out->grants[0]));
    for (size_t i = 0; i < copy->profile_count; ++i) out->profiles[i] = copy->profiles[i].public;
    ai_free(copy);
    return EPD_AI_OK;
}

epd_ai_error_t ai_settings_profile_get(const char *id, ai_profile_t *out, bool *has_secret)
{
    if (!id || !out || !has_secret) return EPD_AI_INVALID;
    memset(out, 0, sizeof(*out)); *has_secret = false;
    ai_config_t *copy = NULL;
    epd_ai_error_t error = ai_config_snapshot(&copy);
    if (error != EPD_AI_OK) return error;
    int index = find_profile(copy, id);
    if (index >= 0)
    {
        *out = copy->profiles[index]; *has_secret = *out->secret != 0;
        memset(out->secret, 0, sizeof(out->secret));
    }
    else error = EPD_AI_NOT_FOUND;
    ai_free(copy);
    return error;
}

static epd_ai_error_t submit(const ai_settings_job_t *job, uint32_t *job_id, bool sync)
{
    if (job_id) *job_id = 0;
    if (!job || !job_id || (unsigned)job->operation > AI_SETTINGS_IMPORT_PREPARE ||
        !terminated(job->profile_id, sizeof(job->profile_id)) ||
        !terminated(job->model_id, sizeof(job->model_id)) ||
        !terminated(job->app_id, sizeof(job->app_id)) || !terminated(job->path, sizeof(job->path)) ||
        (job->operation == AI_SETTINGS_SAVE_PROFILE &&
         !terminated(job->profile.public.id, sizeof(job->profile.public.id)))) return EPD_AI_INVALID;
    settings_state_t *s = state;
    if (!s || !state_lock(s, sync)) return EPD_AI_BUSY;
    if (!s->service || s->stopping || s->quiescing || s->queued == AI_SETTINGS_QUEUE_CAPACITY ||
        (sync && s->sync_id) || s->next_id == UINT32_MAX)
    { state_unlock(s); return EPD_AI_BUSY; }
    if (job->operation == AI_SETTINGS_TEST)
    {
        bool test = s->active.id && s->active.job.operation == AI_SETTINGS_TEST;
        for (size_t i = 0; i < s->queued; ++i)
            test |= s->queue[(s->head + i) % AI_SETTINGS_QUEUE_CAPACITY].job.operation == AI_SETTINGS_TEST;
        if (test) { state_unlock(s); return EPD_AI_BUSY; }
    }
    if (!epd_service_sleep_acquire(s->service)) { state_unlock(s); return EPD_AI_BUSY; }
    queued_job_t *queued = &s->queue[(s->head + s->queued) % AI_SETTINGS_QUEUE_CAPACITY];
    queued->job = *job; queued->id = ++s->next_id; queued->cancelled = false;
    ++s->queued;
    *job_id = queued->id;
    if (sync) { s->sync_id = queued->id; s->sync_done = false; }
    if (mutates(job->operation)) cancel_tests_locked(s, 0);
    epd_service_wake(s->service);
    state_unlock(s);
    return EPD_AI_OK;
}

epd_ai_error_t ai_settings_submit(const ai_settings_job_t *job, uint32_t *job_id)
{ return submit(job, job_id, false); }

epd_ai_error_t ai_settings_poll(uint32_t job_id, ai_settings_result_t *out)
{
    if (!job_id || !out) return EPD_AI_INVALID;
    settings_state_t *s = state;
    if (!s) return EPD_AI_NOT_FOUND;
    if (!state_lock(s, false)) return EPD_AI_BUSY;
    epd_ai_error_t error = EPD_AI_NOT_FOUND;
    for (size_t i = 0; i < AI_SETTINGS_RESULT_CAPACITY; ++i)
        if (s->history[i].job_id == job_id) { *out = s->history[i]; error = EPD_AI_OK; break; }
    if (s->active.id == job_id) error = EPD_AI_BUSY;
    for (size_t i = 0; i < s->queued; ++i)
        if (s->queue[(s->head + i) % AI_SETTINGS_QUEUE_CAPACITY].id == job_id) error = EPD_AI_BUSY;
    state_unlock(s);
    return error;
}

void ai_settings_import_discard(uint32_t id)
{
    settings_state_t *s = state;
    if (!s || !id) return;
    state_lock(s, true);
    if (s->candidate_id == id) s->candidate_discarded = true;
    if (s->active.id == id && s->active.job.operation == AI_SETTINGS_IMPORT_PREPARE) s->active.cancelled = true;
    for (size_t i = 0; i < s->queued; ++i)
    {
        queued_job_t *job = &s->queue[(s->head + i) % AI_SETTINGS_QUEUE_CAPACITY];
        if (job->id == id && job->job.operation == AI_SETTINGS_IMPORT_PREPARE) job->cancelled = true;
    }
    if (s->service) epd_service_wake(s->service);
    state_unlock(s);
}

epd_ai_error_t ai_settings_draft_set(const ai_settings_draft_t *draft, uint32_t *revision)
{
    if (!revision || !ai_settings_draft_valid(draft) || !draft->present) return EPD_AI_INVALID;
    settings_state_t *s = state;
    if (!s) return EPD_AI_BUSY;
    state_lock(s, true);
    /* revision is assigned by the service, not by a reopened page. */
    size_t offset = offsetof(ai_settings_draft_t, present);
    bool same = !memcmp((const char *)&s->draft + offset, (const char *)draft + offset, sizeof(*draft) - offset);
    if (same && s->draft_error == EPD_AI_OK)
    { *revision = s->draft.revision; state_unlock(s); return EPD_AI_OK; }
    if (s->draft.revision == UINT32_MAX) { state_unlock(s); return EPD_AI_LIMIT; }
    if (!s->draft_ready || !s->service || s->stopping || s->quiescing ||
        !epd_service_sleep_acquire(s->service))
    { state_unlock(s); return EPD_AI_BUSY; }
    uint32_t next = s->draft.revision + 1;
    s->draft = *draft; s->draft.revision = *revision = next;
    s->draft_dirty = true; s->draft_error = EPD_AI_OK;
    s->draft_due = rt_tick_get() + rt_tick_from_millisecond(500);
    epd_service_wake(s->service);
    state_unlock(s);
    return EPD_AI_OK;
}

epd_ai_error_t ai_settings_draft_get(ai_settings_draft_t *draft)
{
    if (!draft) return EPD_AI_INVALID;
    settings_state_t *s = state;
    if (!s || !state_lock(s, false)) return EPD_AI_BUSY;
    epd_ai_error_t error = !s->draft_ready ? (s->draft_error ? s->draft_error : EPD_AI_BUSY) :
                          s->draft.present ? EPD_AI_OK : EPD_AI_NOT_FOUND;
    if (error == EPD_AI_OK) *draft = s->draft;
    state_unlock(s);
    return error;
}

epd_ai_error_t ai_settings_draft_discard(uint32_t revision)
{
    settings_state_t *s = state;
    if (!s) return EPD_AI_BUSY;
    state_lock(s, true);
    if (!s->draft_ready)
    { epd_ai_error_t error = s->draft_error ? s->draft_error : EPD_AI_BUSY; state_unlock(s); return error; }
    if (!s->draft.present) { state_unlock(s); return EPD_AI_OK; }
    if (revision == UINT32_MAX) { state_unlock(s); return EPD_AI_LIMIT; }
    if (!s->service || s->stopping || s->quiescing || !revision || revision != s->draft.revision ||
        !epd_service_sleep_acquire(s->service))
    { state_unlock(s); return EPD_AI_BUSY; }
    draft_clear_locked(s); epd_service_wake(s->service);
    state_unlock(s);
    return EPD_AI_OK;
}

void ai_settings_cancel(uint32_t job_id)
{
    settings_state_t *s = state;
    if (!s) return;
    state_lock(s, true);
    cancel_tests_locked(s, job_id);
    if (s->service) epd_service_wake(s->service);
    state_unlock(s);
}

bool ai_settings_flush(void)
{
    settings_state_t *s = state;
    if (!s) return true;
    if (!state_lock(s, false)) return false;
    bool pending = s->initializing || s->active.id || s->queued || s->draft_writing ||
                   (s->draft_dirty && s->draft_error == EPD_AI_OK);
    bool wake = pending && !s->quiescing;
    s->quiescing = pending;
    cancel_tests_locked(s, 0);
    bool done = !s->initializing && !s->active.id && !s->queued && !s->save_failed &&
                !s->draft_dirty && !s->draft_writing;
    /* Wake once for new work. Polling must let the GUI sleep so lower-priority
     * save and storage-handoff workers can complete. */
    if (wake && s->service) epd_service_wake(s->service);
    state_unlock(s);
    return done;
}

void ai_settings_stop(void)
{
    settings_state_t *s = state;
    if (!s) return;
    state_lock(s, true);
    s->stopping = true;
    cancel_tests_locked(s, 0);
    bool running = s->service != NULL;
    state_unlock(s);
    if (running) app_service_stop(AI_SETTINGS_OWNER);
}

bool ai_settings_idle(void)
{
    settings_state_t *s = state;
    if (!s) return true;
    if (!state_lock(s, false)) return false;
    bool idle = !s->active.id && !s->queued && (!s->stopping || !s->service);
    state_unlock(s);
    return idle;
}

epd_ai_error_t ai_settings_revoke_app_sync(const char *app)
{
    if (!app || !*app || !terminated(app, 32)) return EPD_AI_INVALID;
    settings_state_t *s = state;
    if (!s) return EPD_AI_STORAGE;
    /* Waiting from our own worker would prevent the queued commit running. */
    state_lock(s, true);
    bool own_worker = s->service && epd_service_is_worker(s->service);
    state_unlock(s);
    if (own_worker) return EPD_AI_INVALID;
    ai_settings_job_t *job = ai_calloc(1, sizeof(*job));
    if (!job) return EPD_AI_NO_MEMORY;
    job->operation = AI_SETTINGS_REVOKE; strcpy(job->app_id, app);
    uint32_t id = 0;
    epd_ai_error_t error = submit(job, &id, true);
    ai_free(job);
    if (error != EPD_AI_OK) return error;
    for (;;)
    {
        state_lock(s, true);
        bool done = s->sync_done;
        if (done) { error = s->sync_error; s->sync_id = 0; s->sync_done = false; }
        state_unlock(s);
        if (done) return error;
        rt_thread_mdelay(5);
    }
}
