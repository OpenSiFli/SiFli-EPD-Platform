#ifndef AI_DEMO_H
#define AI_DEMO_H
#include "app_service.h"
#include "platform/epd_ai.h"
#include "chat_speech.h"

#define DEMO_MAX_SESSIONS 64u
#define DEMO_MAX_MESSAGES 128u
#define DEMO_TITLE_SIZE 96u
#define DEMO_DRAFT_SIZE 2049u

typedef uint32_t demo_id_t;
/* NEW retains the selection. RETRY retries the last incomplete question and
 * preserves any new draft. Without an incomplete reply, RETRY sends the draft. */
typedef enum { DEMO_REFRESH, DEMO_NEW, DEMO_SAVE, DEMO_RETRY } demo_command_t;
typedef enum { DEMO_LOADING, DEMO_READY, DEMO_RUNNING, DEMO_DONE, DEMO_FAILED } demo_status_t;
typedef enum {
    DEMO_REASON_NONE, DEMO_REASON_SERVICE_CHANGED, DEMO_REASON_INVALID_REQUEST, DEMO_REASON_OTHER
} demo_failure_reason_t;
typedef enum {
    DEMO_PHASE_NONE, DEMO_PHASE_PRECHECK, DEMO_PHASE_SAVE, DEMO_PHASE_MEASURE,
    DEMO_PHASE_PERFORM, DEMO_PHASE_RESPONSE, DEMO_PHASE_RESTORE
} demo_failure_phase_t;
#define DEMO_FAILURE_STAGE_SIZE 32u
#define DEMO_FAILURE_CODE_SIZE 96u
/* Fixed-width persisted diagnostics for the latest generation attempt. status
 * is its return code; view.error can temporarily report a later save failure. */
typedef struct {
    uint32_t reason, phase, status;
    int32_t http_status, detail;
    uint32_t tls_verify_flags;
    char stage[DEMO_FAILURE_STAGE_SIZE], error_code[DEMO_FAILURE_CODE_SIZE];
} demo_failure_t;
typedef struct {
    uint32_t revision;
    demo_status_t status;
    epd_ai_error_t error;
    demo_failure_t failure;
    demo_id_t current_id;
    bool busy, saved;
    bool generating; /* A queued or running send/retry can be cancelled. */
    char title[DEMO_TITLE_SIZE];
    char profile_id[EPD_AI_ID_SIZE], profile_name[EPD_AI_NAME_SIZE];
    char model[EPD_AI_MODEL_SIZE];
    size_t message_count;
    char draft[DEMO_DRAFT_SIZE];
    bool auto_read;
    bool web_search, search_available, searching, search_failed;
    chat_speech_view_t speech;
} demo_view_t;

typedef struct {
    demo_id_t id;
    char title[DEMO_TITLE_SIZE];
    char profile_id[EPD_AI_ID_SIZE], profile_name[EPD_AI_NAME_SIZE];
    char model[EPD_AI_MODEL_SIZE];
    size_t message_count;
    bool saved;
} demo_summary_t;

typedef enum { DEMO_ROLE_USER, DEMO_ROLE_ASSISTANT } demo_role_t;
typedef struct {
    demo_role_t role;
    bool user, complete;
    size_t length; /* UTF-8 bytes, excluding the terminating NUL. */
    uint32_t revision;
} demo_message_meta_t;

void demo_init(void);
void demo_deinit(void);
bool demo_submit(demo_command_t command);
/* Mutations copy their inputs. true means accepted; completion/errors are
 * published through snapshot. Session changes are rejected while busy. */
bool demo_send(const char *text);
bool demo_open(demo_id_t id);
/* The caller obtains deletion confirmation before calling delete. */
bool demo_delete(demo_id_t id);
bool demo_rename(demo_id_t id, const char *title);
/* A session with messages cannot change service identity or model. */
bool demo_select(const char *profile_id, const char *model);
/* Saves the current session, then creates and selects a new session atomically. */
bool demo_new_with(const char *profile_id, const char *model);
/* Reject edits for a different current session. Draft edits are accepted during
 * generation; persistence is debounced 1s. */
bool demo_set_draft(demo_id_t expected_id, const char *text);
bool demo_set_auto_read(bool enabled);
bool demo_set_web_search(bool enabled);
void demo_cancel(void);
epd_service_t *demo_owner(void);
/* Readers never wait for the worker lock and never expose backend pointers. */
bool demo_snapshot(demo_view_t *view);
/* meta-only reads accept text=NULL, capacity=0. Text reads terminate with NUL;
 * offset is a byte offset. Fragments preserve UTF-8 character boundaries. */
bool demo_message(size_t index, demo_message_meta_t *meta, size_t offset,
                  char *text, size_t capacity);
/* count receives the total available count; at most capacity items are copied.
 * NULL with capacity=0 queries the count. IDs remain stable across restarts. */
bool demo_list(demo_summary_t *items, size_t capacity, size_t *count);
/* Cached authorized profiles/models; REFRESH updates the cache in the worker. */
bool demo_profiles(epd_ai_profile_t *items, size_t capacity, size_t *count);
bool demo_profile_get(const char *profile_id, epd_ai_profile_t *profile);
/* UI thread, after worker termination; detaches timers and page borrowing. */
void demo_ui_release(void);
#endif
