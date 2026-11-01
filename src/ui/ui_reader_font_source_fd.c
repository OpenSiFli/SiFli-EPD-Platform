#include "ui_reader_font_source_fd.h"
#include <string.h>

static void state_lock(source_fd *f) { f->pool->ops->state_lock(f->pool->user); }
static void state_unlock(source_fd *f) { f->pool->ops->state_unlock(f->pool->user); }

void source_fd_init(source_fd *f, source_fd_pool *pool, const source_fd_key *key, bool retryable)
{
    memset(f, 0, sizeof(*f));
    f->pool = pool; f->key = *key; f->fd = -1;
    f->retryable = retryable; f->deferred_retry = true;
}

static bool current(source_fd *f)
{
    state_lock(f);
    bool valid = !f->stopped && f->pool->ops->valid(f->pool->user, &f->key);
    state_unlock(f);
    return valid;
}

/* Called by the worker under the storage lock. A failed close does not retire
 * either the DFS descriptor or its watcher pin, so fd/held must be retained. */
static void close_locked(source_fd *f)
{
    if (f->fd < 0) return;
    state_lock(f);
    f->close_attempted = true;
    f->last_close_release = f->release_id;
    state_unlock(f);
    if (f->pool->ops->close(f->pool->user, f->fd) == 0) f->fd = -1;
    else if (!f->retryable) {
        state_lock(f); f->quarantined = true; state_unlock(f);
    }
}

static void finish_transaction(source_fd *f)
{
    state_lock(f);
    f->busy = false;
    if (f->held && f->fd < 0) { f->held = false; --f->pool->held; }
    bool retry = f->held && f->stopped && !f->quarantined && f->deferred_retry;
    state_unlock(f);
    if (retry) f->pool->ops->wake_worker(f->pool->user);
}

source_fd_result source_fd_read(source_fd *f, uint32_t offset, uint32_t length,
                                void *data, uint32_t *actual)
{
    *actual = 0;
    if (offset > f->key.size || length > f->key.size - offset || length > 16u * 1024u) return SFD_IO;
    state_lock(f);
    if (f->stopped) { state_unlock(f); return SFD_STALE; }
    if (f->busy || (!f->held && f->pool->held >= f->pool->limit)) {
        state_unlock(f); return SFD_BUSY;
    }
    /* Reserve before open: a release arriving during open must see this owner. */
    if (!f->held) { f->held = true; ++f->pool->held; }
    f->busy = true;
    state_unlock(f);

    const source_fd_ops *ops = f->pool->ops; void *user = f->pool->user;
    ops->storage_lock(user);
    source_fd_result result = SFD_OK;
    if (!current(f)) result = SFD_STALE;
    if (result == SFD_OK && f->fd < 0) {
        f->fd = ops->open(user, f->key.path);
        if (f->fd < 0 || !ops->stat(user, f->fd, f->key.size)) result = SFD_IO;
    }
    if (result == SFD_OK && !current(f)) result = SFD_STALE;
    if (result == SFD_OK && !ops->seek(user, f->fd, offset)) result = SFD_IO;
    if (result == SFD_OK) {
        int got = ops->read(user, f->fd, data, length);
        if (got < 0) result = SFD_IO;
        else if ((uint32_t)got != length) result = SFD_SHORT;
        else *actual = length;
    }
    if (!current(f)) result = SFD_STALE;
    if (result != SFD_OK) {
        *actual = 0;
        state_lock(f); f->stopped = true; state_unlock(f);
        close_locked(f);
    }
    ops->storage_unlock(user);
    finish_transaction(f);
    return result;
}

bool source_fd_retire(source_fd *f, uint32_t release_id)
{
    state_lock(f);
    bool wake = !f->stopped || (release_id && f->release_id != release_id);
    f->stopped = true;
    if (release_id) f->release_id = release_id;
    bool released = !f->held && !f->busy;
    state_unlock(f);
    if (wake && !released) f->pool->ops->wake_worker(f->pool->user);
    return released;
}

bool source_fd_released(source_fd *f)
{
    state_lock(f);
    bool released = !f->held && !f->busy;
    state_unlock(f);
    return released;
}

source_fd_close_result source_fd_drain(source_fd *f)
{
    state_lock(f);
    if (f->quarantined) { state_unlock(f); return SFD_QUARANTINED; }
    if (!f->stopped || f->busy) { state_unlock(f); return SFD_PENDING; }
    if (!f->held) { state_unlock(f); return SFD_CLOSED; }
    if (f->close_attempted) {
        if (f->deferred_retry) f->deferred_retry = false;
        else if (!f->release_id || f->release_id == f->last_close_release) {
            state_unlock(f); return SFD_PENDING;
        }
    }
    f->busy = true;
    state_unlock(f);
    f->pool->ops->storage_lock(f->pool->user);
    close_locked(f);
    f->pool->ops->storage_unlock(f->pool->user);
    finish_transaction(f);
    state_lock(f);
    source_fd_close_result result = f->quarantined ? SFD_QUARANTINED : f->held ? SFD_PENDING : SFD_CLOSED;
    state_unlock(f);
    return result;
}

bool source_fd_restart(source_fd *f)
{
    state_lock(f);
    bool ok = !f->held && !f->busy && !f->quarantined &&
        f->pool->ops->valid(f->pool->user, &f->key);
    if (ok) {
        f->stopped = false; f->close_attempted = false; f->deferred_retry = true;
        f->release_id = f->last_close_release = 0;
    }
    state_unlock(f);
    return ok;
}
