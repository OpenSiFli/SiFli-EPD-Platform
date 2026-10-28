/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_provider_private.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

static void set_deadline(speech_provider_session_t *s, uint32_t timeout);

bool sp_field(const char *s, size_t capacity, bool token, bool empty)
{
    const char *end = memchr(s, 0, capacity);
    if (!end || (!empty && end == s)) return false;
    while (s != end) {
        unsigned c = (unsigned char)*s++;
        if (c < 33 || c > 126) return false;
        if (token && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}
bool sp_protocol(speech_provider_session_t *s, const char *stage)
{ sp_fail(&s->failure, EPD_SPEECH_PROTOCOL, stage, 0); return false; }
bool sp_uuid(speech_provider_session_t *s, char out[37])
{
    unsigned char bytes[16];
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    if (!sp_transport_random(s->transport, bytes, sizeof(bytes))) return false;
    bytes[6] = (unsigned char)((bytes[6] & 15u) | 0x40u);
    bytes[8] = (unsigned char)((bytes[8] & 63u) | 0x80u);
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
        out[n++] = hex[bytes[i] >> 4]; out[n++] = hex[bytes[i] & 15];
    }
    out[n] = 0; return true;
}
void sp_json_id(sp_json value, char *out, size_t capacity)
{
    char text[128]; size_t n;
    if (sp_json_string(value, text, sizeof(text), &n)) sp_safe_id(out, capacity, text, n);
}
bool sp_server_error(speech_provider_session_t *s, int64_t code, const char *name)
{
    epd_speech_error_t error = EPD_SPEECH_PROTOCOL;
    if (s->provider == SPEECH_TENCENT) {
        if ((s->kind == EPD_SPEECH_ASR && (code == 4002 || code == 4003)) ||
            (s->kind == EPD_SPEECH_TTS && code == 10003)) error = EPD_SPEECH_AUTH;
        else if (s->kind == EPD_SPEECH_ASR && (code == 4004 || code == 4005)) error = EPD_SPEECH_QUOTA;
        else if ((s->kind == EPD_SPEECH_ASR && code == 4006) ||
                 (s->kind == EPD_SPEECH_TTS && code == 10002)) error = EPD_SPEECH_BUSY;
        else if ((s->kind == EPD_SPEECH_ASR && code == 4008) || code == 10004 || code == 20003) error = EPD_SPEECH_TIMEOUT;
        else if (s->kind == EPD_SPEECH_TTS && code == 10007) error = EPD_SPEECH_TEXT_TOO_LONG;
    } else if (name) {
        if (strstr(name, "InvalidApiKey") || strstr(name, "Unauthorized") || strstr(name, "AccessDenied") ||
            strstr(name, "Authentication") || strstr(name, "Forbidden")) error = EPD_SPEECH_AUTH;
        else if (strstr(name, "Quota") || strstr(name, "Arrearage") || strstr(name, "Payment") ||
                 strstr(name, "Balance") || strstr(name, "FreeTier")) error = EPD_SPEECH_QUOTA;
        else if (strstr(name, "Throttl") || strstr(name, "RateLimit")) error = EPD_SPEECH_BUSY;
        else if (strstr(name, "ModelNotFound") || strstr(name, "Unsupported")) error = EPD_SPEECH_UNSUPPORTED;
        else if (strstr(name, "Timeout")) error = EPD_SPEECH_TIMEOUT;
    }
    if (code == 401 || code == 403) error = EPD_SPEECH_AUTH;
    if (code == 402) error = EPD_SPEECH_QUOTA;
    if (code == 429) error = EPD_SPEECH_BUSY;
    if (!s->failure.error) {
        if (name && *name) sp_safe_id(s->failure.code, sizeof(s->failure.code), name, strlen(name));
        if (!s->failure.code[0]) snprintf(s->failure.code, sizeof(s->failure.code), "%lld", (long long)code);
    }
    sp_fail(&s->failure, error, s->ready ? "provider-task" : "provider-ready",
            code >= INT_MIN && code <= INT_MAX ? (int)code : 0);
    return false;
}
bool sp_send_json(speech_provider_session_t *s, const char *json)
{ return sp_ws_send(s->ws, 1, json, strlen(json), &s->guard); }
bool sp_transcript(speech_provider_session_t *s, uint32_t sentence, bool final, sp_json text)
{
    size_t n;
    if (!s->ready || s->done || s->kind != EPD_SPEECH_ASR) return sp_protocol(s, "asr-event-state");
    if (s->have_final && sentence <= s->last_sentence) return true;
    if (text.type != '"') return sp_protocol(s, "asr-text-field");
    char *decoded = speech_alloc(text.n + 1);
    if (!decoded) { sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "asr-text-alloc", 0); return false; }
    bool ok = sp_json_string(text, decoded, text.n + 1, &n);
    if (!ok) sp_protocol(s, "asr-text-escape");
    else if (s->sink.text && !s->sink.text(s->sink.user, sentence, final, decoded, n)) {
        sp_fail(&s->failure, EPD_SPEECH_CANCELLED, "text-consumer", 0); ok = false;
    }
    sp_wipe(decoded, text.n + 1); speech_free(decoded);
    if (ok && final) { s->have_final = true; s->last_sentence = sentence; }
    return ok;
}
bool sp_pcm(speech_provider_session_t *s, const void *p, size_t n)
{
    const unsigned char *bytes = p;
    if (!s->ready || !s->submitted || s->done || s->kind != EPD_SPEECH_TTS) return sp_protocol(s, "pcm-event-state");
    if (n > EPD_SPEECH_PCM_SLOT_BYTES - s->pcm_bytes) {
        sp_fail(&s->failure, EPD_SPEECH_AUDIO_TOO_LARGE, "pcm-capacity", 0); return false;
    }
    s->pcm_bytes += n;
    if (n) {
        s->progress = speech_now_ms();
        set_deadline(s, SP_PROGRESS_MS);
    }
    if (s->pcm_odd && n) {
        unsigned char pair[2] = {s->pcm_tail, *bytes++};
        --n; s->pcm_odd = false;
        if (s->sink.pcm && !s->sink.pcm(s->sink.user, pair, 2)) goto cancelled;
    }
    if (n & 1u) { s->pcm_tail = bytes[n - 1]; s->pcm_odd = true; --n; }
    if (n && s->sink.pcm && !s->sink.pcm(s->sink.user, bytes, n)) goto cancelled;
    return true;
cancelled:
    sp_fail(&s->failure, EPD_SPEECH_CANCELLED, "pcm-consumer", 0); return false;
}
bool sp_complete(speech_provider_session_t *s)
{
    if (!s->ready || !s->finished || s->done) return sp_protocol(s, "premature-completion");
    if (s->kind == EPD_SPEECH_TTS && (!s->pcm_bytes || s->pcm_odd)) return sp_protocol(s, "pcm-incomplete");
    s->done = true; return true;
}
static bool received(void *user, unsigned opcode, const void *p, size_t n, bool begin, bool end)
{
    speech_provider_session_t *s = user;
    if (s->done) return sp_protocol(s, "data-after-completion");
    if (s->provider == SPEECH_DOUBAO) {
        if (opcode != 2) return sp_protocol(s, "doubao-ws-type");
        return sp_doubao_data(s, p, n, begin, end);
    }
    if (opcode == 2) return sp_pcm(s, p, n);
    if (begin) s->json_used = 0;
    if (n > SP_JSON_MAX - s->json_used) return sp_protocol(s, "json-message-size");
    if (!s->json) {
        s->json = speech_alloc(SP_JSON_MAX + 1);
        if (!s->json) { sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "json-alloc", 0); return false; }
    }
    if (n) memcpy(s->json + s->json_used, p, n);
    s->json_used += n;
    if (!end) return true;
    s->json[s->json_used] = 0;
    sp_json root;
    if (!sp_json_parse(s->json, s->json_used, &root) || root.type != '{') return sp_protocol(s, "json-invalid");
    return s->provider == SPEECH_TENCENT ? sp_tencent_message(s, root) : sp_ali_message(s, root);
}
static bool deadlines(speech_provider_session_t *s)
{
    uint32_t now = speech_now_ms();
    if (s->failure.error) return false;
    if (s->guard.cancel && s->guard.cancel(s->guard.user)) {
        sp_fail(&s->failure, EPD_SPEECH_CANCELLED, "cancel", 0); return false;
    }
    if (s->done) return true;
    const char *stage = NULL;
    if (!s->ready && now - s->opened >= SP_OPEN_MS) stage = "task-ready-timeout";
    if (s->kind == EPD_SPEECH_TTS && s->submitted) {
        if (now - s->submitted_at >= SP_TASK_MS) stage = "tts-total-timeout";
        else if (now - s->progress >= SP_PROGRESS_MS) stage = "tts-progress-timeout";
    }
    if (s->kind == EPD_SPEECH_ASR && s->finished && now - s->finished_at >= SP_FINAL_MS) stage = "asr-final-timeout";
    if (stage) { sp_fail(&s->failure, EPD_SPEECH_TIMEOUT, stage, 0); return false; }
    return true;
}
static void set_deadline(speech_provider_session_t *s, uint32_t timeout)
{
    uint32_t now = speech_now_ms(), remaining = timeout;
    if (!s->ready) remaining = SP_OPEN_MS - (now - s->opened);
    if (s->kind == EPD_SPEECH_TTS && s->submitted) {
        uint32_t total = SP_TASK_MS - (now - s->submitted_at), idle = SP_PROGRESS_MS - (now - s->progress);
        if (total < remaining) remaining = total;
        if (idle < remaining) remaining = idle;
    }
    if (s->kind == EPD_SPEECH_ASR && s->finished) {
        uint32_t final = SP_FINAL_MS - (now - s->finished_at);
        if (final < remaining) remaining = final;
    }
    s->guard.deadline = now + remaining;
}
static void destroy(speech_provider_session_t *s)
{
    if (!s) return;
    sp_ws_free(s->ws); sp_transport_free(s->transport);
    if (s->json) { sp_wipe(s->json, SP_JSON_MAX + 1); speech_free(s->json); }
    sp_wipe(s, sizeof(*s)); speech_free(s);
}
epd_speech_error_t speech_provider_open(const speech_account_t *a, epd_speech_kind_t kind,
    epd_speech_language_t language, speech_cancel_fn cancel, void *cancel_user,
    const speech_provider_sink_t *sink, speech_provider_session_t **out, speech_failure_t *failure)
{
    speech_failure_t invalid = {0};
    if (failure) memset(failure, 0, sizeof(*failure));
    if (out) *out = NULL;
    if (!a || !sink || !out || !a->enabled || (unsigned)kind > EPD_SPEECH_TTS || (unsigned)language > EPD_SPEECH_EN ||
        a->provider <= SPEECH_NONE || a->provider >= SPEECH_PROVIDER_COUNT ||
        (a->provider == SPEECH_DOUBAO && kind != EPD_SPEECH_TTS) ||
        !sp_field(a->secret, sizeof(a->secret), false, false) ||
        (kind == EPD_SPEECH_TTS && (!sp_field(a->voice, sizeof(a->voice), true, false) ||
                                   !(a->rate >= 0.5f && a->rate <= 2.0f)))) {
        sp_fail(&invalid, EPD_SPEECH_INVALID, "account-invalid", 0);
        if (failure) *failure = invalid;
        return invalid.error;
    }
    speech_provider_session_t *s = speech_alloc(sizeof(*s));
    if (!s) {
        sp_fail(&invalid, EPD_SPEECH_NO_MEMORY, "session-alloc", 0);
        if (failure) *failure = invalid;
        return invalid.error;
    }
    memset(s, 0, sizeof(*s)); s->provider = a->provider; s->kind = kind; s->language = language;
    s->sink = *sink; s->opened = speech_now_ms(); s->guard.cancel = cancel; s->guard.user = cancel_user;
    s->guard.failure = &s->failure; s->guard.deadline = s->opened + SP_OPEN_MS;
    if (kind == EPD_SPEECH_TTS) { strcpy(s->voice, a->voice); s->rate100 = (unsigned)(a->rate * 100.0f + 0.5f); }
    sp_endpoint *e = speech_alloc(sizeof(*e));
    if (!e) { sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "endpoint-alloc", 0); goto end; }
    memset(e, 0, sizeof(*e));
    if (!sp_transport_new(&s->transport, &s->guard) || !sp_uuid(s, s->id)) goto end;
    bool ok = s->provider == SPEECH_TENCENT ? sp_tencent_endpoint(s, a, e) :
              s->provider == SPEECH_ALI ? sp_ali_endpoint(s, a, e) : sp_doubao_endpoint(s, a, e);
    if (!ok || !sp_transport_connect(s->transport, e->host, &s->guard)) goto end;
    s->ws = sp_ws_new(s->transport, &s->failure);
    if (!s->ws || !sp_ws_upgrade(s->ws, e->host, e->path, e->headers, &s->guard)) goto end;
    sp_wipe(e, sizeof(*e));
    if (s->provider == SPEECH_ALI && !sp_ali_start(s)) goto end;
    if (s->provider == SPEECH_DOUBAO && !sp_doubao_start(s, false)) goto end;
    while (!s->ready && deadlines(s)) {
        if (sp_ws_poll(s->ws, 20, &s->guard, received, s) < 0) {
            sp_fail(&s->failure, EPD_SPEECH_CONNECT, "task-ready-closed", 0); break;
        }
        if (s->provider == SPEECH_DOUBAO && s->connection_ready && !s->ready && !s->session_started) {
            if (!sp_doubao_start(s, true)) break;
            s->session_started = true;
        }
    }
end:
    if (e) { sp_wipe(e, sizeof(*e)); speech_free(e); }
    if (!s->ready || s->failure.error) {
        if (!s->failure.error) sp_protocol(s, "task-ready-missing");
        epd_speech_error_t error = s->failure.error;
        if (failure) *failure = s->failure;
        destroy(s); return error;
    }
    s->submitted = false; *out = s; return EPD_SPEECH_OK;
}
epd_speech_error_t speech_provider_audio(speech_provider_session_t *s, const void *pcm, size_t bytes)
{
    if (!s || s->kind != EPD_SPEECH_ASR || !pcm || !bytes || bytes > 6400 || (bytes & 1u)) return EPD_SPEECH_INVALID;
    if (!deadlines(s)) return s->failure.error;
    if (!s->ready || s->finished || s->done) return EPD_SPEECH_INVALID;
    set_deadline(s, 2000);
    (void)sp_ws_send(s->ws, 2, pcm, bytes, &s->guard);
    return s->failure.error;
}
epd_speech_error_t speech_provider_text(speech_provider_session_t *s, const char *text, size_t bytes)
{
    size_t characters;
    if (!s || s->kind != EPD_SPEECH_TTS || !text || !bytes) return EPD_SPEECH_INVALID;
    if (!deadlines(s)) return s->failure.error;
    if (!s->ready || s->submitted || s->finished || s->done) return EPD_SPEECH_INVALID;
    if (bytes > EPD_SPEECH_UNIT_CHARACTERS * 4u) return EPD_SPEECH_TEXT_TOO_LONG;
    if (!sp_utf8_text(text, bytes, &characters)) return EPD_SPEECH_INVALID;
    if (characters > EPD_SPEECH_UNIT_CHARACTERS) return EPD_SPEECH_TEXT_TOO_LONG;
    size_t capacity = bytes * 6u + 3u;
    char *quoted = speech_alloc(capacity);
    if (!quoted) return sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "text-quote-alloc", 0);
    if (!sp_json_quote(text, bytes, quoted, capacity)) {
        speech_free(quoted); return sp_fail(&s->failure, EPD_SPEECH_INVALID, "text-quote", 0);
    }
    s->submitted = true; s->submitted_at = s->progress = speech_now_ms();
    set_deadline(s, SP_PROGRESS_MS);
    if (s->provider == SPEECH_TENCENT) (void)sp_tencent_send(s, quoted, false);
    else if (s->provider == SPEECH_ALI) (void)sp_ali_send(s, quoted, false);
    else (void)sp_doubao_send(s, 200, quoted);
    sp_wipe(quoted, capacity); speech_free(quoted);
    return s->failure.error;
}
epd_speech_error_t speech_provider_finish(speech_provider_session_t *s)
{
    if (!s) return EPD_SPEECH_INVALID;
    if (!deadlines(s)) return s->failure.error;
    if (s->finished) return EPD_SPEECH_OK;
    if (!s->ready || (s->kind == EPD_SPEECH_TTS && !s->submitted)) return EPD_SPEECH_INVALID;
    s->finished = true; s->finished_at = speech_now_ms(); set_deadline(s, SP_PROGRESS_MS);
    if (s->provider == SPEECH_TENCENT) (void)sp_tencent_send(s, NULL, true);
    else if (s->provider == SPEECH_ALI) (void)sp_ali_send(s, NULL, true);
    else (void)sp_doubao_send(s, 102, NULL);
    return s->failure.error;
}
epd_speech_error_t speech_provider_poll(speech_provider_session_t *s, uint32_t wait_ms, bool *done)
{
    if (!s || !done) return EPD_SPEECH_INVALID;
    *done = false;
    if (!deadlines(s)) return s->failure.error;
    if (s->done) { *done = true; return EPD_SPEECH_OK; }
    if (wait_ms > SP_PROGRESS_MS) wait_ms = SP_PROGRESS_MS;
    set_deadline(s, SP_PROGRESS_MS);
    int r = sp_ws_poll(s->ws, wait_ms, &s->guard, received, s);
    if (r < 0 && !s->failure.error) sp_fail(&s->failure, EPD_SPEECH_CONNECT, "task-closed", 0);
    (void)deadlines(s); *done = s->done && !s->failure.error; return s->failure.error;
}
void speech_provider_failure(speech_provider_session_t *s, speech_failure_t *out)
{ if (out) { if (s) *out = s->failure; else memset(out, 0, sizeof(*out)); } }
void speech_provider_close(speech_provider_session_t *s)
{
    if (!s) return;
    if (!s->failure.error && !(s->guard.cancel && s->guard.cancel(s->guard.user))) {
        s->guard.deadline = speech_now_ms() + SP_CLOSE_MS - 50u;
        if (s->provider == SPEECH_DOUBAO && s->connection_ready) {
            if (!s->done && s->ready) (void)sp_doubao_send(s, 101, NULL);
            (void)sp_doubao_send(s, 2, NULL);
        }
        sp_ws_close(s->ws, &s->guard);
    }
    destroy(s);
}
