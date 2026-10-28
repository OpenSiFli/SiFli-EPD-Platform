/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_provider_private.h"
#include <stdio.h>
#include <string.h>

static uint32_t be32(const unsigned char *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put32(unsigned char *p, uint32_t v)
{ p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
bool sp_doubao_endpoint(speech_provider_session_t *s, const speech_account_t *a, sp_endpoint *e)
{
    strcpy(e->host, "openspeech.bytedance.com"); strcpy(e->path, "/api/v3/tts/bidirection");
    int n = snprintf(e->headers, sizeof(e->headers),
        "X-Api-Key: %s\r\nX-Api-Resource-Id: seed-tts-2.0\r\nX-Api-Connect-Id: %s\r\n", a->secret, s->id);
    return (n >= 0 && (size_t)n < sizeof(e->headers)) || sp_protocol(s, "doubao-header-size");
}
bool sp_doubao_send(speech_provider_session_t *s, unsigned event, const char *quoted)
{
    bool session = event >= 100;
    size_t json_capacity = (quoted ? strlen(quoted) : 0) + 512;
    char *body = speech_alloc(json_capacity);
    if (!body) { sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "doubao-message-alloc", 0); return false; }
    int n;
    if (event == 100 || event == 200) n = snprintf(body, json_capacity,
        "{\"user\":{\"uid\":\"epd-reader\"},\"event\":%u,\"namespace\":\"BidirectionalTTS\","
        "\"req_params\":{\"speaker\":\"%s\",\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000,"
        "\"speech_rate\":%d}%s%s}}", event, s->voice, (int)s->rate100 - 100,
        quoted ? ",\"text\":" : "", quoted ? quoted : "");
    else n = snprintf(body, json_capacity, "{}");
    if (n < 0 || (size_t)n >= json_capacity) { speech_free(body); return sp_protocol(s, "doubao-json-size"); }
    size_t prefix = session ? 12u + strlen(s->id) : 8u;
    size_t size = prefix + 4u + (size_t)n;
    unsigned char *packet = speech_alloc(size);
    if (!packet) { sp_wipe(body, json_capacity); speech_free(body); sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "doubao-packet-alloc", 0); return false; }
    packet[0] = 0x11; packet[1] = 0x14; packet[2] = 0x10; packet[3] = 0;
    put32(packet + 4, event);
    if (session) { put32(packet + 8, (uint32_t)strlen(s->id)); memcpy(packet + 12, s->id, strlen(s->id)); }
    put32(packet + prefix, (uint32_t)n); memcpy(packet + prefix + 4, body, (size_t)n);
    bool ok = sp_ws_send(s->ws, 2, packet, size, &s->guard);
    sp_wipe(packet, size); speech_free(packet); sp_wipe(body, json_capacity); speech_free(body); return ok;
}
bool sp_doubao_start(speech_provider_session_t *s, bool session)
{ return sp_doubao_send(s, session ? 100 : 1, NULL); }
static bool event_done(speech_provider_session_t *s)
{
    if (s->binary_type == 11) return true;
    sp_json root;
    if (!s->json || !sp_json_parse(s->json, s->json_used, &root) || root.type != '{') return sp_protocol(s, "doubao-json");
    if (s->binary_type == 15 || s->binary_event == 51 || s->binary_event == 153) {
        int64_t code = s->binary_error;
        if (!code && !sp_json_i64(sp_json_get(root, "status_code"), &code))
            (void)sp_json_i64(sp_json_get(root, "code"), &code);
        return sp_server_error(s, code, NULL);
    }
    switch (s->binary_event) {
    case 50:
        if (s->connection_ready) return sp_protocol(s, "doubao-connection-repeat");
        s->connection_ready = true; return true;
    case 150:
        if (!s->connection_ready || !s->session_started || s->ready) return sp_protocol(s, "doubao-ready-order");
        s->ready = true; return true;
    case 152: return sp_complete(s);
    case 350: case 351: case 354:
        return (s->ready && s->submitted) || sp_protocol(s, "doubao-result-state");
    case 151: return sp_protocol(s, "doubao-remote-cancel");
    case 52: return sp_protocol(s, "doubao-remote-finish");
    default: return sp_protocol(s, "doubao-event");
    }
}
bool sp_doubao_data(speech_provider_session_t *s, const void *data, size_t n, bool begin, bool end)
{
    const unsigned char *p = data;
    if (begin) {
        s->binary_used = 0; s->binary_need = 4; s->binary_payload = 0; s->binary_received = 0;
        s->binary_type = 0; s->binary_event = 0; s->binary_error = 0; s->json_used = 0;
    }
    while (n) {
        if (s->binary_need) {
            size_t copy = s->binary_need - s->binary_used;
            if (copy > n) copy = n;
            memcpy(s->binary_prefix + s->binary_used, p, copy); s->binary_used += copy; p += copy; n -= copy;
            if (s->binary_used < s->binary_need) continue;
            const unsigned char *h = s->binary_prefix;
            if (s->binary_need == 4) {
                if (h[0] != 0x11 || h[3] || (h[2] & 15u)) return sp_protocol(s, "doubao-header");
                s->binary_type = h[1] >> 4;
                if ((s->binary_type != 9 && s->binary_type != 11 && s->binary_type != 15) ||
                    (h[1] & 15u) != (s->binary_type == 15 ? 0u : 4u) ||
                    ((unsigned)h[2] >> 4) != (s->binary_type == 11 ? 0u : 1u)) return sp_protocol(s, "doubao-type");
                s->binary_need = 12; continue;
            }
            if (s->binary_need == 12) {
                if (s->binary_type == 15) s->binary_error = be32(h + 4);
                else {
                    s->binary_event = be32(h + 4);
                    uint32_t id_size = be32(h + 8);
                    if (id_size > 128) return sp_protocol(s, "doubao-id-size");
                    if (s->binary_event < 50 || (s->binary_event > 52 && s->binary_event < 100))
                        return sp_protocol(s, "doubao-event");
                    s->binary_need = 12u + id_size + 4u; continue;
                }
            }
            size_t payload_offset = s->binary_need - 4u;
            if (s->binary_type != 15) {
                size_t id_size = be32(h + 8);
                if (s->binary_event >= 100 && (id_size != strlen(s->id) || memcmp(h + 12, s->id, id_size)))
                    return sp_protocol(s, "doubao-session-id");
                if (s->binary_type == 11 && s->binary_event != 352) return sp_protocol(s, "doubao-audio-event");
            }
            s->binary_payload = be32(h + payload_offset); s->binary_need = 0;
            size_t limit = s->binary_type == 11 ? EPD_SPEECH_PCM_SLOT_BYTES : SP_JSON_MAX;
            if (s->binary_payload > limit) return sp_protocol(s, "doubao-payload-size");
            if (s->binary_type != 11 && !s->json) {
                s->json = speech_alloc(SP_JSON_MAX + 1);
                if (!s->json) { sp_fail(&s->failure, EPD_SPEECH_NO_MEMORY, "doubao-json-alloc", 0); return false; }
            }
        } else {
            size_t copy = s->binary_payload - s->binary_received;
            if (!copy) return sp_protocol(s, "doubao-trailing-data");
            if (copy > n) copy = n;
            if (s->binary_type == 11) { if (!sp_pcm(s, p, copy)) return false; }
            else { memcpy(s->json + s->json_used, p, copy); s->json_used += copy; }
            s->binary_received += copy; p += copy; n -= copy;
        }
    }
    if (!end) return true;
    if (s->binary_need || s->binary_received != s->binary_payload) return sp_protocol(s, "doubao-truncated");
    if (s->json) s->json[s->json_used] = 0;
    return event_done(s);
}
