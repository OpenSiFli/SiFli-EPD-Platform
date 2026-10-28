/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_SPEECH_H
#define EPD_SPEECH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct epd_service epd_service_t;
typedef uint32_t epd_speech_session_t;
#define EPD_SPEECH_VERSION 1u
#define EPD_SPEECH_ASR_LIMIT_MS 600000u
#define EPD_SPEECH_SAMPLE_RATE 16000u
#define EPD_SPEECH_PCM_SLOT_BYTES (2u * 1024u * 1024u)
#define EPD_SPEECH_TEXT_BYTES (16u * 1024u)
#define EPD_SPEECH_UNIT_CHARACTERS 512u
typedef enum { EPD_SPEECH_ASR, EPD_SPEECH_TTS } epd_speech_kind_t;
typedef enum { EPD_SPEECH_ZH_EN, EPD_SPEECH_ZH, EPD_SPEECH_EN } epd_speech_language_t;
typedef enum {
    EPD_SPEECH_OK, EPD_SPEECH_INVALID, EPD_SPEECH_BUSY, EPD_SPEECH_NO_MEMORY,
    EPD_SPEECH_NOT_CONFIGURED, EPD_SPEECH_NO_NETWORK, EPD_SPEECH_CONNECT,
    EPD_SPEECH_TLS, EPD_SPEECH_AUTH, EPD_SPEECH_QUOTA, EPD_SPEECH_PROTOCOL,
    EPD_SPEECH_TIMEOUT, EPD_SPEECH_CANCELLED, EPD_SPEECH_STORAGE,
    EPD_SPEECH_INPUT_UNAVAILABLE, EPD_SPEECH_INPUT_OVERRUN,
    EPD_SPEECH_TEXT_TOO_LONG, EPD_SPEECH_AUDIO_TOO_LARGE,
    EPD_SPEECH_OUTPUT_INTERRUPTED, EPD_SPEECH_OUTPUT_TIMEOUT,
    EPD_SPEECH_NOT_FOUND, EPD_SPEECH_UNSUPPORTED
} epd_speech_error_t;
typedef enum {
    EPD_SPEECH_PREPARING, EPD_SPEECH_RECORDING, EPD_SPEECH_FINALIZING,
    EPD_SPEECH_BUFFERING, EPD_SPEECH_PLAYING, EPD_SPEECH_PAUSING,
    EPD_SPEECH_PAUSED, EPD_SPEECH_DRAINING, EPD_SPEECH_STOPPING,
    EPD_SPEECH_COMPLETED, EPD_SPEECH_STOPPED, EPD_SPEECH_FAILED
} epd_speech_state_t;
typedef enum {
    EPD_SPEECH_REASON_NONE, EPD_SPEECH_USER_FINISH, EPD_SPEECH_DURATION_LIMIT,
    EPD_SPEECH_USER_STOP, EPD_SPEECH_OWNER_STOP, EPD_SPEECH_SERVICE_ERROR
} epd_speech_reason_t;
typedef enum {
    EPD_SPEECH_EVENT_STATE, EPD_SPEECH_EVENT_DRAFT, EPD_SPEECH_EVENT_FINAL_TEXT,
    EPD_SPEECH_EVENT_WRITABLE, EPD_SPEECH_EVENT_TERMINAL
} epd_speech_event_kind_t;
typedef struct {
    uint32_t struct_size, version;
    epd_speech_language_t language;
    bool background;
    char title[160];
} epd_speech_request_t;
typedef struct {
    epd_speech_session_t session;
    epd_speech_kind_t kind;
    epd_speech_state_t state;
    epd_speech_error_t error;
    epd_speech_reason_t reason;
    uint32_t revision, elapsed_ms, limit_ms, unit;
    uint64_t submitted_samples;
    bool input_finished;
} epd_speech_snapshot_t;
typedef struct {
    epd_speech_event_kind_t kind;
    epd_speech_snapshot_t snapshot;
    uint32_t sentence, revision;
    const char *text;
    size_t text_bytes;
    int detail, http_status;
    char stage[32], request_id[128], code[96];
    void *private_data;
} epd_speech_event_t;
typedef struct {
    bool configured, busy, input_available;
    uint32_t max_record_ms;
} epd_speech_capability_t;

/* Memory-only, nonblocking application entry points. The owner is a live
 * registered background service; the platform retains it until cleanup ends.
 * No credentials or vendor-specific parameters cross this interface. */
epd_speech_error_t epd_speech_capability(epd_speech_kind_t kind, epd_speech_capability_t *out);
epd_speech_error_t epd_speech_start(epd_service_t *owner, epd_speech_kind_t kind,
                                  const epd_speech_request_t *request, epd_speech_session_t *session);
epd_speech_error_t epd_speech_finish(epd_service_t *owner, epd_speech_session_t session);
epd_speech_error_t epd_speech_cancel(epd_service_t *owner, epd_speech_session_t session);
/* Returns a complete UTF-8 prefix accepted, zero for backpressure, or -error.
 * Caller retains/retries the suffix. finish is valid only after all text was accepted. */
int epd_speech_append(epd_service_t *owner, epd_speech_session_t session, const char *text, size_t bytes);
epd_speech_error_t epd_speech_pause(epd_service_t *owner, epd_speech_session_t session);
epd_speech_error_t epd_speech_resume(epd_service_t *owner, epd_speech_session_t session);
epd_speech_error_t epd_speech_stop(epd_service_t *owner, epd_speech_session_t session);
epd_speech_error_t epd_speech_snapshot(epd_service_t *owner, epd_speech_session_t session,
                                     epd_speech_snapshot_t *out);
/* One consumer per owner. Returned event owns its text until release; never
 * retain more than the bounded event budget. NOT_FOUND means queue empty. */
epd_speech_error_t epd_speech_poll(epd_service_t *owner, epd_speech_event_t *out);
void epd_speech_event_release(epd_speech_event_t *event);
const char *epd_speech_error_name(epd_speech_error_t error);
#ifdef __cplusplus
}
#endif
#endif
