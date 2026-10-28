/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_SESSION_H
#define READER_SESSION_H
#include "../../ui/platform/epd_reader_source.h"

/* One foreground owner, one replaceable open request and two retained save
 * owners. Capacity pressure rejects admission without dropping saved progress. */
#define READER_SESSION_SLOTS 4u
#define READER_WORK_SLOTS 2u
typedef uint64_t reader_session_id_t;
typedef struct {
    reader_session_id_t session;
    uint64_t sequence;
    uint32_t generation;
} reader_work_token_t;
typedef enum {
    READER_SESSION_EMPTY, READER_SESSION_WAITING, READER_SESSION_ACTIVE,
    READER_SESSION_PAUSED, READER_SESSION_CLOSING, READER_SESSION_SAVING
} reader_session_phase_t;

typedef struct {
    const char *app_id, *identity;
    void *owner;
} reader_session_open_t;
typedef struct {
    bool (*retain)(void *owner);
    void (*release)(void *owner);
    /* UI-only; copy/queue the snapshot, then return WAIT, or return OK only
     * after durable completion. Failure retains the owner for an explicit retry.
     * Hooks cannot reenter mutating session operations. */
    int (*save)(void *owner, reader_session_id_t id, uint64_t sequence,
                const epd_read_anchor_t *position);
} reader_session_hooks_t;
typedef struct {
    reader_session_id_t id;
    uint32_t generation;
    reader_session_phase_t phase;
    char app_id[64], identity[256];
    void *owner;
    bool bound, saving;
    int save_error;
    uint64_t visible_revision, saved_revision, save_revision, save_sequence;
    epd_read_anchor_t visible, saving_position;
} reader_session_slot_t;
typedef struct {
    reader_session_hooks_t hooks;
    reader_session_slot_t slots[READER_SESSION_SLOTS];
    reader_work_token_t work[READER_WORK_SLOTS];
    reader_session_id_t active, pending;
    uint64_t sequence;
    bool stopping, in_hook;
} reader_sessions_t;

/* The UI thread owns this coordinator. Workers return tokens through the
 * existing service completion path; they never mutate these structures.
 * The runtime cancels work after generation changes and unbinds UI on close.
 * process activates the next owner only after work is returned and UI unbound. */
void reader_sessions_init(reader_sessions_t *sessions, const reader_session_hooks_t *hooks);
int reader_sessions_open(reader_sessions_t *sessions, const reader_session_open_t *request,
                          reader_session_id_t *id);
const reader_session_slot_t *reader_sessions_get(const reader_sessions_t *sessions, reader_session_id_t id);
reader_session_id_t reader_sessions_process(reader_sessions_t *sessions);
int reader_sessions_bind(reader_sessions_t *sessions, reader_session_id_t id, bool bound);
int reader_sessions_pause(reader_sessions_t *sessions, reader_session_id_t id, bool paused);
int reader_sessions_invalidate(reader_sessions_t *sessions, reader_session_id_t id);
int reader_sessions_close(reader_sessions_t *sessions, reader_session_id_t id);
int reader_sessions_work_begin(reader_sessions_t *sessions, reader_session_id_t id, reader_work_token_t *token);
bool reader_sessions_work_current(const reader_sessions_t *sessions, reader_work_token_t token);
/* Complete exactly once, after consuming/discarding the result and before the
 * next process call. Stale/duplicate tokens cannot release another job. */
int reader_sessions_work_end(reader_sessions_t *sessions, reader_work_token_t token);
int reader_sessions_present(reader_sessions_t *sessions, reader_session_id_t id,
                             uint32_t generation, const epd_read_anchor_t *position);
int reader_sessions_saved(reader_sessions_t *sessions, reader_session_id_t id,
                          uint64_t sequence, int result);
void reader_sessions_retry_saves(reader_sessions_t *sessions);
int reader_sessions_retry_save(reader_sessions_t *sessions, reader_session_id_t id);
bool reader_sessions_flush(reader_sessions_t *sessions);
void reader_sessions_stop(reader_sessions_t *sessions);
bool reader_sessions_empty(const reader_sessions_t *sessions);
#endif
