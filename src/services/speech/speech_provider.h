/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_PROVIDER_H
#define SPEECH_PROVIDER_H
#include "speech_internal.h"
typedef struct speech_provider_session speech_provider_session_t;
typedef struct {
    /* Borrowed data, valid only in callback. false aborts the task. */
    bool (*text)(void *user, uint32_t sentence, bool final, const char *text, size_t bytes);
    bool (*pcm)(void *user, const void *pcm, size_t bytes);
    void *user;
} speech_provider_sink_t;
/* Open includes TLS/auth/task-ready (15 seconds total). All methods run on one
 * network worker per session; poll never calls GUI and obeys wait_ms/cancel.
 * ready: ASR accepts PCM; TTS accepts one complete natural-language unit.
 * No implicit retry. failure excludes signed URLs, credentials and input text. */
epd_speech_error_t speech_provider_open(const speech_account_t *account, epd_speech_kind_t kind,
    epd_speech_language_t language, speech_cancel_fn cancel, void *cancel_user,
    const speech_provider_sink_t *sink, speech_provider_session_t **out, speech_failure_t *failure);
epd_speech_error_t speech_provider_audio(speech_provider_session_t *session, const void *pcm, size_t bytes);
epd_speech_error_t speech_provider_text(speech_provider_session_t *session, const char *text, size_t bytes);
epd_speech_error_t speech_provider_finish(speech_provider_session_t *session);
epd_speech_error_t speech_provider_poll(speech_provider_session_t *session, uint32_t wait_ms, bool *done);
void speech_provider_failure(speech_provider_session_t *session, speech_failure_t *out);
void speech_provider_close(speech_provider_session_t *session);
#endif
