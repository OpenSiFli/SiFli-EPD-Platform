/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_MEMORY_MANAGER_H
#define EPD_MEMORY_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_MEMORY_MANAGER_ABI 1u
#define EPD_MEMORY_OWNER_SYSTEM ((epd_memory_owner_t)0)
#define EPD_MEMORY_OWNER_INVALID ((epd_memory_owner_t)UINT32_MAX)
#define EPD_MEMORY_BACKEND_NONE UINT8_MAX
#define EPD_MEMORY_BACKEND_COUNT 8u
#define EPD_MEMORY_APPID_SIZE 32u
#define EPD_MEMORY_BACKEND_NAME_SIZE 24u
#define EPD_MEMORY_ALL_BACKENDS UINT32_C(0xff)
#define EPD_MEMORY_CREDIT_MAX_SOURCES 64u

typedef uint32_t epd_memory_owner_t;
/* Credits are shared settlement descriptors and can be installed with
 * owner_enter. The allocator stores this token, not a per-block descriptor. */
typedef epd_memory_owner_t epd_memory_credit_t;
typedef uint8_t epd_memory_backend_t;

typedef enum {
    EPD_MEMORY_OK = 0,
    EPD_MEMORY_INVALID,
    EPD_MEMORY_CLOSED,
    EPD_MEMORY_NO_METADATA,
    EPD_MEMORY_NO_CAPACITY,
    EPD_MEMORY_LIMIT,
    EPD_MEMORY_WAITING,
    EPD_MEMORY_CANCELLED,
    EPD_MEMORY_TIMEOUT,
    EPD_MEMORY_BUSY,
    EPD_MEMORY_REGION,
    EPD_MEMORY_OVERFLOW
} epd_memory_result_t;

typedef enum {
    EPD_MEMORY_SCOPE_APP = 0,
    EPD_MEMORY_SCOPE_PAGE,
    EPD_MEMORY_SCOPE_TASK,
    EPD_MEMORY_SCOPE_SERVICE,
    EPD_MEMORY_SCOPE_CLEANUP,
    EPD_MEMORY_SCOPE_SHARED,
    EPD_MEMORY_SCOPE_CREDIT
} epd_memory_scope_kind_t;

typedef enum {
    EPD_MEMORY_REGION_UNKNOWN = 0,
    EPD_MEMORY_REGION_SRAM,
    EPD_MEMORY_REGION_HCPU_PSRAM,
    EPD_MEMORY_REGION_ACPU
} epd_memory_region_t;

typedef enum {
    EPD_MEMORY_RECLAIMABLE = 0,
    EPD_MEMORY_ACTIVE,
    EPD_MEMORY_INFLIGHT,
    EPD_MEMORY_PENDING_FREE,
    EPD_MEMORY_CLASS_COUNT
} epd_memory_class_t;

/* Assigned by the runtime from business state, never from appid. */
typedef enum {
    EPD_MEMORY_PRIORITY_INTERACTIVE = 0,
    EPD_MEMORY_PRIORITY_NORMAL,
    EPD_MEMORY_PRIORITY_NEARBY,
    EPD_MEMORY_PRIORITY_OPTIONAL
} epd_memory_priority_t;

typedef enum {
    EPD_MEMORY_CREDIT_WAITING = 0,
    EPD_MEMORY_CREDIT_LIVE,
    EPD_MEMORY_CREDIT_ENDED,
    EPD_MEMORY_CREDIT_CANCELLED,
    EPD_MEMORY_CREDIT_TIMED_OUT,
    EPD_MEMORY_CREDIT_REJECTED,
    EPD_MEMORY_CREDIT_PREPARED,
    EPD_MEMORY_CREDIT_SUSPENDED
} epd_memory_credit_state_t;

typedef struct {
    size_t physical[EPD_MEMORY_CLASS_COUNT];
    size_t sized; /* Allocator rounded size; not historical raw requested size. */
    size_t unused;
    size_t allocating;
    size_t peak_committed;
    uint32_t allocations;
} epd_memory_usage_t;

typedef struct {
    epd_memory_owner_t owner, parent, account;
    epd_memory_scope_kind_t kind;
    bool closed;
    uint32_t references; /* Explicit, thread, guard and child references. */
    uint32_t allocations;
    uint32_t allocating;
    char appid[EPD_MEMORY_APPID_SIZE];
} epd_memory_owner_snapshot_t;

typedef struct {
    epd_memory_owner_t account;
    epd_memory_usage_t backend[EPD_MEMORY_BACKEND_COUNT];
    /* Deduplicated shared demand, charged only to this application account.
     * Never add these bytes to physical backend/region usage. */
    size_t associated[EPD_MEMORY_BACKEND_COUNT];
} epd_memory_account_snapshot_t;

typedef struct {
    epd_memory_region_t region;
    size_t capacity; /* Physical ceiling, including allocated block headers. */
    size_t unavailable; /* Statically excluded or bounded unhooked consumption. */
    epd_memory_backend_t parent; /* NONE for independent heaps. */
} epd_memory_backend_config_t;

typedef struct {
    const void *key;
    char name[EPD_MEMORY_BACKEND_NAME_SIZE];
    epd_memory_backend_config_t config;
    epd_memory_usage_t usage;
    size_t available; /* Last allocator observation; not a reservation. */
    size_t headroom; /* Shared free-space floor for new prefetch reservations. */
    bool available_known;
} epd_memory_backend_snapshot_t;

typedef struct {
    size_t bytes[EPD_MEMORY_BACKEND_COUNT]; /* Additional peak by actual heap. */
    epd_memory_priority_t priority;
    uint32_t timeout_ms; /* 0: try once; otherwise <= INT32_MAX. */
} epd_memory_credit_spec_t;

typedef struct {
    epd_memory_credit_t credit;
    epd_memory_owner_t owner;
    epd_memory_credit_state_t state;
    epd_memory_result_t result;
    epd_memory_priority_t priority;
    uint32_t queued_at_ms, deadline_ms, bypasses;
    size_t unused[EPD_MEMORY_BACKEND_COUNT];
    size_t allocating[EPD_MEMORY_BACKEND_COUNT];
    size_t physical[EPD_MEMORY_BACKEND_COUNT];
    size_t prepared_bytes[EPD_MEMORY_BACKEND_COUNT]; /* Full new UNUSED target. */
    /* Credits alone never assert contiguous or guaranteed-buffer availability. */
} epd_memory_credit_snapshot_t;

typedef struct {
    size_t fixed_bytes;
    size_t sram_bytes, retained_bytes;
    bool initialized;
    uint32_t descriptors_used, descriptors_capacity, retired_slots;
    uint32_t protocol_errors;
    uint32_t transactions_used, transactions_capacity;
    bool reclaim_running;
} epd_memory_metadata_snapshot_t;

/* All core calls are bounded and nonallocating. No core call invokes a heap,
 * I/O or business callback, except the explicitly named reclaim_run wrapper.
 * A registered capacity notifier may only signal a nonblocking kernel event,
 * and is invoked after the outer interrupt/critical guards have been released.
 * create/scope_create return one reference. appid is diagnostic only.
 * Exhaustion returns INVALID, which must never be replaced with SYSTEM. */
/* Called automatically at RT-Thread PREV init, and by sdk_init. PSRAM must be
 * accessible; normal thread only. Cold-start memset runs outside IRQ guards.
 * Idempotent across later calls/resume: live state is never cleared again.
 * Before this point SYSTEM accounting/backends work entirely in SRAM, while
 * app/scope/credit creation fails closed without touching retained metadata.
 * After initialization, all calls require PSRAM accessibility; no calls from
 * the sleep interval in which the external memory controller is unavailable. */
/* Platform boot/loader authority. Modules use the account already installed
 * by their lifecycle dispatcher, never create an independent application root. */
#if !defined(BUILD_DLMODULE) && !defined(RTT_IN_MODULE)
epd_memory_result_t epd_memory_init(void);
epd_memory_owner_t epd_memory_owner_create(const char *appid);
/* Resident lifecycle dispatch only. These are not module exports; changing
 * context preserves the target account's existing limits and credit rules. */
bool epd_memory_owner_is_current_account(epd_memory_owner_t owner);
epd_memory_owner_t epd_memory_scope_create_trusted(epd_memory_owner_t parent,
                                                  epd_memory_scope_kind_t kind);
bool epd_memory_owner_retain_trusted(epd_memory_owner_t owner);
void epd_memory_owner_release_trusted(epd_memory_owner_t owner);
epd_memory_owner_t epd_memory_owner_enter_trusted(epd_memory_owner_t owner);
void epd_memory_owner_leave_trusted(epd_memory_owner_t previous);
bool epd_memory_owner_close_trusted(epd_memory_owner_t owner);
epd_memory_result_t epd_memory_block_transfer_try_trusted(void *ptr, epd_memory_owner_t to);
#endif
/* Child scopes inherit their parent's account and limits. A PAGE/TASK/SERVICE
 * kind is a lifetime label, not proof of foreground or accepted business work.
 * Public scope/retain/release/enter/leave/close require the current account.
 * Foreign or stale tokens cannot change context, references or close state. */
epd_memory_owner_t epd_memory_scope_create(epd_memory_owner_t parent,
                                          epd_memory_scope_kind_t kind);
epd_memory_owner_t epd_memory_owner_current(void);
bool epd_memory_owner_retain(epd_memory_owner_t owner);
void epd_memory_owner_release(epd_memory_owner_t owner);
/* enter holds a separate reference to previous until leave. INVALID means
 * no switch occurred: do not run the callback and do not call leave.
 * Closed, still valid owners may be installed for cleanup. Balanced guards
 * must execute on the same thread, in stack order, including error paths. */
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner);
void epd_memory_owner_leave(epd_memory_owner_t previous);
bool epd_memory_owner_close(epd_memory_owner_t owner);
/* Closing a parent rejects ordinary allocations and all new credit requests.
 * Already LIVE credits below a pre-existing CLEANUP scope survive that parent
 * close and may be installed to finish bounded accepted cleanup. Closing the
 * cleanup scope itself, or ending/cancelling its credit, revokes that allowance.
 * A plain cleanup-scope allocation has no such allowance. */
bool epd_memory_owner_get(epd_memory_owner_t owner,
                          epd_memory_owner_snapshot_t *out);
/* Diagnostic slot walk: [0, metadata.descriptors_capacity), with SYSTEM at 0.
 * Copies a generation-tagged snapshot without retaining it. The owner may be
 * collected immediately afterwards; a subsequent usage query may fail. Empty,
 * retired, not-yet-initialized and out-of-range slots return false. */
bool epd_memory_owner_at(unsigned slot, epd_memory_owner_snapshot_t *out);
/* Account snapshot resolves owner to its account and includes that account's
 * scopes. SYSTEM/account 0 includes only owners whose account is 0, including
 * SHARED scopes; APP accounts are excluded. Account views on one backend are
 * disjoint. peak_committed is each account's own historical peak. */
bool epd_memory_account_snapshot(epd_memory_owner_t owner,
                                 epd_memory_account_snapshot_t *out);
/* Scope snapshot includes descendants; SYSTEM scope 0 is the global view,
 * equal to each backend's usage, including APP accounts. These overlapping
 * scope views must not be added together. Parent/child pool views also overlap. */
bool epd_memory_scope_snapshot(epd_memory_owner_t scope,
                               epd_memory_account_snapshot_t *out);
/* Limits and backend masks constrain the owner's entire scope subtree.
 * Setting them on SYSTEM constrains the global scope, including APP accounts. */
epd_memory_result_t epd_memory_owner_set_limit(epd_memory_owner_t owner,
                                              epd_memory_backend_t backend,
                                              size_t bytes);
typedef struct {
    epd_memory_owner_t account;
    size_t capacity, configured_limit, effective_limit;
    size_t private_committed, associated;
    bool application_limit, above_limit;
} epd_memory_budget_snapshot_t;
/* Every APP root automatically uses min(configured_limit, backend net capacity).
 * SYSTEM/SHARED account 0 has no application cap. Capacity is not divided by
 * app count; accepted activity credits and deduplicated shared associations
 * determine the current business commitment. Physical admission remains global.
 * Refinement changes the next admission without cancelling live commitments.
 * private_committed is account-only; above_limit checks the configured scope
 * constraint (global for SYSTEM), including the account's shared associations. */
bool epd_memory_owner_budget(epd_memory_owner_t owner, epd_memory_backend_t backend,
                            epd_memory_budget_snapshot_t *out);
/* Resource-layer settlement of one account-deduplicated shared association.
 * old/new are that association's charge, not the account total. The caller
 * serializes each association and rolls back its publication on failure.
 * Increases share the account limit with private physical+unused+allocating;
 * decreases remain legal after close. No physical reservation is created.
 * Nonzero associations preserve the account generation, not an explicit ref.
 * SYSTEM associations are free of logical charge. Scope owners resolve to
 * their account; scope snapshots report associations only at the account root. */
#if !defined(BUILD_DLMODULE) && !defined(RTT_IN_MODULE)
epd_memory_result_t epd_memory_association_change(epd_memory_owner_t owner,
                          epd_memory_backend_t backend, size_t old_bytes, size_t new_bytes);
#endif
/* Set allowed heaps before allocations/credits. Scope masks intersect with
 * all ancestors; realloc uses the original token and therefore the mask. */
epd_memory_result_t epd_memory_owner_set_backends(epd_memory_owner_t owner,
                                                 uint32_t mask);
/* Changes the class of this descriptor's allocations, not its descendants.
 * The resource provider supplies actual access/lifetime state. */
epd_memory_result_t epd_memory_owner_set_class(epd_memory_owner_t owner,
                                              epd_memory_class_t cls);

/* key is the allocator identity (rt_memheap* for SDK heaps). Re-registering
 * updates an existing key without moving its ID. Parent pools are views of
 * already charged backing storage; only root heaps contribute physical totals.
 * Backing storage must remain allocated until the child is empty/unregistered.
 * This low-level adapter entry can run under a heap lock; the adapter must
 * dispatch_changes after releasing it. sdk_register_heap handles safe startup
 * or quiescent registration outside heap locks. */
epd_memory_result_t epd_memory_backend_register(const void *key,
                          const epd_memory_backend_config_t *config,
                          epd_memory_backend_t *out);
bool epd_memory_backend_find(const void *key, epd_memory_backend_t *out);
bool epd_memory_backend_get(epd_memory_backend_t backend,
                            epd_memory_backend_snapshot_t *out);
bool epd_memory_backend_set_name(epd_memory_backend_t backend, const char *name);
/* Existing grants remain usable. INTERACTIVE/NORMAL work may use this space. */
bool epd_memory_backend_set_headroom(epd_memory_backend_t backend, size_t bytes);
/* Sums independent heaps' net capacity (capacity minus unavailable) only.
 * A child heap is a view of its already charged
 * parent backing block, so its capacity and usage are excluded here. The
 * aggregated peak is a sum of backend peaks, an upper bound rather than a
 * measurement of simultaneous peaks across different heaps. */
epd_memory_result_t epd_memory_region_snapshot(epd_memory_region_t region,
                          size_t *capacity, epd_memory_usage_t *usage);

/* Platform settlement primitives, not module APIs. Only the runtime supplies
 * verified priority and publication ownership. Applications and components use
 * epd_resource_context_* / epd_activity_work_* for capacity and execution.
 * Shared types remain available for snapshots and resource adapter structures. */
#if !defined(BUILD_DLMODULE) && !defined(RTT_IN_MODULE)
/* Request allocates its terminal record before acceptance, reserves all heaps
 * atomically or none, and returns OK/LIVE or WAITING. Other failures return no
 * handle. Caller owns one reference; use owner_release after end/cancel.
 * A live credit's free restores unused bytes only while it remains live.
 * Ending a credit drops unused bytes and prevents expansion; physical blocks
 * and pending allocator commits keep their original settlement descriptor. */
epd_memory_result_t epd_memory_credit_request(epd_memory_owner_t owner,
                          const epd_memory_credit_spec_t *spec,
                          epd_memory_credit_t *out);
bool epd_memory_credit_get(epd_memory_credit_t credit,
                           epd_memory_credit_snapshot_t *out);
bool epd_memory_credit_end(epd_memory_credit_t credit);
bool epd_memory_credit_cancel(epd_memory_credit_t credit);
epd_memory_credit_t epd_memory_credit_current(void);
/* Releases only unused credit, never touches live buffers. */
epd_memory_result_t epd_memory_credit_trim(epd_memory_credit_t credit,
                          epd_memory_backend_t backend, size_t bytes);
/* Fast, all-or-none additional peak admission. Failure preserves all existing
 * credit/buffers. Closed cleanup credits may consume but cannot expand. */
epd_memory_result_t epd_memory_credit_expand(epd_memory_credit_t credit,
                          const size_t additional[EPD_MEMORY_BACKEND_COUNT]);
/* Explicit zero-addition resume, never a failed request's INVALID handle.
 * This is a grant sentinel only; it does not change any allocation's owner. */
#define EPD_MEMORY_CREDIT_NO_GRANT ((epd_memory_credit_t)0)
/* Caller first quiesces producers; references do not prove quiescence.
 * Suspend is idempotent, drops every backend's unused in one guard, retains
 * physical blocks/token and rejects subsequent allocation/growth. Free/shrink
 * remain valid and do not recreate unused. Pending allocator commits or any
 * prepared transaction referencing this token return BUSY without mutation.
 * End/cancel/root-close while suspended is terminal; it cannot be resumed. */
epd_memory_result_t epd_memory_credit_suspend(epd_memory_credit_t credit);
/* Obtain a separate grant with ordinary credit_request under the SAME parent:
 * its spec.bytes are NEW UNUSED, not total physical+unused peak. Existing fair
 * waiting, cancellation, deadline and metadata bounds apply. Waiting/cancelling
 * that grant does not change the suspended token. Commit requires a LIVE grant
 * with no blocks/pending/transaction; validates the target's backend/own limits,
 * atomically moves ALL unused and restores the old token to LIVE. Grant becomes
 * ENDED, but caller still releases its reference. No physical block is moved.
 * NO_GRANT restores LIVE with zero unused, with no new admission. INVALID is
 * rejected. Failure leaves both descriptors and every block unchanged.
 * These primitives never dispatch callbacks; caller dispatches capacity changes
 * after leaving its publication guard. Serialize revision/cancel around commit. */
epd_memory_result_t epd_memory_credit_resume_commit(epd_memory_credit_t suspended,
                                                   epd_memory_credit_t grant);


/* Two-phase unused-credit transfer. Each successful prepare owns one caller
 * reference. PREPARED is not allocatable; only commit changes it to LIVE.
 * Replace reserves only positive deltas and leaves previous usable until commit.
 * spec.bytes is the complete new UNUSED target, excluding old physical/pending.
 * A finite wait queues only the delta; poll transitions WAITING to PREPARED.
 * Share moves existing unused promises into a SYSTEM/SHARED destination; it
 * does not reserve the same bytes again. Sources are unique, at most 64, and
 * insufficient combined unused returns NO_CAPACITY without any mutation.
 * Preparation pins source descriptors but does not freeze their use. Commit
 * revalidates live/open sources and their selected unused amounts atomically;
 * BUSY/CLOSED leaves sources unchanged. Framework must validate its own revision
 * and serialize publication with cancellation around commit. No callbacks,
 * allocation or blocking occur inside these APIs. Replace commit ends previous;
 * its existing blocks retain that original token. Share leaves sources live.
 * abort/end/cancel of a preparation releases its delta and source references;
 * caller must still owner_release the prepared handle. Abort after commit fails.
 * Release a committed ticket using the usual end + owner_release contract. */
epd_memory_result_t epd_memory_credit_replace_prepare(epd_memory_credit_t previous,
                          epd_memory_owner_t owner, const epd_memory_credit_spec_t *spec,
                          epd_memory_credit_t *prepared);
epd_memory_result_t epd_memory_credit_share_prepare(const epd_memory_credit_t *sources,
                          size_t count, epd_memory_owner_t shared_owner,
                          epd_memory_backend_t backend, size_t bytes,
                          epd_memory_credit_t *prepared);
epd_memory_result_t epd_memory_credit_commit(epd_memory_credit_t prepared);
#endif
typedef struct {
    epd_memory_owner_t account; /* Account root, unique within this batch. */
    size_t old_bytes, new_bytes;
} epd_memory_association_t;
#if !defined(BUILD_DLMODULE) && !defined(RTT_IN_MODULE)
/* Atomic actual-size settlement for distinct application accounts on one
 * backend. No partial mutation on failure. Reductions are legal after close. */
epd_memory_result_t epd_memory_association_batch(epd_memory_backend_t backend,
                          const epd_memory_association_t *associations, unsigned count);
/* READY/LOADING reuse: atomically return min(consumed, credit.unused[backend])
 * and settle account associations, checking limits after that return. count=0
 * supports a second activity in an account already associated with the object.
 * Requires a LIVE open credit; physical and allocating bytes are untouched. */
epd_memory_result_t epd_memory_credit_associate(epd_memory_credit_t credit,
                          epd_memory_backend_t backend, size_t consumed,
                          const epd_memory_association_t *associations, unsigned count);
/* Commit a prepared SHARE and its deduplicated demand charges atomically.
 * Charges use the transaction's backend. The account limit is checked after
 * subtracting transferred private unused, so the same promise is not charged
 * twice. All accounts/amounts are validated before either ledger changes.
 * count <= CREDIT_MAX_SOURCES; callers retain accounts until publication. */
epd_memory_result_t epd_memory_credit_share_commit(epd_memory_credit_t prepared,
                          const epd_memory_association_t *associations, unsigned count);
bool epd_memory_credit_abort(epd_memory_credit_t prepared);
#endif
/* Runtime worker calls poll on capacity/lifecycle changes and at the next
 * deadline. Milliseconds wrap modulo 2^32; no infinite wait is accepted.
 * There are no callbacks or automatic per-application retry timers. */
void epd_memory_poll(uint32_t now_ms);
/* Physical shortage for an eligible waiting request. Private limits, older
 * protected waiters and reservation-only shortages do not trigger eviction. */
bool epd_memory_credit_reclaim_need(epd_memory_credit_t credit,
                                   size_t bytes[EPD_MEMORY_BACKEND_COUNT]);
uint32_t epd_memory_now_ms(void);
bool epd_memory_next_deadline(uint32_t now_ms, uint32_t *delay_ms);
/* One permanent runtime notifier, limited to a nonblocking kernel event send.
 * It must not allocate, acquire a mutex or execute business work. Registration
 * requests an initial wake. Epochs wrap modulo 2^32; compare for inequality.
 * Allocator settlement records changes only: SDK post hooks dispatch after
 * releasing heap locks. Calls nested in an outer IRQ/critical guard defer;
 * that caller must dispatch after releasing its guard. No polling timer. */
typedef void (*epd_memory_capacity_notify_t)(void);
void epd_memory_set_capacity_notifier(epd_memory_capacity_notify_t notify);
uint32_t epd_memory_capacity_epoch(void);
void epd_memory_dispatch_changes(void);

/* Allocator adapter contract. try_change sees the exact selected block before
 * mutation. On success mutation MUST complete and notify MUST follow exactly
 * once. FREE has notify only. Both run under the allocator's own lock; the core
 * adds only a short interrupt guard. No allocation or callbacks are performed.
 * Moving realloc admits the FULL new physical block; old remains charged until
 * FREE. size/old_size are allocator-rounded sizes, physical includes header
 * and unsplit capacity. available_after is exact, including coalescing effects. */
typedef enum {
    EPD_MEMORY_CHANGE_ALLOC = 0,
    EPD_MEMORY_CHANGE_REALLOC_INPLACE,
    EPD_MEMORY_CHANGE_REALLOC_ALLOC,
    EPD_MEMORY_CHANGE_FREE
} epd_memory_change_kind_t;
typedef struct {
    epd_memory_change_kind_t kind;
    epd_memory_owner_t token;
    epd_memory_backend_t backend;
    size_t size, physical, old_size, old_physical;
    size_t available_before, available_after;
} epd_memory_change_t;
epd_memory_result_t epd_memory_try_change(const epd_memory_change_t *change);
void epd_memory_notify(const epd_memory_change_t *change);
/* Atomically transfers one already allocated block after checking destination
 * limits. Adapter replaces its header token under its allocator lock, without
 * allocating/freeing storage. Source live credits do not regain unused bytes.
 * Target must be a noncredit scope. This detaches finished operation results.
 * A closed destination permits only same-account settlement of existing owned
 * buffers into an already existing scope; masks/limits still apply. It does not
 * reopen allocation or restore unused credits. Never transfer borrowed buffers. */
epd_memory_result_t epd_memory_transfer(epd_memory_owner_t from,
                          epd_memory_owner_t to, epd_memory_backend_t backend,
                          size_t size, size_t physical);

typedef void (*epd_memory_reclaim_fn)(void *argument);
/* Dedicated worker only. Callback runs outside the guard; nested/concurrent
 * invocations return BUSY. Allocator hooks never call this function. */
epd_memory_result_t epd_memory_reclaim_run(epd_memory_reclaim_fn fn, void *argument);
void epd_memory_metadata_snapshot(epd_memory_metadata_snapshot_t *out);

typedef enum {
    EPD_MEMORY_DIAG_CONTROL = 0,
    EPD_MEMORY_DIAG_ADMIT,
    EPD_MEMORY_DIAG_SETTLE,
    EPD_MEMORY_DIAG_TRANSFER,
    EPD_MEMORY_DIAG_COUNT
} epd_memory_diagnostic_path_t;
typedef struct {
    uint32_t samples, max_cycles, entered_irq_off;
} epd_memory_diagnostic_path_snapshot_t;
typedef struct {
    bool active, exhausted, timed_out;
    uint32_t remaining, frequency_hz, duration_ms, deadline_tick, nested_calls;
    epd_memory_diagnostic_path_snapshot_t path[EPD_MEMORY_DIAG_COUNT];
} epd_memory_diagnostic_snapshot_t;
/* Opt-in, allocation-free DWT measurement. Bounds: 1..1,000,000 outer core
 * guards and 1..60,000 ms. Default off; no counter reads/printing when off.
 * Measures the guarded core body, including nested core calls, not allocator
 * heap-search/lock waiting or time already spent in a caller's IRQ guard.
 * Raw cycles avoid a false time conversion across CPU frequency changes.
 * PSRAM must be accessible; start/stop/read are normal-thread diagnostics. */
epd_memory_result_t epd_memory_diagnostic_start(uint32_t samples, uint32_t duration_ms);
void epd_memory_diagnostic_stop(void);
bool epd_memory_diagnostic_read(epd_memory_diagnostic_snapshot_t *out);

typedef enum {
    EPD_MEMORY_REJECT_CHANGE = 0,
    EPD_MEMORY_REJECT_TOKEN,
    EPD_MEMORY_REJECT_ACCOUNTING,
    EPD_MEMORY_REJECT_OVERFLOW,
    EPD_MEMORY_REJECT_CLOSED_PATH,
    EPD_MEMORY_REJECT_CREDIT_STATE,
    EPD_MEMORY_REJECT_BACKEND,
    EPD_MEMORY_REJECT_CREDIT_UNUSED,
    EPD_MEMORY_REJECT_CAPACITY,
    EPD_MEMORY_REJECT_LIMIT,
    EPD_MEMORY_REJECT_FAIRNESS,
    EPD_MEMORY_REJECT_RESERVED,
    EPD_MEMORY_REJECT_CONTEXT,
    EPD_MEMORY_REJECT_ADAPTER
} epd_memory_rejection_reason_t;

typedef enum {
    EPD_MEMORY_RECOVERY_DENIED = 0,
    EPD_MEMORY_RECOVERY_PENDING,
    EPD_MEMORY_RECOVERY_READY
} epd_memory_recovery_result_t;
typedef struct {
    epd_memory_owner_t owner;
    epd_memory_backend_t backend;
    size_t physical, min_available;
    uint32_t generation, started;
    epd_memory_result_t result;
    epd_memory_rejection_reason_t reason;
    epd_memory_recovery_result_t state;
    bool used, pressure_requested, fragmentation_requested;
} epd_memory_recovery_t;
/* One nonblocking recovery round for fresh allocations on a known backend.
 * Zero-initialize state. physical is the required additional working-set peak,
 * including allocator overhead. allocation_failed is true only after an actual
 * allocation returned NULL; false performs admission before allocation.
 * Captures the raw context token and preserves existing credit ownership.
 * READY permits an allocation attempt; LIVE credit unused has been admitted,
 * but neither READY nor a logical credit guarantees contiguous storage.
 * Ordinary owners still require the final allocator hook's admission.
 * Poll on the same context; cancellation, context replacement and the bounded
 * deadline terminate the round. No retain, heap allocation or blocking occurs.
 * result/reason describe current admission, not a historical allocator error.
 * Non-NULL realloc must use its original block ownership and is not supported. */
epd_memory_recovery_result_t epd_memory_recovery_begin(epd_memory_recovery_t *state,
    epd_memory_backend_t backend, size_t physical, bool allocation_failed);
epd_memory_recovery_result_t epd_memory_recovery_poll(epd_memory_recovery_t *state,
    bool cancelled);
/* Continue the same operation after its caller has validated a replacement
 * execution context. next must be unused and distinct from previous. Retires
 * previous permanently and admits next under the current raw context, keeping
 * the original deadline, pressure generation and spent recovery allowances.
 * A terminal previous round cannot be continued. No old token is re-entered;
 * this conveys no authority beyond the caller's current context. */
epd_memory_recovery_result_t epd_memory_recovery_continue(epd_memory_recovery_t *next,
    epd_memory_recovery_t *previous);
/* Report a real NULL after READY. Capacity recovery may be upgraded once to a
 * contiguous-block request. The original deadline remains unchanged; repeated
 * allocation failures after that request do not start more pressure rounds. */
epd_memory_recovery_result_t epd_memory_recovery_failed(epd_memory_recovery_t *state);
/* Fresh application-PSRAM allocations with an exact original allocator payload.
 * physical remains the working-set credit peak; payload only controls contiguous
 * recovery. Both include caller padding as appropriate. No state/ABI growth.
 * Invalid payload/backend returns DENIED without changing the recovery state.
 * As with begin(), an initial failed=true requires a preceding real NULL. */
epd_memory_recovery_result_t epd_memory_recovery_begin_allocation(epd_memory_recovery_t *state,
    epd_memory_backend_t backend, size_t physical, size_t payload, bool allocation_failed);
epd_memory_recovery_result_t epd_memory_recovery_failed_allocation(epd_memory_recovery_t *state,
    size_t payload);

/* SDK bridge: evaluate the captured raw token under the core guard. Report
 * allocation_failed only when the failed physical demand remains unresolved.
 * A fresh
 * heap observation may be supplied while holding that heap's lock. Successful
 * extra credit uses the same admission path as credit_expand. No notification
 * or pressure callback runs here. reclaim distinguishes physical shortage from
 * waiting on existing reservations/fairness; it does not promise reclaimability
 * of any particular object. */
epd_memory_recovery_result_t epd_memory_recovery_admit(epd_memory_recovery_t *state,
    bool allocation_failed, bool available_known, size_t available, bool *reclaim);
typedef struct {
    bool active, valid, exhausted, timed_out;
    uint32_t remaining, captured, deadline_tick, tick;
    epd_memory_owner_t token, current_token, account;
    epd_memory_backend_t backend;
    epd_memory_change_kind_t kind;
    epd_memory_scope_kind_t scope;
    epd_memory_credit_state_t credit_state;
    epd_memory_result_t result;
    epd_memory_rejection_reason_t reason;
    bool closed;
    size_t size, physical, old_physical, delta, unused, reserved, available_after;
} epd_memory_rejection_snapshot_t;
/* Independent, default-off diagnostic: retain only the latest rejected backend
 * attempt, bounded by 1..1024 rejections and 1..60000 ms. A later heap fallback
 * may succeed; this is not an allocator-call failure log. No allocation, scan,
 * callbacks or logging in hooks. Storage is retained PSRAM, gated at boot.
 * Start/stop/read require accessible PSRAM and normal thread context. Expiry is
 * observed on the next rejection or read. Unknown fields use INVALID/NONE/255;
 * size is allocator-rounded, physical includes overhead. No snapshot retains
 * an owner. current_token may differ from token for original-owner realloc. */
epd_memory_result_t epd_memory_rejection_start(uint32_t count, uint32_t duration_ms);
void epd_memory_rejection_stop(void);
bool epd_memory_rejection_read(epd_memory_rejection_snapshot_t *out);
/* Bridge-only rejection before core admission; NULL change means resolution
 * failed before a block/heap was selected. Uses the same bounded recorder. */
void epd_memory_rejection_adapter(epd_memory_owner_t token,
                                 const epd_memory_change_t *change,
                                 epd_memory_rejection_reason_t reason);

/* SDK adapter: registers/reconfigures a heap without changing its backend ID.
 * Early allocator events lazily register unknown-region heaps; startup then
 * supplies actual regions and pool parentage using this same key. */
epd_memory_result_t epd_memory_sdk_register_heap(const void *heap,
                          const epd_memory_backend_config_t *config,
                          epd_memory_backend_t *out);
/* Call after LVGL/app pools exist; safe to repeat when additional pools appear.
 * Enumerates actual RT-Thread heaps, refines boot keys with hardware-address
 * regions/names, and records nested backing ranges as parent/child views. */
epd_memory_result_t epd_memory_sdk_init(void);
/* Exact backend used by epd_app_alloc(EPD_APP_PSRAM), after heap registration.
 * HCPU_PSRAM is currently the only supported region. SRAM allocation uses
 * rt_malloc's fallback policy and cannot promise one backend. This query does
 * not allocate, initialize pools, or export a resident heap object's address. */
bool epd_memory_app_backend(epd_memory_region_t region, epd_memory_backend_t *out);
/* Upper bound for one fresh SDK memheap allocation, including rounding,
 * header and an unsplittable tail. Zero payload returns zero; overflow returns
 * SIZE_MAX. This neither reserves credit nor guarantees a contiguous buffer.
 * Moving realloc needs the old live physical charge plus this new-block bound;
 * an in-place shrink can retain its original, larger physical charge. */
size_t epd_app_allocation_peak(size_t payload);
typedef struct {
    epd_memory_owner_t owner, account;
    epd_memory_backend_t backend;
    size_t size, physical;
} epd_memory_block_snapshot_t;
/* A live SDK allocation base pointer is required. Caller owns its lifetime and
 * excludes concurrent free/realloc/transfer. The SDK heap is taken with timeout
 * zero: BUSY preserves out, INVALID covers bad arguments/disabled support.
 * Reads actual header ownership; no retain, allocation or accounting changes. */
epd_memory_result_t epd_memory_block_get(const void *ptr, epd_memory_block_snapshot_t *out);
epd_memory_result_t epd_memory_block_transfer(void *ptr, epd_memory_owner_t to);
/* Same settlement rules, but takes the heap lock with timeout 0. Normal
 * thread context only; caller must own the live buffer throughout the call.
 * BUSY (lock contention or rejected settlement) leaves header/accounting
 * unchanged. INVALID is returned for bad arguments or disabled SDK support. */
epd_memory_result_t epd_memory_block_transfer_try(void *ptr, epd_memory_owner_t to);

#ifdef __cplusplus
}
#endif
#endif
