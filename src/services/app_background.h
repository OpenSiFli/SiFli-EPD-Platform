/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_APP_BACKGROUND_H
#define EPD_APP_BACKGROUND_H

#include "../platform/epd_memory_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t epd_app_task_group_t;
#define EPD_APP_TASK_GROUP_NONE 0u

typedef enum {
    EPD_APP_BG_RELEASE,
    EPD_APP_BG_STATE,
    EPD_APP_BG_TASKS
} epd_app_background_mode_t;

typedef struct {
    epd_app_background_mode_t mode;
    epd_app_task_group_t group;
    epd_app_background_mode_t after_tasks; /* RELEASE or STATE. */
} epd_app_background_policy_t;

typedef enum {
    EPD_APP_BG_FOREGROUND,
    EPD_APP_BG_RUNNING,
    EPD_APP_BG_RETAINED,
    EPD_APP_BG_COLD,
    EPD_APP_BG_STOPPING,
    EPD_APP_BG_RELEASED
} epd_app_background_state_t;

typedef struct {
    epd_memory_owner_t owner;
    epd_app_background_policy_t requested;
    epd_app_background_mode_t effective;
    epd_app_background_state_t actual;
    uint32_t revision, applied_revision, event_revision;
    uint32_t dirty_revision, saved_revision, tasks;
    bool foreground, stopping, explicit_policy;
    epd_memory_result_t last_result;
} epd_app_background_snapshot_t;

typedef uint32_t epd_app_background_subscription_t;
typedef enum {
    EPD_APP_BG_EVENT_INITIAL,
    EPD_APP_BG_EVENT_POLICY,
    EPD_APP_BG_EVENT_FOREGROUND,
    EPD_APP_BG_EVENT_TASKS,
    EPD_APP_BG_EVENT_ACTUAL,
    EPD_APP_BG_EVENT_PERSISTENCE,
    EPD_APP_BG_EVENT_STOP,
    EPD_APP_BG_EVENT_REJECTED
} epd_app_background_reason_t;
typedef struct {
    uint64_t sequence; /* Monotonic, saturating; delivery does not depend on wrap. */
    uint32_t instance_generation;
    epd_app_background_reason_t reason;
    epd_memory_result_t result; /* Optional discard retains the save failure through RELEASED. */
    epd_app_background_snapshot_t state;
} epd_app_background_event_t;
typedef void (*epd_app_background_notify_t)(const epd_app_background_event_t *event, void *context);

/* Captures the current open PAGE on GUI, or TASK/SERVICE on the operation worker.
 * No app ID, owner pin or background task is retained by an idle subscription.
 * Initial and changed snapshots are asynchronous and may coalesce to the newest.
 * Register/unsubscribe on the same executor as notification. Successful removal
 * prevents later calls; self-removal leaves only the current callback in flight.
 * Owner close suppresses queued callbacks. Drain accepted operations before
 * destroying a context/code that an already claimed callback may still access.
 * The event pointer is valid only during notify. Handles never wrap or alias. */
epd_memory_result_t epd_app_background_subscribe(epd_app_background_notify_t notify,
    void *context, epd_app_background_subscription_t *out);
epd_memory_result_t epd_app_background_unsubscribe(epd_app_background_subscription_t subscription);

/* Current registered application context. Acceptance does not imply teardown
 * or persistence has finished. Neither call waits or invokes application code. */
epd_memory_result_t epd_app_background_set(const epd_app_background_policy_t *policy,
                                         uint32_t *revision_out);
epd_memory_result_t epd_app_background_get(epd_app_background_snapshot_t *out);
/* Explicit work outside platform components. Create/begin in the application
 * context, select TASKS after begin, and end only after all accesses finish.
 * End/release may run in any retained scope of the same application. */
epd_memory_result_t epd_app_task_group_create(epd_app_task_group_t *out);
epd_memory_result_t epd_app_task_begin(epd_app_task_group_t group, epd_memory_owner_t *out);
bool epd_app_task_end(epd_memory_owner_t task);
epd_memory_result_t epd_app_task_group_release(epd_app_task_group_t group);

/* Runtime integration; these functions are not exported to dynamic modules.
 * Discovery uses false; only a new user action or authorized business trigger
 * uses true. A pending explicit enter waits for the old instance to unbind.
 * Memory recovery and network notifications are not new execution intents. */
bool app_background_should_start(const char *id, bool explicit_enter);
/* One retained root per binding; same root is idempotent. Unbind before the
 * loader's final reference check, after tasks/groups and dirty data drain. */
epd_memory_result_t app_background_bind(const char *id, epd_memory_owner_t owner);
epd_memory_result_t app_background_unbind(epd_memory_owner_t owner);
/* Successful uninstallation only, after the instance and registered work are
 * gone and scheduling has been cancelled. Ordinary release/eviction and failed
 * installation must keep the record's stop intent. Pending schedules refuse
 * retirement; old nonzero schedule generations remain invalid after reuse. */
bool app_background_forget(const char *id);
epd_memory_result_t app_background_foreground(epd_memory_owner_t owner, bool foreground);
epd_memory_result_t app_background_snapshot(epd_memory_owner_t owner,
                                           epd_app_background_snapshot_t *out);
/* GUI close/eviction gate. Quiesces services and schedules durable state capture.
 * STATE without a checkpoint may be released once its dirty revision is saved;
 * the next explicit enter rebuilds it through normal application startup.
 * Optional IO/FULL discard returns RELEASED with saved_revision unchanged and
 * the failure in last_result and event.result. COLD requires durable state.
 * Keep the root and module alive on any non-OK result. OK seals new work and
 * returns COLD or RELEASED; then detach cold providers before module cleanup.
 * Final report/unbind follows operation/resource draining and module cleanup. */
epd_memory_result_t app_background_prepare_close(epd_memory_owner_t owner,
                                                epd_app_background_state_t *terminal_state);
/* applied_revision means policy application started. actual is reported only
 * after the runtime reaches it; stale revisions cannot overwrite new policy. */
epd_memory_result_t app_background_report(epd_memory_owner_t owner, uint32_t revision,
                                         epd_app_background_state_t actual);
void app_background_stopped(epd_memory_owner_t owner);
/* Withdraw retention and new work for installation. Existing tasks and dirty
 * data drain through prepare_close; the memory owner remains open until then. */
epd_memory_result_t app_background_request_stop(epd_memory_owner_t owner);
/* A root service ending disables discovery restart, not the page's lifetime. */
void app_background_service_stopped(epd_memory_owner_t owner);
bool app_background_accepting(epd_memory_owner_t owner);

/* One resident, one-shot deadline per application; no code or owner pin.
 * Scheduling/cancellation is generation checked, so a stale UI poll cannot
 * consume a replacement deadline, even after uninstall/reinstall or slot reuse.
 * Only authorized service callers register. Generation exhaustion refuses new
 * schedules without changing the currently accepted deadline. */
bool app_background_schedule(epd_memory_owner_t owner, int32_t delay_ticks,
                             bool require_network, uint32_t now);
void app_background_schedule_cancel(const char *id);
bool app_background_schedule_due(uint32_t now, bool network_ready,
                                 char *id, size_t capacity, uint32_t *generation);
bool app_background_schedule_claim(const char *id, uint32_t generation, uint32_t now);
/* Accepted work survives deferred catalog discovery. Zero means no live
 * dispatch; cancellation or replacement invalidates the returned generation. */
uint32_t app_background_schedule_claimed(const char *id);
/* Complete only a dispatched generation. Zero acknowledges the current start;
 * a deadline rearmed by the worker has already cleared dispatched. */
void app_background_schedule_ack(const char *id, uint32_t generation);

/* Register actual accepted operations, not idle worker threads. Group and task
 * handles belong to one instance; an emptied group cannot accept new tasks.
 * Each successful begin owns a TASK scope, released exactly once by end.
 * These hooks do not grant memory priority or hold a sleep lock. */
epd_memory_result_t app_background_group_create(epd_memory_owner_t owner,
                                               epd_app_task_group_t *out);
epd_memory_result_t app_background_task_begin(epd_app_task_group_t group,
                                             epd_memory_owner_t *task_out);
bool app_background_task_end(epd_memory_owner_t task);
epd_memory_result_t app_background_group_release(epd_app_task_group_t group);
bool app_background_group_accepting(epd_app_task_group_t group);
/* Component defaults form a union and never override an explicit policy. */
epd_memory_result_t app_background_group_default(epd_app_task_group_t group,
                                                 epd_app_background_mode_t after_tasks);

/* Mark before publishing a data change. Completion acknowledges only the
 * captured revision; an intervening edit keeps the barrier dirty. */
epd_memory_result_t app_background_dirty(epd_memory_owner_t owner, uint32_t *revision_out);
bool app_background_saved(epd_memory_owner_t owner, uint32_t revision);
bool app_background_clean(epd_memory_owner_t owner);

#ifdef __cplusplus
}
#endif
#endif
