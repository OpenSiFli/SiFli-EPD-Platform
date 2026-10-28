#include "demo.h"
#include "platform/epd_memory.h"
#include "storage_file.h"
#include <rthw.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _MSC_VER
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif

#define TEXT_BYTES (512u * 1024u)
#define NATIVE_BYTES (256u * 1024u)
#define RECORD_MAGIC 0x41494348u
#define INDEX_MAGIC 0x41494958u
#define RECORD_VERSION 4u
#define DRAFT_DELAY_MS 1000
#define SPEECH_STACK_BYTES 8192u
#define READ_START (1u << 8)
#define READ_STOP (1u << 9)

typedef struct {
    uint32_t role, complete, offset, length, native_offset, native_length, revision;
} saved_message_t;
typedef struct {
    uint32_t id, identity, count, used, native_used, context_start;
    uint32_t status, error;
    demo_failure_t failure;
    char title[DEMO_TITLE_SIZE], profile[EPD_AI_ID_SIZE], profile_name[EPD_AI_NAME_SIZE];
    char model[EPD_AI_MODEL_SIZE], draft[DEMO_DRAFT_SIZE];
    saved_message_t messages[DEMO_MAX_MESSAGES];
    char text[TEXT_BYTES], native[NATIVE_BYTES];
} demo_record_t;
typedef struct {
    uint32_t next_id, current_id, count, auto_read;
    demo_summary_t items[DEMO_MAX_SESSIONS];
} demo_index_t;
enum { CMD_SEND = 100, CMD_OPEN, CMD_DELETE, CMD_RENAME, CMD_SELECT, CMD_NEW_WITH };
typedef struct {
    int kind;
    demo_id_t id;
    char text[DEMO_DRAFT_SIZE], profile[EPD_AI_ID_SIZE], model[EPD_AI_MODEL_SIZE];
} command_t;
typedef struct {
    rt_mutex_t lock;
    epd_service_t *service;
    epd_ai_cancel_t cancel;
    bool ready, queued, busy, dirty, save_ok, flushing, flush_pending, cancelled;
    bool storage_bound, readonly, draft_pending;
    storage_volume_t volume;
    uint32_t session, draft_revision, message_revision;
    rt_tick_t draft_deadline;
    char directory[STORAGE_PATH_MAX - 32];
    demo_view_t view;
    demo_index_t index;
    demo_record_t *record;
    epd_ai_profile_t profiles[EPD_AI_MAX_PROFILES];
    size_t profile_count;
    command_t command;
    chat_speech_t *speech;
    struct rt_thread speech_thread;
    void *speech_stack;
    bool speech_started, speech_unavailable;
    uint32_t read_commands, read_chat, read_message;
} demo_state_t;
static demo_state_t *state;

static void lock(void) { rt_mutex_take(state->lock, RT_WAITING_FOREVER); }
static void unlock(void) { rt_mutex_release(state->lock); }
static void changed(void) { ++state->view.revision; }
static uint32_t failure_reason(epd_ai_error_t error)
{
    return error == EPD_AI_OK ? DEMO_REASON_NONE :
        error == EPD_AI_INVALID ? DEMO_REASON_INVALID_REQUEST : DEMO_REASON_OTHER;
}
static void failure_stage(demo_failure_t *failure, demo_failure_phase_t phase, const char *stage)
{
    failure->phase = phase;
    snprintf(failure->stage, sizeof(failure->stage), "%s", stage);
}
/* Diagnostic identifiers have a bounded ASCII alphabet. Reject free-form
 * strings rather than retaining response prose or control characters. */
static void failure_token(char *out, size_t capacity, const char *input, size_t size)
{
    size_t length = 0;
    while (length < size && input[length]) {
        unsigned char c = (unsigned char)input[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            out[0] = 0; return;
        }
        ++length;
    }
    if (length >= capacity) length = capacity - 1;
    memcpy(out, input, length); out[length] = 0;
}
static bool failure_valid(const demo_failure_t *failure)
{
    if (failure->reason > DEMO_REASON_OTHER || failure->phase > DEMO_PHASE_RESTORE ||
        failure->status > EPD_AI_NOT_FOUND ||
        ((failure->reason == DEMO_REASON_NONE) != (failure->status == EPD_AI_OK)) ||
        ((failure->phase == DEMO_PHASE_NONE) != (failure->status == EPD_AI_OK)) ||
        (failure->reason == DEMO_REASON_SERVICE_CHANGED &&
         (failure->status != EPD_AI_INVALID || failure->phase != DEMO_PHASE_PRECHECK)) ||
        (failure->reason == DEMO_REASON_INVALID_REQUEST && failure->status != EPD_AI_INVALID) ||
        (failure->http_status && (failure->http_status < 100 || failure->http_status > 599)) ||
        !memchr(failure->stage, 0, sizeof(failure->stage)) ||
        !memchr(failure->error_code, 0, sizeof(failure->error_code))) return false;
    char token[DEMO_FAILURE_CODE_SIZE];
    failure_token(token, sizeof(token), failure->stage, sizeof(failure->stage));
    if (strcmp(token, failure->stage)) return false;
    failure_token(token, sizeof(token), failure->error_code, sizeof(failure->error_code));
    return !strcmp(token, failure->error_code);
}
static void log_failure(const demo_failure_t *failure)
{
    /* stage/code have already passed failure_token's length/alphabet filter. */
    rt_kprintf("[ai_demo] failed reason=%lu phase=%lu status=%lu http=%ld detail=%ld tls=%lu stage=%s code=%s\n",
        (unsigned long)failure->reason, (unsigned long)failure->phase,
        (unsigned long)failure->status, (long)failure->http_status,
        (long)failure->detail, (unsigned long)failure->tls_verify_flags,
        failure->stage[0] ? failure->stage : "-", failure->error_code[0] ? failure->error_code : "-");
}
static void restore_status_locked(void)
{
    state->view.status = (demo_status_t)state->record->status;
    state->view.error = (epd_ai_error_t)state->record->error;
    state->view.failure = state->record->failure;
}
static int find_id(const demo_index_t *index, demo_id_t id)
{
    for (uint32_t i = 0; i < index->count; ++i)
        if (index->items[i].id == id) return (int)i;
    return -1;
}
static void summary(demo_summary_t *out, const demo_record_t *r, bool saved)
{
    memset(out, 0, sizeof(*out));
    out->id = r->id; out->message_count = r->count; out->saved = saved;
    strcpy(out->title, r->title); strcpy(out->profile_id, r->profile);
    strcpy(out->profile_name, r->profile_name); strcpy(out->model, r->model);
}
static void publish_locked(void)
{
    demo_record_t *r = state->record;
    state->view.current_id = r->id;
    state->view.message_count = r->count;
    strcpy(state->view.title, r->title); strcpy(state->view.profile_id, r->profile);
    strcpy(state->view.profile_name, r->profile_name); strcpy(state->view.model, r->model);
    state->view.saved = !state->dirty && state->save_ok;
    state->view.search_available = false;
    for (size_t p = 0; p < state->profile_count; ++p) {
        const epd_ai_profile_t *profile = &state->profiles[p];
        if (strcmp(profile->id, r->profile) || profile->protocol == EPD_AI_CHAT) continue;
        for (size_t m = 0; m < profile->model_count; ++m)
            if (!strcmp(profile->models[m].id, r->model))
                state->view.search_available = (profile->models[m].capabilities & EPD_AI_CAP_WEB_SEARCH) != 0;
    }
    if (!state->view.search_available) state->view.web_search = false;
    int i = find_id(&state->index, r->id);
    if (i >= 0) summary(&state->index.items[i], r, state->view.saved);
    changed();
}
static void dirty_locked(void)
{
    state->dirty = true;
    publish_locked();
}
static void status(demo_status_t value, epd_ai_error_t error)
{
    lock();
    state->view.status = value; state->view.error = error;
    if (error == EPD_AI_STORAGE) state->view.failure = state->record->failure;
    else {
        memset(&state->view.failure, 0, sizeof(state->view.failure));
        state->view.failure.reason = failure_reason(error);
        state->view.failure.status = error;
    }
    changed(); unlock();
}
static bool cancelled(void)
{
    lock(); bool value = state->cancelled; unlock();
    return value || epd_service_cancelled(state->service);
}
static bool storage_valid(void)
{
    return state->storage_bound && !state->readonly &&
           storage_session_valid(state->volume, state->session);
}
static void path_for(char path[STORAGE_PATH_MAX], demo_id_t id)
{
    if (id) snprintf(path, STORAGE_PATH_MAX, "%s/chat-%08lx.bin", state->directory, (unsigned long)id);
    else snprintf(path, STORAGE_PATH_MAX, "%s/sessions.idx", state->directory);
}
static demo_id_t unused_id(demo_index_t *index)
{
    char path[STORAGE_PATH_MAX];
    demo_id_t id = 0;
    storage_lock();
    while (storage_valid() && index->next_id && index->next_id < UINT32_MAX)
    {
        demo_id_t candidate = index->next_id++;
        path_for(path, candidate);
        if (!storage_file_recover(path)) break;
        struct stat info;
        if (stat(path, &info) == 0) continue;
        if (errno == ENOENT || errno == -ENOENT) id = candidate;
        break;
    }
    storage_unlock(); return id;
}
static bool record_valid(const demo_record_t *r, demo_id_t id)
{
    if (!id || r->id != id || r->count > DEMO_MAX_MESSAGES || r->used > TEXT_BYTES ||
        r->native_used > NATIVE_BYTES || r->context_start > r->count || !failure_valid(&r->failure) ||
        r->status > DEMO_FAILED || r->error > EPD_AI_NOT_FOUND ||
        !memchr(r->title, 0, sizeof(r->title)) || !memchr(r->profile, 0, sizeof(r->profile)) ||
        !memchr(r->profile_name, 0, sizeof(r->profile_name)) ||
        !memchr(r->model, 0, sizeof(r->model)) || !memchr(r->draft, 0, sizeof(r->draft))) return false;
    uint32_t end = 0;
    for (uint32_t i = 0; i < r->count; ++i)
    {
        const saved_message_t *m = &r->messages[i];
        if (m->role > DEMO_ROLE_ASSISTANT || m->complete > 1 || m->offset != end ||
            m->offset >= r->used || m->length >= r->used - m->offset ||
            r->text[m->offset + m->length] || memchr(r->text + m->offset, 0, m->length) ||
            m->native_offset > r->native_used || m->native_length > r->native_used - m->native_offset ||
            (m->native_length && (m->role != DEMO_ROLE_ASSISTANT || !m->complete))) return false;
        end = m->offset + m->length + 1;
    }
    return end == r->used;
}
static bool index_valid(const demo_index_t *index)
{
    if (!index->next_id || index->auto_read > 1 || index->count > DEMO_MAX_SESSIONS ||
        (index->count && find_id(index, index->current_id) < 0) ||
        (!index->count && index->current_id)) return false;
    for (uint32_t i = 0; i < index->count; ++i)
    {
        const demo_summary_t *s = &index->items[i];
        if (!s->id || s->id >= index->next_id || s->message_count > DEMO_MAX_MESSAGES ||
            !memchr(s->title, 0, sizeof(s->title)) || !memchr(s->profile_id, 0, sizeof(s->profile_id)) ||
            !memchr(s->profile_name, 0, sizeof(s->profile_name)) || !memchr(s->model, 0, sizeof(s->model))) return false;
        for (uint32_t j = 0; j < i; ++j) if (index->items[j].id == s->id) return false;
    }
    return true;
}
/* storage_record framing consists of four uint32_t fields. The payload is
 * length-checked before allocation; record_load verifies its framing and CRC. */
static void *load_payload(const char *path, uint32_t magic, size_t minimum, size_t maximum, size_t *size)
{
    struct stat info;
    if (!storage_file_recover(path) || stat(path, &info) || info.st_size < 16 ||
        (uint64_t)info.st_size - 16 < minimum || (uint64_t)info.st_size - 16 > maximum) return NULL;
    *size = (size_t)info.st_size - 16;
    void *data = epd_app_alloc(*size, EPD_APP_PSRAM);
    if (data && !storage_record_load(path, magic, RECORD_VERSION, data, *size))
    { epd_app_free(data); data = NULL; }
    return data;
}
static bool save_record(const char *path, const demo_record_t *r)
{
    size_t head = offsetof(demo_record_t, messages), messages = r->count * sizeof(saved_message_t);
    size_t size = head + messages + r->used + r->native_used;
    char *data = epd_app_alloc(size, EPD_APP_PSRAM);
    if (!data) return false;
    memcpy(data, r, head); memcpy(data + head, r->messages, messages);
    memcpy(data + head + messages, r->text, r->used);
    memcpy(data + head + messages + r->used, r->native, r->native_used);
    bool ok = storage_record_save(path, RECORD_MAGIC, RECORD_VERSION, data, size);
    epd_app_free(data); return ok;
}
static bool load_index(const char *path, demo_index_t *index)
{
    size_t head = offsetof(demo_index_t, items), size = 0;
    char *data = load_payload(path, INDEX_MAGIC, head, sizeof(*index), &size);
    if (!data) return false;
    memset(index, 0, sizeof(*index)); memcpy(index, data, head);
    bool ok = index->count <= DEMO_MAX_SESSIONS && size == head + index->count * sizeof(demo_summary_t);
    if (ok) { memcpy(index->items, data + head, size - head); ok = index_valid(index); }
    if (!ok) { memset(index, 0, sizeof(*index)); index->next_id = 1; }
    epd_app_free(data); return ok;
}
static bool persist(const demo_record_t *r, const demo_index_t *index)
{
    char path[STORAGE_PATH_MAX];
    storage_lock();
    bool ok = storage_valid();
    if (ok && r)
    {
        path_for(path, r->id);
        ok = save_record(path, r);
    }
    if (ok)
    {
        path_for(path, 0);
        ok = storage_record_save(path, INDEX_MAGIC, RECORD_VERSION, index,
                                 offsetof(demo_index_t, items) + index->count * sizeof(demo_summary_t));
    }
    storage_unlock(); return ok;
}
static demo_record_t *load_record(demo_id_t id)
{
    demo_record_t *r = epd_app_alloc(sizeof(*r), EPD_APP_PSRAM);
    if (!r) return NULL;
    char path[STORAGE_PATH_MAX]; path_for(path, id);
    storage_lock();
    size_t head = offsetof(demo_record_t, messages), size = 0;
    char *data = storage_valid() ? load_payload(path, RECORD_MAGIC, head, sizeof(*r), &size) : NULL;
    storage_unlock();
    bool ok = data != NULL;
    memset(r, 0, sizeof(*r));
    if (ok)
    {
        memcpy(r, data, head);
        size_t messages = r->count <= DEMO_MAX_MESSAGES ? r->count * sizeof(saved_message_t) : 0;
        ok = r->count <= DEMO_MAX_MESSAGES && r->used <= TEXT_BYTES && r->native_used <= NATIVE_BYTES &&
             size == head + messages + r->used + r->native_used;
        if (ok)
        {
            memcpy(r->messages, data + head, messages); memcpy(r->text, data + head + messages, r->used);
            memcpy(r->native, data + head + messages + r->used, r->native_used);
        }
    }
    epd_app_free(data);
    if (!ok || !record_valid(r, id)) { epd_app_free(r); return NULL; }
    /* A persisted in-flight reply becomes an explicit retry, never a POST. */
    if (r->status == DEMO_RUNNING)
    {
        r->status = DEMO_FAILED; r->error = EPD_AI_CANCELLED;
        memset(&r->failure, 0, sizeof(r->failure));
        r->failure.reason = DEMO_REASON_OTHER; r->failure.status = EPD_AI_CANCELLED;
        failure_stage(&r->failure, DEMO_PHASE_RESTORE, "restore");
    }
    return r;
}
static demo_index_t *copy_index(void)
{
    demo_index_t *next = epd_app_alloc(sizeof(*next), EPD_APP_PSRAM);
    if (next) { lock(); *next = state->index; unlock(); }
    return next;
}
static bool save_current(void)
{
    lock();
    bool dirty = state->dirty;
    uint32_t revision = state->draft_revision;
    strcpy(state->record->draft, state->view.draft);
    uint32_t old_error = state->record->error, old_status = state->record->status;
    if (dirty && old_error == EPD_AI_STORAGE)
    {
        demo_record_t *r = state->record;
        bool incomplete = r->count && r->messages[r->count - 1].role == DEMO_ROLE_ASSISTANT &&
                          !r->messages[r->count - 1].complete;
        r->error = incomplete ? EPD_AI_INCOMPLETE : EPD_AI_OK;
        r->status = incomplete ? DEMO_FAILED : DEMO_READY;
    }
    unlock();
    if (!dirty) return state->save_ok;
    demo_index_t *next = copy_index();
    bool ok = false;
    if (next && state->record->id)
    {
        int i = find_id(next, state->record->id);
        if (i >= 0) summary(&next->items[i], state->record, true);
        ok = persist(state->record, next);
    }
    epd_app_free(next);
    lock();
    state->save_ok = ok;
    if (ok && revision == state->draft_revision) state->dirty = state->draft_pending = false;
    if (!ok) {
        state->record->status = old_status; state->record->error = old_error;
        state->view.status = DEMO_FAILED; state->view.error = EPD_AI_STORAGE;
    } else if (state->view.error == EPD_AI_STORAGE || old_error == EPD_AI_STORAGE) {
        restore_status_locked();
    }
    publish_locked(); unlock();
    return ok;
}
static void install_record(demo_record_t *r, const demo_index_t *index)
{
    lock();
    demo_record_t *old = state->record;
    state->record = r; state->index = *index;
    state->view.auto_read = index->auto_read != 0;
    state->view.searching = state->view.search_failed = false;
    state->dirty = state->draft_pending = false; state->save_ok = true;
    strcpy(state->view.draft, r->draft); ++state->draft_revision;
    for (uint32_t i = 0; i < r->count; ++i) r->messages[i].revision = ++state->message_revision;
    restore_status_locked();
    publish_locked(); unlock();
    epd_app_free(old);
}
static epd_ai_error_t refresh_profiles(void)
{
    epd_ai_profile_t *profiles = epd_app_alloc(sizeof(state->profiles), EPD_APP_PSRAM);
    if (!profiles) return EPD_AI_NO_MEMORY;
    size_t count = 0;
    epd_ai_error_t error = epd_ai_profile_list(state->service, profiles, EPD_AI_MAX_PROFILES, &count);
    if (count > EPD_AI_MAX_PROFILES) { count = 0; error = EPD_AI_PROTOCOL; }
    lock();
    state->profile_count = error == EPD_AI_OK ? count : 0;
    if (state->profile_count) memcpy(state->profiles, profiles, count * sizeof(*profiles));
    publish_locked(); unlock();
    epd_app_free(profiles); return error;
}
static epd_ai_error_t select_model(const epd_ai_profile_t *out, const char *model,
                                  const epd_ai_model_t **selected)
{
    if (!out->enabled) return EPD_AI_UNAUTHORIZED;
    for (unsigned i = 0; i < out->model_count && i < EPD_AI_MAX_MODELS; ++i)
        if (!strcmp(model, out->models[i].id))
        {
            const epd_ai_model_t *m = &out->models[i];
            if ((m->capabilities & (EPD_AI_CAP_TEXT | EPD_AI_CAP_NATIVE)) !=
                (EPD_AI_CAP_TEXT | EPD_AI_CAP_NATIVE) || !m->max_output_tokens) return EPD_AI_UNSUPPORTED;
            *selected = m; return EPD_AI_OK;
        }
    return EPD_AI_NOT_FOUND;
}

static epd_ai_error_t set_selection(demo_record_t *r, const char *id, const char *model)
{
    if (!id[0] && !model[0]) return EPD_AI_OK;
    epd_ai_profile_t profile;
    const epd_ai_model_t *selected = NULL;
    epd_ai_error_t error = epd_ai_profile_get(state->service, id, &profile);
    if (error == EPD_AI_OK) error = select_model(&profile, model, &selected);
    if (error != EPD_AI_OK) return error;
    strcpy(r->profile, id); strcpy(r->profile_name, profile.name); strcpy(r->model, model);
    r->identity = profile.identity_revision;
    return EPD_AI_OK;
}
static epd_ai_error_t new_session(const char *id, const char *model)
{
    if (state->index.count >= DEMO_MAX_SESSIONS || state->index.next_id == UINT32_MAX) return EPD_AI_LIMIT;
    if (!save_current()) return EPD_AI_STORAGE;
    demo_record_t *r = epd_app_alloc(sizeof(*r), EPD_APP_PSRAM);
    demo_index_t *next = copy_index();
    if (!r || !next) { epd_app_free(r); epd_app_free(next); return EPD_AI_NO_MEMORY; }
    memset(r, 0, sizeof(*r)); r->id = unused_id(next); r->status = DEMO_READY;
    if (!r->id) { epd_app_free(r); epd_app_free(next); return EPD_AI_STORAGE; }
    epd_ai_error_t error = set_selection(r, id, model);
    if (error == EPD_AI_OK)
    {
        next->current_id = r->id; summary(&next->items[next->count++], r, true);
        if (!persist(r, next)) error = EPD_AI_STORAGE;
    }
    if (error == EPD_AI_OK) install_record(r, next); else epd_app_free(r);
    epd_app_free(next); return error;
}
static epd_ai_error_t open_session(demo_id_t id)
{
    if (find_id(&state->index, id) < 0) return EPD_AI_NOT_FOUND;
    if (!save_current()) return EPD_AI_STORAGE;
    if (state->record->id == id) return EPD_AI_OK;
    demo_record_t *r = load_record(id);
    demo_index_t *next = copy_index();
    if (!r || !next) { epd_app_free(r); epd_app_free(next); return EPD_AI_STORAGE; }
    next->current_id = id;
    summary(&next->items[find_id(next, id)], r, true);
    bool ok = persist(NULL, next);
    if (ok) install_record(r, next); else epd_app_free(r);
    epd_app_free(next); return ok ? EPD_AI_OK : EPD_AI_STORAGE;
}
static epd_ai_error_t rename_session(demo_id_t id, const char *title)
{
    int i = find_id(&state->index, id);
    if (i < 0) return EPD_AI_NOT_FOUND;
    if (!save_current()) return EPD_AI_STORAGE;
    demo_record_t *r = load_record(id);
    demo_index_t *next = copy_index();
    if (!r || !next) { epd_app_free(r); epd_app_free(next); return EPD_AI_STORAGE; }
    strcpy(r->title, title); summary(&next->items[i], r, true);
    bool ok = persist(r, next);
    if (ok && id == state->record->id) install_record(r, next);
    else
    {
        if (ok) { lock(); state->index = *next; changed(); unlock(); }
        epd_app_free(r);
    }
    epd_app_free(next); return ok ? EPD_AI_OK : EPD_AI_STORAGE;
}
static bool remove_session_file(demo_id_t id)
{
    char path[STORAGE_PATH_MAX], extra[STORAGE_PATH_MAX + 4]; path_for(path, id);
    storage_lock();
    bool ok = storage_valid();
    if (ok)
    {
        const char *suffixes[] = {".old", ".tmp", ""};
        for (unsigned i = 0; i < 3; ++i)
        {
            snprintf(extra, sizeof(extra), "%s%s", path, suffixes[i]);
            if (unlink(extra) && errno != ENOENT && errno != -ENOENT) { ok = false; break; }
        }
    }
    storage_unlock(); return ok;
}
static epd_ai_error_t delete_session(demo_id_t id)
{
    int i = find_id(&state->index, id);
    if (i < 0) return EPD_AI_NOT_FOUND;
    demo_index_t *next = copy_index();
    if (!next) return EPD_AI_NO_MEMORY;
    memmove(&next->items[i], &next->items[i + 1], (next->count - (unsigned)i - 1) * sizeof(next->items[0]));
    --next->count;
    demo_record_t *replacement = NULL;
    if (id == state->record->id)
    {
        if (next->count)
        { next->current_id = next->items[0].id; replacement = load_record(next->current_id); }
        else if (next->next_id != UINT32_MAX)
        {
            replacement = epd_app_alloc(sizeof(*replacement), EPD_APP_PSRAM);
            if (replacement)
            {
                memset(replacement, 0, sizeof(*replacement)); replacement->status = DEMO_READY;
                replacement->id = unused_id(next);
                if (!replacement->id) { epd_app_free(replacement); replacement = NULL; }
                else
                {
                    next->current_id = replacement->id;
                    summary(&next->items[next->count++], replacement, true);
                }
            }
        }
        if (!replacement) { epd_app_free(next); return EPD_AI_STORAGE; }
    }
    /* The user explicitly confirmed this deletion. Free its storage before
     * writing the index, so a full volume can be recovered inside the app. */
    bool ok = remove_session_file(id);
    if (ok)
    {
        if (replacement) install_record(replacement, next);
        else { lock(); state->index = *next; changed(); unlock(); }
        lock(); dirty_locked(); unlock();
        ok = save_current();
    }
    else epd_app_free(replacement);
    epd_app_free(next); return ok ? EPD_AI_OK : EPD_AI_STORAGE;
}

static size_t utf8_prefix(const char *text, size_t size, size_t capacity)
{
    size_t n = size < capacity ? size : capacity;
    if (!n) return 0;
    size_t start = n - 1;
    while (start && ((unsigned char)text[start] & 0xc0) == 0x80) --start;
    unsigned char lead = (unsigned char)text[start];
    size_t width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    return start + width > n ? start : n;
}
/* Caller owns the short state lock; history text is append-only. */
static saved_message_t *append_message(demo_role_t role, const char *text, size_t size, bool complete)
{
    demo_record_t *r = state->record;
    if (r->count >= DEMO_MAX_MESSAGES || size >= TEXT_BYTES - r->used) return NULL;
    saved_message_t *m = &r->messages[r->count++]; memset(m, 0, sizeof(*m));
    m->role = role; m->complete = complete; m->offset = r->used; m->length = (uint32_t)size;
    m->revision = ++state->message_revision;
    if (size) memcpy(r->text + r->used, text, size);
    r->used += (uint32_t)size; r->text[r->used++] = 0;
    return m;
}
static bool on_event(const epd_ai_event_t *event, void *user)
{
    (void)user;
    if (cancelled()) return false;
    if (event->kind == EPD_AI_EVENT_SEARCH_START || event->kind == EPD_AI_EVENT_SEARCH_DONE ||
        event->kind == EPD_AI_EVENT_SEARCH_ERROR) {
        lock();
        state->view.searching = event->kind == EPD_AI_EVENT_SEARCH_START;
        if (event->kind == EPD_AI_EVENT_SEARCH_ERROR) state->view.search_failed = true;
        changed(); unlock(); return true;
    }
    if (event->kind != EPD_AI_EVENT_TEXT) return true;
    lock();
    demo_record_t *r = state->record;
    saved_message_t *m = &r->messages[r->count - 1];
    if (event->text_size > EPD_AI_TEXT_LIMIT - m->length || event->text_size > TEXT_BYTES - r->used)
    { unlock(); return false; }
    if (event->text_size)
    {
        memcpy(r->text + m->offset + m->length, event->text, event->text_size);
        m->length += (uint32_t)event->text_size; r->used += (uint32_t)event->text_size;
        r->text[r->used - 1] = 0; m->revision = ++state->message_revision;
        dirty_locked();
    }
    unlock(); return true;
}
static void discard_context_before(uint32_t start)
{
    demo_record_t *r = state->record;
    r->context_start = start;
    uint32_t used = 0;
    for (uint32_t i = 0; i < r->count; ++i)
    {
        saved_message_t *m = &r->messages[i];
        if (i < start) m->native_length = 0;
        if (m->native_length) memmove(r->native + used, r->native + m->native_offset, m->native_length);
        m->native_offset = used; used += m->native_length;
    }
    r->native_used = used;
}
static size_t request_messages(epd_ai_message_t *input, uint32_t user_index)
{
    demo_record_t *r = state->record;
    size_t count = 0;
    uint32_t user = UINT32_MAX;
    for (uint32_t i = r->context_start; i < user_index; ++i)
    {
        const saved_message_t *m = &r->messages[i];
        if (m->role == DEMO_ROLE_USER) user = i;
        else if (user != UINT32_MAX && m->complete && m->native_length)
        {
            const saved_message_t *u = &r->messages[user];
            input[count++] = (epd_ai_message_t){.kind = EPD_AI_USER, .text = r->text + u->offset, .text_size = u->length};
            input[count++] = (epd_ai_message_t){.kind = EPD_AI_NATIVE_ASSISTANT,
                .native = r->native + m->native_offset, .native_size = m->native_length};
            user = UINT32_MAX;
        }
    }
    const saved_message_t *u = &r->messages[user_index];
    input[count++] = (epd_ai_message_t){.kind = EPD_AI_USER, .text = r->text + u->offset, .text_size = u->length};
    return count;
}
static void read_current_locked(void)
{
    demo_record_t *r = state->record;
    if (!state->view.auto_read || state->speech_unavailable || state->cancelled ||
        !state->busy || r->status != DEMO_RUNNING || !r->count) return;
    const saved_message_t *message = &r->messages[r->count - 1];
    if (message->role != DEMO_ROLE_ASSISTANT || message->complete) return;
    state->read_chat = r->id; state->read_message = r->count - 1;
    state->read_commands |= READ_START;
}
static epd_ai_error_t generate(const char *text, bool retry, demo_failure_t *failure)
{
    demo_record_t *r = state->record;
    failure_stage(failure, DEMO_PHASE_PRECHECK, "precheck");
    if (cancelled()) return EPD_AI_CANCELLED;
    if (!r->id) return EPD_AI_INVALID;
    failure_stage(failure, DEMO_PHASE_PRECHECK, "selection");
    if (!r->profile[0] || !r->model[0]) return EPD_AI_UNAUTHORIZED;
    failure_stage(failure, DEMO_PHASE_PRECHECK, "retry");
    uint32_t user_index = r->count;
    if (retry)
    {
        if (!r->count || r->messages[r->count - 1].complete) return EPD_AI_INVALID;
        while (user_index && r->messages[user_index - 1].role != DEMO_ROLE_USER) --user_index;
        if (!user_index) return EPD_AI_INVALID;
        --user_index;
    }
    size_t size = retry ? 0 : strlen(text);
    failure_stage(failure, DEMO_PHASE_PRECHECK, "capacity");
    if (r->count + (retry ? 1u : 2u) > DEMO_MAX_MESSAGES ||
        size + (retry ? 0u : 1u) + EPD_AI_TEXT_LIMIT + 1 > TEXT_BYTES - r->used) return EPD_AI_LIMIT;
    epd_ai_profile_t profile;
    const epd_ai_model_t *model = NULL;
    failure_stage(failure, DEMO_PHASE_PRECHECK, "profile");
    epd_ai_error_t error = epd_ai_profile_get(state->service, r->profile, &profile);
    if (error != EPD_AI_OK) return error;
    failure_stage(failure, DEMO_PHASE_PRECHECK, "identity");
    if (r->count && r->identity != profile.identity_revision) {
        failure->reason = DEMO_REASON_SERVICE_CHANGED;
        return EPD_AI_INVALID;
    }
    failure_stage(failure, DEMO_PHASE_PRECHECK, "model");
    error = select_model(&profile, r->model, &model);
    if (error != EPD_AI_OK) return error;
    failure_stage(failure, DEMO_PHASE_PRECHECK, "allocate");
    epd_ai_message_t *input = epd_app_alloc(sizeof(*input) * DEMO_MAX_MESSAGES, EPD_APP_PSRAM);
    if (!input) return EPD_AI_NO_MEMORY;
    lock();
    r->identity = profile.identity_revision;
    if (!retry)
    {
        append_message(DEMO_ROLE_USER, text, size, true);
        if (!r->title[0])
        { size_t n = utf8_prefix(text, size, sizeof(r->title) - 1); memcpy(r->title, text, n); r->title[n] = 0; }
        if (!strcmp(state->view.draft, text))
        { state->view.draft[0] = 0; ++state->draft_revision; }
    }
    append_message(DEMO_ROLE_ASSISTANT, "", 0, false);
    r->status = DEMO_RUNNING; r->error = EPD_AI_OK;
    read_current_locked();
    state->view.searching = state->view.search_failed = false;
    bool web_search = state->view.web_search;
    memset(&r->failure, 0, sizeof(r->failure));
    dirty_locked(); unlock();
    /* Persist the accepted question before starting its non-idempotent POST. */
    failure_stage(failure, DEMO_PHASE_SAVE, "save");
    if (!save_current()) { epd_app_free(input); return EPD_AI_STORAGE; }
    epd_ai_request_t request = {.struct_size = sizeof(request), .version = EPD_AI_VERSION,
        .profile_id = r->profile, .model = r->model, .messages = input,
        .max_output_tokens = model->max_output_tokens,
        .stream = (model->capabilities & EPD_AI_CAP_STREAM) != 0, .format = EPD_AI_FORMAT_TEXT,
        .web_search = web_search};
    request.message_count = request_messages(input, user_index);
    size_t encoded = 0, skipped = 0;
    failure_stage(failure, DEMO_PHASE_MEASURE, "measure");
    do
    {
        if (cancelled()) { error = EPD_AI_CANCELLED; break; }
        error = epd_ai_request_measure(state->service, &request, 0, &encoded);
        if (error != EPD_AI_LIMIT || request.message_count <= 1) break;
        request.messages += 2; request.message_count -= 2; skipped += 2;
    } while (true);
    epd_ai_result_t result = {0};
    if (error == EPD_AI_OK && !cancelled()) {
        failure_stage(failure, DEMO_PHASE_PERFORM, "perform");
        error = epd_ai_perform(state->service, &request, &state->cancel, on_event, NULL, &result);
        failure->http_status = result.http_status >= 100 && result.http_status <= 599 ? result.http_status : 0;
        failure->detail = result.detail; failure->tls_verify_flags = result.tls_verify_flags;
        char stage[DEMO_FAILURE_STAGE_SIZE];
        failure_token(stage, sizeof(stage), result.stage, sizeof(result.stage));
        if (stage[0]) memcpy(failure->stage, stage, strlen(stage) + 1);
        failure_token(failure->error_code, sizeof(failure->error_code), result.error_code, sizeof(result.error_code));
    }
    if (cancelled()) error = EPD_AI_CANCELLED;
    lock(); state->view.searching = false; changed(); unlock();
    saved_message_t *m = &r->messages[r->count - 1];
    if (result.text && result.text_size <= EPD_AI_TEXT_LIMIT && result.text_size >= m->length)
    {
        lock();
        memcpy(r->text + m->offset, result.text, result.text_size);
        m->length = (uint32_t)result.text_size; r->text[m->offset + m->length] = 0;
        r->used = m->offset + m->length + 1; m->revision = ++state->message_revision;
        dirty_locked(); unlock();
    }
    if (error == EPD_AI_OK && (!result.complete || !result.replayable ||
        result.finish != EPD_AI_FINISH_STOP || result.tool_count || !result.native || !result.native_size))
    {
        failure->phase = DEMO_PHASE_RESPONSE;
        error = EPD_AI_INCOMPLETE;
    }
    if (error == EPD_AI_OK && result.native_size > NATIVE_BYTES) {
        failure->phase = DEMO_PHASE_RESPONSE; error = EPD_AI_LIMIT;
    }
    if (error == EPD_AI_OK)
    {
        if (skipped)
        {
            /* The request starts at a user message after whole completed turns. */
            for (uint32_t i = r->context_start; i <= user_index; ++i)
                if (r->text + r->messages[i].offset == request.messages[0].text)
                { discard_context_before(i); break; }
        }
        for (uint32_t i = r->context_start; result.native_size > NATIVE_BYTES - r->native_used && i < user_index; ++i)
            if (r->messages[i].native_length) discard_context_before(i + 1);
        m->native_offset = r->native_used; m->native_length = (uint32_t)result.native_size;
        memcpy(r->native + r->native_used, result.native, result.native_size);
        r->native_used += (uint32_t)result.native_size;
        lock(); m->complete = 1; m->revision = ++state->message_revision; dirty_locked(); unlock();
    }
    epd_ai_result_clear(&result); epd_app_free(input); return error;
}

static bool missing_file(const char *path)
{
    struct stat info;
    return stat(path, &info) != 0 && (errno == ENOENT || errno == -ENOENT);
}
static bool prune_deleted_sessions(demo_index_t *index, bool *changed_index)
{
    char path[STORAGE_PATH_MAX];
    *changed_index = false;
    for (uint32_t i = 0; i < index->count;)
    {
        path_for(path, index->items[i].id);
        if (!storage_file_recover(path)) return false;
        if (!missing_file(path)) { ++i; continue; }
        memmove(&index->items[i], &index->items[i + 1],
                (index->count - i - 1) * sizeof(index->items[0]));
        --index->count; *changed_index = true;
    }
    if (find_id(index, index->current_id) < 0)
        index->current_id = index->count ? index->items[0].id : 0;
    return true;
}
static void initialize_storage(void)
{
    char path[STORAGE_PATH_MAX];
    storage_lock();
    state->session = storage_card_session();
    state->storage_bound = storage_app_path(path, sizeof(path), "ai_demo", STORAGE_APP_DATA, "sessions.idx");
    if (state->storage_bound)
    {
        const char *root = storage_path_root(path);
        state->volume = root && !strcmp(root, STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
        char *slash = strrchr(path, '/');
        if (!slash || (size_t)(slash - path) >= sizeof(state->directory)) state->storage_bound = false;
        else { memcpy(state->directory, path, (size_t)(slash - path)); state->directory[slash - path] = 0; }
    }
    bool loaded = storage_valid() && load_index(path, &state->index);
    bool pruned = false;
    if (loaded && !prune_deleted_sessions(&state->index, &pruned)) loaded = false;
    bool missing = storage_valid() && !loaded && missing_file(path);
    storage_unlock();
    epd_ai_error_t error = EPD_AI_OK;
    if (loaded && state->index.count)
    {
        demo_record_t *r = load_record(state->index.current_id);
        if (r) {
            install_record(r, &state->index);
            if (pruned) {
                lock(); dirty_locked(); unlock();
                if (!save_current()) error = EPD_AI_STORAGE;
            }
        } else error = EPD_AI_STORAGE;
    }
    else if (loaded || missing)
    {
        if (missing) { memset(&state->index, 0, sizeof(state->index)); state->index.next_id = 1; }
        error = new_session("", "");
    }
    else { state->readonly = true; error = EPD_AI_STORAGE; }
    epd_ai_error_t profiles_error = refresh_profiles();
    if (error == EPD_AI_OK && profiles_error != EPD_AI_OK) error = profiles_error;
    lock(); state->ready = true; publish_locked(); unlock();
    if (error != EPD_AI_OK) status(DEMO_FAILED, error);
    else if (state->view.status == DEMO_LOADING) status(DEMO_READY, EPD_AI_OK);
}
static epd_ai_error_t execute(const command_t *command, demo_failure_t *failure)
{
    switch (command->kind)
    {
    case DEMO_REFRESH: return refresh_profiles();
    case DEMO_SAVE: return save_current() ? EPD_AI_OK : EPD_AI_STORAGE;
    case DEMO_NEW: return new_session(state->record->profile, state->record->model);
    case CMD_NEW_WITH: return new_session(command->profile, command->model);
    case CMD_OPEN: return open_session(command->id);
    case CMD_DELETE: return delete_session(command->id);
    case CMD_RENAME: return rename_session(command->id, command->text);
    case CMD_SELECT:
    {
        if (state->record->count) return EPD_AI_INVALID;
        epd_ai_error_t error = set_selection(state->record, command->profile, command->model);
        if (error == EPD_AI_OK)
        {
            lock(); state->record->status = DEMO_READY; state->record->error = EPD_AI_OK;
            memset(&state->record->failure, 0, sizeof(state->record->failure));
            dirty_locked(); unlock();
        }
        return error;
    }
    case CMD_SEND: return generate(command->text, false, failure);
    case DEMO_RETRY:
    {
        demo_record_t *r = state->record;
        if (r->count && r->messages[r->count - 1].role == DEMO_ROLE_ASSISTANT &&
            !r->messages[r->count - 1].complete) return generate("", true, failure);
        char draft[DEMO_DRAFT_SIZE];
        lock(); strcpy(draft, state->view.draft); unlock();
        return draft[0] ? generate(draft, false, failure) : generate("", true, failure);
    }
    default: return EPD_AI_INVALID;
    }
}
/* Process one queued command or one due persistence barrier. */
static bool worker_step(void)
{
    lock();
    bool queued = state->queued, flushing = state->flush_pending;
    bool draft_due = state->draft_pending && (int32_t)(rt_tick_get() - state->draft_deadline) >= 0;
    if (queued) state->queued = false;
    if (flushing || queued || draft_due) state->draft_pending = false;
    unlock();
    if (!queued && !flushing && !draft_due) return false;
    if (queued)
    {
        bool generation = state->command.kind == CMD_SEND || state->command.kind == DEMO_RETRY;
        if (generation) { epd_ai_cancel_init(&state->cancel); status(DEMO_RUNNING, EPD_AI_OK); }
        demo_failure_t failure = {0};
        epd_ai_error_t error = execute(&state->command, &failure);
        if (generation)
        {
            if (error == EPD_AI_OK) memset(&failure, 0, sizeof(failure));
            else {
                failure.status = error;
                if (failure.reason == DEMO_REASON_NONE) failure.reason = failure_reason(error);
                log_failure(&failure);
            }
            lock();
            state->record->status = error == EPD_AI_OK ? DEMO_DONE : DEMO_FAILED;
            state->record->error = error;
            state->record->failure = failure;
            restore_status_locked();
            dirty_locked(); unlock();
        }
        else if (error != EPD_AI_OK) status(DEMO_FAILED, error);
        else if (state->command.kind == DEMO_REFRESH || state->command.kind == CMD_SELECT || state->command.kind == DEMO_SAVE)
        {
            lock(); restore_status_locked(); changed(); unlock();
        }
    }
    save_current();
    lock();
    if (queued) state->busy = state->view.busy = false;
    if (flushing || state->flush_pending) state->flush_pending = false;
    changed(); unlock();
    return true;
}
static bool speech_source(void *user, uint32_t chat, uint32_t message, char *text,
                          size_t capacity, size_t *bytes, bool *finished)
{
    (void)user;
    lock();
    demo_record_t *r = state->record;
    bool valid = r->id == chat && message < r->count && r->messages[message].role == DEMO_ROLE_ASSISTANT;
    if (valid) {
        const saved_message_t *m = &r->messages[message];
        valid = m->length < capacity;
        if (valid) {
            *bytes = m->length; memcpy(text, r->text + m->offset, *bytes); text[*bytes] = 0;
            *finished = m->complete || (r->status != DEMO_RUNNING && !state->busy);
        }
    }
    unlock(); return valid;
}
static void speech_step(void)
{
    lock();
    chat_speech_t *speech = state->speech;
    speech->owner = state->service;
    uint32_t commands = state->read_commands;
    uint32_t chat = state->read_chat, message = state->read_message;
    state->read_commands &= ~READ_STOP;
    bool start = (commands & READ_START) && !speech->view.output_active;
    if (start) state->read_commands &= ~READ_START;
    unlock();
    if ((commands & READ_STOP) ||
        ((commands & READ_START) && speech->view.output_active && !speech->finishing))
        chat_speech_stop_reading(speech);
    if (start) chat_speech_read(speech, chat, message, true);
    chat_speech_step(speech);
    chat_speech_view_t view = speech->view;
    lock();
    /* Speech controls/notices have their own UI comparison, separate from the
     * conversation revision that triggers message layout. */
    state->view.speech = view;
    unlock();
}
static void speech_run(void *parameter)
{
    epd_service_t *owner = parameter;
    while (!epd_service_cancelled(owner)) { speech_step(); rt_thread_mdelay(100); }
    chat_speech_dispose(state->speech);
    /* Owner stop suppresses events; the platform retains the service until
     * teardown ends. No platform work borrows this controller or its hooks. */
}
static bool speech_start_worker(epd_service_t *service)
{
    state->speech_stack = epd_app_alloc(SPEECH_STACK_BYTES, EPD_APP_PSRAM);
    if (!state->speech_stack) return false;
    int result = rt_thread_init(&state->speech_thread, "ai_read", speech_run, service,
                               state->speech_stack, SPEECH_STACK_BYTES, 25, 10);
    if (result == RT_EOK) {
        result = rt_thread_startup(&state->speech_thread);
        if (result != RT_EOK) rt_thread_detach(&state->speech_thread);
    }
    if (result != RT_EOK) { epd_app_free(state->speech_stack); state->speech_stack = NULL; return false; }
    state->speech_started = true; return true;
}
static void speech_join(void)
{
    if (!state->speech_started) return;
    for (;;) {
        rt_base_t level = rt_hw_interrupt_disable();
        bool closed = (state->speech_thread.stat & RT_THREAD_STAT_MASK) == RT_THREAD_CLOSE;
        rt_hw_interrupt_enable(level);
        if (closed) break;
        rt_thread_mdelay(1);
    }
    epd_app_free(state->speech_stack); state->speech_stack = NULL; state->speech_started = false;
}
static void run(epd_service_t *service)
{
    if (!state) return;
    lock(); state->service = service; unlock();
    while (!epd_service_cancelled(service) && !epd_service_sleep_acquire(service))
        epd_service_wait(service, RT_WAITING_FOREVER);
    if (!epd_service_cancelled(service)) initialize_storage();
    if (!epd_service_cancelled(service) && !speech_start_worker(service)) {
        lock(); state->speech_unavailable = true;
        state->view.speech.output_error = EPD_SPEECH_NO_MEMORY;
        changed(); unlock();
    }
    while (!epd_service_cancelled(service))
    {
        worker_step();
        lock();
        bool pending = state->queued || state->flush_pending;
        rt_int32_t timeout = RT_WAITING_FOREVER;
        if (!pending && state->draft_pending)
        {
            timeout = (rt_int32_t)(state->draft_deadline - rt_tick_get());
            if (timeout <= 0) pending = true;
        }
        if (!pending && !state->draft_pending) epd_service_sleep_release(service);
        unlock();
        if (!pending) epd_service_wait(service, timeout);
    }
    /* A queued send has not posted yet; its accepted input is still a draft. */
    lock(); state->cancelled = true; state->queued = false; unlock();
    speech_join();
    save_current();
    lock(); state->busy = state->view.busy = false; changed(); unlock();
    epd_service_sleep_release(service);
}
static bool flush(void)
{
    if (!state) return true;
    if (rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    bool ok = state->ready && !state->busy && !state->queued && !state->flush_pending &&
              !state->dirty && state->save_ok;
    bool wake = false;
    if (!ok && !state->flush_pending && state->service && epd_service_sleep_acquire(state->service))
    {
        state->flushing = state->flush_pending = state->cancelled = true;
        wake = true;
    }
    epd_service_t *service = state->service;
    unlock();
    if (wake) { epd_ai_cancel(&state->cancel); epd_service_wake(service); }
    return ok;
}
static bool ui_process(epd_service_t *service, bool stopping)
{
    (void)service;
    if (!stopping) return false;
    demo_ui_release();
    if (state) { lock(); state->service = NULL; unlock(); }
    return true;
}
const epd_background_t epd_app_background = {
    .run = run, .stack_size = 12288, .priority = 24, .ui_process = ui_process, .flush = flush
};

void demo_init(void)
{
    if (state) return;
    state = epd_app_alloc(sizeof(*state), EPD_APP_PSRAM);
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->record = epd_app_alloc(sizeof(*state->record), EPD_APP_PSRAM);
    state->speech = epd_app_alloc(sizeof(*state->speech), EPD_APP_PSRAM);
    state->lock = rt_mutex_create("ai_demo", RT_IPC_FLAG_PRIO);
    if (!state->record || !state->lock || !state->speech)
    {
        epd_app_free(state->record);
        epd_app_free(state->speech);
        if (state->lock) rt_mutex_delete(state->lock);
        epd_app_free(state); state = NULL; return;
    }
    memset(state->record, 0, sizeof(*state->record));
    state->record->status = DEMO_READY; state->view.status = DEMO_LOADING;
    state->save_ok = true; state->index.next_id = 1;
    chat_speech_hooks_t hooks = {speech_source, NULL};
    chat_speech_init(state->speech, NULL, &hooks);
    epd_ai_cancel_init(&state->cancel);
}
void demo_deinit(void)
{
    if (!state) return;
    chat_speech_dispose(state->speech); epd_app_free(state->speech);
    rt_mutex_delete(state->lock); epd_app_free(state->record); epd_app_free(state); state = NULL;
}
epd_service_t *demo_owner(void)
{
    if (!state || rt_mutex_take(state->lock, 0) != RT_EOK) return NULL;
    epd_service_t *service = state->service; unlock(); return service;
}
static bool has_text(const char *text)
{
    if (!text) return false;
    for (; *text; ++text) if (!strchr(" \t\r\n", *text)) return true;
    return false;
}
static bool enqueue(int kind, demo_id_t id, const char *text, const char *profile, const char *model)
{
    if (!state || (text && strlen(text) >= DEMO_DRAFT_SIZE) ||
        (profile && strlen(profile) >= EPD_AI_ID_SIZE) || (model && strlen(model) >= EPD_AI_MODEL_SIZE) ||
        rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    bool accepted = state->ready && state->service && !state->busy && !state->queued &&
        !state->flush_pending && !state->readonly && !epd_service_cancelled(state->service);
    if (accepted && kind == CMD_SELECT && state->record->count)
    {
        accepted = false;
    }
    if (accepted && !epd_service_sleep_acquire(state->service)) accepted = false;
    if (accepted)
    {
        if (kind == DEMO_NEW || kind == CMD_NEW_WITH ||
            (kind == CMD_OPEN && id != state->record->id) ||
            (kind == CMD_DELETE && id == state->record->id)) {
            state->read_commands = READ_STOP;
        }
        memset(&state->command, 0, sizeof(state->command));
        state->command.kind = kind; state->command.id = id;
        if (text) strcpy(state->command.text, text);
        if (profile) strcpy(state->command.profile, profile);
        if (model) strcpy(state->command.model, model);
        if (kind == CMD_SEND)
        {
            strcpy(state->view.draft, text); ++state->draft_revision; dirty_locked();
        }
        state->queued = state->busy = state->view.busy = true;
        state->flushing = state->cancelled = false;
        changed();
    }
    epd_service_t *service = state->service;
    unlock();
    if (accepted) epd_service_wake(service);
    return accepted;
}
bool demo_submit(demo_command_t command)
{
    if (command < DEMO_REFRESH || command > DEMO_RETRY) return false;
    return enqueue(command, 0, NULL, NULL, NULL);
}
bool demo_send(const char *text) { return has_text(text) && enqueue(CMD_SEND, 0, text, NULL, NULL); }
bool demo_open(demo_id_t id) { return id && enqueue(CMD_OPEN, id, NULL, NULL, NULL); }
bool demo_delete(demo_id_t id) { return id && enqueue(CMD_DELETE, id, NULL, NULL, NULL); }
bool demo_rename(demo_id_t id, const char *title)
{
    return id && has_text(title) && strlen(title) < DEMO_TITLE_SIZE && enqueue(CMD_RENAME, id, title, NULL, NULL);
}
bool demo_select(const char *profile, const char *model)
{
    return profile && *profile && model && *model && enqueue(CMD_SELECT, 0, NULL, profile, model);
}
bool demo_new_with(const char *profile, const char *model)
{
    return profile && *profile && model && *model && enqueue(CMD_NEW_WITH, 0, NULL, profile, model);
}
bool demo_set_draft(demo_id_t expected_id, const char *text)
{
    if (!state || !text || strlen(text) >= DEMO_DRAFT_SIZE || rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    if (!state->ready || state->readonly || !state->record->id || expected_id != state->record->id ||
        state->flush_pending || !state->service ||
        (state->busy && state->command.kind != CMD_SEND && state->command.kind != DEMO_RETRY) ||
        epd_service_cancelled(state->service)) { unlock(); return false; }
    if (!strcmp(state->view.draft, text)) { unlock(); return true; }
    if (!epd_service_sleep_acquire(state->service)) { unlock(); return false; }
    bool wake = !state->draft_pending;
    strcpy(state->view.draft, text); ++state->draft_revision;
    state->draft_pending = true; state->draft_deadline = rt_tick_get() + rt_tick_from_millisecond(DRAFT_DELAY_MS);
    dirty_locked();
    epd_service_t *service = state->service;
    unlock();
    if (wake) epd_service_wake(service);
    return true;
}
bool demo_set_web_search(bool enabled)
{
    if (!state || rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    bool ok = state->ready && !state->busy && (!enabled || state->view.search_available);
    if (ok) { state->view.web_search = enabled; state->view.search_failed = false; changed(); }
    unlock(); return ok;
}
bool demo_set_auto_read(bool enabled)
{
    if (!state || rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    if (state->view.auto_read == enabled) { unlock(); return true; }
    if (!state->ready || state->readonly || !state->service || state->flush_pending ||
        (state->busy && state->command.kind != CMD_SEND && state->command.kind != DEMO_RETRY) ||
        !epd_service_sleep_acquire(state->service)) { unlock(); return false; }
    state->view.auto_read = enabled; state->index.auto_read = enabled;
    if (!enabled) {
        state->read_commands = READ_STOP;
    } else read_current_locked();
    ++state->draft_revision; state->draft_pending = true;
    state->draft_deadline = rt_tick_get() + rt_tick_from_millisecond(DRAFT_DELAY_MS);
    dirty_locked(); epd_service_t *owner = state->service;
    unlock(); epd_service_wake(owner); return true;
}
void demo_cancel(void)
{
    if (!state) return;
    lock(); state->cancelled = true; state->read_commands = READ_STOP;
    epd_service_t *service = state->service; unlock();
    epd_ai_cancel(&state->cancel);
    if (service) epd_service_wake(service);
}
bool demo_snapshot(demo_view_t *view)
{
    if (!state || !view || rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    *view = state->view;
    view->generating = state->busy && (state->command.kind == CMD_SEND || state->command.kind == DEMO_RETRY);
    unlock(); return true;
}
bool demo_message(size_t index, demo_message_meta_t *meta, size_t offset, char *text, size_t capacity)
{
    if (!state || !meta || (capacity && !text) || rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    if (!state->ready || index >= state->record->count) { unlock(); return false; }
    const saved_message_t *m = &state->record->messages[index];
    const char *source = state->record->text + m->offset;
    meta->role = (demo_role_t)m->role; meta->user = m->role == DEMO_ROLE_USER;
    meta->complete = m->complete != 0; meta->revision = m->revision;
    meta->length = utf8_prefix(source, m->length, m->length);
    if (capacity)
    {
        if (offset > meta->length) offset = meta->length;
        while (offset < meta->length && ((unsigned char)source[offset] & 0xc0) == 0x80) ++offset;
        size_t length = utf8_prefix(source + offset, meta->length - offset, capacity - 1);
        memcpy(text, source + offset, length); text[length] = 0;
    }
    unlock(); return true;
}
bool demo_list(demo_summary_t *items, size_t capacity, size_t *count)
{
    if (!state || !count || (capacity && !items) || rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    *count = state->ready ? state->index.count : 0;
    if (capacity > *count) capacity = *count;
    if (capacity) memcpy(items, state->index.items, capacity * sizeof(*items));
    unlock(); return true;
}
bool demo_profiles(epd_ai_profile_t *items, size_t capacity, size_t *count)
{
    if (!state || !count || (capacity && !items) || rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    *count = state->profile_count;
    if (capacity > *count) capacity = *count;
    if (capacity) memcpy(items, state->profiles, capacity * sizeof(*items));
    unlock(); return true;
}
bool demo_profile_get(const char *id, epd_ai_profile_t *profile)
{
    if (!state || !id || !profile || rt_mutex_take(state->lock, 0) != RT_EOK) return false;
    bool found = false;
    for (size_t i = 0; i < state->profile_count; ++i)
        if (!strcmp(id, state->profiles[i].id)) { *profile = state->profiles[i]; found = true; break; }
    unlock(); return found;
}
