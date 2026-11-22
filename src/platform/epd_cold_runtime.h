/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_COLD_RUNTIME_H
#define EPD_COLD_RUNTIME_H
#include "epd_cold_state.h"

#define EPD_CHECKPOINT_CAPACITY 16u
#define EPD_CHECKPOINT_BYTES_MAX (EPD_COLD_STATE_BYTES_MAX - 12u)
typedef uint32_t epd_checkpoint_t;
typedef struct {
    /* Relative business identity; platform prefixes the current account appid.
     * Identity affects storage lookup only, never admission or background policy. */
    const char *key;
    uint32_t format_version, source_version;
    size_t maximum_bytes;
    bool optional;
    void *user;
    /* GUI thread, bounded stable model capture, no filesystem I/O. Failure must
     * leave the model usable. user/model/code live until detach returns. */
    bool (*capture)(void *user, void *bytes, size_t capacity, size_t *size);
    /* GUI thread, after format/source/generation checks. Publish rebuilt objects
     * only on success; failure retains the loaded bytes for explicit retry. */
    bool (*restore)(void *user, const void *bytes, size_t size);
    /* GUI thread, once after accepted work ends and dirty required data is saved.
     * Release view/model references here; loader teardown follows this callback. */
    void (*detach)(void *user);
} epd_checkpoint_spec_t;

/* GUI merge after edits during this load. pending_edits counts successful
 * changed() calls since loading started, not the old baseline. Merge those
 * edits with the validated bytes and publish only on success. Failure retains
 * both the model and loaded bytes for explicit retry. */
typedef bool (*epd_checkpoint_restore_edits_t)(void *user, const void *bytes, size_t size,
                                              uint32_t pending_edits);

typedef struct {
    bool registered, durable, busy, failed, restoring, closing;
    uint32_t captured_policy_revision, captured_dirty_revision;
    uint32_t data_revision, saved_revision;
    epd_memory_result_t result;
    epd_cold_state_error_t storage_error;
} epd_cold_runtime_snapshot_t;

/* Public GUI APIs. One provider per live application account. Registration
 * retains the account and a cleanup scope and starts asynchronous restoration.
 * Duplicate live storage identities are refused until the old owner detaches.
 * Every public handle call requires a live scope in its owning APP account;
 * an appid match never grants access to another application instance. */
bool epd_checkpoint_register(const epd_checkpoint_spec_t *spec, epd_checkpoint_t *out);
/* Additive registration keeps the original spec/snapshot ABI unchanged.
 * register() keeps replacement semantics; this form requires a merge callback. */
bool epd_checkpoint_register_edits(const epd_checkpoint_spec_t *spec,
                                    epd_checkpoint_restore_edits_t restore_edits, epd_checkpoint_t *out);
/* Call before publishing an edit. Also marks app_background_dirty(account).
 * Edits during loading use restore_edits when provided; otherwise they replace
 * the loaded bytes. Callers must keep their pending edits usable on failure. */
bool epd_checkpoint_changed(epd_checkpoint_t checkpoint);
bool epd_checkpoint_get(epd_checkpoint_t checkpoint, epd_cold_runtime_snapshot_t *out);
bool epd_checkpoint_restore(epd_checkpoint_t checkpoint);
bool epd_checkpoint_retry(epd_checkpoint_t checkpoint);
/* Nonblocking: rejects discarding unsaved mandatory changes. A successful call
 * requests GUI detach; get stays valid until that callback has returned. */
bool epd_checkpoint_unregister(epd_checkpoint_t checkpoint);

/* Resident background/loader APIs, not module exports. The request is idempotent
 * for one policy+dirty revision. capture/restore/detach never execute inline. */
epd_memory_result_t epd_cold_runtime_checkpoint(epd_memory_owner_t owner,
                                               uint32_t policy_revision, uint32_t dirty_revision);
bool epd_cold_runtime_get(epd_memory_owner_t owner, epd_cold_runtime_snapshot_t *out);
/* Optional save failed with IO/FULL for these current policy/dirty revisions.
 * False for required data, stale captures, foreground/tasks or active work.
 * This is not durable success; the close gate must also quiesce services and
 * seal the unchanged request before authorizing RELEASED. */
bool epd_cold_runtime_discardable(epd_memory_owner_t owner,
                                 uint32_t policy_revision, uint32_t dirty_revision);
/* After background_prepare_close returns OK, request detach before calling any
 * module cleanup or closing the core owner. Keep module code/user alive while busy. */
void epd_cold_runtime_close_owner(epd_memory_owner_t owner);
bool epd_cold_runtime_owner_busy(epd_memory_owner_t owner);
size_t epd_cold_runtime_ui_pump(size_t budget);
size_t epd_cold_runtime_worker_pump(size_t budget);
#endif
