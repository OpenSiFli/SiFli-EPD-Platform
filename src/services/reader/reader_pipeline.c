/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_pipeline.h"
#include <string.h>

static void lock(reader_pipeline_t *p) { p->sync.lock(p->sync.user); }
static void unlock(reader_pipeline_t *p) { p->sync.unlock(p->sync.user); }
static void worker(reader_pipeline_t *p) { p->sync.wake_worker(p->sync.user); }
static void ui(reader_pipeline_t *p) { p->sync.wake_ui(p->sync.user); }
static bool cancelled(void *user)
{
    reader_pipeline_t *p = user;
    lock(p);
    bool stop = p->closing || p->operation_generation != p->generation;
    unlock(p);
    return stop || (p->input.cancelled && p->input.cancelled(p->input.user));
}
static int open_stream(void *user, const char *path, book_stream_t *stream)
{
    reader_pipeline_t *p = user;
    return p->input.open_stream ? p->input.open_stream(p->input.user, path, stream) : EPD_READ_UNSUPPORTED;
}
static void discard_ready(reader_pipeline_t *p)
{
    if (p->ready.key.serial) reader_window_release(&p->window, &p->ready);
}
int reader_pipeline_init(reader_pipeline_t *p, const reader_pipeline_sync_t *sync,
                         const book_allocator_t *memory, size_t limit, size_t block_size)
{
    if (!p || !sync || !sync->lock || !sync->unlock || !sync->wake_worker || !sync->wake_ui)
        return EPD_READ_INVALID;
    memset(p, 0, sizeof(*p)); p->sync = *sync;
    return reader_window_init(&p->window, memory, limit, block_size);
}
int reader_pipeline_open(reader_pipeline_t *p, uint64_t session,
                         const epd_read_provider_t *provider, const epd_read_source_options_t *options)
{
    if (!p || !session || !options || !provider || provider->abi != EPD_READER_SOURCE_ABI ||
        provider->size < sizeof(*provider) || !provider->open || !provider->next || !provider->seek ||
        !provider->close || !provider->content_version ||
        (options->content_version && options->content_version != provider->content_version)) return EPD_READ_INVALID;
    lock(p);
    if (p->session || p->window.allocated || p->running || p->instance)
    { unlock(p); return EPD_READ_BUSY; }
    p->provider = provider; p->input = p->options = *options;
    p->options.user = p; p->options.cancelled = cancelled; p->options.open_stream = open_stream;
    p->session = session; p->generation = 1; p->operation_generation = 0;
    p->opening = p->requested = true;
    p->opened = p->closing = p->paused = p->seeking = p->ended = p->waiting = false; p->error = 0;
    unlock(p); worker(p); return EPD_READ_OK;
}
int reader_pipeline_next(reader_pipeline_t *p)
{
    lock(p);
    int result = EPD_READ_OK;
    if (!p->session || p->closing) result = EPD_READ_STALE;
    else if (p->ended) result = EPD_READ_END;
    else if (p->error) result = p->error;
    else if (p->requested || p->running || p->ready.key.serial) result = EPD_READ_BUSY;
    else { p->requested = true; p->waiting = false; }
    unlock(p); if (!result) worker(p); return result;
}
int reader_pipeline_seek(reader_pipeline_t *p, const epd_read_anchor_t *position)
{
    if (!position || position->version != EPD_READER_POSITION_VERSION) return EPD_READ_INVALID;
    lock(p);
    if (!p->session || p->closing) { unlock(p); return EPD_READ_STALE; }
    if (p->generation == UINT32_MAX) { unlock(p); return EPD_READ_BUSY; }
    ++p->generation; p->target = *position; p->seeking = p->requested = true;
    p->ended = p->waiting = false; p->error = 0;
    discard_ready(p); reader_window_retire(&p->window, p->session, p->generation);
    unlock(p); worker(p); return EPD_READ_OK;
}
void reader_pipeline_pause(reader_pipeline_t *p, bool paused)
{
    lock(p); p->paused = paused; unlock(p); worker(p);
}
void reader_pipeline_close(reader_pipeline_t *p)
{
    lock(p);
    bool requested = p->session && !p->closing;
    if (requested)
    {
        p->closing = true; p->requested = false;
        discard_ready(p); reader_window_retire(&p->window, 0, 0);
    }
    unlock(p); if (requested) worker(p);
}
bool reader_pipeline_closed(reader_pipeline_t *p)
{
    lock(p); bool result = !p->session; unlock(p); return result;
}
bool reader_pipeline_collect(reader_pipeline_t *p)
{
    lock(p);
    if (p->closing && !p->running && !p->instance && !p->opening && reader_window_close(&p->window))
    { p->session = 0; p->provider = NULL; }
    bool done = !p->session;
    unlock(p); return done;
}
reader_pipeline_status_t reader_pipeline_status(reader_pipeline_t *p)
{
    lock(p);
    reader_pipeline_status_t status = {p->generation, p->opened, p->closing, p->paused,
        p->running, p->ready.key.serial != 0, p->ended, p->error, p->window.allocated, p->window.peak};
    unlock(p); return status;
}
int reader_pipeline_take(reader_pipeline_t *p, reader_window_lease_t *lease)
{
    if (!lease || lease->key.serial || lease->batch.data) return EPD_READ_INVALID;
    lock(p);
    int result = EPD_READ_WAIT;
    if (!p->session || p->closing) result = EPD_READ_STALE;
    else if (p->ready.key.serial) { *lease = p->ready; p->ready = (reader_window_lease_t){0}; result = EPD_READ_OK; }
    else if (p->error) result = p->error;
    else if (p->ended) result = EPD_READ_END;
    unlock(p); return result;
}
int reader_pipeline_return(reader_pipeline_t *p, reader_window_lease_t *lease)
{
    lock(p); int result = reader_window_release(&p->window, lease); unlock(p);
    if (!result) worker(p);
    return result;
}
bool reader_pipeline_step(reader_pipeline_t *p)
{
    lock(p);
    if (!p->session || p->running) { unlock(p); return false; }
    if (p->closing)
    {
        void *instance = p->instance; p->instance = NULL; p->opened = false; p->running = true;
        unlock(p);
        if (instance) p->provider->close(instance);
        lock(p); p->running = false; p->opening = false;
        bool done = reader_window_close(&p->window);
        if (done) { p->session = 0; p->provider = NULL; p->opening = false; }
        unlock(p); if (done || instance) ui(p);
        return done || instance != NULL;
    }
    if (p->paused || p->waiting || p->error || (!p->opening && !p->seeking && (!p->requested || p->ready.key.serial)))
    { unlock(p); return false; }
    bool opening = p->opening, seeking = !opening && p->seeking;
    epd_read_anchor_t target = p->target;
    reader_window_key_t key = {0}; epd_read_batch_t batch = {0};
    if (!opening && !seeking)
    {
        int reserve = reader_window_reserve(&p->window, p->session, p->generation, &key, &batch);
        if (reserve)
        {
            if (reserve != EPD_READ_BUSY) { p->error = reserve; p->requested = false; }
            unlock(p); if (reserve != EPD_READ_BUSY) ui(p);
            return false;
        }
    }
    p->operation_generation = p->generation; p->running = true;
    unlock(p);
    int result;
    void *opened = NULL;
    if (opening) result = p->provider->open(&p->options, &opened);
    else if (seeking) result = p->provider->seek(p->instance, &target);
    else result = p->provider->next(p->instance, &batch);
    if ((opening || seeking) && result > 0 && result != EPD_READ_WAIT &&
        !(seeking && result == EPD_READ_YIELD)) result = EPD_READ_INVALID;
    if (opening && result == EPD_READ_OK && !opened) result = EPD_READ_INVALID;
    if (opening && result != EPD_READ_OK && opened) { p->provider->close(opened); opened = NULL; }
    lock(p);
    bool stale = p->closing || p->operation_generation != p->generation;
    if (opening && opened) { p->instance = opened; p->opening = false; p->opened = true; }
    if (key.serial)
    {
        if (stale) result = EPD_READ_CANCELLED;
        int published = reader_window_publish(&p->window, key, &batch, result);
        if (!stale && published == EPD_READ_OK)
            published = reader_window_acquire(&p->window, key, p->session, p->generation, &p->ready);
        if (!stale && published < 0) result = published;
    }
    if (!stale)
    {
        if (result < 0) { p->error = result; p->requested = false; }
        else if (result == EPD_READ_WAIT) { p->requested = false; p->waiting = true; }
        else if (result == EPD_READ_YIELD) { /* Continue after other worker work. */ }
        else if (opening) p->opening = false;
        else if (seeking) p->seeking = false;
        else { p->requested = false; p->ended = result == EPD_READ_END; }
    }
    p->running = false;
    unlock(p);
    if (result != EPD_READ_YIELD || stale) ui(p);
    return true;
}
