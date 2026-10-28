/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_input.h"
#include <string.h>

void speech_input_init(speech_input_t *s, char *draft, size_t capacity,
                       speech_input_commit_t commit, void *user)
{
    memset(s, 0, sizeof(*s));
    s->draft = draft; s->capacity = capacity; s->commit = commit; s->user = user;
    if (draft && capacity) draft[0] = 0;
}
epd_speech_error_t speech_input_start(speech_input_t *s, epd_service_t *owner,
                                     const epd_speech_request_t *request, uint32_t target)
{
    if (s->active) return EPD_SPEECH_BUSY;
    if (!s->draft || !s->capacity || !s->commit) return s->error = EPD_SPEECH_INVALID;
    epd_speech_session_t session = 0;
    s->error = epd_speech_start(owner, EPD_SPEECH_ASR, request, &session);
    if (s->error != EPD_SPEECH_OK) return s->error;
    memset(&s->snapshot, 0, sizeof(s->snapshot));
    s->snapshot.session = session; s->snapshot.kind = EPD_SPEECH_ASR;
    s->snapshot.state = EPD_SPEECH_PREPARING; s->snapshot.limit_ms = EPD_SPEECH_ASR_LIMIT_MS;
    s->owner = owner; s->target = target; s->active = s->accepting = true;
    s->have_final = s->have_sentence = false; s->draft[0] = 0;
    return EPD_SPEECH_OK;
}
void speech_input_finish(speech_input_t *s)
{
    if (!s->active || !s->accepting) return;
    epd_speech_error_t e = epd_speech_finish(s->owner, s->snapshot.session);
    if (e == EPD_SPEECH_OK) s->snapshot.state = EPD_SPEECH_FINALIZING;
    else if (e != EPD_SPEECH_NOT_FOUND) s->error = e;
}
void speech_input_cancel(speech_input_t *s)
{
    s->accepting = false;
    if (s->draft && s->capacity) s->draft[0] = 0;
    if (s->active) {
        epd_speech_error_t e = epd_speech_cancel(s->owner, s->snapshot.session);
        if (e == EPD_SPEECH_OK) s->snapshot.state = EPD_SPEECH_STOPPING;
        else if (e != EPD_SPEECH_NOT_FOUND) s->error = e;
    }
}
bool speech_input_event(speech_input_t *s, const epd_speech_event_t *e)
{
    if (!s->active || e->snapshot.session != s->snapshot.session || e->snapshot.kind != EPD_SPEECH_ASR)
        return false;
    s->snapshot = e->snapshot;
    if (e->kind == EPD_SPEECH_EVENT_TERMINAL) {
        s->active = s->accepting = false; s->draft[0] = 0;
        if ((e->snapshot.error != EPD_SPEECH_OK && e->snapshot.error != EPD_SPEECH_CANCELLED) ||
            s->error == EPD_SPEECH_OK) s->error = e->snapshot.error;
        return true;
    }
    if (!s->accepting || !e->text || (s->have_final && e->sentence <= s->final_sentence)) return true;
    if (e->kind == EPD_SPEECH_EVENT_FINAL_TEXT) {
        epd_speech_error_t error = s->commit(s->user, s->target, e->text, e->text_bytes);
        s->have_final = true; s->final_sentence = e->sentence;
        if (!s->have_sentence || e->sentence >= s->sentence) s->draft[0] = 0;
        if (error != EPD_SPEECH_OK) { s->error = error; speech_input_cancel(s); }
    } else if (e->kind == EPD_SPEECH_EVENT_DRAFT) {
        if (s->have_sentence && (e->sentence < s->sentence ||
            (e->sentence == s->sentence && e->revision <= s->revision))) return true;
        if (e->text_bytes >= s->capacity) {
            s->error = EPD_SPEECH_TEXT_TOO_LONG; speech_input_cancel(s); return true;
        }
        memcpy(s->draft, e->text, e->text_bytes); s->draft[e->text_bytes] = 0;
        s->sentence = e->sentence; s->revision = e->revision; s->have_sentence = true;
    }
    return true;
}
void speech_input_refresh(speech_input_t *s)
{
    if (!s->active) return;
    epd_speech_snapshot_t snapshot;
    epd_speech_error_t error = epd_speech_snapshot(s->owner, s->snapshot.session, &snapshot);
    if (error == EPD_SPEECH_OK)
        s->snapshot = snapshot;
    else if (error == EPD_SPEECH_NOT_FOUND) {
        /* Global shutdown can retire the session without publishing a terminal
         * event to its owner. Missing snapshot confirms cleanup has finished. */
        s->active = s->accepting = false;
        s->draft[0] = 0;
        s->snapshot.state = EPD_SPEECH_STOPPED;
        s->snapshot.reason = EPD_SPEECH_OWNER_STOP;
        if (s->error == EPD_SPEECH_OK) s->error = EPD_SPEECH_CANCELLED;
    }
}
bool speech_input_merge(const char *previous, const char *current, char *edited, size_t capacity)
{
    if (!previous || !current || !edited || !capacity) return false;
    size_t base = strlen(previous), size = strlen(current), length = strlen(edited);
    if (length >= capacity || size < base || memcmp(previous, current, base) || size - base >= capacity - length)
        return false;
    memcpy(edited + length, current + base, size - base + 1);
    return true;
}
