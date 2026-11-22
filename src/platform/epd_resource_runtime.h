#ifndef EPD_RESOURCE_RUNTIME_H
#define EPD_RESOURCE_RUNTIME_H

#include "epd_resource.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *context;
    /* Optional UI wake notification (lane is UI). Worker wake uses epd_ops.
     * Enqueue GUI execution, never pump inline. NULL uses the existing GUI loop. */
    void (*schedule)(void *context, epd_resource_lane_t lane);
    /* Optional deadline observation; epd_ops owns the actual timed event wait. */
    void (*deadline)(void *context, uint32_t delay_ms);
    /* GUI dispatcher only: return the actual active PAGE of this account.
     * Borrowed token; the runtime validates its live page binding again. */
    epd_memory_owner_t (*foreground_page)(void *context, epd_memory_owner_t account);
} epd_resource_runtime_config_t;

/* Registers with epd_operation_runtime; NULL selects normal platform scheduling.
 * Main initializes epd_operation_runtime on GUI and calls resource_ui in its pump. */
int epd_resource_runtime_init(const epd_resource_runtime_config_t *config);
size_t epd_resource_runtime_worker(size_t budget);
size_t epd_resource_runtime_ui(size_t budget);
/* Capacity, task, storage or deadline event; publishes work without doing callbacks. */
void epd_resource_runtime_changed(void);

/* Runtime establishes a real core task/page/service scope before exposing a binding.
 * Eligibility reflects validated business state; it never comes from an app ID. */
int epd_resource_runtime_bind(epd_memory_owner_t scope, uint64_t instance_generation,
                              uint32_t kind_mask, epd_resource_binding_t *binding_out);

/* Trusted lifecycle hooks, not dynamic-module exports. Only the page dispatcher
 * and managed operation/service runtime may publish these facts. */
int epd_resource_runtime_page_state(epd_memory_owner_t page, uint64_t generation, bool active);
int epd_resource_runtime_task_state(epd_memory_owner_t task_or_service, uint64_t generation, bool running);
/* Independent service fact; CONTINUOUS remains eligible while either source runs. */
int epd_resource_runtime_service_state(epd_memory_owner_t service, uint64_t generation, bool running);
/* Trusted synchronous GUI service dispatch. These hooks do not change memory
 * ownership and are not module exports. Balance leave even if the callback
 * changes pages; the active page is resolved at each interactive submission. */
epd_memory_owner_t epd_resource_runtime_ui_enter(epd_memory_owner_t service);
void epd_resource_runtime_ui_leave(epd_memory_owner_t previous);
/* Internal queued-work adapter: retain the verified PAGE for INTERACTIVE or
 * the live interactive parent's PAGE for NEARBY. Caller releases the token. */
epd_memory_owner_t epd_resource_context_acquire(epd_resource_kind_t kind,
    epd_resource_activity_t interactive_parent);

typedef struct {
    epd_resource_kind_t kind;
    uint32_t backend;
    size_t minimum_bytes, target_bytes;
    uint64_t deadline;
    epd_resource_activity_t interactive_parent;
    size_t companion_bytes[EPD_MEMORY_BACKEND_COUNT];
} epd_resource_context_request_t;

/* Public application entry: eligibility comes from live owner ancestry, trusted
 * GUI dispatch, or a validated interactive parent. No caller-supplied priority
 * mask or application identity is accepted. */
int epd_resource_context_begin(const epd_resource_context_request_t *request,
                               const epd_resource_observer_t *observer,
                               epd_resource_activity_t *out);
int epd_resource_context_update(epd_resource_activity_t activity,
                                const epd_resource_context_request_t *request,
                                uint64_t *revision_out);
#if !defined(BUILD_DLMODULE) && !defined(RTT_IN_MODULE)
/* Resident admission hint. Observe only requested backends after validating
 * the current PAGE/TASK/SERVICE binding. No grant, queue or deadline changes.
 * A capacity observation does not bypass fair waiting or reserve memory. */
int epd_resource_context_capacity(const epd_resource_context_request_t *request,
    size_t available[EPD_MEMORY_BACKEND_COUNT], uint32_t *revision);
#endif
/* Resume a PAUSED private activity using current PAGE/TASK/SERVICE facts.
 * The worker jointly admits all backend additions, then restores the original
 * token. ACCEPTED is not admission; wait for ADMITTED at the returned revision.
 * Failure preserves PAUSED physical data with zero unused reservation. */
int epd_resource_context_resume(epd_resource_activity_t activity,
                                const epd_resource_context_request_t *request,
                                const size_t unused[EPD_MEMORY_BACKEND_COUNT],
                                uint64_t *revision_out);

/* Consume the admitted reservation in a synchronous operation. Returns the
 * previous allocation context, or INVALID for an unadmitted/ended/zero credit.
 * Only the activity's owner or its descendants may enter. Always balance leave. */
epd_memory_owner_t epd_resource_context_enter(epd_resource_activity_t activity);
void epd_resource_context_leave(epd_memory_owner_t previous);
/* Before a component allocates an explicitly sized object, ensure the current
 * live credit has this much unused physical space in the allocator region.
 * Existing blocks remain charged; failure preserves the complete old working
 * set. With no credit, normal owner admission remains in the allocator. */
int epd_resource_context_reserve(epd_memory_region_t region, size_t physical_bytes);
/* Additional SRAM bound for one absolute-path POSIX file/directory operation
 * on an already mounted ELM volume. Includes fd, path copies, FIL/DIR, directory
 * stream and configured LFN workspace. Existing open files remain charged.
 * path_bytes includes NUL and bounds both paths for rename. Excludes stdio,
 * volume mounting/formatting and explicitly enabled fast-seek tables.
 * Returns SIZE_MAX for overflow or an unavailable filesystem adapter. */
size_t epd_resource_file_io_peak(size_t path_bytes);

/* A component keeps its own key table, pin checks and victim ordering. This
 * adapter publishes one incremental candidate per real allocator backend.
 * Open/update/close are serialized by the component's existing mutex or UI lane.
 * The current account supplies the module lifetime; resident platform caches
 * open in SYSTEM context. This API grants no activity priority. */
typedef struct {
    epd_resource_provider_t provider;
    epd_memory_owner_t owner;
} epd_resource_cache_t;
int epd_resource_cache_open(epd_resource_cache_t *cache, epd_resource_lane_t lane,
    int (*shrink)(void *, uint32_t, size_t, size_t *), void *context);
int epd_resource_cache_update(epd_resource_cache_t *cache, const void *allocator,
    size_t total, size_t reclaimable, uint32_t rebuild_cost, uint32_t local_value);
int epd_resource_cache_update_region(epd_resource_cache_t *cache, epd_memory_region_t region,
    size_t total, size_t reclaimable, uint32_t rebuild_cost, uint32_t local_value);
int epd_resource_cache_close(epd_resource_cache_t *cache);
/* Transfer a completed cache allocation without copying it. The cache owner
 * survives individual work credits and remains subject to its account budget. */
int epd_resource_cache_adopt(epd_resource_cache_t *cache, void *allocation);

/* Safe under loader coordination: mark-only, nonwaiting, no provider callbacks.
 * Stop this scope's descendants and providers before final dlclose.
 * SYSTEM providers have independent lifetime and survive another owner's close. */
int epd_resource_runtime_close_owner(epd_memory_owner_t owner);
bool epd_resource_runtime_owner_busy(epd_memory_owner_t owner);

#ifdef __cplusplus
}
#endif
#endif
