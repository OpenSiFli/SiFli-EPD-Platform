/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_AI_SETTINGS_H
#define EPD_AI_SETTINGS_H
#include "ai_internal.h"
#include "ai_settings_draft.h"

#define AI_SETTINGS_QUEUE_CAPACITY 4u
#define AI_SETTINGS_RESULT_CAPACITY 8u
#define AI_SETTINGS_PATH_SIZE 512u

typedef enum {
    AI_SETTINGS_RELOAD, AI_SETTINGS_SAVE_PROFILE, AI_SETTINGS_REMOVE,
    AI_SETTINGS_IMPORT, AI_SETTINGS_EXPORT, AI_SETTINGS_DEFAULT,
    AI_SETTINGS_GRANT, AI_SETTINGS_REVOKE, AI_SETTINGS_TEST, AI_SETTINGS_IMPORT_PREPARE
} ai_settings_operation_t;

/* submit copies the complete job before returning. No caller pointer survives
 * submission. SAVE_PROFILE uses profile; other operations use profile_id.
 * Empty profile_id on REVOKE removes every grant for app_id. EXPORT is always
 * redacted. IMPORT_PREPARE reads/validates path in card_session and returns a
 * candidate_id. IMPORT commits that exact candidate with the same session;
 * it never reads path. DEFAULT accepts an
 * empty ID to clear the recommendation. TEST requires an explicit model ID. */
typedef struct {
    ai_settings_operation_t operation;
    ai_profile_t profile;
    bool keep_secret;
    char profile_id[EPD_AI_ID_SIZE];
    char model_id[EPD_AI_MODEL_SIZE];
    char app_id[32];
    char path[AI_SETTINGS_PATH_SIZE];
    uint32_t card_session, candidate_id, draft_revision;
} ai_settings_job_t;

typedef struct {
    uint32_t job_id;
    ai_settings_operation_t operation;
    epd_ai_error_t error;
    int http_status;
    epd_ai_finish_t finish;
    epd_ai_usage_t usage;
    char error_code[96], stage[32];
    bool complete;
    uint32_t candidate_id;
} ai_settings_result_t;

typedef struct {
    bool initialized, busy, stopping;
    epd_ai_error_t initialization_error;
    uint32_t generation, active_job_id, completed_job_id;
    size_t queued, profile_count, grant_count;
    char default_id[EPD_AI_ID_SIZE];
    epd_ai_profile_t profiles[EPD_AI_MAX_PROFILES];
    ai_grant_t grants[AI_MAX_GRANTS];
    ai_settings_result_t last_result;
    /* ready means the record was loaded or confirmed absent, never read failure. */
    bool draft_available, draft_ready;
    epd_ai_error_t draft_error;
} ai_settings_snapshot_t;

/* UI/platform owner starts the resident service. Initialization and all file
 * operations execute on its registered worker. These UI APIs never do I/O. */
epd_ai_error_t ai_settings_start(void);
/* Normal awake UI loop: starts/restarts the worker outside shutdown, reset,
 * media handoff and service retirement. No file I/O runs on this caller. */
void ai_settings_process(void);
epd_ai_error_t ai_settings_snapshot(ai_settings_snapshot_t *out);
/* Returns editable fields with secret cleared. keep_secret=true on save
 * preserves the current credential; has_secret drives the masked UI. */
epd_ai_error_t ai_settings_profile_get(const char *id, ai_profile_t *out,
                                     bool *has_secret);
epd_ai_error_t ai_settings_submit(const ai_settings_job_t *job, uint32_t *job_id);
/* BUSY means pending; NOT_FOUND means unknown or aged out of the bounded
 * completion history. OK means result was copied (inspect result.error). */
epd_ai_error_t ai_settings_poll(uint32_t job_id, ai_settings_result_t *out);
/* Discard a prepared/queued import; its worker owns and frees the candidate. */
void ai_settings_import_discard(uint32_t candidate_id);
/* Short memory-only copies. set coalesces edits; the worker persists this
 * separate draft (including an unfinished editor) without applying it. */
epd_ai_error_t ai_settings_draft_set(const ai_settings_draft_t *draft, uint32_t *revision);
epd_ai_error_t ai_settings_draft_get(ai_settings_draft_t *draft);
epd_ai_error_t ai_settings_draft_discard(uint32_t revision);
/* job_id=0 cancels every queued/running TEST. Other operations already
 * accepted remain durable work. A stale ID never cancels a later test. */
void ai_settings_cancel(uint32_t job_id);
/* Nonblocking barriers. flush cancels tests, rejects new work until queued
 * commits finish, and returns false while pending or after a save failure.
 * stop drains accepted mutations and then retires the service. */
bool ai_settings_flush(void);
void ai_settings_stop(void);
bool ai_settings_idle(void);
/* Installer worker only, before storage_lock. Queues a durable revoke and
 * waits for that exact completion; no UI or storage lock may be held. */
epd_ai_error_t ai_settings_revoke_app_sync(const char *app);

#endif
