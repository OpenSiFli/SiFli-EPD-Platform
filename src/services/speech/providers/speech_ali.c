/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_provider_private.h"
#include <stdio.h>
#include <string.h>

bool sp_ali_endpoint(speech_provider_session_t *s, const speech_account_t *a, sp_endpoint *e)
{
    if (!sp_field(a->workspace, sizeof(a->workspace), true, true)) return sp_protocol(s, "ali-workspace");
    if (a->workspace[0]) {
        for (const char *p = a->workspace; *p; ++p)
            if (*p == '.' || *p == '_') return sp_protocol(s, "ali-workspace-label");
        size_t length = strlen(a->workspace);
        if (length > 63 || a->workspace[0] == '-' || a->workspace[length - 1] == '-') return sp_protocol(s, "ali-workspace-label");
        snprintf(e->host, sizeof(e->host), "%s.cn-beijing.maas.aliyuncs.com", a->workspace);
    } else strcpy(e->host, "dashscope.aliyuncs.com");
    strcpy(e->path, "/api-ws/v1/inference");
    int n = snprintf(e->headers, sizeof(e->headers), "Authorization: Bearer %s\r\n", a->secret);
    return (n >= 0 && (size_t)n < sizeof(e->headers)) || sp_protocol(s, "ali-header-size");
}
bool sp_ali_start(speech_provider_session_t *s)
{
    char body[1024], parameters[512];
    const char *hint = s->language == EPD_SPEECH_ZH ? ",\"language_hints\":[\"zh\"]" :
                       s->language == EPD_SPEECH_EN ? ",\"language_hints\":[\"en\"]" : "";
    if (s->kind == EPD_SPEECH_ASR) snprintf(parameters, sizeof(parameters),
        "{\"format\":\"pcm\",\"sample_rate\":16000%s}", hint);
    else snprintf(parameters, sizeof(parameters),
        "{\"text_type\":\"PlainText\",\"voice\":\"%s\",\"format\":\"pcm\",\"sample_rate\":16000,"
        "\"rate\":%u.%02u,\"enable_ssml\":false%s}", s->voice, s->rate100 / 100, s->rate100 % 100, hint);
    int n = snprintf(body, sizeof(body),
        "{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
        "\"payload\":{\"task_group\":\"audio\",\"task\":\"%s\",\"function\":\"%s\",\"model\":\"%s\","
        "\"parameters\":%s,\"input\":{}}}", s->id, s->kind == EPD_SPEECH_ASR ? "asr" : "tts",
        s->kind == EPD_SPEECH_ASR ? "recognition" : "SpeechSynthesizer",
        s->kind == EPD_SPEECH_ASR ? "fun-asr-realtime" : "cosyvoice-v3-flash", parameters);
    if (n < 0 || (size_t)n >= sizeof(body)) return sp_protocol(s, "ali-start-size");
    return sp_send_json(s, body);
}
bool sp_ali_send(speech_provider_session_t *s, const char *quoted, bool finish)
{
    size_t capacity = (quoted ? strlen(quoted) : 0) + 256;
    char *body = speech_alloc(capacity);
    if (!body) { sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "ali-message-alloc", 0); return false; }
    int n = snprintf(body, capacity,
        "{\"header\":{\"action\":\"%s\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
        "\"payload\":{\"input\":{%s%s}}}", finish ? "finish-task" : "continue-task", s->id,
        finish ? "" : "\"text\":", finish ? "" : quoted);
    bool ok = n >= 0 && (size_t)n < capacity && sp_send_json(s, body);
    if (!ok && !s->failure.error) sp_protocol(s, "ali-message-size");
    sp_wipe(body, capacity); speech_free(body); return ok;
}
bool sp_ali_message(speech_provider_session_t *s, sp_json root)
{
    sp_json header = sp_json_get(root, "header"), payload = sp_json_get(root, "payload");
    sp_json event = sp_json_get(header, "event");
    if (!sp_json_is(sp_json_get(header, "task_id"), s->id)) return sp_protocol(s, "ali-task-id");
    sp_json_id(sp_json_get(header, "request_id"), s->failure.request_id, sizeof(s->failure.request_id));
    if (!s->failure.request_id[0]) sp_safe_id(s->failure.request_id, sizeof(s->failure.request_id), s->id, strlen(s->id));
    if (sp_json_is(event, "task-failed")) {
        char code[96] = {0}; int64_t number = 0;
        sp_json error = sp_json_get(header, "error_code");
        if (error.type == '"') sp_json_id(error, code, sizeof(code));
        else (void)sp_json_i64(error, &number);
        return sp_server_error(s, number, code);
    }
    if (sp_json_is(event, "task-started")) {
        if (s->ready) return sp_protocol(s, "ali-ready-repeat");
        s->ready = true; return true;
    }
    if (sp_json_is(event, "task-finished")) return sp_complete(s);
    if (!s->ready) return sp_protocol(s, "ali-before-ready");
    if (sp_json_is(event, "result-generated")) {
        if (s->kind == EPD_SPEECH_TTS) return true;
        sp_json sentence = sp_json_get(sp_json_get(payload, "output"), "sentence");
        bool heartbeat = false, final;
        int64_t id;
        (void)sp_json_bool(sp_json_get(sentence, "heartbeat"), &heartbeat);
        if (heartbeat) return true;
        if (!sp_json_i64(sp_json_get(sentence, "sentence_id"), &id) || id < 1 || id > UINT32_MAX ||
            !sp_json_bool(sp_json_get(sentence, "sentence_end"), &final)) return sp_protocol(s, "ali-sentence");
        return sp_transcript(s, (uint32_t)id, final, sp_json_get(sentence, "text"));
    }
    return sp_protocol(s, "ali-event");
}
