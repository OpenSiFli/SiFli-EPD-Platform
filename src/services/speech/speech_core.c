/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service_internal.h"
#include "storage_internal.h"
#include "speech_core.h"
#include "speech_text.h"
#include "speech_audio.h"
#include "platform/epd_memory.h"
#include "platform/epd_power.h"
#include "net/network.h"
#include "storage.h"
#include "system_reset_guard.h"
#include "boards/board_service.h"
#include <rtm.h>
#include <string.h>

#define DBG_TAG "speech"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define EVENT_BUDGET (48u * 1024u)
struct speech_event_node {
    struct speech_event_node *next;
    epd_speech_event_t event;
    size_t charge;
    char text[];
};
static struct rt_mutex mutex;
static bool initialized;
static bool accepting;
static uint32_t revocations[SPEECH_PROVIDER_COUNT];
static speech_session_t *sessions[2];
static epd_service_t *workers[2];
static const char *const worker_ids[2] = { "sp_asr", "sp_tts" };
static uint32_t next_session;
static size_t event_bytes;

void *speech_alloc(size_t size) { return epd_app_alloc(size, EPD_APP_PSRAM); }
void speech_free(void *p) { epd_app_free(p); }
uint32_t speech_now_ms(void) { return (uint32_t)((uint64_t)rt_tick_get() * 1000u / RT_TICK_PER_SECOND); }
void speech_lock(void) { rt_mutex_take(&mutex, RT_WAITING_FOREVER); }
void speech_unlock(void) { rt_mutex_release(&mutex); }
void speech_session_wake(speech_session_t *s)
{
    epd_service_wake(s->owner);
    epd_service_ui_request(s->owner, 0);
}
void speech_output_wake(speech_session_t *s)
{
    if (s->output_event_ready) rt_event_send(&s->output_wakeup, 1u);
}
static bool terminal(epd_speech_state_t state)
{ return state == EPD_SPEECH_COMPLETED || state == EPD_SPEECH_STOPPED || state == EPD_SPEECH_FAILED; }
static speech_session_t *find(epd_service_t *owner, epd_speech_session_t id)
{
    for (unsigned i = 0; i < 2; ++i)
        if (sessions[i] && sessions[i]->owner == owner && sessions[i]->view.session == id) return sessions[i];
    return NULL;
}

void speech_session_state(speech_session_t *s, epd_speech_state_t state)
{
    speech_lock();
    bool changed = false;
    if (s->view.state != state && !s->completed && !s->cancelled) {
        s->view.state = state;
        ++s->view.revision;
        s->state_pending = true;
        changed = true;
    }
    speech_unlock();
    if (changed) speech_session_wake(s);
}

bool speech_session_cancelled(void *session)
{
    speech_session_t *s = session;
    speech_lock();
    bool cancelled = s->cancelled;
    speech_unlock();
    return cancelled || epd_service_cancelled(s->owner) || epd_service_cancelled(s->worker);
}

void speech_session_error(speech_session_t *s, epd_speech_error_t error, const char *stage)
{
    speech_lock();
    if (s->failure.error == EPD_SPEECH_OK) {
        s->failure.error = error;
        rt_strncpy(s->failure.stage, stage, sizeof(s->failure.stage) - 1);
    }
    speech_unlock();
}

static void event_free(speech_event_node_t *node)
{
    speech_lock();
    event_bytes -= node->charge;
    speech_unlock();
    speech_free(node);
}

bool speech_session_text(void *session, uint32_t sentence, bool final, const char *text, size_t bytes)
{
    speech_session_t *s = session;
    size_t valid = 0;
    if (!speech_text_prefix(text, bytes, bytes, &valid) || valid != bytes || bytes > EVENT_BUDGET / 2) {
        speech_session_error(s, EPD_SPEECH_PROTOCOL, "transcript"); return false;
    }
    size_t charge = sizeof(speech_event_node_t) + bytes + 1;
    speech_lock();
    if (s->cancelled || s->owner_stopping || s->completed) { speech_unlock(); return false; }
    /* Superseded drafts do not consume space alongside their replacement.
     * Final results and events already borrowed by the consumer are retained. */
    speech_event_node_t *obsolete = NULL;
    speech_event_node_t **previous = &s->events;
    while (*previous) {
        speech_event_node_t *old = *previous;
        if (old->event.kind == EPD_SPEECH_EVENT_DRAFT && old->event.sentence == sentence) {
            *previous = old->next; old->next = obsolete; obsolete = old;
            event_bytes -= old->charge;
        } else previous = &old->next;
    }
    bool fits = charge <= EVENT_BUDGET - event_bytes;
    if (fits) event_bytes += charge;
    speech_unlock();
    while (obsolete) { speech_event_node_t *next = obsolete->next; speech_free(obsolete); obsolete = next; }
    if (!fits) { speech_session_error(s, EPD_SPEECH_NO_MEMORY, "event-budget"); return false; }
    speech_event_node_t *node = speech_alloc(charge);
    if (!node) {
        speech_lock(); event_bytes -= charge; speech_unlock();
        speech_session_error(s, EPD_SPEECH_NO_MEMORY, "event"); return false;
    }
    memset(node, 0, sizeof(*node));
    node->charge = charge;
    memcpy(node->text, text, bytes); node->text[bytes] = 0;
    node->event.kind = final ? EPD_SPEECH_EVENT_FINAL_TEXT : EPD_SPEECH_EVENT_DRAFT;
    node->event.text = node->text; node->event.text_bytes = bytes;
    node->event.sentence = sentence; node->event.private_data = node;
    speech_lock();
    if (s->cancelled || s->owner_stopping || s->completed) { speech_unlock(); event_free(node); return false; }
    node->event.snapshot = s->view;
    node->event.revision = ++s->view.revision;
    /* Replace queued interim revisions of this sentence; finals remain ordered. */
    speech_event_node_t **link = &s->events, *discard = NULL;
    while (*link) {
        speech_event_node_t *old = *link;
        if (old->event.kind == EPD_SPEECH_EVENT_DRAFT && old->event.sentence == sentence) {
            *link = old->next; old->next = discard; discard = old;
        } else link = &old->next;
    }
    *link = node; s->events_tail = node;
    speech_unlock();
    while (discard) { speech_event_node_t *next = discard->next; event_free(discard); discard = next; }
    speech_session_wake(s);
    return !speech_session_cancelled(s);
}

bool speech_session_pcm(void *session, const void *pcm, size_t bytes)
{
    speech_session_t *s = session;
    speech_lock();
    speech_slot_t *slot = &s->slots[s->write_slot];
    if ((bytes & 1) || bytes > EPD_SPEECH_PCM_SLOT_BYTES - slot->produced || !slot->pcm || !slot->busy) {
        speech_unlock(); speech_session_error(s, EPD_SPEECH_AUDIO_TOO_LARGE, "pcm-slot"); return false;
    }
    memcpy(slot->pcm + slot->produced, pcm, bytes);
    slot->produced += bytes;
    speech_output_wake(s);
    speech_unlock();
    return !speech_session_cancelled(s);
}

void speech_session_complete(speech_session_t *s, epd_speech_error_t error)
{
    speech_lock();
    if (s->completed) { speech_unlock(); return; }
    if (!error && s->cancelled) error = EPD_SPEECH_CANCELLED;
    if (s->failure.error) error = s->failure.error;
    s->view.error = error;
    s->view.state = error == EPD_SPEECH_OK ? EPD_SPEECH_COMPLETED :
        error == EPD_SPEECH_CANCELLED ? EPD_SPEECH_STOPPED : EPD_SPEECH_FAILED;
    if (error && error != EPD_SPEECH_CANCELLED) s->view.reason = EPD_SPEECH_SERVICE_ERROR;
    s->completed = true;
    s->state_pending = false;
    ++s->view.revision;
    sleep_guard_release(&s->sleep);
    speech_event_node_t *discard = NULL;
    speech_event_node_t **link = &s->events;
    while (*link) {
        speech_event_node_t *node = *link;
        if (node->event.kind == EPD_SPEECH_EVENT_DRAFT) {
            *link = node->next; node->next = discard; discard = node;
        } else link = &node->next;
    }
    void *pcm0 = s->slots[0].pcm, *pcm1 = s->slots[1].pcm, *text = s->text;
    s->slots[0].pcm = s->slots[1].pcm = NULL;
    s->text = NULL;
    speech_unlock();
    if (error && error != EPD_SPEECH_CANCELLED)
        LOG_W("provider=%u kind=%u session=%lu error=%s stage=%s detail=%d http=%d verify=%08lx code=%s request=%s",
            (unsigned)s->account.provider, (unsigned)s->view.kind, (unsigned long)s->view.session,
            epd_speech_error_name(error), s->failure.stage, s->failure.detail,
            s->failure.http_status, (unsigned long)s->failure.tls_verify_flags, s->failure.code, s->failure.request_id);
    speech_free(pcm0); speech_free(pcm1); speech_free(text);
    while (discard) { speech_event_node_t *next = discard->next; event_free(discard); discard = next; }
    speech_session_wake(s);
}

static void destroy(speech_session_t *s)
{
    speech_event_node_t *event = s->events;
    while (event) { speech_event_node_t *next = event->next; event_free(event); event = next; }
    sleep_guard_release(&s->sleep);
    for (unsigned i = 0; i < 2; ++i) speech_free(s->slots[i].pcm);
    speech_free(s->text);
    speech_free(s->output_stack);
    if (s->output_event_ready) rt_event_detach(&s->output_wakeup);
    epd_service_release(s->owner);
    memset(&s->account, 0, sizeof(s->account));
    speech_free(s);
}

static void run_kind(epd_service_t *service, unsigned kind)
{
    for (;;) {
        speech_lock();
        speech_session_t *s = sessions[kind];
        if (s && !s->completed && !s->worker_running) { s->worker_running = true; s->worker = service; }
        else s = NULL;
        speech_unlock();
        if (s) {
            if (kind == EPD_SPEECH_ASR) speech_asr_run(s); else speech_tts_run(s);
            speech_lock();
            s->worker_running = false;
            speech_session_wake(s);
            speech_unlock();
            epd_power_notify();
        }
        if (epd_service_cancelled(service)) break;
        epd_service_wait(service, s ? 1 : RT_WAITING_FOREVER);
    }
}
static void run_asr(epd_service_t *s) { run_kind(s, EPD_SPEECH_ASR); }
static void run_tts(epd_service_t *s) { run_kind(s, EPD_SPEECH_TTS); }
static bool worker_ui(epd_service_t *service, bool stopping)
{
    if (!stopping) return false;
    speech_lock();
    for (unsigned i = 0; i < 2; ++i) if (workers[i] == service) workers[i] = NULL;
    speech_unlock();
    return true;
}
static const epd_background_t definitions[2] = {
    {.run=run_asr, .stack_size=12288, .priority=20, .network=true, .ui_process=worker_ui, .flush=speech_flush},
    {.run=run_tts, .stack_size=12288, .priority=20, .network=true, .ui_process=worker_ui, .flush=speech_flush}
};

void speech_process(void)
{
    if (!initialized) {
        if (rt_mutex_init(&mutex, "speech", RT_IPC_FLAG_PRIO) != RT_EOK) return;
        initialized = true;
    }
    for (unsigned i = 0; i < 2; ++i) {
        speech_lock();
        speech_session_t *s = sessions[i];
        bool reap = s && s->completed && !s->worker_running && (s->terminal_taken || s->owner_stopping);
        if (reap) sessions[i] = NULL;
        speech_unlock();
        if (reap) destroy(s);
    }
    if (storage_changing() || storage_release_requested() || board_power_pending() ||
        system_reset_blocks_apps() || epd_power_sleeping()) return;
    for (unsigned i = 0; i < 2; ++i)
        if (!workers[i]) workers[i] = app_service_start(worker_ids[i], &definitions[i], NULL);
    speech_lock();
    /* Start checks readiness of the requested worker independently. */
    accepting = true;
    speech_unlock();
}

epd_speech_error_t epd_speech_capability(epd_speech_kind_t kind, epd_speech_capability_t *out)
{
    if (!out || (unsigned)kind > EPD_SPEECH_TTS) return EPD_SPEECH_INVALID;
    memset(out, 0, sizeof(*out));
    speech_account_t account;
    out->configured = speech_config_account(kind, &account) == EPD_SPEECH_OK;
    memset(&account, 0, sizeof(account));
    out->max_record_ms = EPD_SPEECH_ASR_LIMIT_MS;
    /* The audio adapter refines hardware availability in the start operation. */
    out->input_available = speech_audio_input_available();
    if (initialized) { speech_lock(); out->busy = sessions[kind] != NULL; speech_unlock(); }
    return EPD_SPEECH_OK;
}
RTM_EXPORT(epd_speech_capability);

epd_speech_error_t epd_speech_start(epd_service_t *owner, epd_speech_kind_t kind,
                                  const epd_speech_request_t *request, epd_speech_session_t *id)
{
    if (!owner || !request || !id || (unsigned)kind > EPD_SPEECH_TTS || request->version != EPD_SPEECH_VERSION ||
        request->struct_size != sizeof(*request) || (unsigned)request->language > EPD_SPEECH_EN ||
        !memchr(request->title, 0, sizeof(request->title))) return EPD_SPEECH_INVALID;
    *id = 0;
    if (!initialized || !workers[kind]) return EPD_SPEECH_BUSY;
    if (storage_changing() || storage_release_requested() || board_power_pending() ||
        system_reset_blocks_apps() || epd_power_sleeping()) return EPD_SPEECH_BUSY;
    if (kind == EPD_SPEECH_ASR && !speech_audio_input_available()) return EPD_SPEECH_INPUT_UNAVAILABLE;
    if (!network_ready()) return EPD_SPEECH_NO_NETWORK;
    uint32_t account_epochs[SPEECH_PROVIDER_COUNT];
    speech_lock();
    bool allowed = accepting && workers[kind] && !epd_service_cancelled(workers[kind]);
    memcpy(account_epochs, revocations, sizeof(account_epochs));
    speech_unlock();
    if (!allowed) return EPD_SPEECH_BUSY;
    speech_session_t *s = speech_alloc(sizeof(*s));
    if (!s) return EPD_SPEECH_NO_MEMORY;
    memset(s, 0, sizeof(*s));
    epd_speech_error_t e = speech_config_account(kind, &s->account);
    if (e) { speech_free(s); return e; }
    if (kind == EPD_SPEECH_TTS) {
        s->text = speech_alloc(EPD_SPEECH_TEXT_BYTES);
        if (!s->text) { memset(&s->account, 0, sizeof(s->account)); speech_free(s); return EPD_SPEECH_NO_MEMORY; }
    }
    s->owner = owner; s->request = *request;
    s->sleep.id = worker_ids[kind];
    if (!epd_service_retain(owner)) {
        speech_free(s->text); memset(&s->account, 0, sizeof(s->account)); speech_free(s); return EPD_SPEECH_CANCELLED;
    }
    speech_lock();
    bool revoked = (unsigned)s->account.provider >= SPEECH_PROVIDER_COUNT ||
        account_epochs[s->account.provider] != revocations[s->account.provider];
    if (revoked || !accepting || !workers[kind] || epd_service_cancelled(workers[kind]) ||
        sessions[kind] || (kind == EPD_SPEECH_TTS && sessions[EPD_SPEECH_ASR] && !sessions[EPD_SPEECH_ASR]->completed) ||
        epd_service_cancelled(owner) || !sleep_guard_acquire(&s->sleep)) {
        speech_unlock(); epd_service_release(owner); speech_free(s->text);
        memset(&s->account, 0, sizeof(s->account)); speech_free(s);
        return revoked ? EPD_SPEECH_NOT_CONFIGURED : EPD_SPEECH_BUSY;
    }
    if (++next_session == 0) ++next_session;
    s->view.session = next_session; s->view.kind = kind; s->view.state = EPD_SPEECH_PREPARING;
    s->view.limit_ms = kind == EPD_SPEECH_ASR ? EPD_SPEECH_ASR_LIMIT_MS : 0;
    s->view.revision = 1; s->state_pending = true;
    s->started = speech_now_ms(); s->text_changed = s->started;
    sessions[kind] = s; *id = s->view.session;
    epd_service_wake(workers[kind]);
    speech_unlock();
    return EPD_SPEECH_OK;
}
RTM_EXPORT(epd_speech_start);

int epd_speech_append(epd_service_t *owner, epd_speech_session_t id, const char *text, size_t bytes)
{
    if (!initialized || (!text && bytes)) return -EPD_SPEECH_INVALID;
    speech_lock(); speech_session_t *s = find(owner, id);
    if (!s || s->view.kind != EPD_SPEECH_TTS || s->finished || s->cancelled || s->completed) {
        speech_unlock(); return -EPD_SPEECH_INVALID;
    }
    size_t take = 0;
    /* The high watermark is real admission backpressure, not just a UI hint. */
    size_t room = s->pause || s->text_blocked || s->text_used >= 12u*1024u ? 0 : 12u*1024u - s->text_used;
    bool valid = speech_text_prefix(text, bytes, room, &take);
    if (!valid) { speech_unlock(); return -EPD_SPEECH_INVALID; }
    if (take) {
        memcpy(s->text + s->text_used, text, take); s->text_used += take; s->text_changed = speech_now_ms();
        if (s->text_used >= 12u*1024u) s->text_blocked = true;
    }
    epd_service_wake(workers[EPD_SPEECH_TTS]);
    speech_unlock();
    return (int)take;
}
RTM_EXPORT(epd_speech_append);

static epd_speech_error_t control(epd_service_t *owner, epd_speech_session_t id, unsigned action)
{
    if (!initialized) return EPD_SPEECH_NOT_FOUND;
    speech_lock(); speech_session_t *s = find(owner, id);
    if (!s || s->completed) { speech_unlock(); return EPD_SPEECH_NOT_FOUND; }
    if (s->cancelled) { speech_unlock(); return action == 1 ? EPD_SPEECH_OK : EPD_SPEECH_CANCELLED; }
    if ((action == 2 || action == 3) && s->view.kind != EPD_SPEECH_TTS) { speech_unlock(); return EPD_SPEECH_UNSUPPORTED; }
    if (action == 0) {
        s->finished = s->view.input_finished = true;
        if (s->view.reason == EPD_SPEECH_REASON_NONE) s->view.reason = EPD_SPEECH_USER_FINISH;
    }
    if (action == 1) { s->cancelled = true; s->view.reason = EPD_SPEECH_USER_STOP; s->view.state = EPD_SPEECH_STOPPING; }
    if (action == 2) { s->pause = true; s->resume_requested = false; ++s->intent; }
    if (action == 3) {
        if (sessions[EPD_SPEECH_ASR] && !sessions[EPD_SPEECH_ASR]->completed) { speech_unlock(); return EPD_SPEECH_BUSY; }
        if (!sleep_guard_acquire(&s->sleep)) { speech_unlock(); return EPD_SPEECH_BUSY; }
        s->resume_requested = s->pause;
        ++s->intent;
    }
    ++s->view.revision; s->state_pending = true;
    speech_output_wake(s);
    epd_service_wake(workers[s->view.kind]);
    speech_unlock(); return EPD_SPEECH_OK;
}
epd_speech_error_t epd_speech_finish(epd_service_t *o, epd_speech_session_t s) { return control(o,s,0); }
epd_speech_error_t epd_speech_cancel(epd_service_t *o, epd_speech_session_t s) { return control(o,s,1); }
epd_speech_error_t epd_speech_stop(epd_service_t *o, epd_speech_session_t s) { return control(o,s,1); }
epd_speech_error_t epd_speech_pause(epd_service_t *o, epd_speech_session_t s) { return control(o,s,2); }
epd_speech_error_t epd_speech_resume(epd_service_t *o, epd_speech_session_t s) { return control(o,s,3); }
RTM_EXPORT(epd_speech_finish); RTM_EXPORT(epd_speech_cancel); RTM_EXPORT(epd_speech_stop);
RTM_EXPORT(epd_speech_pause); RTM_EXPORT(epd_speech_resume);

epd_speech_error_t epd_speech_snapshot(epd_service_t *owner, epd_speech_session_t id, epd_speech_snapshot_t *out)
{
    if (!out) return EPD_SPEECH_INVALID;
    if (!initialized) return EPD_SPEECH_NOT_FOUND;
    speech_lock(); speech_session_t *s = find(owner, id);
    if (s) *out = s->view;
    speech_unlock(); return s ? EPD_SPEECH_OK : EPD_SPEECH_NOT_FOUND;
}
RTM_EXPORT(epd_speech_snapshot);

epd_speech_error_t epd_speech_poll(epd_service_t *owner, epd_speech_event_t *out)
{
    if (!owner || !out) return EPD_SPEECH_INVALID;
    if (!initialized) return EPD_SPEECH_NOT_FOUND;
    memset(out, 0, sizeof(*out));
    speech_lock();
    for (unsigned i = 0; i < 2; ++i) {
        speech_session_t *s = sessions[i];
        if (!s || s->owner != owner || s->terminal_taken || s->owner_stopping) continue;
        if (s->events) {
            speech_event_node_t *node = s->events; s->events = node->next;
            if (!s->events) s->events_tail = NULL;
            *out = node->event; speech_unlock(); return EPD_SPEECH_OK;
        }
        out->snapshot = s->view;
        if (s->completed && !s->worker_running) {
            out->kind = EPD_SPEECH_EVENT_TERMINAL; s->terminal_taken = true;
            out->detail = s->failure.detail; out->http_status = s->failure.http_status;
            memcpy(out->stage, s->failure.stage, sizeof(out->stage));
            memcpy(out->code, s->failure.code, sizeof(out->code));
            memcpy(out->request_id, s->failure.request_id, sizeof(out->request_id));
        } else if (s->state_pending) { out->kind = EPD_SPEECH_EVENT_STATE; s->state_pending = false; }
        else if (s->writable_pending) { out->kind = EPD_SPEECH_EVENT_WRITABLE; s->writable_pending = false; }
        else continue;
        speech_unlock(); epd_power_notify(); return EPD_SPEECH_OK;
    }
    speech_unlock(); return EPD_SPEECH_NOT_FOUND;
}
RTM_EXPORT(epd_speech_poll);
void epd_speech_event_release(epd_speech_event_t *event)
{
    if (!event) return;
    speech_event_node_t *node = event->private_data;
    memset(event, 0, sizeof(*event));
    if (node) event_free(node);
}
RTM_EXPORT(epd_speech_event_release);

void speech_owner_stop(epd_service_t *owner)
{
    if (!initialized) return;
    speech_lock();
    for (unsigned i = 0; i < 2; ++i) {
        speech_session_t *s = sessions[i];
        if (!s || s->owner != owner) continue;
        s->cancelled = s->owner_stopping = true; s->view.reason = EPD_SPEECH_OWNER_STOP;
        speech_output_wake(s);
        epd_service_wake(workers[i]);
    }
    speech_unlock(); epd_power_notify();
}
void speech_stop_all(void)
{
    if (!initialized) return;
    speech_lock();
    accepting = false;
    for (unsigned i = 0; i < 2; ++i) if (sessions[i] && !sessions[i]->owner_stopping) {
        sessions[i]->cancelled = sessions[i]->owner_stopping = true;
        sessions[i]->view.reason = EPD_SPEECH_OWNER_STOP;
        speech_output_wake(sessions[i]);
        epd_service_wake(workers[i]);
    }
    speech_unlock();
    app_service_stop(worker_ids[EPD_SPEECH_ASR]);
    app_service_stop(worker_ids[EPD_SPEECH_TTS]);
}
bool speech_idle(void)
{
    if (!initialized) return true;
    speech_lock(); bool idle = !sessions[0] && !sessions[1]; speech_unlock(); return idle;
}
bool speech_flush(void) { speech_stop_all(); return speech_idle(); }
void speech_config_changed(const speech_config_t *old, const speech_config_t *next)
{
    (void)old;
    if (!initialized || !next) return;
    speech_lock();
    for (unsigned p = 1; p < SPEECH_PROVIDER_COUNT; ++p)
        if (!next->accounts[p].enabled || !next->accounts[p].secret[0]) ++revocations[p];
    for (unsigned i = 0; i < 2; ++i) {
        speech_session_t *s = sessions[i];
        if (!s || s->completed) continue;
        speech_provider_t p = s->account.provider;
        if ((unsigned)p >= SPEECH_PROVIDER_COUNT || !next->accounts[p].enabled || !next->accounts[p].secret[0]) {
            s->cancelled = true; s->view.reason = EPD_SPEECH_SERVICE_ERROR;
            s->failure.error = EPD_SPEECH_NOT_CONFIGURED;
            speech_output_wake(s);
            rt_strncpy(s->failure.stage, "configuration-changed", sizeof(s->failure.stage)-1);
            epd_service_wake(workers[i]);
        }
    }
    speech_unlock();
}
bool speech_tts_suspend(speech_session_t *asr, uint32_t *id, uint32_t *intent)
{
    (void)asr; *id = *intent = 0;
    if (!initialized) return true;
    speech_lock(); speech_session_t *s = sessions[EPD_SPEECH_TTS];
    if (s && s->resume_requested) {
        s->resume_requested = false; ++s->intent;
        speech_output_wake(s); epd_service_wake(workers[EPD_SPEECH_TTS]);
    }
    if (s && !s->completed && !s->pause) {
        s->pause = true; *id = s->view.session; *intent = s->intent;
        speech_output_wake(s);
        epd_service_wake(workers[EPD_SPEECH_TTS]);
    }
    speech_unlock(); return true;
}
void speech_tts_restore(uint32_t id, uint32_t intent)
{
    speech_lock(); speech_session_t *s = sessions[EPD_SPEECH_TTS];
    if (s && id && s->view.session == id && s->intent == intent && !s->completed && !s->cancelled && sleep_guard_acquire(&s->sleep)) {
        s->pause = false; s->pause_confirmed = false;
        speech_output_wake(s); epd_service_wake(workers[EPD_SPEECH_TTS]);
    }
    speech_unlock();
}

bool speech_tts_quiet(void)
{
    speech_lock();
    speech_session_t *s = sessions[EPD_SPEECH_TTS];
    bool quiet = !s || s->completed || (s->pause && s->pause_confirmed && !s->output_busy);
    speech_unlock();
    return quiet;
}

static bool resume_current(speech_session_t *s, uint32_t intent)
{
    return s->resume_requested && s->intent == intent && !s->cancelled && !s->completed &&
        (!sessions[EPD_SPEECH_ASR] || sessions[EPD_SPEECH_ASR]->completed);
}
bool speech_tts_resume_begin(speech_session_t *s, uint32_t *intent)
{
    speech_lock();
    *intent = s->intent;
    bool ready = resume_current(s, *intent) && s->pause_confirmed && !s->output_busy && !s->resume_busy;
    if (ready) s->resume_busy = true;
    speech_unlock(); return ready;
}
bool speech_tts_resume_current(speech_session_t *s, uint32_t intent)
{
    speech_lock(); bool current = resume_current(s, intent); speech_unlock(); return current;
}
bool speech_tts_asr_pending(void)
{
    speech_lock();
    speech_session_t *asr = sessions[EPD_SPEECH_ASR];
    bool pending = asr && !asr->completed && !asr->cancelled;
    speech_unlock(); return pending;
}
void speech_tts_resume_finish(speech_session_t *s, uint32_t intent, epd_speech_error_t error)
{
    speech_lock();
    s->resume_busy = false;
    bool current = resume_current(s, intent);
    if (current) {
        s->resume_requested = false;
        s->view.error = error;
        if (!error) { s->pause = false; s->pause_confirmed = false; }
        else if (!s->network_busy) sleep_guard_release(&s->sleep);
        ++s->view.revision; s->state_pending = true;
    }
    /* A newer pause or ASR may have superseded this resume while media was
     * waiting. The output owner still has to settle the old sleep hold. */
    speech_output_wake(s);
    speech_unlock();
    if (current) speech_session_wake(s);
}

const char *epd_speech_error_name(epd_speech_error_t error)
{
    static const char *const names[] = {"ok","invalid","busy","no-memory","not-configured","no-network",
        "connect","tls","auth","quota","protocol","timeout","cancelled","storage","input-unavailable",
        "input-overrun","text-too-long","audio-too-large","output-interrupted","output-timeout","not-found","unsupported"};
    return (unsigned)error < sizeof(names)/sizeof(names[0]) ? names[error] : "unknown";
}
RTM_EXPORT(epd_speech_error_name);
