/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_PROVIDER_PRIVATE_H
#define SPEECH_PROVIDER_PRIVATE_H
#include "speech_wire.h"

struct speech_provider_session {
    speech_provider_t provider;
    epd_speech_kind_t kind;
    epd_speech_language_t language;
    speech_provider_sink_t sink;
    speech_failure_t failure;
    sp_guard guard;
    sp_transport *transport;
    sp_ws *ws;
    char id[37], voice[128];
    unsigned rate100;
    uint32_t opened, submitted_at, progress, finished_at, last_sentence;
    bool ready, submitted, finished, done, have_final, connection_ready, session_started;
    bool pcm_odd;
    unsigned char pcm_tail;
    size_t pcm_bytes;
    char *json;
    size_t json_used;
    unsigned char binary_prefix[160];
    size_t binary_used, binary_need, binary_payload, binary_received;
    unsigned binary_type, binary_event;
    uint32_t binary_error;
};
typedef struct { char host[256], path[2048], headers[1024]; } sp_endpoint;
bool sp_uuid(speech_provider_session_t *s, char out[37]);
bool sp_send_json(speech_provider_session_t *s, const char *json);
bool sp_pcm(speech_provider_session_t *s, const void *p, size_t n);
bool sp_transcript(speech_provider_session_t *s, uint32_t sentence, bool final, sp_json text);
bool sp_complete(speech_provider_session_t *s);
bool sp_protocol(speech_provider_session_t *s, const char *stage);
bool sp_server_error(speech_provider_session_t *s, int64_t code, const char *name);
void sp_json_id(sp_json value, char *out, size_t capacity);
bool sp_tencent_endpoint(speech_provider_session_t *s, const speech_account_t *a, sp_endpoint *e);
bool sp_tencent_message(speech_provider_session_t *s, sp_json root);
bool sp_tencent_send(speech_provider_session_t *s, const char *quoted, bool finish);
bool sp_ali_endpoint(speech_provider_session_t *s, const speech_account_t *a, sp_endpoint *e);
bool sp_ali_start(speech_provider_session_t *s);
bool sp_ali_message(speech_provider_session_t *s, sp_json root);
bool sp_ali_send(speech_provider_session_t *s, const char *quoted, bool finish);
bool sp_doubao_endpoint(speech_provider_session_t *s, const speech_account_t *a, sp_endpoint *e);
bool sp_doubao_start(speech_provider_session_t *s, bool session);
bool sp_doubao_send(speech_provider_session_t *s, unsigned event, const char *quoted);
bool sp_doubao_data(speech_provider_session_t *s, const void *p, size_t n, bool begin, bool end);
bool sp_field(const char *s, size_t capacity, bool token, bool empty);
#endif
