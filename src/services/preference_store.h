/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_PREFERENCE_STORE_H
#define EPD_PREFERENCE_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PREFERENCE_IDENTITY, PREFERENCE_TIME, PREFERENCE_LANGUAGE, PREFERENCE_COUNT
} preference_id_t;
typedef struct { bool pending, failed; } preference_status_t;
typedef enum {
    PREFERENCE_SYNC_UNCHANGED, PREFERENCE_SYNC_SAVED, PREFERENCE_SYNC_FAILED
} preference_sync_result_t;

/* UI thread: prepare the resident worker before taking a settings snapshot
 * lock. No storage I/O. submit also prepares it when needed. */
bool preference_store_prepare(void);

/* UI producer. Copies the record; each preference retains only its latest
 * pending value. path must have firmware lifetime. Failure to accept leaves
 * the running value unchanged. */
bool preference_store_submit(preference_id_t id, const char *path, uint32_t magic,
                             uint32_t version, const void *data, size_t size);
preference_status_t preference_store_status(preference_id_t id);

/* failed describes the last completed write, and stays set during a retry.
 * A completed failure has no outstanding I/O and does not hold service exit. */

/* Ordinary-thread durable APIs/reloads only. Wait up to 2 seconds for earlier
 * writes/exclusive access, then exclude submissions until end_sync. A timeout
 * leaves existing work and its failure status intact. Never use in a UI callback.
 * Loads and operations that never attempted a write end with UNCHANGED. */
bool preference_store_begin_sync(preference_id_t id);
void preference_store_end_sync(preference_id_t id, preference_sync_result_t result);
#endif
