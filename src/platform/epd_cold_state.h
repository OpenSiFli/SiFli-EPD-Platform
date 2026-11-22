/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_COLD_STATE_H
#define EPD_COLD_STATE_H
#include "epd_operation.h"

#define EPD_COLD_STATE_JOBS 8u
#define EPD_COLD_STATE_SLOTS 16u
#define EPD_COLD_STATE_BYTES_MAX 4096u
#define EPD_COLD_STATE_KEY_MAX 95u
typedef uint32_t epd_cold_state_t;
typedef enum {
    EPD_COLD_STATE_QUEUED, EPD_COLD_STATE_WORKING,
    EPD_COLD_STATE_SAVED, EPD_COLD_STATE_LOADED, EPD_COLD_STATE_FAILED
} epd_cold_state_status_t;
typedef enum {
    EPD_COLD_STATE_OK, EPD_COLD_STATE_IO, EPD_COLD_STATE_NOT_FOUND,
    EPD_COLD_STATE_FORMAT, EPD_COLD_STATE_STALE, EPD_COLD_STATE_FULL
} epd_cold_state_error_t;
typedef struct {
    /* Stable application/data identity, e.g. "app-id/reader-position".
     * It is stored inside a bounded slot file, never interpreted as a path. */
    const char *key;
    /* Retained execution/allocation token. For I/O after parent close use an
     * already LIVE credit below a core CLEANUP scope, covering buffer + backend
     * workspace. A plain closed owner cannot fund backend allocations. */
    epd_memory_owner_t cleanup_owner;
    uint32_t instance_generation, format_version, revision;
    bool optional; /* Only optional persisted states may be evicted. */
} epd_cold_state_spec_t;
typedef struct {
    epd_cold_state_status_t state;
    epd_cold_state_error_t error;
    uint32_t instance_generation, format_version, revision;
    size_t size;
    bool saving, retry_required;
} epd_cold_state_snapshot_t;
typedef struct {
    void *allocation;
    const void *data;
    size_t size;
    epd_memory_owner_t owner;
} epd_cold_state_blob_t;

/* Resident initialization only, not a dynamic-module export.
 * Bind once to a resident storage worker. No thread is created implicitly.
 * Worker calls dispatch after wake; producers never perform filesystem I/O.
 * Worker stack must allow a 4.3 KiB record plus storage backend call depth.
 * Completed jobs wake the existing GUI loop; get/copy retrieve the result.
 * Records occupy EPD_COLD_STATE_SLOTS fixed paths, with bounded temporary/backup
 * files owned by the existing atomic replacement backend. */
bool epd_cold_state_init(const epd_operation_executor_t *worker);
/* Copies already serialized bytes; no native object/pointer serialization.
 * Acceptance reserves one job and one bounded PSRAM buffer. The source remains
 * caller-owned. Failed acceptance preserves all caller state. Revisions are
 * monotonically increasing per key (no wrapping); change keys to start an epoch.
 * The caller must check loaded source/data identity before rebuilding handles. */
bool epd_cold_state_put(const epd_cold_state_spec_t *spec, const void *data, size_t size,
                        epd_cold_state_t *out);
bool epd_cold_state_load(const epd_cold_state_spec_t *spec, epd_cold_state_t *out);
bool epd_cold_state_get(epd_cold_state_t state, epd_cold_state_snapshot_t *out);
/* Bounded copy; failed capacity checks leave the output untouched. Also permits
 * retrieval of a failed save's bytes without ending its retry responsibility. */
bool epd_cold_state_copy(epd_cold_state_t state, void *out, size_t capacity, size_t *size);
bool epd_cold_state_retry(epd_cold_state_t state);
/* Successful saves/loads and failed loads can be released. Failed mandatory
 * saves remain owned, retain their cleanup owner, and can be retried even after
 * the original business scope closes. No automatic retry loop. */
bool epd_cold_state_release(epd_cold_state_t state);
/* Explicitly abandon an optional, non-running save; mandatory data is refused. */
bool epd_cold_state_abandon(epd_cold_state_t state);
/* Move a failed save's byte buffer AND owner reference to an explicit recovery
 * owner. Success retires the job; caller assumes persistence/retry responsibility.
 * The blob is a runtime handle, never a serialized state. Release it only after
 * saving or transferring that responsibility again. No allocation or copy. */
bool epd_cold_state_take(epd_cold_state_t state, epd_cold_state_blob_t *out);
void epd_cold_state_blob_release(epd_cold_state_blob_t *blob);
/* Resident worker only, not a dynamic-module export. */
size_t epd_cold_state_dispatch(size_t job_budget);
#endif
