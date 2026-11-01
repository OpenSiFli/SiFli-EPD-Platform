#ifndef UI_READER_FONT_SOURCE_FD_H
#define UI_READER_FONT_SOURCE_FD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reading_font.h"
typedef reading_font_lease_t source_fd_key;

typedef enum { SFD_OK, SFD_STALE, SFD_IO, SFD_SHORT, SFD_BUSY } source_fd_result;
typedef enum { SFD_CLOSED, SFD_PENDING, SFD_QUARANTINED } source_fd_close_result;

/* The state lock protects memory only. The worker drops it before acquiring
 * the storage lock; no filesystem callback runs while the state lock is held. */
typedef struct {
    void (*state_lock)(void *), (*state_unlock)(void *);
    void (*storage_lock)(void *), (*storage_unlock)(void *);
    bool (*valid)(void *, const source_fd_key *);
    int (*open)(void *, const char *);
    bool (*stat)(void *, int, uint32_t);
    bool (*seek)(void *, int, uint32_t);
    int (*read)(void *, int, void *, uint32_t);
    int (*close)(void *, int);
    void (*wake_worker)(void *);
} source_fd_ops;

typedef struct {
    const source_fd_ops *ops;
    void *user;
    unsigned held;
    unsigned limit;
} source_fd_pool;

typedef struct {
    source_fd_pool *pool;
    source_fd_key key;
    int fd;                 /* Worker-owned; UI checks held/busy, never fd. */
    bool held, busy, stopped, retryable, quarantined;
    bool close_attempted, deferred_retry;
    uint32_t release_id, last_close_release;
} source_fd;

void source_fd_init(source_fd *, source_fd_pool *, const source_fd_key *, bool retryable_close);
/* Worker only. One bounded read transaction; descriptors survive successful reads.
 * The caller rechecks its lease/cancel state before publishing the returned bytes. */
source_fd_result source_fd_read(source_fd *, uint32_t offset, uint32_t length, void *, uint32_t *actual);
/* UI safe: mark retired and wake the worker. Repeated release IDs do not add retries. */
bool source_fd_retire(source_fd *, uint32_t release_id);
/* Memory-only acknowledgment gate. The object must remain owned until true. */
bool source_fd_released(source_fd *);
/* Worker only: at most one close attempt per call. Retain failed-close ownership. */
source_fd_close_result source_fd_drain(source_fd *);
/* Worker only: reuse a drained face for another live shell, never during handoff. */
bool source_fd_restart(source_fd *);
#endif
