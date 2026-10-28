#ifndef EPD_SLEEP_GUARD_H
#define EPD_SLEEP_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* State starts zeroed. id points to a resident string (a service's copied ID or
 * a platform string literal); both owner and ID outlive the held interval. */
typedef struct sleep_guard_owner
{
    bool held;
    const char *id;
    struct sleep_guard_owner *next;
} sleep_guard_owner_t;
#define SLEEP_GUARD_ID_MAX 32
typedef enum
{
    SLEEP_GUARD_AWAKE,
    SLEEP_GUARD_PREPARING,
    SLEEP_GUARD_SLEEPING
} sleep_guard_state_t;
typedef struct
{
    uint32_t held_count;
    uint32_t revision;
    uint32_t changed_tick;
    sleep_guard_state_t state;
} sleep_guard_snapshot_t;

/* Idempotent. New work must acquire before it is accepted. Preparation is
 * cancelled on acquire; committed sleep rejects work until UI cancels sleep. */
bool sleep_guard_acquire(sleep_guard_owner_t *owner);
void sleep_guard_release(sleep_guard_owner_t *owner);
void sleep_guard_snapshot(sleep_guard_snapshot_t *snapshot);
/* Copy at most capacity holder IDs plus a coherent snapshot. Returns the total
 * holder count, including omitted entries. NULL/zero capacity reads only count.
 * Copies include stopping services until their final resource cleanup. */
size_t sleep_guard_holders(char (*ids)[SLEEP_GUARD_ID_MAX], size_t capacity,
                          sleep_guard_snapshot_t *snapshot);
/* UI owner only. Commit succeeds only while preparation is still valid. */
bool sleep_guard_prepare(void);
bool sleep_guard_commit(void);
void sleep_guard_cancel(void);

#endif
