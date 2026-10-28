/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_session.h"
#include <string.h>

const reader_session_slot_t *reader_sessions_get(const reader_sessions_t *s, reader_session_id_t id)
{
    if (!s || !id) return NULL;
    for (unsigned i = 0; i < READER_SESSION_SLOTS; ++i)
        if (s->slots[i].id == id) return &s->slots[i];
    return NULL;
}
static reader_session_slot_t *find(reader_sessions_t *s, reader_session_id_t id)
{ return (reader_session_slot_t *)reader_sessions_get(s, id); }
static bool identity_valid(const char *p, size_t max, bool app)
{
    if (!p || !*p) return false;
    for (size_t i = 0; i < max; ++i)
    {
        unsigned char c = (unsigned char)p[i];
        if (!c) return true;
        if (app && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    }
    return false;
}
static bool running(const reader_sessions_t *s, const reader_session_slot_t *p)
{
    return p && s->active == p->id &&
        (p->phase == READER_SESSION_ACTIVE || p->phase == READER_SESSION_PAUSED);
}
static bool working(const reader_sessions_t *s, reader_session_id_t id)
{
    for (unsigned i = 0; i < READER_WORK_SLOTS; ++i) if (s->work[i].session == id) return true;
    return false;
}
static void release(reader_sessions_t *s, reader_session_slot_t *p)
{
    void *owner = p->owner;
    memset(p, 0, sizeof(*p));
    s->in_hook = true;
    s->hooks.release(owner);
    s->in_hook = false;
}
static void retire(reader_session_slot_t *p)
{
    p->phase = READER_SESSION_CLOSING;
    /* A closing phase rejects all completions even at the generation limit. */
    if (p->generation != UINT32_MAX) ++p->generation;
}
void reader_sessions_init(reader_sessions_t *s, const reader_session_hooks_t *hooks)
{
    memset(s, 0, sizeof(*s));
    if (hooks) s->hooks = *hooks;
}
int reader_sessions_open(reader_sessions_t *s, const reader_session_open_t *r, reader_session_id_t *id)
{
    if (id) *id = 0;
    if (!s || !r || !id || !s->hooks.retain || !s->hooks.release || !s->hooks.save ||
        !identity_valid(r->app_id, sizeof(s->slots[0].app_id), true) ||
        !identity_valid(r->identity, sizeof(s->slots[0].identity), false)) return EPD_READ_INVALID;
    if (s->in_hook || s->stopping || s->sequence == UINT64_MAX) return EPD_READ_BUSY;
    reader_session_slot_t *p = find(s, s->pending);
    if (!p)
        for (unsigned i = 0; i < READER_SESSION_SLOTS; ++i)
            if (!s->slots[i].id) { p = &s->slots[i]; break; }
    if (!p) return EPD_READ_BUSY;
    /* Copy before releasing a replaced request, which may own the input text. */
    reader_session_slot_t next = {.phase = READER_SESSION_WAITING, .generation = 1, .owner = r->owner};
    memcpy(next.app_id, r->app_id, strlen(r->app_id) + 1);
    memcpy(next.identity, r->identity, strlen(r->identity) + 1);
    s->in_hook = true;
    bool retained = s->hooks.retain(r->owner);
    s->in_hook = false;
    if (!retained) return EPD_READ_NO_MEMORY;
    if (p->id) release(s, p);
    next.id = ++s->sequence;
    *p = next; s->pending = p->id; *id = p->id;
    reader_session_slot_t *old = find(s, s->active);
    if (old && old->phase != READER_SESSION_CLOSING) retire(old);
    return EPD_READ_OK;
}
static void save(reader_sessions_t *s, reader_session_slot_t *p)
{
    if (p->saving || p->save_error || p->visible_revision == p->saved_revision) return;
    if (s->sequence == UINT64_MAX) { p->save_error = EPD_READ_BUSY; return; }
    p->save_sequence = ++s->sequence;
    p->save_revision = p->visible_revision;
    p->saving_position = p->visible;
    s->in_hook = true;
    int result = s->hooks.save(p->owner, p->id, p->save_sequence, &p->saving_position);
    s->in_hook = false;
    if (result == EPD_READ_OK) p->saved_revision = p->save_revision;
    else if (result == EPD_READ_WAIT) p->saving = true;
    else p->save_error = result < 0 ? result : EPD_READ_INVALID;
}
reader_session_id_t reader_sessions_process(reader_sessions_t *s)
{
    if (!s || s->in_hook) return 0;
    for (unsigned i = 0; i < READER_SESSION_SLOTS; ++i)
    {
        reader_session_slot_t *p = &s->slots[i];
        if (p->phase == READER_SESSION_CLOSING && !p->bound && !working(s, p->id))
        {
            if (s->active == p->id) s->active = 0;
            p->phase = READER_SESSION_SAVING;
        }
        if (p->phase == READER_SESSION_SAVING || p->phase == READER_SESSION_PAUSED)
            save(s, p);
        if (p->phase == READER_SESSION_SAVING && !p->saving &&
            p->visible_revision == p->saved_revision) release(s, p);
    }
    if (!s->active && s->pending && !s->stopping)
    {
        reader_session_slot_t *p = find(s, s->pending);
        s->active = s->pending; s->pending = 0;
        p->phase = READER_SESSION_ACTIVE;
        return p->id;
    }
    return 0;
}
int reader_sessions_bind(reader_sessions_t *s, reader_session_id_t id, bool bound)
{
    if (!s || s->in_hook) return EPD_READ_BUSY;
    reader_session_slot_t *p = find(s, id);
    if (!p || s->active != id || (bound && p->phase != READER_SESSION_ACTIVE)) return EPD_READ_STALE;
    p->bound = bound;
    return EPD_READ_OK;
}
int reader_sessions_pause(reader_sessions_t *s, reader_session_id_t id, bool paused)
{
    if (!s || s->in_hook) return EPD_READ_BUSY;
    reader_session_slot_t *p = find(s, id);
    if (!running(s, p)) return EPD_READ_STALE;
    p->phase = paused ? READER_SESSION_PAUSED : READER_SESSION_ACTIVE;
    return EPD_READ_OK;
}
int reader_sessions_invalidate(reader_sessions_t *s, reader_session_id_t id)
{
    if (!s || s->in_hook) return EPD_READ_BUSY;
    reader_session_slot_t *p = find(s, id);
    if (!running(s, p)) return EPD_READ_STALE;
    if (p->generation == UINT32_MAX) return EPD_READ_BUSY;
    ++p->generation;
    return EPD_READ_OK;
}
int reader_sessions_close(reader_sessions_t *s, reader_session_id_t id)
{
    if (!s || s->in_hook) return EPD_READ_BUSY;
    reader_session_slot_t *p = find(s, id);
    if (!p) return EPD_READ_STALE;
    if (s->pending == id)
    {
        s->pending = 0; release(s, p); return EPD_READ_OK;
    }
    if (!running(s, p)) return EPD_READ_STALE;
    retire(p);
    return EPD_READ_OK;
}
static bool same_token(reader_work_token_t a, reader_work_token_t b)
{ return a.session && a.session == b.session && a.sequence == b.sequence && a.generation == b.generation; }
int reader_sessions_work_begin(reader_sessions_t *s, reader_session_id_t id, reader_work_token_t *token)
{
    if (!token) return EPD_READ_INVALID;
    *token = (reader_work_token_t){0};
    if (!s || s->in_hook || s->sequence == UINT64_MAX) return EPD_READ_BUSY;
    reader_session_slot_t *p = find(s, id);
    if (!p || s->active != id || p->phase != READER_SESSION_ACTIVE) return EPD_READ_STALE;
    for (unsigned i = 0; i < READER_WORK_SLOTS; ++i)
        if (!s->work[i].session)
        {
            *token = (reader_work_token_t){id, ++s->sequence, p->generation};
            s->work[i] = *token; return EPD_READ_OK;
        }
    return EPD_READ_BUSY;
}
bool reader_sessions_work_current(const reader_sessions_t *s, reader_work_token_t token)
{
    const reader_session_slot_t *p = reader_sessions_get(s, token.session);
    if (!p || s->active != token.session || p->phase != READER_SESSION_ACTIVE ||
        p->generation != token.generation) return false;
    for (unsigned i = 0; i < READER_WORK_SLOTS; ++i) if (same_token(s->work[i], token)) return true;
    return false;
}
int reader_sessions_work_end(reader_sessions_t *s, reader_work_token_t token)
{
    if (!s || s->in_hook) return EPD_READ_BUSY;
    for (unsigned i = 0; i < READER_WORK_SLOTS; ++i)
        if (same_token(s->work[i], token))
        { s->work[i] = (reader_work_token_t){0}; return EPD_READ_OK; }
    return EPD_READ_STALE;
}
int reader_sessions_present(reader_sessions_t *s, reader_session_id_t id,
                             uint32_t generation, const epd_read_anchor_t *position)
{
    if (!s || s->in_hook) return EPD_READ_BUSY;
    if (!position || position->version != EPD_READER_POSITION_VERSION) return EPD_READ_INVALID;
    reader_session_slot_t *p = find(s, id);
    if (!p || s->active != id || p->phase != READER_SESSION_ACTIVE ||
        !p->bound || p->generation != generation) return EPD_READ_STALE;
    if (p->visible_revision && !memcmp(&p->visible, position, sizeof(*position))) return EPD_READ_OK;
    if (p->visible_revision == UINT64_MAX) return EPD_READ_BUSY;
    p->visible = *position; ++p->visible_revision;
    return EPD_READ_OK;
}
int reader_sessions_saved(reader_sessions_t *s, reader_session_id_t id, uint64_t sequence, int result)
{
    if (!s || s->in_hook) return EPD_READ_BUSY;
    reader_session_slot_t *p = find(s, id);
    if (!p || !p->saving || p->save_sequence != sequence) return EPD_READ_STALE;
    if (result != EPD_READ_OK && result >= 0) return EPD_READ_INVALID;
    p->saving = false;
    if (result == EPD_READ_OK) { p->saved_revision = p->save_revision; p->save_error = 0; }
    else p->save_error = result;
    return EPD_READ_OK;
}
void reader_sessions_retry_saves(reader_sessions_t *s)
{
    if (!s || s->in_hook) return;
    for (unsigned i = 0; i < READER_SESSION_SLOTS; ++i) s->slots[i].save_error = 0;
}
int reader_sessions_retry_save(reader_sessions_t *s, reader_session_id_t id)
{
    if (!s || s->in_hook) return EPD_READ_BUSY;
    reader_session_slot_t *p = find(s, id);
    if (!p) return EPD_READ_STALE;
    p->save_error = 0;
    return EPD_READ_OK;
}
bool reader_sessions_flush(reader_sessions_t *s)
{
    if (!s || s->in_hook) return false;
    bool complete = true;
    for (unsigned i = 0; i < READER_SESSION_SLOTS; ++i)
    {
        reader_session_slot_t *p = &s->slots[i];
        if (!p->id) continue;
        save(s, p);
        if (p->saving || p->visible_revision != p->saved_revision) complete = false;
    }
    return complete;
}
void reader_sessions_stop(reader_sessions_t *s)
{
    if (!s || s->in_hook) return;
    s->stopping = true;
    if (s->pending) reader_sessions_close(s, s->pending);
    reader_session_slot_t *p = find(s, s->active);
    if (p) retire(p);
}
bool reader_sessions_empty(const reader_sessions_t *s)
{
    if (!s) return true;
    for (unsigned i = 0; i < READER_SESSION_SLOTS; ++i) if (s->slots[i].id) return false;
    return true;
}
