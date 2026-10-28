/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_WIRE_H
#define SPEECH_WIRE_H
#include "speech_provider.h"
#include <stdint.h>

#define SP_OPEN_MS 15000u
#define SP_PROGRESS_MS 20000u
#define SP_TASK_MS 120000u
#define SP_FINAL_MS 10000u
#define SP_CLOSE_MS 2000u
#define SP_JSON_MAX (48u * 1024u)
#define SP_MESSAGE_MAX (EPD_SPEECH_PCM_SLOT_BYTES + 4096u)
#define SP_IO_AGAIN (-1)
#define SP_IO_ERROR (-2)

typedef struct sp_transport sp_transport;
typedef struct {
    speech_cancel_fn cancel;
    void *user;
    speech_failure_t *failure;
    uint32_t deadline;
} sp_guard;
bool sp_check(sp_guard *g);
epd_speech_error_t sp_fail(speech_failure_t *f, epd_speech_error_t error,
                           const char *stage, int detail);
void sp_wipe(void *p, size_t n);
void sp_safe_id(char *out, size_t capacity, const char *p, size_t n);
bool sp_transport_new(sp_transport **out, sp_guard *g);
bool sp_transport_connect(sp_transport *t, const char *host, sp_guard *g);
int sp_transport_read(sp_transport *t, void *data, size_t n);
int sp_transport_write(sp_transport *t, const void *data, size_t n);
bool sp_transport_random(sp_transport *t, void *data, size_t n);
bool sp_transport_route(sp_transport *t, sp_guard *g);
void sp_transport_free(sp_transport *t);
uint64_t sp_utc(void);
void sp_delay(uint32_t ms);
bool sp_sha1(const void *p, size_t n, unsigned char digest[20]);
bool sp_hmac_sha1(const char *key, const char *p, unsigned char digest[20]);
bool sp_base64(const void *p, size_t n, char *out, size_t capacity);

typedef struct { uint32_t value, minimum; unsigned need; } sp_utf8;
bool sp_utf8_feed(sp_utf8 *s, const void *p, size_t n);
bool sp_utf8_text(const char *p, size_t n, size_t *characters);

typedef struct sp_ws sp_ws;
/* Data are streamed in bounded pieces. begin/end denote WebSocket messages,
 * independently of transport reads and continuation frames. */
typedef bool (*sp_ws_data_fn)(void *user, unsigned opcode, const void *p,
                             size_t n, bool begin, bool end);
sp_ws *sp_ws_new(sp_transport *transport, speech_failure_t *failure);
bool sp_ws_upgrade(sp_ws *w, const char *host, const char *path,
                   const char *headers, sp_guard *g);
bool sp_ws_send(sp_ws *w, unsigned opcode, const void *p, size_t n, sp_guard *g);
/* 1: one message/control frame processed; 0: wait expired; -1: failure/close. */
int sp_ws_poll(sp_ws *w, uint32_t wait_ms, sp_guard *g, sp_ws_data_fn fn, void *user);
void sp_ws_close(sp_ws *w, sp_guard *g);
void sp_ws_free(sp_ws *w);

/* Private, allocation-free JSON views. A view borrows its source message. */
typedef struct { const char *p; size_t n; char type; } sp_json;
bool sp_json_parse(const char *p, size_t n, sp_json *out);
sp_json sp_json_get(sp_json object, const char *name);
sp_json sp_json_at(sp_json array, unsigned index);
bool sp_json_i64(sp_json v, int64_t *out);
bool sp_json_bool(sp_json v, bool *out);
bool sp_json_string(sp_json v, char *out, size_t capacity, size_t *used);
bool sp_json_is(sp_json v, const char *text);
bool sp_json_quote(const char *p, size_t n, char *out, size_t capacity);
#endif
