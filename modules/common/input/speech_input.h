/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MODULE_SPEECH_INPUT_H
#define MODULE_SPEECH_INPUT_H
#include "platform/epd_speech.h"

/* Runs synchronously in the application's event consumer. Check target under
 * the same application lock used to invalidate an editor before committing. */
typedef epd_speech_error_t (*speech_input_commit_t)(void *user, uint32_t target,
                                                   const char *text, size_t bytes);
typedef struct {
    epd_service_t *owner;
    epd_speech_snapshot_t snapshot;
    epd_speech_error_t error;
    uint32_t target, sentence, revision, final_sentence;
    bool active, accepting, have_sentence, have_final;
    char *draft;
    size_t capacity;
    speech_input_commit_t commit;
    void *user;
} speech_input_t;

/* Caller supplies draft storage and serializes all operations. No UI pointer,
 * allocation, thread, event poller or retained event is created here. */
void speech_input_init(speech_input_t *input, char *draft, size_t capacity,
                       speech_input_commit_t commit, void *user);
epd_speech_error_t speech_input_start(speech_input_t *input, epd_service_t *owner,
                                     const epd_speech_request_t *request, uint32_t target);
void speech_input_finish(speech_input_t *input);
/* Invalidate the application target before queuing this operation on exit. */
void speech_input_cancel(speech_input_t *input);
/* The owner's sole poller dispatches every event, then releases it itself.
 * active remains true until a matching TERMINAL event has been consumed, or
 * refresh confirms that global shutdown has already retired the session. */
bool speech_input_event(speech_input_t *input, const epd_speech_event_t *event);
void speech_input_refresh(speech_input_t *input);

/* Merge finals appended to current since previous into a manually edited copy.
 * All strings are NUL-terminated UTF-8; buffers must not overlap. False leaves
 * edited unchanged (conflicting target/content or insufficient capacity). */
bool speech_input_merge(const char *previous, const char *current, char *edited, size_t capacity);
#endif
