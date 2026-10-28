/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_provider_private.h"
#include <stdio.h>
#include <string.h>

static bool digits(const char *p, size_t capacity)
{
    if (!sp_field(p, capacity, true, false)) return false;
    for (; *p; ++p) if (*p < '0' || *p > '9') return false;
    return true;
}
static bool url_encode(const char *p, char *out, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *p; ++p) {
        unsigned c = (unsigned char)*p;
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if ((plain ? 1u : 3u) >= capacity - n) return false;
        if (plain) out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = 0; return true;
}
bool sp_tencent_endpoint(speech_provider_session_t *s, const speech_account_t *a, sp_endpoint *e)
{
    unsigned char digest[20], random[4];
    char signature[32], escaped[96], speed[24];
    uint64_t now = sp_utc();
    if (!digits(a->app_id, sizeof(a->app_id)) || !sp_field(a->secret_id, sizeof(a->secret_id), true, false) ||
        (s->kind == EPD_SPEECH_TTS && !digits(a->voice, sizeof(a->voice))))
        return sp_protocol(s, "tencent-account");
    if (!now || now > UINT32_MAX - 3600u) {
        sp_fail(&s->failure, EPD_SPEECH_TLS, "clock-invalid", 0); return false;
    }
    if (!sp_transport_random(s->transport, random, sizeof(random))) return false;
    uint32_t nonce = (((uint32_t)random[0] << 24) | ((uint32_t)random[1] << 16) |
                      ((uint32_t)random[2] << 8) | random[3]) % 2147483646u + 1u;
    strcpy(e->host, s->kind == EPD_SPEECH_ASR ? "asr.cloud.tencent.com" : "tts.cloud.tencent.com");
    int count;
    if (s->kind == EPD_SPEECH_ASR) {
        count = snprintf(e->path, sizeof(e->path),
            "/asr/v2/%s?engine_model_type=16k_zh_en_2.0&expired=%lu&needvad=1&nonce=%lu"
            "&secretid=%s&timestamp=%lu&voice_format=1&voice_id=%s", a->app_id,
            (unsigned long)(now + 3600u), (unsigned long)nonce, a->secret_id, (unsigned long)now, s->id);
    } else {
        int rate = (int)s->rate100, value;
        /* Piecewise interpolation of the documented Speed/rate landmarks. */
        if (rate <= 60) value = -200;
        else if (rate <= 120) value = (rate - 100) * 5;
        else if (rate <= 150) value = 100 + (rate - 120) * 100 / 30;
        else value = 200 + (rate - 150) * 4;
        snprintf(speed, sizeof(speed), "%s%d.%02d", value < 0 ? "-" : "", (value < 0 ? -value : value) / 100,
                 (value < 0 ? -value : value) % 100);
        count = snprintf(e->path, sizeof(e->path),
            "/stream_wsv2?Action=TextToStreamAudioWSv2&AppId=%s&Codec=pcm&Expired=%lu&SampleRate=16000"
            "&SecretId=%s&SessionId=%s&Speed=%s&Timestamp=%lu&VoiceType=%s",
            a->app_id, (unsigned long)(now + 3600u), a->secret_id, s->id, speed, (unsigned long)now, a->voice);
    }
    if (count < 0 || (size_t)count >= sizeof(e->path)) return sp_protocol(s, "tencent-url-size");
    size_t capacity = strlen(e->path) + strlen(e->host) + 4;
    char *canonical = speech_alloc(capacity);
    if (!canonical) { sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "signature-alloc", 0); return false; }
    snprintf(canonical, capacity, "%s%s%s", s->kind == EPD_SPEECH_TTS ? "GET" : "", e->host, e->path);
    bool ok = sp_hmac_sha1(a->secret, canonical, digest) && sp_base64(digest, sizeof(digest), signature, sizeof(signature)) &&
              url_encode(signature, escaped, sizeof(escaped));
    sp_wipe(canonical, capacity); speech_free(canonical); sp_wipe(digest, sizeof(digest));
    if (!ok) return sp_protocol(s, "tencent-signature");
    size_t used = strlen(e->path);
    count = snprintf(e->path + used, sizeof(e->path) - used, "&%s=%s", s->kind == EPD_SPEECH_ASR ? "signature" : "Signature", escaped);
    sp_wipe(signature, sizeof(signature)); sp_wipe(escaped, sizeof(escaped));
    return (count >= 0 && (size_t)count < sizeof(e->path) - used) || sp_protocol(s, "signature-url-size");
}
static bool sentence(speech_provider_session_t *s, sp_json v)
{
    int64_t id, state;
    if (v.type != '{' || !sp_json_i64(sp_json_get(v, "sentence_id"), &id) || id < 0 || id > UINT32_MAX ||
        !sp_json_i64(sp_json_get(v, "sentence_type"), &state) || (state != 0 && state != 1))
        return sp_protocol(s, "tencent-sentence");
    sp_json text = sp_json_get(v, "sentence");
    /* The official V2 example also names the transcript field text. */
    if (!text.p) text = sp_json_get(v, "text");
    return sp_transcript(s, (uint32_t)id, state == 1, text);
}
bool sp_tencent_message(speech_provider_session_t *s, sp_json root)
{
    int64_t code, flag = 0;
    if (!sp_json_i64(sp_json_get(root, "code"), &code)) return sp_protocol(s, "tencent-code");
    sp_json_id(sp_json_get(root, "request_id"), s->failure.request_id, sizeof(s->failure.request_id));
    if (code) return sp_server_error(s, code, NULL);
    sp_json identity = sp_json_get(root, s->kind == EPD_SPEECH_ASR ? "voice_id" : "session_id");
    if (!sp_json_is(identity, s->id)) return sp_protocol(s, "tencent-task-id");
    if (!s->failure.request_id[0]) sp_safe_id(s->failure.request_id, sizeof(s->failure.request_id), s->id, strlen(s->id));
    sp_json final = sp_json_get(root, "final");
    if (final.p && (!sp_json_i64(final, &flag) || (flag != 0 && flag != 1))) return sp_protocol(s, "tencent-final");
    if (s->kind == EPD_SPEECH_ASR) {
        sp_json sentences = sp_json_get(root, "sentences");
        if (!s->ready) {
            if (flag || (sentences.p && sentences.type != 'n')) return sp_protocol(s, "tencent-ready-order");
            s->ready = true; return true;
        }
        if (sentences.type == '{') { if (!sentence(s, sentences)) return false; }
        else if (sentences.type == '[') {
            for (unsigned i = 0;; ++i) {
                sp_json item = sp_json_at(sentences, i);
                if (!item.p) break;
                if (!sentence(s, item)) return false;
            }
        } else if (sentences.p && sentences.type != 'n') return sp_protocol(s, "tencent-sentences-type");
    } else {
        sp_json ready = sp_json_get(root, "ready");
        int64_t r;
        if (ready.p) {
            if (!sp_json_i64(ready, &r) || (r != 0 && r != 1)) return sp_protocol(s, "tencent-ready");
            if (r) {
                if (s->ready) return sp_protocol(s, "tencent-ready-repeat");
                s->ready = true;
            }
        }
    }
    return !flag || sp_complete(s);
}
bool sp_tencent_send(speech_provider_session_t *s, const char *quoted, bool finish)
{
    if (s->kind == EPD_SPEECH_ASR) return sp_send_json(s, "{\"type\":\"end\"}");
    char message_id[37];
    if (!sp_uuid(s, message_id)) return false;
    size_t capacity = (quoted ? strlen(quoted) : 2) + 256;
    char *body = speech_alloc(capacity);
    if (!body) { sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "tencent-message-alloc", 0); return false; }
    int n = snprintf(body, capacity, "{\"session_id\":\"%s\",\"message_id\":\"%s\",\"action\":\"%s\",\"data\":%s}",
        s->id, message_id, finish ? "ACTION_COMPLETE" : "ACTION_SYNTHESIS", quoted ? quoted : "\"\"");
    bool ok = n >= 0 && (size_t)n < capacity && sp_send_json(s, body);
    if (!ok && !s->failure.error) sp_protocol(s, "tencent-message-size");
    sp_wipe(body, capacity); speech_free(body); return ok;
}
