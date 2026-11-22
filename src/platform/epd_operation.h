/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_OPERATION_H
#define EPD_OPERATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <rtthread.h>
#include <epd_memory_manager.h>

#define EPD_OPERATION_CAPACITY 32u
#define EPD_OPERATION_SCOPE_CAPACITY 64u
typedef uint32_t epd_operation_t;
typedef uint32_t epd_operation_scope_t;
typedef enum {
    EPD_OPERATION_OK, EPD_OPERATION_CANCELLED, EPD_OPERATION_EXPIRED,
    EPD_OPERATION_FAILED
} epd_operation_status_t;
typedef enum {
    EPD_OPERATION_QUEUED, EPD_OPERATION_RUNNING, EPD_OPERATION_SETTLING,
    EPD_OPERATION_READY, EPD_OPERATION_DELIVERING, EPD_OPERATION_FINISHED
} epd_operation_state_t;

/* Executors and their optional wake events must outlive all submitted work.
 * dispatch() checks rt_thread_self(); no callback runs in submit/cancel/complete.
 * The event is a coalescible wake only; the result stays in the operation slot.
 * Without an event, wake uses epd_power_notify for the existing GUI loop. */
typedef struct {
    rt_thread_t thread;
    struct rt_event *wake;
    uint32_t wake_bits;
} epd_operation_executor_t;
typedef struct {
    epd_operation_state_t state;
    epd_operation_status_t status;
    uint32_t instance_generation;
    bool cancel_requested, execution_done, result_pending, delivery_blocked;
    int error;
} epd_operation_snapshot_t;
typedef struct {
    epd_memory_owner_t owner;
    uint32_t instance_generation, operations;
    bool closing;
} epd_operation_scope_snapshot_t;
typedef struct {
    epd_operation_scope_t scope, cleanup_scope;
    epd_operation_executor_t execute, deliver, cleanup;
    bool has_deadline;
    uint32_t deadline_tick; /* Absolute RT tick; at most INT32_MAX ticks ahead. */
    void *user;
    /* May start asynchronous work. Call complete only after ALL accesses end. */
    void (*run)(epd_operation_t operation, void *user);
    /* true transfers the result to this receiver; false keeps it in the slot.
     * Retry after queue space becomes available. Do not retain a false result.
     * Scope close suppresses callbacks not yet claimed for delivery. A callback
     * already claimed is an in-flight reference; drain the scope before teardown. */
    bool (*receive)(epd_operation_t operation, const epd_operation_snapshot_t *state,
                    void *result, void *user);
    void (*dispose_result)(void *result, void *user);
    /* Last callback, on cleanup.thread, after run/receive/dispose have returned.
     * Release callback contexts and dynamic-module dependencies here. */
    void (*dispose_user)(void *user);
} epd_operation_spec_t;

/* Ordinary thread context. Fixed metadata pools, no allocation or I/O.
 * Scopes retain owner contexts. Register module lifetime dependencies before
 * submit; a successful submission owns user until dispose_user. Failure leaves
 * user with the caller. Use a DISTINCT scope for cleanup, created before closing
 * the business scope. Closing cleanup also refuses new work, but drains accepted
 * cleanup. release succeeds only after close and operations==0. If cleanup needs
 * allocations after parent close, bind this scope to an already LIVE memory
 * credit below a core CLEANUP scope; entering a plain closed owner grants no
 * allocation allowance. Retain that credit until all cleanup has completed. */
bool epd_operation_scope_open(epd_memory_owner_t owner, uint32_t instance_generation,
                              epd_operation_scope_t *out);
bool epd_operation_scope_get(epd_operation_scope_t scope, epd_operation_scope_snapshot_t *out);
bool epd_operation_scope_close(epd_operation_scope_t scope);
bool epd_operation_scope_release(epd_operation_scope_t scope);
/* Lifecycle bridge. Closes the core owner (idempotently, without releasing its
 * caller reference), then closes matching operation scopes and descendants.
 * Retained running operations still own their cleanup and callback dependencies.
 * Matching operation scope handles retire automatically after drain (become stale).
 * SYSTEM/INVALID are not close targets. drained checks both execution and cleanup
 * ancestry; true says nothing about other services/hardware outside this manager. */
void epd_operation_close_owner(epd_memory_owner_t owner);
bool epd_operation_owner_drained(epd_memory_owner_t owner);
bool epd_operation_submit(const epd_operation_spec_t *spec, epd_operation_t *out);
bool epd_operation_cancel(epd_operation_t operation);
/* Cooperative continuation, from run on execute.thread only, after this step
 * has ended all accesses. Requeues the SAME accepted slot; does not allocate,
 * renew its deadline, or release scopes. Return from run immediately afterward. */
bool epd_operation_yield(epd_operation_t operation);
/* true consumes result, including cancelled/expired operations; false leaves it
 * with the caller. Duplicate completion never consumes another result. A non-NULL
 * result requires dispose_result. Cancellation is a request, not completion. */
bool epd_operation_complete(epd_operation_t operation, epd_operation_status_t status,
                            int error, void *result);
bool epd_operation_get(epd_operation_t operation, epd_operation_snapshot_t *out);
bool epd_operation_retry_delivery(epd_operation_t operation);
/* Drops the sole observer handle; execution and cleanup remain manager-owned.
 * This does not cancel. A slot is recycled only after all callbacks return. */
bool epd_operation_release(epd_operation_t operation);
/* Resident scheduler only; not dynamic-module exports. */
void epd_operation_poll(void);
uint32_t epd_operation_next_timeout(uint32_t maximum_ticks);
size_t epd_operation_dispatch(size_t callback_budget);
#endif
