/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_CORE_H
#define READER_CORE_H
#include "reader_pipeline.h"
#include "reader_provider.h"
#include "reader_session.h"

typedef struct {
    void *user;
    bool (*retain)(void *user);
    void (*release)(void *user);
    int (*save)(void *user, reader_session_id_t session, uint64_t request,
                const epd_read_anchor_t *position);
} reader_owner_t;
typedef struct {
    epd_read_source_options_t source;
    const char *format;
    /* NULL selects a registered provider. An application's own provider uses
     * the same contract; its callbacks are pinned by owner.retain/release. */
    const epd_read_provider_t *provider;
    reader_owner_t owner;
    epd_read_anchor_t restore; /* version=0 starts at the beginning. */
} reader_open_t;
typedef struct {
    reader_sessions_t sessions;
    reader_pipeline_t pipeline;
    reader_providers_t *providers;
    book_allocator_t memory;
    reader_work_token_t worker;
} reader_core_t;

/* UI owns session/provider admission. step is the single worker entry; all
 * file access stays there. This core owns no threads and no LVGL objects. */
int reader_core_init(reader_core_t *core, reader_providers_t *providers,
                      const reader_pipeline_sync_t *sync, const book_allocator_t *memory,
                      size_t window_limit, size_t block_size);
int reader_core_open(reader_core_t *core, const reader_open_t *request, reader_session_id_t *id);
void reader_core_process(reader_core_t *core);
bool reader_core_step(reader_core_t *core);
int reader_core_pause(reader_core_t *core, reader_session_id_t id, bool paused);
int reader_core_close(reader_core_t *core, reader_session_id_t id);
int reader_core_seek(reader_core_t *core, reader_session_id_t id, const epd_read_anchor_t *position);
int reader_core_next(reader_core_t *core, reader_session_id_t id);
int reader_core_ready(reader_core_t *core, reader_session_id_t id);
int reader_core_take(reader_core_t *core, reader_session_id_t id, reader_window_lease_t *lease);
int reader_core_return(reader_core_t *core, reader_session_id_t id, reader_window_lease_t *lease);
int reader_core_error(reader_core_t *core, reader_session_id_t id);
/* Retires sessions; caller unbinds UI and returns borrowed batches. Flush and
 * saved-completion use the session coordinator's durable-save barrier. */
void reader_core_stop(reader_core_t *core);
bool reader_core_empty(reader_core_t *core);
#endif
