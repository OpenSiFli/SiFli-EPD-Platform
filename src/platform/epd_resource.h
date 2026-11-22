#ifndef EPD_RESOURCE_H
#define EPD_RESOURCE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "epd_memory_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handles carry a generation; zero is never a valid handle. */
typedef uint64_t epd_resource_activity_t;
typedef uint64_t epd_resource_binding_t;
typedef uint64_t epd_resource_provider_t;
typedef uint64_t epd_resource_t;
typedef uint64_t epd_resource_ref_t;
typedef uint64_t epd_resource_credit_t;

typedef enum {
    EPD_RESOURCE_OK = 0,
    EPD_RESOURCE_ACCEPTED = 1,
    EPD_RESOURCE_PENDING = 2,
    EPD_RESOURCE_INVALID = -1,
    EPD_RESOURCE_CAPACITY = -2,
    EPD_RESOURCE_STOPPED = -3,
    EPD_RESOURCE_STALE = -4,
    EPD_RESOURCE_DENIED = -5,
    EPD_RESOURCE_FAILED = -6,
    EPD_RESOURCE_CANCELLED = -7,
    EPD_RESOURCE_BUSY = -8
} epd_resource_result_t;

/* Values increase with retention priority; none of these imply a pin or PM vote. */
typedef enum {
    EPD_RESOURCE_OPTIONAL,
    EPD_RESOURCE_NEARBY,
    EPD_RESOURCE_CONTINUOUS,
    EPD_RESOURCE_INTERACTIVE
} epd_resource_kind_t;
#define EPD_RESOURCE_KIND_MASK(kind) (1u << (unsigned)(kind))

typedef enum { EPD_RESOURCE_WORKER, EPD_RESOURCE_UI } epd_resource_lane_t;
typedef enum {
    EPD_RESOURCE_WAITING, EPD_RESOURCE_ADMITTED, EPD_RESOURCE_REJECTED,
    EPD_RESOURCE_ENDING, EPD_RESOURCE_ENDED,
    EPD_RESOURCE_PAUSING, EPD_RESOURCE_PAUSED, EPD_RESOURCE_RESUMING
} epd_resource_activity_state_t;

typedef struct {
    epd_resource_binding_t binding;
    epd_resource_activity_t interactive_parent; /* Required for NEARBY. */
    epd_resource_kind_t kind;
    uint32_t backend;
    size_t minimum_bytes;
    size_t target_bytes;
    uint64_t deadline; /* Absolute runtime time; zero means no waiting deadline. */
    /* Additional unused capacity on other backends, admitted atomically. */
    size_t companion_bytes[EPD_MEMORY_BACKEND_COUNT];
} epd_resource_activity_spec_t;

typedef struct {
    epd_resource_activity_t activity;
    epd_memory_owner_t owner;
    uint64_t instance_generation;
    uint64_t requested_revision;
    uint64_t effective_revision;
    epd_resource_activity_state_t state;
    epd_resource_activity_spec_t requested;
    epd_resource_activity_spec_t effective;
    epd_resource_credit_t credit;
    size_t guaranteed_bytes;
    size_t unused_bytes;
    size_t associated_bytes; /* Shared physical bytes, once per activity. */
    uint32_t references;
    uint32_t pins;
    int reason;
} epd_resource_activity_snapshot_t;

typedef void (*epd_resource_activity_notify_t)(
    void *context, const epd_resource_activity_snapshot_t *snapshot);

typedef struct {
    epd_resource_activity_notify_t notify;
    void *context;
    epd_resource_lane_t lane;
} epd_resource_observer_t;

/* ACCEPTED is registration only. The worker performs admission later. */
int epd_resource_activity_begin(const epd_resource_activity_spec_t *spec,
                               const epd_resource_observer_t *observer,
                               epd_resource_activity_t *activity_out);
int epd_resource_activity_update(epd_resource_activity_t activity,
                                const epd_resource_activity_spec_t *spec,
                                uint64_t *revision_out);
int epd_resource_activity_get(epd_resource_activity_t activity,
                             epd_resource_activity_snapshot_t *snapshot);
/* Runtime-only, balanced by the execution context's leave. */
epd_resource_credit_t epd_resource_execution_acquire(epd_resource_activity_t activity);
void epd_resource_execution_release(epd_resource_credit_t credit);
/* Caller first stops execution and drains allocation guards. Only private
 * activities without resource references/borrowers can pause. ACCEPTED blocks
 * new entry immediately; PAUSED confirms that every unused backend was returned.
 * Physical blocks and their original credit token survive until free or end. */
int epd_resource_activity_pause(epd_resource_activity_t activity);
/* Runtime entry: validate the genuine context before supplying its binding.
 * unused is the additional reservation required on each backend; the main
 * backend must equal spec.target_bytes. An all-zero reservation is valid. */
int epd_resource_activity_resume(epd_resource_activity_t activity,
                                 const epd_resource_activity_spec_t *spec,
                                 const size_t unused[EPD_MEMORY_BACKEND_COUNT],
                                 uint64_t *revision_out);
int epd_resource_activity_end(epd_resource_activity_t activity);
/* Acknowledge ENDED after notifications have drained, then recycle the handle. */
int epd_resource_activity_forget(epd_resource_activity_t activity);
/* End and retire the observer handle automatically after all uses/notifications drain. */
int epd_resource_activity_dispose(epd_resource_activity_t activity);

#define EPD_RESOURCE_KEY_BYTES 48u
typedef struct {
    uint8_t bytes[EPD_RESOURCE_KEY_BYTES];
    uint8_t length;
    uint64_t version;
} epd_resource_key_t;

typedef struct {
    epd_resource_key_t key;
    uint32_t backend;
    size_t estimated_bytes;
    uint32_t rebuild_cost;
    uint32_t local_value; /* Compared only within one provider. */
    bool discardable;
} epd_resource_descriptor_t;

typedef enum {
    EPD_RESOURCE_LOADING, EPD_RESOURCE_READY, EPD_RESOURCE_LOAD_FAILED,
    EPD_RESOURCE_RETIRING, EPD_RESOURCE_DESTROYING
} epd_resource_state_t;

typedef struct {
    epd_resource_t resource;
    epd_resource_state_t state;
    epd_resource_descriptor_t descriptor;
    epd_resource_kind_t priority;
    size_t physical_bytes;
    uint32_t references;
    uint32_t pins;
    uint64_t data_revision;
    uint64_t saved_revision;
    bool dirty;
    bool invalidated;
    int result;
} epd_resource_snapshot_t;

typedef void (*epd_resource_notify_t)(void *context, epd_resource_ref_t reference,
                                    int result);

typedef struct {
    void *context;
    epd_memory_owner_t code_owner;
    epd_memory_owner_t lifetime_owner; /* Zero defaults to code_owner. */
    uint64_t implementation_version;
    epd_resource_lane_t load_lane;
    epd_resource_lane_t destroy_lane;
    /* PENDING keeps code/data alive until load_complete. All callbacks run unlocked.
     * A successful load transfers data exactly once through load_complete, including
     * synchronous completion. Return a negative result for failure without data. */
    int (*load)(void *context, epd_resource_t resource,
                const epd_resource_descriptor_t *descriptor,
                epd_resource_credit_t physical_credit);
    /* Cancellation requests cooperation; it never substitutes for load_complete. */
    void (*cancel)(void *context, epd_resource_t resource);
    /* Failure preserves the complete object. OK means physical destruction finished. */
    int (*destroy)(void *context, epd_resource_t resource, void *data, size_t bytes);
    /* Optional cache-bank adapter. Recheck local pins under the component lock;
     * free a bounded batch and report only physically returned bytes. Publish
     * the new totals with provider_cache_update before returning. */
    int (*shrink)(void *context, uint32_t backend, size_t requested_bytes, size_t *released_bytes);
} epd_resource_provider_spec_t;

int epd_resource_provider_register(const epd_resource_provider_spec_t *spec,
                                   epd_resource_provider_t *provider_out);
/* Call before final module close. PENDING retains code until all refs/callbacks drain.
 * Repeating unregister returns OK once drained and recycles the provider handle. */
int epd_resource_provider_unregister(epd_resource_provider_t provider);
/* Incremental candidate publication for existing component-owned cache banks.
 * total includes protected bytes, reclaimable excludes dirty/in-use bytes.
 * The bank is one aggregate view; its allocations must not also be adopted.
 * Update after local mutations, including final release while unregistering.
 * A queued reclaim whose offer becomes zero settles without a shrink callback;
 * a shrink already in flight still requires its normal completion.
 * Local cache/draw locks may be held; no callback executes in this function. */
int epd_resource_provider_cache_update(epd_resource_provider_t provider, uint32_t backend,
                                       size_t total_bytes, size_t reclaimable_bytes,
                                       uint32_t rebuild_cost, uint32_t local_value);
/* A shrink may return PENDING after dispatch to the component's own executor.
 * That executor publishes current totals, then completes exactly once. Pending
 * reclamation and provider code ownership survive until this call. */
int epd_resource_provider_cache_complete(epd_resource_provider_t provider, uint32_t backend,
                                         size_t released_bytes, int result);
int epd_resource_acquire(epd_resource_activity_t activity,
                         epd_resource_provider_t provider,
                         const epd_resource_descriptor_t *descriptor,
                         epd_resource_notify_t notify, void *context,
                         epd_resource_lane_t lane, epd_resource_ref_t *reference_out);
/* Publish an already core-accounted allocation without another reservation.
 * data must be the live SDK allocation's base address, with no concurrent
 * free/realloc/ownership transfer. Aligned interior or borrowed pointers are invalid.
 * ACCEPTED transfers ownership; every other result leaves it with the caller.
 * A duplicate key with a different payload returns BUSY. */
int epd_resource_adopt(epd_resource_activity_t activity, epd_resource_provider_t provider,
                       const epd_resource_descriptor_t *descriptor, void *data,
                       size_t physical_bytes, epd_resource_ref_t *reference_out);
/* Transfer a sole, unpinned, clean reference back to its caller for local disposal.
 * Core physical accounting remains with the allocation until its actual free.
 * This is an ownership transfer and is not counted as pressure reclamation. */
int epd_resource_take(epd_resource_ref_t reference, void **data_out);
/* Each reference owns independent pin counts; pin is the only pointer access API. */
int epd_resource_pin(epd_resource_ref_t reference, void **data_out);
int epd_resource_unpin(epd_resource_ref_t reference);
int epd_resource_release(epd_resource_ref_t reference);
int epd_resource_get(epd_resource_t resource, epd_resource_snapshot_t *snapshot);
int epd_resource_ref_get(epd_resource_ref_t reference, epd_resource_t *resource_out);
int epd_resource_load_complete(epd_resource_t resource, int result,
                               void *data, size_t physical_bytes);
int epd_resource_invalidate(epd_resource_t resource);
int epd_resource_mark_dirty(epd_resource_t resource, uint64_t data_revision);
/* A failed save leaves dirty state and the provider dependency intact. */
int epd_resource_saved(epd_resource_t resource, uint64_t saved_revision, int result);
int epd_resource_set_local_value(epd_resource_t resource, uint32_t local_value);

/* Runtime-only authority: bind genuine managed task/resource scope, never app IDs.
 * mask is runtime-verified eligibility; restrict/end consume lifecycle events. */
int epd_resource_binding_open(epd_memory_owner_t owner, uint64_t instance_generation,
                              uint64_t resource_scope, uint32_t kind_mask,
                              epd_resource_binding_t *binding_out);
int epd_resource_binding_restrict(epd_resource_binding_t binding, uint32_t kind_mask);
int epd_resource_binding_end(epd_resource_binding_t binding);

typedef struct {
    epd_resource_credit_t credit;
    size_t guaranteed_bytes;
    size_t unused_bytes;
} epd_resource_grant_t;

/* The core owns all reservation/accounting. Adapters are called outside the lock.
 * apply must leave previous credit unchanged on PENDING/failure. Success returns
 * a separately owned replacement; the framework releases previous after commit.
 * existing physical associations are reported so core does not reserve them twice.
 * Resource admission is called once per shared physical load, with all admitted
 * activities; adding another waiter does not allocate another physical credit. */
typedef struct {
    void *context;
    void (*lock)(void *context);
    void (*unlock)(void *context);
    epd_memory_owner_t (*current_owner)(void *context);
    epd_memory_owner_t (*owner_account)(void *context, epd_memory_owner_t owner);
    bool (*owner_is_within)(void *context, epd_memory_owner_t owner, epd_memory_owner_t ancestor);
    int (*owner_retain)(void *context, epd_memory_owner_t owner);
    void (*owner_release)(void *context, epd_memory_owner_t owner);
    epd_memory_owner_t (*owner_enter)(void *context, epd_memory_owner_t owner);
    void (*owner_leave)(void *context, epd_memory_owner_t previous);
    uint64_t (*now)(void *context);
    /* Schedule a pump on the requested lane; never invoke the pump inline. */
    void (*wake)(void *context, epd_resource_lane_t lane);
    int (*activity_apply)(void *context,
                          const epd_resource_activity_snapshot_t *request,
                          epd_resource_grant_t *grant_out);
    int (*resource_admit)(void *context, epd_resource_t resource,
                          const epd_resource_descriptor_t *descriptor,
                          const epd_resource_credit_t *activity_credits,
                          size_t credit_count, epd_resource_credit_t *credit_out);
    void (*credit_release)(void *context, epd_resource_credit_t credit);
    epd_memory_owner_t (*credit_enter)(void *context, epd_resource_credit_t credit);
    void (*credit_loaded)(void *context, epd_resource_credit_t credit);
    void (*payload_released)(void *context, uint32_t backend, size_t bytes);
    /* Platform core transaction primitive only: bounded, nonallocating, no
     * callbacks/I/O/component locks. Runs inside the short revision commit guard. */
    int (*credit_commit)(void *context, epd_resource_credit_t credit);
    int (*shared_commit)(void *context, epd_resource_credit_t credit, uint32_t backend,
                         const epd_memory_association_t *changes, unsigned count);
    int (*association_change)(void *context, uint32_t backend,
                              const epd_memory_association_t *changes, unsigned count);
    /* Called unlocked, while the caller still owns the live allocator block. */
    int (*payload_info)(void *context, const void *data, uint32_t backend,
                        epd_memory_owner_t *account, size_t *physical);
    int (*credit_physical)(void *context, epd_resource_credit_t credit, uint32_t backend, size_t *physical);
    int (*reference_commit)(void *context, epd_resource_credit_t credit, uint32_t backend,
                            size_t consumed, const epd_memory_association_t *changes, unsigned count);
    int (*activity_suspend)(void *context, epd_resource_credit_t credit);
    int (*activity_resume)(void *context, const epd_resource_activity_snapshot_t *request,
                           const size_t unused[EPD_MEMORY_BACKEND_COUNT], epd_resource_grant_t *grant);
    /* Same bounded, callback-free commit rules as credit_commit. Zero grant
     * explicitly means no additional reservation; the suspended token survives. */
    int (*credit_resume_commit)(void *context, epd_resource_credit_t suspended,
                                epd_resource_credit_t grant);
    /* Called once, unlocked, during exclusive initialization. The zeroed
     * control block belongs to the runtime for its entire system lifetime. */
    void *(*control_allocate)(void *context, size_t bytes);
} epd_resource_runtime_t;

/* Initialization is exclusive and occurs before publication to other threads. */
int epd_resource_init(const epd_resource_runtime_t *runtime);
/* Execute at most budget jobs; only call from the indicated actual runtime lane.
 * Callbacks can re-enter APIs; a lane cannot recursively pump itself. */
size_t epd_resource_process(epd_resource_lane_t lane, size_t budget);
/* Event-driven retry (capacity/task/storage changes), never a periodic lease. */
void epd_resource_retry(void);
/* Incrementally ordered, backend-specific candidates; marks at most max_objects.
 * Returns selected bytes, NOT physically freed bytes. Completion is in stats. */
size_t epd_resource_reclaim(uint32_t backend, size_t target_bytes, size_t max_objects);
size_t epd_resource_reclaim_for(uint32_t backend, size_t target_bytes, size_t max_objects,
                                epd_resource_kind_t highest_kind);
typedef struct {
    size_t pending_bytes, reclaimed_bytes;
    uint32_t pending_objects, failed_objects;
} epd_resource_reclaim_snapshot_t;
void epd_resource_reclaim_status(uint32_t backend, epd_resource_reclaim_snapshot_t *snapshot);

typedef struct {
    size_t physical_bytes;
    size_t reclaimed_bytes;
    uint32_t activities;
    uint32_t providers;
    uint32_t resources;
    uint32_t references;
    uint64_t candidate_visits;
    uint64_t loads;
    uint64_t load_failures;
} epd_resource_stats_t;
void epd_resource_stats(epd_resource_stats_t *stats);

/* Runtime account shutdown: only queue cleanup; safe before final module close. */
int epd_resource_close_owner(epd_memory_owner_t owner);
bool epd_resource_owner_busy(epd_memory_owner_t owner);

#ifdef __cplusplus
}
#endif
#endif
