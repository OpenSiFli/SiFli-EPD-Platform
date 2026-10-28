/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_CORE_H
#define SPEECH_CORE_H
#include "speech_internal.h"
#include "speech_provider.h"
#include "app_service.h"
#include "sleep_guard.h"

typedef struct speech_event_node speech_event_node_t;
typedef struct {
    unsigned char *pcm;
    size_t produced, consumed;
    bool busy, complete;
    uint32_t unit;
} speech_slot_t;
typedef struct speech_session {
    epd_service_t *owner, *worker;
    epd_speech_request_t request;
    epd_speech_snapshot_t view;
    speech_account_t account;
    speech_failure_t failure;
    sleep_guard_owner_t sleep;
    bool cancelled, owner_stopping, worker_running, finished, pause;
    bool state_pending, writable_pending, terminal_taken, completed;
    bool network_busy, output_busy, output_done, text_blocked, pause_confirmed, resume_requested, resume_busy;
    epd_speech_error_t output_error;
    char *text;
    size_t text_used;
    speech_slot_t slots[2];
    unsigned write_slot, read_slot;
    uint32_t next_unit, text_changed, started, network_generation;
    uint32_t intent;
    speech_event_node_t *events, *events_tail;
    struct rt_thread output_thread;
    struct rt_event output_wakeup;
    bool output_event_ready;
    void *output_stack;
    /* Set by idle cleanup, not merely by the output entry returning. */
    volatile bool output_thread_done;
} speech_session_t;

void speech_lock(void);
void speech_unlock(void);
void speech_session_state(speech_session_t *s, epd_speech_state_t state);
bool speech_session_cancelled(void *session);
bool speech_session_text(void *session, uint32_t sentence, bool final, const char *text, size_t bytes);
bool speech_session_pcm(void *session, const void *pcm, size_t bytes);
void speech_session_error(speech_session_t *s, epd_speech_error_t error, const char *stage);
void speech_session_wake(speech_session_t *s);
void speech_output_wake(speech_session_t *s);
void speech_session_complete(speech_session_t *s, epd_speech_error_t error);
/* ASR owns a temporary pause of TTS only when it was playing. */
bool speech_tts_suspend(speech_session_t *asr, uint32_t *session, uint32_t *intent);
bool speech_tts_quiet(void);
void speech_tts_restore(uint32_t session, uint32_t intent);
bool speech_tts_resume_begin(speech_session_t *s, uint32_t *intent);
bool speech_tts_resume_current(speech_session_t *s, uint32_t intent);
bool speech_tts_asr_pending(void);
void speech_tts_resume_finish(speech_session_t *s, uint32_t intent, epd_speech_error_t error);
void speech_asr_run(speech_session_t *s);
void speech_tts_run(speech_session_t *s);
#endif
