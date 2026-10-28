/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service_internal.h"
#include "storage_internal.h"
#include "speech_settings.h"
#include "app_service.h"
#include "storage.h"
#include "system_reset_guard.h"
#include "board_service.h"
#include <string.h>

typedef struct { speech_settings_job_t job; uint32_t id; bool discarded; } queued_t;
typedef struct {
    struct rt_mutex mutex;
    epd_service_t *service;
    bool initializing, initialized, stopping, quiescing, worker_done, save_failed, resetting;
    epd_speech_error_t initialization_error;
    uint32_t next_id, candidate_id, candidate_session;
    bool candidate_discarded;
    speech_config_t *candidate;
    size_t head, queued, history_next;
    queued_t queue[SPEECH_SETTINGS_QUEUE_SIZE], active;
    speech_settings_result_t history[SPEECH_SETTINGS_HISTORY_SIZE];
} settings_t;
static settings_t *state;

static bool lock(settings_t *s, bool wait)
{ return rt_mutex_take(&s->mutex, wait ? RT_WAITING_FOREVER : 0) == RT_EOK; }
static void unlock(settings_t *s) { rt_mutex_release(&s->mutex); }

static void discard_candidate(settings_t *s)
{
    lock(s, true);
    speech_config_t *candidate = s->candidate;
    s->candidate = NULL; s->candidate_id = s->candidate_session = 0; s->candidate_discarded = false;
    unlock(s);
    if (candidate) { speech_config_wipe(candidate, sizeof(*candidate)); speech_free(candidate); }
}

static epd_speech_error_t initialize(settings_t *s)
{
    epd_speech_error_t error = speech_config_load();
    lock(s, true);
    if (!error) s->initialized = true;
    s->initialization_error = error;
    unlock(s); return error;
}

static epd_speech_error_t copy_config(speech_config_t *out)
{
    epd_speech_error_t error;
    do {
        error = speech_config_snapshot(out);
        if (error == EPD_SPEECH_BUSY) rt_thread_mdelay(1);
    } while (error == EPD_SPEECH_BUSY);
    return error;
}

static void clear_default(speech_config_t *c, speech_provider_t provider)
{
    if (c->asr == provider) c->asr = SPEECH_NONE;
    if (c->tts == provider) c->tts = SPEECH_NONE;
}

static epd_speech_error_t change(const speech_settings_job_t *job)
{
    if (job->operation == SPEECH_SETTINGS_CLEAR) return speech_config_clear();
    speech_config_t *next = speech_alloc(sizeof(*next));
    if (!next) return EPD_SPEECH_NO_MEMORY;
    epd_speech_error_t error = copy_config(next);
    if (error) goto done;
    switch (job->operation) {
    case SPEECH_SETTINGS_SAVE_ACCOUNT: {
        speech_provider_t p = job->account.provider;
        speech_account_t *a = &next->accounts[p];
        if (job->keep_credentials) {
            if (a->provider != p) { error = EPD_SPEECH_NOT_CONFIGURED; break; }
            a->enabled = job->account.enabled; a->rate = job->account.rate;
            memcpy(a->voice, job->account.voice, sizeof(a->voice));
            if (job->account.app_id[0]) memcpy(a->app_id, job->account.app_id, sizeof(a->app_id));
            if (job->account.secret_id[0]) memcpy(a->secret_id, job->account.secret_id, sizeof(a->secret_id));
            if (job->account.secret[0]) memcpy(a->secret, job->account.secret, sizeof(a->secret));
            if (job->account.workspace[0]) memcpy(a->workspace, job->account.workspace, sizeof(a->workspace));
        } else *a = job->account;
        if (!a->enabled) clear_default(next, p);
        else if (!a->voice[0] && next->tts == p) next->tts = SPEECH_NONE;
        break;
    }
    case SPEECH_SETTINGS_REMOVE:
        speech_config_wipe(&next->accounts[job->provider], sizeof(speech_account_t));
        clear_default(next, job->provider); break;
    case SPEECH_SETTINGS_DEFAULT:
        if (job->kind == EPD_SPEECH_ASR) next->asr = job->provider;
        else next->tts = job->provider;
        break;
    default: error = EPD_SPEECH_INVALID; break;
    }
    if (!error) error = speech_config_commit(next, 0);
done:
    speech_config_wipe(next, sizeof(*next)); speech_free(next); return error;
}

static epd_speech_error_t prepare(settings_t *s, const queued_t *q, uint32_t *id)
{
    discard_candidate(s);
    speech_config_t *candidate = speech_alloc(sizeof(*candidate));
    if (!candidate) return EPD_SPEECH_NO_MEMORY;
    epd_speech_error_t error = speech_config_import_read(q->job.path, q->job.card_session, candidate);
    speech_settings_snapshot_t snapshot = {0};
    epd_speech_error_t current_error = speech_config_public(&snapshot);
    if (!error && current_error != EPD_SPEECH_OK && current_error != EPD_SPEECH_NOT_CONFIGURED) error = current_error;
    if (!error) candidate->revision = snapshot.revision;
    lock(s, true);
    if (s->active.discarded || s->stopping || s->quiescing) error = EPD_SPEECH_CANCELLED;
    if (!error) {
        s->candidate = candidate; candidate = NULL;
        s->candidate_id = *id = q->id; s->candidate_session = q->job.card_session;
    }
    unlock(s);
    if (candidate) { speech_config_wipe(candidate, sizeof(*candidate)); speech_free(candidate); }
    return error;
}

static void run(epd_service_t *service)
{
    settings_t *s = state;
    lock(s, true); s->service = service; bool ready = s->initialized; unlock(s);
    epd_service_sleep_acquire(service);
    if (!ready) initialize(s);
    lock(s, true); s->initializing = false; unlock(s);
    for (;;) {
        lock(s, true);
        if (epd_service_cancelled(service)) s->stopping = true;
        if (s->candidate_discarded || (s->stopping && !s->queued && s->candidate)) {
            unlock(s); discard_candidate(s); continue;
        }
        if (!s->queued) {
            bool stop = s->stopping;
            if (stop) s->worker_done = true;
            epd_service_sleep_release(service);
            unlock(s);
            if (stop) break;
            epd_service_wait(service, RT_WAITING_FOREVER); continue;
        }
        s->active = s->queue[s->head];
        speech_config_wipe(&s->queue[s->head], sizeof(queued_t));
        s->head = (s->head + 1) % SPEECH_SETTINGS_QUEUE_SIZE; --s->queued;
        unlock(s);
        speech_settings_result_t result = {.job_id=s->active.id, .operation=s->active.job.operation};
        speech_settings_operation_t op = result.operation;
        if (op == SPEECH_SETTINGS_RELOAD) result.error = initialize(s);
        else if (op == SPEECH_SETTINGS_IMPORT_PREPARE) result.error = prepare(s, &s->active, &result.candidate_id);
        else if (op == SPEECH_SETTINGS_IMPORT) {
            lock(s, true);
            bool valid = s->candidate && !s->candidate_discarded && s->active.job.candidate_id &&
                s->candidate_id == s->active.job.candidate_id && s->candidate_session == s->active.job.card_session;
            unlock(s);
            result.error = valid ? speech_config_commit(s->candidate, s->candidate_session) : EPD_SPEECH_CANCELLED;
            discard_candidate(s);
        } else result.error = change(&s->active.job);
        lock(s, true);
        if (op != SPEECH_SETTINGS_IMPORT_PREPARE && op != SPEECH_SETTINGS_RELOAD &&
            (result.error == EPD_SPEECH_OK || result.error == EPD_SPEECH_STORAGE))
            s->save_failed = result.error == EPD_SPEECH_STORAGE;
        if (!result.error && (op == SPEECH_SETTINGS_IMPORT || op == SPEECH_SETTINGS_CLEAR)) {
            s->initialized = true; s->initialization_error = EPD_SPEECH_OK;
        }
        s->history[s->history_next++ % SPEECH_SETTINGS_HISTORY_SIZE] = result;
        speech_config_wipe(&s->active, sizeof(s->active));
        unlock(s);
    }
}

static bool service_ui(epd_service_t *service, bool stopping)
{
    if (!stopping) return false;
    settings_t *s = state;
    if (!s) return true;
    if (!lock(s, false)) return false;
    bool done = s->worker_done && !s->queued && !s->active.id;
    if (done && s->service == service) s->service = NULL;
    unlock(s); return done;
}
static const epd_background_t definition = {
    .run=run, .stack_size=8192, .priority=25, .network=false,
    .ui_process=service_ui, .flush=speech_settings_flush
};

epd_speech_error_t speech_settings_start(void)
{
    if (storage_changing() || storage_release_requested() || board_power_pending() ||
        system_reset_blocks_apps() || app_service_stopping()) return EPD_SPEECH_BUSY;
    settings_t *s = state;
    if (!s) {
        s = speech_alloc(sizeof(*s));
        if (!s) return EPD_SPEECH_NO_MEMORY;
        memset(s, 0, sizeof(*s));
        if (rt_mutex_init(&s->mutex, "sp_set", RT_IPC_FLAG_PRIO) != RT_EOK) { speech_free(s); return EPD_SPEECH_NO_MEMORY; }
        s->initialization_error = EPD_SPEECH_BUSY; state = s;
    }
    if (!lock(s, false)) return EPD_SPEECH_BUSY;
    if (s->service || s->resetting) {
        epd_speech_error_t error = s->stopping || s->resetting ? EPD_SPEECH_BUSY : EPD_SPEECH_OK;
        unlock(s); return error;
    }
    s->stopping = s->quiescing = s->worker_done = false; s->initializing = true;
    unlock(s);
    epd_service_t *service = app_service_start(SPEECH_SETTINGS_OWNER, &definition, NULL);
    lock(s, true); s->service = service;
    if (!service) s->initializing = false;
    unlock(s); return service ? EPD_SPEECH_OK : EPD_SPEECH_BUSY;
}

void speech_settings_process(void) { (void)speech_settings_start(); }

epd_speech_error_t speech_settings_snapshot(speech_settings_snapshot_t *out)
{
    if (!out) return EPD_SPEECH_INVALID;
    memset(out, 0, sizeof(*out));
    settings_t *s = state;
    if (!s) { out->initialization_error = EPD_SPEECH_BUSY; return EPD_SPEECH_OK; }
    if (!lock(s, false)) return EPD_SPEECH_BUSY;
    out->initialization_error = s->initialization_error;
    out->busy = s->initializing || s->active.id || s->queued || s->resetting;
    out->stopping = s->stopping;
    bool initialized = s->initialized;
    unlock(s);
    return initialized ? speech_config_public(out) : EPD_SPEECH_OK;
}

epd_speech_error_t speech_settings_submit(const speech_settings_job_t *job, uint32_t *id)
{
    if (id) *id = 0;
    if (!job || !id || (unsigned)job->operation > SPEECH_SETTINGS_CLEAR ||
        !memchr(job->path, 0, sizeof(job->path))) return EPD_SPEECH_INVALID;
    if (job->operation == SPEECH_SETTINGS_SAVE_ACCOUNT &&
        (job->account.provider <= SPEECH_NONE || job->account.provider >= SPEECH_PROVIDER_COUNT)) return EPD_SPEECH_INVALID;
    if (job->operation == SPEECH_SETTINGS_REMOVE &&
        (job->provider <= SPEECH_NONE || job->provider >= SPEECH_PROVIDER_COUNT)) return EPD_SPEECH_INVALID;
    if (job->operation == SPEECH_SETTINGS_DEFAULT &&
        ((job->kind != EPD_SPEECH_ASR && job->kind != EPD_SPEECH_TTS) ||
         (job->provider != SPEECH_NONE && !speech_provider_supports(job->provider, job->kind)))) return EPD_SPEECH_UNSUPPORTED;
    settings_t *s = state;
    if (!s || !lock(s, false)) return EPD_SPEECH_BUSY;
    if (!s->service || s->stopping || s->quiescing || s->resetting || s->queued == SPEECH_SETTINGS_QUEUE_SIZE ||
        s->next_id == UINT32_MAX || !epd_service_sleep_acquire(s->service)) { unlock(s); return EPD_SPEECH_BUSY; }
    queued_t *q = &s->queue[(s->head + s->queued) % SPEECH_SETTINGS_QUEUE_SIZE];
    q->job = *job; q->id = *id = ++s->next_id; q->discarded = false;
    ++s->queued; epd_service_wake(s->service); unlock(s); return EPD_SPEECH_OK;
}

epd_speech_error_t speech_settings_poll(uint32_t id, speech_settings_result_t *out)
{
    if (!id || !out) return EPD_SPEECH_INVALID;
    memset(out, 0, sizeof(*out));
    settings_t *s = state;
    if (!s) return EPD_SPEECH_NOT_FOUND;
    if (!lock(s, false)) return EPD_SPEECH_BUSY;
    epd_speech_error_t error = EPD_SPEECH_NOT_FOUND;
    for (size_t i = 0; i < SPEECH_SETTINGS_HISTORY_SIZE; ++i)
        if (s->history[i].job_id == id) { *out = s->history[i]; error = EPD_SPEECH_OK; break; }
    if (s->active.id == id) error = EPD_SPEECH_BUSY;
    for (size_t i = 0; i < s->queued; ++i)
        if (s->queue[(s->head+i) % SPEECH_SETTINGS_QUEUE_SIZE].id == id) error = EPD_SPEECH_BUSY;
    unlock(s); return error;
}

void speech_settings_import_discard(uint32_t id)
{
    settings_t *s = state;
    if (!s || !id) return;
    lock(s, true);
    if (s->candidate_id == id) s->candidate_discarded = true;
    if (s->active.id == id && s->active.job.operation == SPEECH_SETTINGS_IMPORT_PREPARE) s->active.discarded = true;
    for (size_t i = 0; i < s->queued; ++i) {
        queued_t *q = &s->queue[(s->head+i) % SPEECH_SETTINGS_QUEUE_SIZE];
        if (q->id == id && q->job.operation == SPEECH_SETTINGS_IMPORT_PREPARE) q->discarded = true;
    }
    if (s->service) epd_service_wake(s->service);
    unlock(s);
}

bool speech_settings_flush(void)
{
    settings_t *s = state;
    if (!s) return true;
    if (!lock(s, false)) return false;
    bool pending = s->initializing || s->active.id || s->queued || s->resetting;
    bool wake = pending && !s->quiescing;
    s->quiescing = pending;
    bool done = !pending && !s->save_failed;
    if (wake && s->service) epd_service_wake(s->service);
    unlock(s); return done;
}

void speech_settings_stop(void)
{
    settings_t *s = state;
    if (!s) return;
    lock(s, true); s->stopping = true; bool running = s->service != NULL; unlock(s);
    if (running) app_service_stop(SPEECH_SETTINGS_OWNER);
}

bool speech_settings_idle(void)
{
    settings_t *s = state;
    if (!s) return true;
    if (!lock(s, false)) return false;
    bool idle = !s->initializing && !s->active.id && !s->queued && !s->resetting && (!s->stopping || !s->service);
    unlock(s); return idle;
}

epd_speech_error_t speech_settings_reset_storage(void)
{
    settings_t *s = state;
    if (s) {
        if (!lock(s, false)) return EPD_SPEECH_BUSY;
        if (s->service || s->initializing || s->active.id || s->queued || s->resetting) { unlock(s); return EPD_SPEECH_BUSY; }
        s->resetting = true; unlock(s);
        discard_candidate(s);
    }
    epd_speech_error_t error = speech_config_clear();
    if (s) {
        lock(s, true);
        if (!error) { s->initialized = true; s->initialization_error = EPD_SPEECH_OK; s->save_failed = false; }
        s->resetting = false; unlock(s);
    }
    return error;
}
