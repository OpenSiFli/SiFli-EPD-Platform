/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_PIPELINE_H
#define READER_PIPELINE_H
#include "reader_window.h"

typedef struct {
    void *user;
    void (*lock)(void *user);
    void (*unlock)(void *user);
    void (*wake_worker)(void *user);
    void (*wake_ui)(void *user);
} reader_pipeline_sync_t;
typedef struct {
    reader_window_t window;
    reader_pipeline_sync_t sync;
    const epd_read_provider_t *provider;
    epd_read_source_options_t input, options;
    void *instance;
    uint64_t session;
    uint32_t generation, operation_generation;
    epd_read_anchor_t target;
    reader_window_lease_t ready;
    bool opening, closing, paused, requested, seeking, running, opened, ended, waiting;
    int error;
} reader_pipeline_t;
typedef struct {
    uint32_t generation;
    bool open, closing, paused, running, ready, ended;
    int error;
    size_t buffer_bytes, peak_buffer_bytes;
} reader_pipeline_status_t;

/* One existing reader worker calls step. UI commands only publish intent and
 * borrow ready records; they never call provider/file operations. The owner
 * keeps provider/module/options strings and callback objects alive until
 * closed() succeeds, including after close was requested. The supplied lock
 * protects short metadata/allocator operations, not parsing or filesystem I/O. */
int reader_pipeline_init(reader_pipeline_t *pipeline, const reader_pipeline_sync_t *sync,
                         const book_allocator_t *memory, size_t limit, size_t block_size);
int reader_pipeline_open(reader_pipeline_t *pipeline, uint64_t session,
                         const epd_read_provider_t *provider, const epd_read_source_options_t *options);
int reader_pipeline_next(reader_pipeline_t *pipeline);
int reader_pipeline_seek(reader_pipeline_t *pipeline, const epd_read_anchor_t *position);
void reader_pipeline_pause(reader_pipeline_t *pipeline, bool paused);
void reader_pipeline_close(reader_pipeline_t *pipeline);
bool reader_pipeline_closed(reader_pipeline_t *pipeline);
/* UI or worker: metadata-only final collection after the worker has closed
 * its parser. This lets service shutdown join the worker before UI cleanup. */
bool reader_pipeline_collect(reader_pipeline_t *pipeline);
reader_pipeline_status_t reader_pipeline_status(reader_pipeline_t *pipeline);
int reader_pipeline_take(reader_pipeline_t *pipeline, reader_window_lease_t *lease);
int reader_pipeline_return(reader_pipeline_t *pipeline, reader_window_lease_t *lease);
/* At most one provider open, seek or next per call. true means a step ran.
 * WAIT needs an explicit retry/wakeup when external content becomes ready.
 * Capacity pressure resumes when a consumer returns its lease. */
bool reader_pipeline_step(reader_pipeline_t *pipeline);
#endif
