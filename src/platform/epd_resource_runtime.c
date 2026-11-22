#include "epd_resource_runtime.h"
#include "epd_operation_runtime.h"
#include "epd_memory.h"
#include "../ui/platform/epd_power.h"
#include <rtthread.h>
#include <string.h>
#include <limits.h>
#include <rtm.h>
#include <mem_section.h>
#ifdef RT_USING_DFS_ELMFAT
#include <ff.h>
#define HAVE_DIR_STRUCTURE
#define DIR epd_resource_posix_dir_t
#include <dfs_posix.h>
#undef DIR
#endif

#define PENDING_COUNT 64
#define NO_RESERVATION (UINT64_C(1) << 63)
typedef struct {
    epd_resource_activity_t activity;
    uint64_t revision;
    epd_memory_credit_t credit;
    uint32_t reclaim_epoch;
    bool reclaim_tried;
} pending_t;
typedef struct {
    epd_memory_owner_t owner;
    uint64_t generation, revision;
    epd_resource_binding_t binding;
    bool used, page, eligible;
    bool operation_running, service_running;
} context_t;

L2_RET_BSS_SECT_BEGIN(epd_resource_runtime)
static struct {
    bool changed, in_worker;
    struct rt_mutex lock;
    epd_resource_runtime_config_t config;
    epd_memory_owner_t shared;
    epd_memory_owner_t ui_dispatch;
    pending_t pending[PENDING_COUNT]; /* Only the worker accesses this array. */
    context_t contexts[PENDING_COUNT];
    uint64_t zero_serial;
    uint32_t capacity_epoch;
} runtime L2_RET_BSS_SECT(epd_resource_runtime);
L2_RET_BSS_SECT_END
static bool runtime_initialized;

static void state_lock(void *context)
{
    (void)context;
    rt_mutex_take(&runtime.lock, RT_WAITING_FOREVER);
}
static void state_unlock(void *context)
{
    (void)context;
    rt_mutex_release(&runtime.lock);
}
static epd_memory_owner_t current_owner(void *context)
{ (void)context; return epd_memory_owner_current(); }
static epd_memory_owner_t owner_account(void *context, epd_memory_owner_t owner)
{
    epd_memory_owner_snapshot_t snapshot;
    (void)context;
    return epd_memory_owner_get(owner, &snapshot) ? snapshot.account : EPD_MEMORY_OWNER_INVALID;
}
static bool owner_is_within(void *context, epd_memory_owner_t owner, epd_memory_owner_t ancestor)
{
    epd_memory_owner_snapshot_t snapshot;
    unsigned depth;
    (void)context;
    for (depth = 0; depth < 17; ++depth) {
        if (owner == ancestor) return true;
        if (owner == EPD_MEMORY_OWNER_SYSTEM || !epd_memory_owner_get(owner, &snapshot)) return false;
        owner = snapshot.parent;
    }
    return false;
}
static int retain_owner(void *context, epd_memory_owner_t owner)
{
    epd_memory_owner_snapshot_t snapshot;
    epd_memory_owner_t cursor = owner;
    unsigned depth;
    (void)context;
    for (depth = 0; depth < 17; ++depth) {
        if (!epd_memory_owner_get(cursor, &snapshot) || snapshot.closed) return EPD_RESOURCE_STOPPED;
        if (cursor == EPD_MEMORY_OWNER_SYSTEM) break;
        cursor = snapshot.parent;
    }
    if (depth == 17) return EPD_RESOURCE_STOPPED;
    return epd_memory_owner_retain(owner) ? EPD_RESOURCE_OK : EPD_RESOURCE_STOPPED;
}
static void release_owner(void *context, epd_memory_owner_t owner)
{ (void)context; epd_memory_owner_release_trusted(owner); }
static epd_memory_owner_t enter_owner(void *context, epd_memory_owner_t owner)
{ (void)context; return epd_memory_owner_enter_trusted(owner); }
static void leave_owner(void *context, epd_memory_owner_t previous)
{ (void)context; epd_memory_owner_leave_trusted(previous); }
static uint64_t now(void *context)
{ (void)context; return epd_memory_now_ms(); }
static void wake(void *context, epd_resource_lane_t lane)
{
    (void)context;
    if (lane == EPD_RESOURCE_WORKER) epd_operation_runtime_wake();
    else if (runtime.config.schedule) runtime.config.schedule(runtime.config.context, lane);
    else epd_power_notify();
}
static int translate(epd_memory_result_t result)
{
    switch (result) {
    case EPD_MEMORY_OK: return EPD_RESOURCE_OK;
    case EPD_MEMORY_WAITING: return EPD_RESOURCE_PENDING;
    case EPD_MEMORY_CLOSED: case EPD_MEMORY_CANCELLED: return EPD_RESOURCE_STOPPED;
    case EPD_MEMORY_NO_METADATA: return EPD_RESOURCE_CAPACITY;
    case EPD_MEMORY_BUSY: return EPD_RESOURCE_BUSY;
    case EPD_MEMORY_NO_CAPACITY: case EPD_MEMORY_LIMIT: case EPD_MEMORY_TIMEOUT: return EPD_RESOURCE_DENIED;
    default: return EPD_RESOURCE_INVALID;
    }
}
static epd_memory_priority_t priority(epd_resource_kind_t kind)
{
    return kind == EPD_RESOURCE_INTERACTIVE ? EPD_MEMORY_PRIORITY_INTERACTIVE :
        kind == EPD_RESOURCE_NEARBY ? EPD_MEMORY_PRIORITY_NEARBY :
        kind == EPD_RESOURCE_OPTIONAL ? EPD_MEMORY_PRIORITY_OPTIONAL : EPD_MEMORY_PRIORITY_NORMAL;
}
static void drop(epd_memory_credit_t credit)
{
    if (credit == EPD_MEMORY_OWNER_INVALID) return;
    epd_memory_credit_end(credit);
    epd_memory_owner_release_trusted(credit);
}
static void release_credit(void *context, epd_resource_credit_t credit)
{ (void)context; if (!(credit & NO_RESERVATION)) drop((epd_memory_credit_t)credit); }
static int commit_credit(void *context, epd_resource_credit_t credit)
{
    epd_memory_credit_snapshot_t snapshot;
    (void)context;
    if (credit & NO_RESERVATION) return EPD_RESOURCE_OK;
    if (!epd_memory_credit_get((epd_memory_credit_t)credit, &snapshot)) return EPD_RESOURCE_STALE;
    if (snapshot.state == EPD_MEMORY_CREDIT_LIVE) return EPD_RESOURCE_OK;
    return translate(epd_memory_credit_commit((epd_memory_credit_t)credit));
}
static int change_associations(void *context, uint32_t backend,
    const epd_memory_association_t *changes, unsigned count)
{
    (void)context;
    if (backend >= EPD_MEMORY_BACKEND_COUNT) return EPD_RESOURCE_INVALID;
    return translate(epd_memory_association_batch((epd_memory_backend_t)backend, changes, count));
}
static int commit_shared(void *context, epd_resource_credit_t credit, uint32_t backend,
    const epd_memory_association_t *changes, unsigned count)
{
    epd_memory_credit_snapshot_t snapshot;
    if (!credit || credit > UINT32_MAX || !epd_memory_credit_get((epd_memory_credit_t)credit, &snapshot))
        return EPD_RESOURCE_STALE;
    if (snapshot.state == EPD_MEMORY_CREDIT_PREPARED)
        return translate(epd_memory_credit_share_commit((epd_memory_credit_t)credit, changes, count));
    if (snapshot.state != EPD_MEMORY_CREDIT_LIVE) return EPD_RESOURCE_STOPPED;
    return change_associations(context, backend, changes, count);
}
static int commit_reference(void *context, epd_resource_credit_t credit, uint32_t backend,
    size_t consumed, const epd_memory_association_t *changes, unsigned count)
{
    if (!credit || (credit & NO_RESERVATION)) return change_associations(context, backend, changes, count);
    if (backend >= EPD_MEMORY_BACKEND_COUNT || credit > UINT32_MAX) return EPD_RESOURCE_INVALID;
    return translate(epd_memory_credit_associate((epd_memory_credit_t)credit,
        (epd_memory_backend_t)backend, consumed, changes, count));
}
static int payload_info(void *context, const void *data, uint32_t backend,
    epd_memory_owner_t *account, size_t *physical)
{
    epd_memory_block_snapshot_t block;
    (void)context;
    int result = translate(epd_memory_block_get(data, &block));
    if (result != EPD_RESOURCE_OK) return result;
    if (block.backend != backend) return EPD_RESOURCE_INVALID;
    *account = block.account; *physical = block.physical; return EPD_RESOURCE_OK;
}
static int credit_physical(void *context, epd_resource_credit_t credit, uint32_t backend, size_t *physical)
{
    epd_memory_credit_snapshot_t snapshot;
    (void)context;
    if (backend >= EPD_MEMORY_BACKEND_COUNT || !credit || credit > UINT32_MAX ||
        !epd_memory_credit_get((epd_memory_credit_t)credit, &snapshot)) return EPD_RESOURCE_STALE;
    if (snapshot.allocating[backend]) return EPD_RESOURCE_BUSY;
    *physical = snapshot.physical[backend]; return EPD_RESOURCE_OK;
}
static epd_memory_owner_t enter_credit(void *context, epd_resource_credit_t credit)
{ (void)context; return epd_memory_owner_enter_trusted((epd_memory_credit_t)credit); }
static void loaded_credit(void *context, epd_resource_credit_t credit)
{
    (void)context;
    /* Payload blocks keep the descriptor alive after unused peak is returned. */
    epd_memory_credit_end((epd_memory_credit_t)credit);
    epd_memory_owner_set_class((epd_memory_owner_t)credit, EPD_MEMORY_RECLAIMABLE);
}
static void payload_released(void *context, uint32_t backend, size_t bytes)
{
    (void)context; (void)backend; (void)bytes;
    epd_resource_runtime_changed(); epd_power_notify();
}

static int apply_activity_request(void *context, const epd_resource_activity_snapshot_t *request,
    const size_t unused[EPD_MEMORY_BACKEND_COUNT], epd_resource_grant_t *grant)
{
    epd_memory_credit_spec_t spec = {0};
    epd_memory_credit_snapshot_t snapshot;
    epd_memory_result_t result;
    epd_memory_credit_t credit = EPD_MEMORY_OWNER_INVALID;
    pending_t *pending = NULL;
    unsigned i;
    uint64_t remaining;
    (void)context;
    if (request->requested.backend >= EPD_MEMORY_BACKEND_COUNT) return EPD_RESOURCE_INVALID;
    for (i = 0; i < PENDING_COUNT; ++i) {
        if (runtime.pending[i].activity == request->activity) { pending = &runtime.pending[i]; break; }
    }
    if (pending && pending->revision != request->requested_revision) {
        drop(pending->credit); memset(pending, 0, sizeof(*pending)); pending = NULL;
    }
    if (pending) credit = pending->credit;
    else {
        for (i = 0; i < PENDING_COUNT; ++i)
            if (!runtime.pending[i].activity) { pending = &runtime.pending[i]; break; }
        if (!pending) return EPD_RESOURCE_CAPACITY;
        bool any = false;
        if (unused) memcpy(spec.bytes, unused, sizeof(spec.bytes));
        else {
            memcpy(spec.bytes, request->requested.companion_bytes, sizeof(spec.bytes));
            spec.bytes[request->requested.backend] = request->requested.target_bytes > request->associated_bytes ?
                request->requested.target_bytes - request->associated_bytes : 0;
        }
        for (i = 0; i < EPD_MEMORY_BACKEND_COUNT; ++i) if (spec.bytes[i]) any = true;
        if (!any) {
            grant->credit = unused ? EPD_MEMORY_CREDIT_NO_GRANT : NO_RESERVATION | ++runtime.zero_serial;
            grant->guaranteed_bytes = request->associated_bytes;
            grant->unused_bytes = 0;
            return EPD_RESOURCE_OK;
        }
        spec.priority = priority(request->requested.kind);
        if (request->requested.deadline) {
            uint64_t current = now(NULL);
            if (request->requested.deadline <= current) return EPD_RESOURCE_DENIED;
            remaining = request->requested.deadline - current;
            spec.timeout_ms = remaining > INT32_MAX ? INT32_MAX : (uint32_t)remaining;
        }
        if (!unused && request->credit && !(request->credit & NO_RESERVATION)) {
            result = epd_memory_credit_replace_prepare((epd_memory_credit_t)request->credit,
                request->owner, &spec, &credit);
        } else result = epd_memory_credit_request(request->owner, &spec, &credit);
        if (result != EPD_MEMORY_OK && result != EPD_MEMORY_WAITING) return translate(result);
        pending->activity = request->activity; pending->revision = request->requested_revision; pending->credit = credit;
    }
    if (!epd_memory_credit_get(credit, &snapshot)) {
        memset(pending, 0, sizeof(*pending)); drop(credit); return EPD_RESOURCE_STALE;
    }
    if (snapshot.state == EPD_MEMORY_CREDIT_WAITING) return EPD_RESOURCE_PENDING;
    memset(pending, 0, sizeof(*pending));
    if (snapshot.state != EPD_MEMORY_CREDIT_LIVE && snapshot.state != EPD_MEMORY_CREDIT_PREPARED) {
        drop(credit); return translate(snapshot.result);
    }
    grant->credit = credit;
    grant->unused_bytes = snapshot.state == EPD_MEMORY_CREDIT_PREPARED ?
        snapshot.prepared_bytes[request->requested.backend] : snapshot.unused[request->requested.backend];
    grant->guaranteed_bytes = request->associated_bytes + grant->unused_bytes;
    return EPD_RESOURCE_OK;
}
static int apply_activity(void *context, const epd_resource_activity_snapshot_t *request,
                          epd_resource_grant_t *grant)
{ return apply_activity_request(context, request, NULL, grant); }
static int suspend_activity(void *context, epd_resource_credit_t credit)
{
    (void)context;
    if (!credit || credit > UINT32_MAX) return EPD_RESOURCE_INVALID;
    return translate(epd_memory_credit_suspend((epd_memory_credit_t)credit));
}
static int resume_activity(void *context, const epd_resource_activity_snapshot_t *request,
    const size_t unused[EPD_MEMORY_BACKEND_COUNT], epd_resource_grant_t *grant)
{
    epd_memory_credit_snapshot_t credit;
    if (!request->credit || request->credit > UINT32_MAX ||
        !epd_memory_credit_get((epd_memory_credit_t)request->credit, &credit) ||
        credit.state != EPD_MEMORY_CREDIT_SUSPENDED) return EPD_RESOURCE_STOPPED;
    return apply_activity_request(context, request, unused, grant);
}
static int commit_resume(void *context, epd_resource_credit_t suspended, epd_resource_credit_t grant)
{
    (void)context;
    if (!suspended || suspended > UINT32_MAX || grant > UINT32_MAX) return EPD_RESOURCE_INVALID;
    return translate(epd_memory_credit_resume_commit((epd_memory_credit_t)suspended, (epd_memory_credit_t)grant));
}

static int admit_resource(void *context, epd_resource_t resource,
                          const epd_resource_descriptor_t *descriptor,
                          const epd_resource_credit_t *credits, size_t count,
                          epd_resource_credit_t *out)
{
    epd_memory_credit_t sources[PENDING_COUNT], shared = EPD_MEMORY_OWNER_INVALID;
    epd_memory_credit_spec_t spec = {0};
    epd_memory_credit_snapshot_t snapshot;
    epd_resource_snapshot_t resource_snapshot;
    epd_memory_result_t result;
    bool reserved = false;
    size_t i, source_count = 0;
    (void)context;
    if (descriptor->backend >= EPD_MEMORY_BACKEND_COUNT || count > PENDING_COUNT) return EPD_RESOURCE_INVALID;
    for (i = 0; i < count; ++i) {
        if (credits[i] & NO_RESERVATION) continue;
        sources[source_count] = (epd_memory_credit_t)credits[i];
        if (!epd_memory_credit_get(sources[source_count++], &snapshot)) return EPD_RESOURCE_STALE;
        if (snapshot.unused[descriptor->backend]) reserved = true;
    }
    if (reserved) {
        result = epd_memory_credit_share_prepare(sources, source_count, runtime.shared,
            (epd_memory_backend_t)descriptor->backend, descriptor->estimated_bytes, &shared);
    } else {
        spec.bytes[descriptor->backend] = descriptor->estimated_bytes;
        if (epd_resource_get(resource, &resource_snapshot) != EPD_RESOURCE_OK) return EPD_RESOURCE_STALE;
        spec.priority = priority(resource_snapshot.priority);
        result = epd_memory_credit_request(runtime.shared, &spec, &shared);
    }
    if (result != EPD_MEMORY_OK) return translate(result);
    epd_memory_owner_set_class(shared, EPD_MEMORY_INFLIGHT);
    *out = shared;
    return EPD_RESOURCE_OK;
}

static size_t worker_client(size_t budget, void *user)
{ (void)user; return epd_resource_runtime_worker(budget); }
static uint32_t next_timeout(uint32_t maximum, void *user)
{
    uint32_t delay;
    uint64_t ticks;
    (void)user;
    if (!runtime_initialized || !epd_memory_next_deadline(epd_memory_now_ms(), &delay)) return maximum;
    ticks = ((uint64_t)delay * RT_TICK_PER_SECOND + 999u) / 1000u;
    if (ticks > INT32_MAX) ticks = INT32_MAX;
    return ticks < maximum ? (uint32_t)ticks : maximum;
}
static bool on_lane(epd_resource_lane_t lane)
{
    epd_operation_executor_t execute, deliver, cleanup;
    if (!epd_operation_runtime_executors(&execute, &deliver, &cleanup)) return false;
    return rt_thread_self() == (lane == EPD_RESOURCE_WORKER ? execute.thread : deliver.thread);
}

static void *control_allocate(void *context, size_t bytes)
{
    (void)context;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    if (previous == EPD_MEMORY_OWNER_INVALID) return NULL;
    void *memory = epd_app_realloc(NULL, bytes, EPD_APP_PSRAM);
    epd_memory_owner_leave_trusted(previous);
    return memory;
}

int epd_resource_runtime_init(const epd_resource_runtime_config_t *config)
{
    epd_resource_runtime_t api = {0};
    epd_operation_runtime_client_t client = {&runtime, NULL, worker_client, next_timeout};
    int result;
    if (runtime_initialized) return EPD_RESOURCE_BUSY;
    if (!epd_operation_runtime_register(&client)) return EPD_RESOURCE_CAPACITY;
    memset(&runtime, 0, sizeof(runtime));
    runtime.ui_dispatch = EPD_MEMORY_OWNER_INVALID;
    if (config) runtime.config = *config;
    runtime.shared = epd_memory_scope_create_trusted(EPD_MEMORY_OWNER_SYSTEM, EPD_MEMORY_SCOPE_SHARED);
    if (runtime.shared == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_CAPACITY;
    if (rt_mutex_init(&runtime.lock, "resource", RT_IPC_FLAG_PRIO) != RT_EOK) {
        epd_memory_owner_close_trusted(runtime.shared); epd_memory_owner_release_trusted(runtime.shared);
        return EPD_RESOURCE_CAPACITY;
    }
    api.lock = state_lock; api.unlock = state_unlock; api.current_owner = current_owner;
    api.owner_account = owner_account; api.owner_retain = retain_owner; api.owner_release = release_owner;
    api.owner_is_within = owner_is_within;
    api.owner_enter = enter_owner; api.owner_leave = leave_owner; api.now = now; api.wake = wake;
    api.activity_apply = apply_activity; api.resource_admit = admit_resource;
    api.credit_release = release_credit; api.credit_enter = enter_credit; api.credit_loaded = loaded_credit;
    api.payload_released = payload_released;
    api.credit_commit = commit_credit;
    api.shared_commit = commit_shared; api.association_change = change_associations;
    api.payload_info = payload_info; api.credit_physical = credit_physical;
    api.reference_commit = commit_reference;
    api.activity_suspend = suspend_activity; api.activity_resume = resume_activity;
    api.credit_resume_commit = commit_resume;
    api.control_allocate = control_allocate;
    result = epd_resource_init(&api);
    if (result != EPD_RESOURCE_OK) {
        rt_mutex_detach(&runtime.lock);
        epd_memory_owner_close_trusted(runtime.shared); epd_memory_owner_release_trusted(runtime.shared);
        return result;
    }
    runtime_initialized = true;
    runtime.capacity_epoch = epd_memory_capacity_epoch();
    epd_memory_set_capacity_notifier(epd_operation_runtime_wake);
    epd_resource_runtime_changed();
    return EPD_RESOURCE_OK;
}

size_t epd_resource_runtime_worker(size_t budget)
{
    size_t processed;
    unsigned i;
    bool retry;
    uint32_t delay = UINT32_MAX, current;
    if (!runtime_initialized || !on_lane(EPD_RESOURCE_WORKER)) return 0;
    state_lock(NULL);
    if (runtime.in_worker) { state_unlock(NULL); return 0; }
    runtime.in_worker = true; retry = runtime.changed; runtime.changed = false;
    state_unlock(NULL);
    uint32_t epoch = epd_memory_capacity_epoch();
    if (epoch != runtime.capacity_epoch) { runtime.capacity_epoch = epoch; retry = true; }
    current = epd_memory_now_ms(); epd_memory_poll(current);
    for (i = 0; i < PENDING_COUNT; ++i) {
        epd_memory_credit_snapshot_t credit;
        if (runtime.pending[i].activity &&
            (!epd_memory_credit_get(runtime.pending[i].credit, &credit) || credit.state != EPD_MEMORY_CREDIT_WAITING))
            retry = true;
    }
    if (retry) epd_resource_retry();
    processed = epd_resource_process(EPD_RESOURCE_WORKER, budget);
    for (i = 0; i < PENDING_COUNT; ++i) {
        pending_t *p = &runtime.pending[i];
        epd_resource_activity_snapshot_t s;
        if (!p->activity) continue;
        /* Resource-level expiry can finish without calling the credit adapter. */
        if (epd_resource_activity_get(p->activity, &s) != EPD_RESOURCE_OK ||
            (s.state != EPD_RESOURCE_WAITING && s.state != EPD_RESOURCE_RESUMING) ||
            s.requested_revision != p->revision) {
            drop(p->credit); memset(p, 0, sizeof(*p));
            continue;
        }
        if (p->activity && (!p->reclaim_tried || p->reclaim_epoch != epoch)) {
            size_t need[EPD_MEMORY_BACKEND_COUNT];
            p->reclaim_tried = true; p->reclaim_epoch = epoch;
            if (epd_memory_credit_reclaim_need(p->credit, need)) {
                for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
                    epd_resource_reclaim_snapshot_t reclaim;
                    if (!need[b]) continue;
                    epd_resource_reclaim_status(b, &reclaim);
                    if (need[b] > reclaim.pending_bytes && epd_resource_reclaim_for(b,
                        need[b] - reclaim.pending_bytes, budget, s.requested.kind)) ++processed;
                }
            }
        }
    }
    epd_memory_dispatch_changes();
    epd_memory_next_deadline(current, &delay);
    if (runtime.config.deadline) runtime.config.deadline(runtime.config.context, delay);
    state_lock(NULL); runtime.in_worker = false; state_unlock(NULL);
    return processed;
}
size_t epd_resource_runtime_ui(size_t budget)
{ return runtime_initialized && on_lane(EPD_RESOURCE_UI) ? epd_resource_process(EPD_RESOURCE_UI, budget) : 0; }
void epd_resource_runtime_changed(void)
{
    if (!runtime_initialized) return;
    state_lock(NULL); runtime.changed = true; state_unlock(NULL);
    wake(NULL, EPD_RESOURCE_WORKER);
}
int epd_resource_runtime_bind(epd_memory_owner_t scope, uint64_t generation,
                              uint32_t mask, epd_resource_binding_t *out)
{
    epd_memory_owner_snapshot_t snapshot;
    if (!runtime_initialized || !epd_memory_owner_get(scope, &snapshot) || snapshot.closed ||
        (snapshot.kind != EPD_MEMORY_SCOPE_PAGE && snapshot.kind != EPD_MEMORY_SCOPE_TASK &&
         snapshot.kind != EPD_MEMORY_SCOPE_SERVICE && snapshot.kind != EPD_MEMORY_SCOPE_CLEANUP))
        return EPD_RESOURCE_DENIED;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(scope);
    if (previous == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_STOPPED;
    int result = epd_resource_binding_open(scope, generation, scope, mask, out);
    epd_memory_owner_leave_trusted(previous);
    return result;
}
int epd_resource_runtime_close_owner(epd_memory_owner_t owner)
{
    unsigned i;
    if (owner == EPD_MEMORY_OWNER_SYSTEM || owner == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_INVALID;
    epd_memory_owner_close_trusted(owner);
    if (runtime_initialized) {
        for (i = 0; i < PENDING_COUNT; ++i) {
            epd_memory_owner_t subject; bool used;
            state_lock(NULL); subject = runtime.contexts[i].owner; used = runtime.contexts[i].used; state_unlock(NULL);
            if (used && owner_is_within(NULL, subject, owner)) {
                state_lock(NULL);
                if (runtime.contexts[i].owner == subject) runtime.contexts[i].used = false;
                state_unlock(NULL);
            }
        }
    }
    return runtime_initialized ? epd_resource_close_owner(owner) : EPD_RESOURCE_OK;
}
bool epd_resource_runtime_owner_busy(epd_memory_owner_t owner)
{ return runtime_initialized && epd_resource_owner_busy(owner); }

static int context_state(epd_memory_owner_t owner, uint64_t generation, bool eligible, unsigned source)
{
    epd_memory_owner_snapshot_t snapshot;
    epd_resource_binding_t binding = 0;
    uint32_t full = EPD_RESOURCE_KIND_MASK(EPD_RESOURCE_OPTIONAL);
    unsigned i, slot = PENDING_COUNT;
    int result;
    uint64_t revision;
    bool page = source == 0, create = false;
    if (!runtime_initialized || !generation || !epd_memory_owner_get(owner, &snapshot) || snapshot.closed)
        return EPD_RESOURCE_STOPPED;
    if (page ? snapshot.kind != EPD_MEMORY_SCOPE_PAGE :
        snapshot.kind != EPD_MEMORY_SCOPE_TASK && snapshot.kind != EPD_MEMORY_SCOPE_SERVICE)
        return EPD_RESOURCE_DENIED;
    if (source == 2 && snapshot.kind != EPD_MEMORY_SCOPE_SERVICE) return EPD_RESOURCE_DENIED;
    full |= page ? EPD_RESOURCE_KIND_MASK(EPD_RESOURCE_INTERACTIVE) | EPD_RESOURCE_KIND_MASK(EPD_RESOURCE_NEARBY) :
        EPD_RESOURCE_KIND_MASK(EPD_RESOURCE_CONTINUOUS);
    state_lock(NULL);
    for (i = 0; i < PENDING_COUNT; ++i) {
        context_t *c = &runtime.contexts[i];
        if (c->used && c->owner == owner) {
            if (c->generation != generation || c->page != page) { state_unlock(NULL); return EPD_RESOURCE_STALE; }
            slot = i; break;
        }
        if (!c->used && slot == PENDING_COUNT) slot = i;
    }
    if (slot == PENDING_COUNT) { state_unlock(NULL); return EPD_RESOURCE_CAPACITY; }
    context_t *c = &runtime.contexts[slot];
    if (!c->used) {
        memset(c, 0, sizeof(*c)); c->owner = owner; c->generation = generation;
        c->page = page; c->used = true; create = true;
    }
    if (source == 1) c->operation_running = eligible;
    if (source == 2) c->service_running = eligible;
    c->eligible = page ? eligible : c->operation_running || c->service_running;
    ++c->revision; binding = c->binding;
    state_unlock(NULL);
    if (create) {
        result = epd_resource_runtime_bind(owner, generation, full, &binding);
        state_lock(NULL);
        bool current = c->used && c->owner == owner && c->generation == generation;
        if (current && result == EPD_RESOURCE_OK) c->binding = binding;
        else if (current) c->used = false;
        state_unlock(NULL);
        if (!current) { if (result == EPD_RESOURCE_OK) epd_resource_binding_end(binding); return EPD_RESOURCE_STOPPED; }
        if (result != EPD_RESOURCE_OK) return result;
    } else if (!binding) return EPD_RESOURCE_ACCEPTED; /* Initial publisher incorporates this fact. */
    for (;;) {
        state_lock(NULL);
        if (!c->used || c->owner != owner || c->generation != generation || c->binding != binding) {
            state_unlock(NULL); return EPD_RESOURCE_STOPPED;
        }
        revision = c->revision; eligible = c->eligible;
        state_unlock(NULL);
        result = epd_resource_binding_restrict(binding, eligible ? full : EPD_RESOURCE_KIND_MASK(EPD_RESOURCE_OPTIONAL));
        state_lock(NULL);
        bool current = !c->used || c->binding != binding || c->revision == revision;
        state_unlock(NULL);
        if (current || result != EPD_RESOURCE_OK) return result;
        /* An overlapping service/operation publication changed the OR result.
         * Repair the mask before returning, without holding a lock across wake. */
    }
}

int epd_resource_runtime_page_state(epd_memory_owner_t page, uint64_t generation, bool active)
{ return context_state(page, generation, active, 0); }
int epd_resource_runtime_task_state(epd_memory_owner_t task, uint64_t generation, bool running)
{ return context_state(task, generation, running, 1); }
int epd_resource_runtime_service_state(epd_memory_owner_t task, uint64_t generation, bool running)
{ return context_state(task, generation, running, 2); }

epd_memory_owner_t epd_resource_runtime_ui_enter(epd_memory_owner_t service)
{
    epd_memory_owner_snapshot_t source;
    if (!runtime_initialized || !on_lane(EPD_RESOURCE_UI) ||
        !epd_memory_owner_get(service, &source) || source.closed) return EPD_MEMORY_OWNER_INVALID;
    epd_memory_owner_t previous = runtime.ui_dispatch;
    runtime.ui_dispatch = service;
    return previous;
}
void epd_resource_runtime_ui_leave(epd_memory_owner_t previous)
{
    if (runtime_initialized && on_lane(EPD_RESOURCE_UI)) runtime.ui_dispatch = previous;
}

static bool context_at(epd_memory_owner_t owner, const epd_resource_context_request_t *request,
                       epd_resource_activity_spec_t *spec)
{
    bool found = false;
    state_lock(NULL);
    for (unsigned i = 0; i < PENDING_COUNT; ++i) {
        context_t *c = &runtime.contexts[i];
        bool eligible = request->kind == EPD_RESOURCE_OPTIONAL ||
            (c->eligible && (c->page ? request->kind == EPD_RESOURCE_INTERACTIVE || request->kind == EPD_RESOURCE_NEARBY :
                                        request->kind == EPD_RESOURCE_CONTINUOUS));
        if (c->used && c->owner == owner && eligible) {
            *spec = (epd_resource_activity_spec_t){
                .binding = c->binding, .interactive_parent = request->interactive_parent,
                .kind = request->kind, .backend = request->backend,
                .minimum_bytes = request->minimum_bytes, .target_bytes = request->target_bytes,
                .deadline = request->deadline};
            memcpy(spec->companion_bytes, request->companion_bytes, sizeof(spec->companion_bytes));
            found = true; break;
        }
    }
    state_unlock(NULL);
    return found;
}

static bool open_account(epd_memory_owner_t owner, epd_memory_owner_t account)
{
    for (unsigned depth = 0; depth < 17; ++depth) {
        epd_memory_owner_snapshot_t state;
        if (!epd_memory_owner_get(owner, &state) || state.closed || state.account != account) return false;
        if (owner == account) return true;
        owner = state.parent;
    }
    return false;
}

static int context_spec(const epd_resource_context_request_t *request,
                         epd_resource_activity_spec_t *spec, epd_memory_owner_t *owner)
{
    epd_memory_owner_snapshot_t snapshot;
    epd_memory_owner_t cursor = epd_memory_owner_current();
    unsigned depth;
    if (!runtime_initialized || !request || (unsigned)request->kind > EPD_RESOURCE_INTERACTIVE ||
        request->minimum_bytes > request->target_bytes)
        return EPD_RESOURCE_INVALID;
    if (!epd_memory_owner_get(cursor, &snapshot) || snapshot.closed) return EPD_RESOURCE_STOPPED;
    epd_memory_owner_t account = snapshot.account;
    if (!open_account(cursor, account)) return EPD_RESOURCE_STOPPED;
    /* A live interactive parent is the capability carried from GUI to a
     * service worker. Sharing an account alone never grants INTERACTIVE. */
    if (request->kind == EPD_RESOURCE_NEARBY) {
        epd_resource_activity_snapshot_t parent;
        if (!request->interactive_parent ||
            epd_resource_activity_get(request->interactive_parent, &parent) != EPD_RESOURCE_OK ||
            parent.state == EPD_RESOURCE_ENDING || parent.state == EPD_RESOURCE_ENDED ||
            parent.requested.kind != EPD_RESOURCE_INTERACTIVE ||
            !open_account(parent.owner, account) || !context_at(parent.owner, request, spec))
            return EPD_RESOURCE_DENIED;
        *owner = parent.owner;
        return EPD_RESOURCE_OK;
    }
    for (depth = 0; depth < 17; ++depth) {
        if (!epd_memory_owner_get(cursor, &snapshot) || snapshot.closed) return EPD_RESOURCE_STOPPED;
        if (context_at(cursor, request, spec)) { *owner = cursor; return EPD_RESOURCE_OK; }
        if (cursor == EPD_MEMORY_OWNER_SYSTEM) break;
        cursor = snapshot.parent;
    }
    if (request->kind == EPD_RESOURCE_INTERACTIVE && on_lane(EPD_RESOURCE_UI) &&
        runtime.config.foreground_page && open_account(runtime.ui_dispatch, account)) {
        cursor = runtime.config.foreground_page(runtime.config.context, account);
        if (open_account(cursor, account) && context_at(cursor, request, spec)) {
            *owner = cursor; return EPD_RESOURCE_OK;
        }
    }
    return EPD_RESOURCE_DENIED;
}
int epd_resource_context_capacity(const epd_resource_context_request_t *request,
    size_t available[EPD_MEMORY_BACKEND_COUNT], uint32_t *revision)
{
    if (!available || !revision) return EPD_RESOURCE_INVALID;
    memset(available, 0, sizeof(size_t) * EPD_MEMORY_BACKEND_COUNT);
    *revision = 0;
    epd_resource_activity_spec_t spec, checked;
    epd_memory_owner_t owner, checked_owner;
    int result = context_spec(request, &spec, &owner);
    if (result != EPD_RESOURCE_OK) return result;
    if (spec.backend >= EPD_MEMORY_BACKEND_COUNT) return EPD_RESOURCE_INVALID;
    uint32_t before = epd_memory_capacity_epoch();
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        size_t wanted = b == spec.backend ? spec.target_bytes : spec.companion_bytes[b];
        if (!wanted) continue;
        epd_memory_backend_snapshot_t heap;
        epd_memory_budget_snapshot_t budget;
        if (!epd_memory_backend_get((epd_memory_backend_t)b, &heap) ||
            !epd_memory_owner_budget(owner, (epd_memory_backend_t)b, &budget)) return EPD_RESOURCE_STOPPED;
        size_t reserved = heap.usage.unused + heap.usage.allocating;
        size_t used = reserved;
        for (unsigned c = 0; c < EPD_MEMORY_CLASS_COUNT; ++c) used += heap.usage.physical[c];
        size_t total = heap.config.capacity - heap.config.unavailable;
        size_t room = total > used ? total - used : 0;
        if (heap.available_known) {
            size_t physical = heap.available > reserved ? heap.available - reserved : 0;
            if (physical < room) room = physical;
        }
        size_t floor = priority(spec.kind) >= EPD_MEMORY_PRIORITY_NEARBY ? heap.headroom : 0;
        room = room > floor ? room - floor : 0;
        size_t committed = budget.private_committed + budget.associated;
        size_t account = budget.effective_limit > committed ? budget.effective_limit - committed : 0;
        available[b] = room < account ? room : account;
    }
    result = context_spec(request, &checked, &checked_owner);
    if (result != EPD_RESOURCE_OK) return result;
    if (checked_owner != owner || checked.binding != spec.binding) return EPD_RESOURCE_STALE;
    if (epd_memory_capacity_epoch() != before) return EPD_RESOURCE_BUSY;
    *revision = before;
    return EPD_RESOURCE_OK;
}
epd_memory_owner_t epd_resource_context_acquire(epd_resource_kind_t kind, epd_resource_activity_t parent)
{
    if (kind != EPD_RESOURCE_INTERACTIVE && kind != EPD_RESOURCE_NEARBY) return EPD_MEMORY_OWNER_INVALID;
    epd_resource_context_request_t request = {0};
    request.kind = kind; request.interactive_parent = parent;
    epd_resource_activity_spec_t spec;
    epd_memory_owner_t owner;
    if (context_spec(&request, &spec, &owner) != EPD_RESOURCE_OK ||
        !epd_memory_owner_retain_trusted(owner)) return EPD_MEMORY_OWNER_INVALID;
    return owner;
}
int epd_resource_context_begin(const epd_resource_context_request_t *request,
                               const epd_resource_observer_t *observer, epd_resource_activity_t *out)
{
    epd_resource_activity_spec_t spec;
    epd_memory_owner_t owner, previous;
    int result = context_spec(request, &spec, &owner);
    if (result != EPD_RESOURCE_OK) return result;
    previous = epd_memory_owner_enter(owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_STOPPED;
    result = epd_resource_activity_begin(&spec, observer, out);
    epd_memory_owner_leave(previous); return result;
}
int epd_resource_context_update(epd_resource_activity_t activity,
                                const epd_resource_context_request_t *request, uint64_t *revision)
{
    epd_resource_activity_spec_t spec;
    epd_memory_owner_t owner, previous;
    int result = context_spec(request, &spec, &owner);
    if (result != EPD_RESOURCE_OK) return result;
    previous = epd_memory_owner_enter(owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_STOPPED;
    result = epd_resource_activity_update(activity, &spec, revision);
    epd_memory_owner_leave(previous); return result;
}
RTM_EXPORT(epd_resource_context_begin);
RTM_EXPORT(epd_resource_context_update);

int epd_resource_context_resume(epd_resource_activity_t activity,
    const epd_resource_context_request_t *request, const size_t unused[EPD_MEMORY_BACKEND_COUNT], uint64_t *revision)
{
    epd_resource_activity_spec_t spec;
    epd_memory_owner_t owner, previous;
    int result = context_spec(request, &spec, &owner);
    if (result != EPD_RESOURCE_OK) return result;
    previous = epd_memory_owner_enter(owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_STOPPED;
    result = epd_resource_activity_resume(activity, &spec, unused, revision);
    epd_memory_owner_leave(previous); return result;
}
RTM_EXPORT(epd_resource_context_resume);

epd_memory_owner_t epd_resource_context_enter(epd_resource_activity_t activity)
{
    epd_resource_credit_t token = epd_resource_execution_acquire(activity);
    if (!token) return EPD_MEMORY_OWNER_INVALID;
    epd_memory_credit_snapshot_t credit;
    epd_memory_owner_t previous = EPD_MEMORY_OWNER_INVALID;
    if (token <= UINT32_MAX &&
        epd_memory_credit_get((epd_memory_credit_t)token, &credit) &&
        credit.state == EPD_MEMORY_CREDIT_LIVE)
        previous = epd_memory_owner_enter((epd_memory_owner_t)token);
    if (previous == EPD_MEMORY_OWNER_INVALID) epd_resource_execution_release(token);
    return previous;
}
void epd_resource_context_leave(epd_memory_owner_t previous)
{
    if (previous == EPD_MEMORY_OWNER_INVALID) return;
    epd_memory_credit_t credit = epd_memory_credit_current();
    epd_memory_owner_leave(previous);
    if (credit != EPD_MEMORY_OWNER_INVALID) {
        /* owner_current projects a credit to its parent owner. */
        epd_memory_owner_t restored = epd_memory_credit_current();
        if (restored == EPD_MEMORY_OWNER_INVALID) restored = epd_memory_owner_current();
        if (restored == previous) epd_resource_execution_release(credit);
    }
}
RTM_EXPORT(epd_resource_context_enter);
RTM_EXPORT(epd_resource_context_leave);

int epd_resource_context_reserve(epd_memory_region_t region, size_t bytes)
{
    epd_memory_credit_t token = epd_memory_credit_current();
    epd_memory_credit_snapshot_t credit;
    epd_memory_backend_t backend = EPD_MEMORY_BACKEND_NONE;
    size_t additional[EPD_MEMORY_BACKEND_COUNT] = {0};
    if (bytes == SIZE_MAX) return EPD_RESOURCE_INVALID;
    if (token == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_OK;
    bool found = epd_memory_credit_get(token, &credit);
    if (!found || credit.state != EPD_MEMORY_CREDIT_LIVE) {
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
        rt_kprintf("resource reserve_stopped credit=%lu owner=%lu found=%u state=%u result=%d region=%u bytes=%lu tick=%lu\n",
            (unsigned long)token, (unsigned long)(found ? credit.owner : 0), found,
            found ? (unsigned)credit.state : UINT_MAX, found ? (int)credit.result : 0,
            (unsigned)region, (unsigned long)bytes, (unsigned long)rt_tick_get());
#endif
        return EPD_RESOURCE_STOPPED;
    }
    if (region == EPD_MEMORY_REGION_HCPU_PSRAM) {
        if (!epd_memory_app_backend(region, &backend)) return EPD_RESOURCE_INVALID;
    } else if (region == EPD_MEMORY_REGION_SRAM) {
        for (unsigned i = 0; i < EPD_MEMORY_BACKEND_COUNT; ++i) {
            epd_memory_backend_snapshot_t b;
            if (epd_memory_backend_get((epd_memory_backend_t)i, &b) &&
                b.config.region == region && b.config.parent == EPD_MEMORY_BACKEND_NONE && !strcmp(b.name, "heap")) {
                backend = (epd_memory_backend_t)i; break;
            }
        }
    }
    if (backend == EPD_MEMORY_BACKEND_NONE) return EPD_RESOURCE_INVALID;
    if (credit.unused[backend] >= bytes) return EPD_RESOURCE_OK;
    additional[backend] = bytes - credit.unused[backend];
    epd_memory_result_t result = epd_memory_credit_expand(token, additional);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    if (result != EPD_MEMORY_OK) {
        epd_memory_backend_snapshot_t heap = {0};
        bool known = epd_memory_backend_get(backend, &heap) && heap.available_known;
        rt_kprintf("resource reserve_expand_failed credit=%lu owner=%lu state=%u region=%u backend=%u bytes=%lu unused=%lu additional=%lu memory_result=%d available_known=%u available=%lu heap_unused=%lu allocating=%lu tick=%lu\n",
            (unsigned long)token, (unsigned long)credit.owner, (unsigned)credit.state,
            (unsigned)region, (unsigned)backend, (unsigned long)bytes,
            (unsigned long)credit.unused[backend], (unsigned long)additional[backend],
            (int)result, known, (unsigned long)heap.available, (unsigned long)heap.usage.unused,
            (unsigned long)heap.usage.allocating, (unsigned long)rt_tick_get());
    }
#endif
    return translate(result);
}
RTM_EXPORT(epd_resource_context_reserve);

size_t epd_resource_file_io_peak(size_t path_bytes)
{
#ifdef RT_USING_DFS_ELMFAT
    size_t parts[7], total = 0, object = sizeof(FIL);
    if (!path_bytes) return SIZE_MAX;
    if (sizeof(DIR) > object) object = sizeof(DIR);
    parts[0] = epd_app_allocation_peak(sizeof(struct dfs_fd));
    parts[1] = parts[2] = epd_app_allocation_peak(path_bytes);
    parts[3] = epd_app_allocation_peak(object);
    parts[4] = epd_app_allocation_peak(sizeof(epd_resource_posix_dir_t));
    parts[5] = FF_VOLUMES > 1 ? epd_app_allocation_peak(256u) : 0;
    parts[6] = 0;
#if FF_USE_LFN == 3
    parts[6] = epd_app_allocation_peak((FF_MAX_LFN + 1u) * 2u +
        (FF_FS_EXFAT ? (FF_MAX_LFN + 44u) / 15u * 32u : 0u));
#endif
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        if (parts[i] > SIZE_MAX - total) return SIZE_MAX;
        total += parts[i];
    }
    return total;
#else
    (void)path_bytes; return SIZE_MAX;
#endif
}
RTM_EXPORT(epd_resource_file_io_peak);

int epd_resource_cache_open(epd_resource_cache_t *cache, epd_resource_lane_t lane,
    int (*shrink)(void *, uint32_t, size_t, size_t *), void *context)
{
    epd_memory_owner_snapshot_t owner;
    epd_resource_provider_spec_t spec = {0};
    if (!cache || !shrink || lane > EPD_RESOURCE_UI) return EPD_RESOURCE_INVALID;
    if (cache->provider) return EPD_RESOURCE_OK;
    if (!runtime_initialized || !epd_memory_owner_get(epd_memory_owner_current(), &owner) || owner.closed)
        return EPD_RESOURCE_STOPPED;
    spec.code_owner = owner.account; spec.implementation_version = 1;
    spec.load_lane = spec.destroy_lane = lane; spec.context = context; spec.shrink = shrink;
    cache->owner = epd_memory_scope_create(owner.account, EPD_MEMORY_SCOPE_SHARED);
    if (cache->owner == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_CAPACITY;
    spec.lifetime_owner = cache->owner;
    int result = epd_resource_provider_register(&spec, &cache->provider);
    /* The provider holds this scope until its callbacks and resources drain. */
    epd_memory_owner_release(cache->owner);
    if (result != EPD_RESOURCE_OK) {
        cache->owner = EPD_MEMORY_OWNER_INVALID;
    }
    return result;
}
int epd_resource_cache_update(epd_resource_cache_t *cache, const void *allocator,
    size_t total, size_t reclaimable, uint32_t cost, uint32_t local)
{
    epd_memory_backend_t backend;
    if (!cache || !cache->provider || !epd_memory_backend_find(allocator, &backend)) return EPD_RESOURCE_INVALID;
    return epd_resource_provider_cache_update(cache->provider, backend, total, reclaimable, cost, local);
}
int epd_resource_cache_update_region(epd_resource_cache_t *cache, epd_memory_region_t region,
    size_t total, size_t reclaimable, uint32_t cost, uint32_t local)
{
    epd_memory_backend_t backend;
    if (!cache || !cache->provider || !epd_memory_app_backend(region, &backend)) return EPD_RESOURCE_INVALID;
    return epd_resource_provider_cache_update(cache->provider, backend, total, reclaimable, cost, local);
}
int epd_resource_cache_close(epd_resource_cache_t *cache)
{
    int result;
    if (!cache) return EPD_RESOURCE_INVALID;
    if (!cache->provider) return EPD_RESOURCE_OK;
    epd_memory_owner_close(cache->owner);
    result = epd_resource_provider_unregister(cache->provider);
    if (result == EPD_RESOURCE_OK || result == EPD_RESOURCE_STALE) {
        cache->provider = 0;
        cache->owner = EPD_MEMORY_OWNER_INVALID;
    }
    return result;
}
int epd_resource_cache_adopt(epd_resource_cache_t *cache, void *allocation)
{
    if (!cache || !cache->provider || !allocation) return EPD_RESOURCE_INVALID;
    return translate(epd_memory_block_transfer(allocation, cache->owner));
}
RTM_EXPORT(epd_resource_cache_open);
RTM_EXPORT(epd_resource_cache_update);
RTM_EXPORT(epd_resource_cache_update_region);
RTM_EXPORT(epd_resource_cache_close);
RTM_EXPORT(epd_resource_cache_adopt);
