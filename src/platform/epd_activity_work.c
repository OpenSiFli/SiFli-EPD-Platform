/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_activity_work.h"
#include "epd_resource_runtime.h"
#include "epd_operation_runtime.h"
#include "../services/app_background.h"
#include <rthw.h>
#include <rtm.h>
#include <string.h>
#include <mem_section.h>

#if defined(RT_USING_MEMHEAP_OWNER) && defined(RT_USING_THREAD_MEM_CONTEXT)
extern struct rt_memheap app_image_psram_memheap;
struct epd_activity_work {
    struct epd_activity_work *next, *live_next;
    struct rt_semaphore changed;
    epd_resource_activity_t activity;
    epd_memory_owner_t owner;
    epd_memory_credit_t credit;
    epd_memory_backend_t psram, sram;
    epd_app_task_group_t group;
    epd_memory_owner_t task;
    size_t psram_peak, sram_peak;
    size_t prepare_psram, prepare_sram;
    void (*wake)(void *);
    void *wake_context;
    epd_resource_kind_t kind;
    unsigned references;
    bool closing, settled, resumable, paused;
};
static epd_activity_work_t *retiring, *live;
static const uint8_t client_key = 0;

/* Admission must remain possible while payload allocations wait for reclaim. */
#define WORK_COUNT 32
L2_RET_BSS_SECT_BEGIN(epd_activity_work)
static epd_activity_work_t work_slots[WORK_COUNT] L2_RET_BSS_SECT(epd_activity_work);
static bool work_used[WORK_COUNT] L2_RET_BSS_SECT(epd_activity_work);
L2_RET_BSS_SECT_END
static bool work_initialized;
static epd_activity_work_t *work_allocate(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!work_initialized) {
        memset(work_used, 0, sizeof(work_used));
        work_initialized = true;
    }
    epd_activity_work_t *work = NULL;
    for (unsigned i = 0; i < WORK_COUNT; ++i) if (!work_used[i]) {
        work_used[i] = true; work = &work_slots[i]; break;
    }
    rt_hw_interrupt_enable(level);
    if (work) memset(work, 0, sizeof(*work));
    return work;
}
static void work_release(epd_activity_work_t *work)
{
    if (!work) return;
    rt_base_t level = rt_hw_interrupt_disable();
    work_used[work - work_slots] = false;
    rt_hw_interrupt_enable(level);
}

static void register_work(epd_activity_work_t *work)
{
    rt_base_t level = rt_hw_interrupt_disable();
    work->live_next = live; live = work;
    rt_hw_interrupt_enable(level);
}

/* Queued operations outlive the operation that submits them. Preserve the
 * caller's PAGE/service/task, skipping only this helper's temporary scopes. */
static epd_memory_owner_t queue_parent(epd_memory_owner_t owner)
{
    rt_base_t level = rt_hw_interrupt_disable();
    for (;;) {
        epd_activity_work_t *work = live;
        while (work && work->owner != owner) work = work->live_next;
        if (!work) break;
        epd_memory_owner_snapshot_t source;
        if (!epd_memory_owner_get(owner, &source) || source.closed) {
            owner = EPD_MEMORY_OWNER_INVALID; break;
        }
        owner = source.parent;
    }
    rt_hw_interrupt_enable(level);
    return owner;
}

static void notice(void *context, const epd_resource_activity_snapshot_t *snapshot)
{
    epd_activity_work_t *work = context;
    (void)snapshot;
    rt_sem_release(&work->changed);
    rt_base_t level = rt_hw_interrupt_disable();
    void (*wake)(void *) = work->closing ? NULL : work->wake;
    void *wake_context = work->wake_context;
    rt_hw_interrupt_enable(level);
    if (wake) wake(wake_context);
    epd_operation_runtime_wake();
}
static size_t retire_work(size_t budget, void *unused)
{
    (void)unused;
    size_t done = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    epd_activity_work_t *work = retiring;
    rt_hw_interrupt_enable(level);
    while (work && done < budget) {
        epd_activity_work_t *next = work->next;
        int result = epd_resource_activity_forget(work->activity);
        /* Owner close may already have removed this activity after its jobs and
         * observer drained. STALE refers to the complete, generation-tagged ID. */
        if (result == EPD_RESOURCE_OK || result == EPD_RESOURCE_STALE) {
            level = rt_hw_interrupt_disable();
            epd_activity_work_t **link = &retiring;
            while (*link != work) link = &(*link)->next;
            *link = work->next;
            link = &live;
            while (*link && *link != work) link = &(*link)->live_next;
            if (*link) *link = work->live_next;
            rt_hw_interrupt_enable(level);
            epd_memory_owner_t owner = work->owner;
            rt_sem_detach(&work->changed);
            work_release(work);
            epd_resource_runtime_close_owner(owner);
            epd_memory_owner_close_trusted(owner);
            epd_memory_owner_release_trusted(owner);
            ++done;
        }
        work = next;
    }
    return done;
}

void epd_activity_work_close(epd_activity_work_t *work)
{
    if (!work) return;
    rt_base_t level = rt_hw_interrupt_disable();
    if (work->closing) { rt_hw_interrupt_enable(level); return; }
    if (--work->references) { rt_hw_interrupt_enable(level); return; }
    work->closing = true;
    rt_hw_interrupt_enable(level);
    if (work->credit != EPD_MEMORY_OWNER_INVALID)
        epd_memory_owner_set_class(work->credit, EPD_MEMORY_ACTIVE);
    epd_resource_runtime_task_state(work->owner, work->owner, false);
    epd_resource_activity_end(work->activity);
    if (work->group) {
        app_background_task_end(work->task);
        app_background_group_release(work->group);
        work->group = EPD_APP_TASK_GROUP_NONE;
    }
    level = rt_hw_interrupt_disable();
    work->next = retiring; retiring = work;
    rt_hw_interrupt_enable(level);
    epd_operation_runtime_wake();
}
bool epd_activity_work_retain(epd_activity_work_t *work)
{
    if (!work) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    bool retained = !work->closing && work->references != UINT32_MAX;
    if (retained) ++work->references;
    rt_hw_interrupt_enable(level);
    return retained;
}

epd_activity_work_t *epd_activity_work_queue(epd_resource_kind_t kind,
    epd_resource_activity_t parent, size_t psram, size_t sram,
    void (*wake)(void *), void *context)
{
    if (!psram || !rt_thread_self() || rt_interrupt_get_nest() || rt_critical_level()) return NULL;
    epd_operation_runtime_client_t client = {&client_key, NULL, retire_work, NULL};
    if (!epd_operation_runtime_register(&client)) return NULL;
    epd_memory_backend_t backend, internal = EPD_MEMORY_BACKEND_NONE;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend)) return NULL;
    for (unsigned i = 0; sram && i < EPD_MEMORY_BACKEND_COUNT; ++i) {
        epd_memory_backend_snapshot_t b;
        if (epd_memory_backend_get((epd_memory_backend_t)i, &b) && b.config.region == EPD_MEMORY_REGION_SRAM &&
            b.config.parent == EPD_MEMORY_BACKEND_NONE && !strcmp(b.name, "heap")) internal = (epd_memory_backend_t)i;
    }
    if (sram && internal == EPD_MEMORY_BACKEND_NONE) return NULL;
    epd_memory_owner_t current = queue_parent(epd_memory_owner_current());
    epd_memory_owner_t page = EPD_MEMORY_OWNER_INVALID;
    if (kind == EPD_RESOURCE_INTERACTIVE || kind == EPD_RESOURCE_NEARBY) {
        page = epd_resource_context_acquire(kind, parent);
        if (page == EPD_MEMORY_OWNER_INVALID) return NULL;
        current = page;
    }
    epd_memory_owner_snapshot_t source;
    epd_app_task_group_t group = EPD_APP_TASK_GROUP_NONE;
    epd_memory_owner_t task = EPD_MEMORY_OWNER_INVALID;
    if (!epd_memory_owner_get(current, &source)) goto failed_group;
    if (source.account != EPD_MEMORY_OWNER_SYSTEM) {
        if (app_background_group_create(current, &group) != EPD_MEMORY_OK) goto failed_group;
        if (app_background_task_begin(group, &task) != EPD_MEMORY_OK) goto failed_group;
        epd_memory_result_t policy = app_background_group_default(group, EPD_APP_BG_STATE);
        if (policy != EPD_MEMORY_OK && !(policy == EPD_MEMORY_BUSY && app_background_group_accepting(group))) goto failed_group;
    }
    epd_memory_owner_t owner = epd_memory_scope_create_trusted(current, EPD_MEMORY_SCOPE_TASK);
    if (page != EPD_MEMORY_OWNER_INVALID) epd_memory_owner_release_trusted(page);
    page = EPD_MEMORY_OWNER_INVALID;
    if (owner == EPD_MEMORY_OWNER_INVALID) goto failed_group;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    epd_activity_work_t *work = NULL;
    bool sem_ready = false;
    if (previous == EPD_MEMORY_OWNER_INVALID) goto failed_owner;
    work = work_allocate();
    if (!work) goto restore;
    memset(work, 0, sizeof(*work));
    work->references = 1;
    work->owner = owner; work->credit = EPD_MEMORY_OWNER_INVALID;
    work->psram = backend; work->sram = internal; work->sram_peak = sram; work->psram_peak = psram;
    work->prepare_psram = psram; work->prepare_sram = sram;
    work->group = group; work->task = task; work->wake = wake; work->wake_context = context;
    work->kind = kind;
    if (rt_sem_init(&work->changed, "mem_queue", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto restore;
    sem_ready = true;
    if (epd_resource_runtime_task_state(owner, owner, true) != EPD_RESOURCE_OK) goto restore;
    epd_resource_context_request_t request = {kind, backend, psram, psram,
        (uint64_t)epd_memory_now_ms() + 500u, parent, {0}};
    if (sram) request.companion_bytes[internal] = sram;
    epd_resource_observer_t observer = {notice, work, EPD_RESOURCE_WORKER};
    if (epd_resource_context_begin(&request, &observer, &work->activity) != EPD_RESOURCE_ACCEPTED) goto restore;
    register_work(work);
    epd_memory_owner_leave_trusted(previous);
    return work;
restore:
    epd_memory_owner_leave_trusted(previous);
failed_owner:
    if (sem_ready) rt_sem_detach(&work->changed);
    work_release(work);
    epd_resource_runtime_close_owner(owner);
    epd_memory_owner_close_trusted(owner); epd_memory_owner_release_trusted(owner);
failed_group:
    if (page != EPD_MEMORY_OWNER_INVALID) epd_memory_owner_release_trusted(page);
    if (group) { if (task != EPD_MEMORY_OWNER_INVALID) app_background_task_end(task); app_background_group_release(group); }
    return NULL;
}

bool epd_activity_work_demand(epd_activity_work_t *work, epd_activity_demand_t *out)
{
    if (!work || !out || work->closing) return false;
    *out = (epd_activity_demand_t){work->prepare_psram, work->prepare_sram, work->credit};
    return true;
}
int epd_activity_work_capacity(epd_resource_kind_t kind, epd_resource_activity_t parent,
    size_t psram, size_t sram, epd_activity_capacity_t *out)
{
    if (!out || !psram) return EPD_RESOURCE_INVALID;
    memset(out, 0, sizeof(*out));
    epd_memory_backend_t backend, internal = EPD_MEMORY_BACKEND_NONE;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend)) return EPD_RESOURCE_INVALID;
    for (unsigned i = 0; sram && i < EPD_MEMORY_BACKEND_COUNT; ++i) {
        epd_memory_backend_snapshot_t b;
        if (epd_memory_backend_get((epd_memory_backend_t)i, &b) && b.config.region == EPD_MEMORY_REGION_SRAM &&
            b.config.parent == EPD_MEMORY_BACKEND_NONE && !strcmp(b.name, "heap")) internal = (epd_memory_backend_t)i;
    }
    if (sram && internal == EPD_MEMORY_BACKEND_NONE) return EPD_RESOURCE_INVALID;
    epd_resource_context_request_t request = {kind, backend, psram, psram, 0, parent, {0}};
    if (sram) request.companion_bytes[internal] = sram;
    size_t available[EPD_MEMORY_BACKEND_COUNT];
    int result = epd_resource_context_capacity(&request, available, &out->revision);
    if (result != EPD_RESOURCE_OK) return result;
    out->psram = available[backend]; out->sram = sram ? available[internal] : 0;
    out->ready = out->psram >= psram && out->sram >= sram;
    return EPD_RESOURCE_OK;
}
int epd_activity_work_ready(epd_activity_work_t *work)
{
    epd_resource_activity_snapshot_t state;
    if (!work || work->closing || work->settled || (work->group && !app_background_group_accepting(work->group)) ||
        epd_resource_activity_get(work->activity, &state) != EPD_RESOURCE_OK) return -1;
    if (state.state == EPD_RESOURCE_WAITING) return 0;
    if (state.state != EPD_RESOURCE_ADMITTED || !state.credit || state.credit > UINT32_MAX) return -1;
    if (work->credit != (epd_memory_credit_t)state.credit) {
        work->credit = (epd_memory_credit_t)state.credit;
        epd_memory_owner_set_class(work->credit, EPD_MEMORY_INFLIGHT);
    }
    return 1;
}
int epd_activity_work_prepare(epd_activity_work_t *work, size_t psram, size_t sram)
{
    int ready = epd_activity_work_ready(work);
    if (ready != 1) return ready;
    epd_memory_credit_snapshot_t credit;
    if ((sram && work->sram == EPD_MEMORY_BACKEND_NONE) ||
        !epd_memory_credit_get(work->credit, &credit) || credit.state != EPD_MEMORY_CREDIT_LIVE) return -1;
    work->prepare_psram = psram > credit.unused[work->psram] ? psram : credit.unused[work->psram];
    work->prepare_sram = work->sram == EPD_MEMORY_BACKEND_NONE ? 0 :
        (sram > credit.unused[work->sram] ? sram : credit.unused[work->sram]);
    size_t additional[EPD_MEMORY_BACKEND_COUNT] = {0};
    if (psram > credit.unused[work->psram]) additional[work->psram] = psram - credit.unused[work->psram];
    if (sram && sram > credit.unused[work->sram]) additional[work->sram] = sram - credit.unused[work->sram];
    epd_memory_result_t expanded = epd_memory_credit_expand(work->credit, additional);
    if (expanded == EPD_MEMORY_OK) return 1;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    epd_memory_backend_snapshot_t external = {0}, internal = {0};
    epd_memory_backend_get(work->psram, &external);
    if (work->sram != EPD_MEMORY_BACKEND_NONE) epd_memory_backend_get(work->sram, &internal);
    rt_kprintf("work_prepare credit=%lu kind=%u result=%d psram_add=%lu psram_available=%lu sram_add=%lu sram_available=%lu\n",
        (unsigned long)work->credit, (unsigned)work->kind, (int)expanded,
        (unsigned long)additional[work->psram], (unsigned long)external.available,
        (unsigned long)(sram ? additional[work->sram] : 0), (unsigned long)internal.available);
#endif

    epd_resource_activity_snapshot_t state;
    if (epd_resource_activity_get(work->activity, &state) != EPD_RESOURCE_OK ||
        state.state != EPD_RESOURCE_ADMITTED) return -1;
    epd_resource_context_request_t request = {state.requested.kind, state.requested.backend,
        0, 0, (uint64_t)epd_memory_now_ms() + 500u, state.requested.interactive_parent, {0}};
    size_t unused = psram > credit.unused[work->psram] ? psram : credit.unused[work->psram];
    if (unused > SIZE_MAX - state.associated_bytes) return -1;
    request.minimum_bytes = request.target_bytes = unused + state.associated_bytes;
    for (unsigned i = 0; i < EPD_MEMORY_BACKEND_COUNT; ++i)
        request.companion_bytes[i] = i == work->psram ? 0 : credit.unused[i];
    if (sram > 0 && sram > request.companion_bytes[work->sram]) request.companion_bytes[work->sram] = sram;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(work->owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return -1;
    uint64_t revision;
    /* INTERACTIVE/NEARBY activities belong to their verified PAGE, while this
     * helper executes under a child TASK. Resolve the same live binding again. */
    int result = epd_resource_context_update(work->activity, &request, &revision);
    epd_memory_owner_leave_trusted(previous);
    return result == EPD_RESOURCE_ACCEPTED ? 0 : -1;
}
epd_resource_activity_t epd_activity_work_activity(const epd_activity_work_t *work)
{ return work && !work->closing ? work->activity : 0; }
bool epd_activity_work_settle(epd_activity_work_t *work)
{
    if (!work || work->closing || work->kind != EPD_RESOURCE_INTERACTIVE) return false;
    if (work->settled) return true;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(work->owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return false;
    epd_resource_context_request_t request = {EPD_RESOURCE_INTERACTIVE, work->psram, 0, 0, 0, 0, {0}};
    uint64_t revision;
    int result = epd_resource_context_update(work->activity, &request, &revision);
    epd_memory_owner_leave_trusted(previous);
    if (result != EPD_RESOURCE_ACCEPTED) return false;
    work->settled = true;
    if (work->credit != EPD_MEMORY_OWNER_INVALID) epd_memory_owner_set_class(work->credit, EPD_MEMORY_ACTIVE);
    epd_resource_runtime_task_state(work->owner, work->owner, false);
    if (work->group) {
        app_background_task_end(work->task); app_background_group_release(work->group);
        work->group = EPD_APP_TASK_GROUP_NONE;
    }
    return true;
}

epd_activity_work_t *epd_activity_work_open(size_t psram, size_t sram)
{
    epd_operation_executor_t execute, deliver, cleanup;
    if (!psram || !rt_thread_self() || rt_interrupt_get_nest() || rt_critical_level() ||
        !epd_operation_runtime_executors(&execute, &deliver, &cleanup) ||
        rt_thread_self() == execute.thread || rt_thread_self() == deliver.thread) return NULL;
    epd_operation_runtime_client_t client = {&client_key, NULL, retire_work, NULL};
    if (!epd_operation_runtime_register(&client)) return NULL;
    epd_memory_backend_t psram_backend, sram_backend = EPD_MEMORY_BACKEND_NONE;
    if (!epd_memory_backend_find(&app_image_psram_memheap, &psram_backend)) return NULL;
    for (unsigned i = 0; i < EPD_MEMORY_BACKEND_COUNT; ++i) {
        epd_memory_backend_snapshot_t b;
        if (epd_memory_backend_get((epd_memory_backend_t)i, &b) && b.config.region == EPD_MEMORY_REGION_SRAM &&
            b.config.parent == EPD_MEMORY_BACKEND_NONE && !strcmp(b.name, "heap"))
            sram_backend = (epd_memory_backend_t)i;
    }
    if (sram && sram_backend == EPD_MEMORY_BACKEND_NONE) return NULL;
    epd_memory_owner_t owner = epd_memory_scope_create_trusted(epd_memory_owner_current(), EPD_MEMORY_SCOPE_TASK);
    if (owner == EPD_MEMORY_OWNER_INVALID) return NULL;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    epd_activity_work_t *work = NULL;
    bool sem_ready = false;
    if (previous == EPD_MEMORY_OWNER_INVALID) goto failed;
    work = work_allocate();
    if (!work) goto restore;
    memset(work, 0, sizeof(*work));
    work->references = 1;
    work->owner = owner; work->credit = EPD_MEMORY_OWNER_INVALID;
    work->psram = psram_backend; work->sram = sram_backend;
    work->psram_peak = psram; work->sram_peak = sram;
    work->prepare_psram = psram; work->prepare_sram = sram;
    work->kind = EPD_RESOURCE_CONTINUOUS; work->resumable = true;
    if (rt_sem_init(&work->changed, "mem_work", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto restore;
    sem_ready = true;
    if (epd_resource_runtime_task_state(owner, owner, true) != EPD_RESOURCE_OK) goto restore;
    epd_resource_context_request_t request = {EPD_RESOURCE_CONTINUOUS, psram_backend, psram, psram,
                                             (uint64_t)epd_memory_now_ms() + 500u, 0, {0}};
    if (sram) request.companion_bytes[sram_backend] = sram;
    epd_resource_observer_t observer = {notice, work, EPD_RESOURCE_WORKER};
    if (epd_resource_context_begin(&request, &observer, &work->activity) != EPD_RESOURCE_ACCEPTED) goto restore;
    register_work(work);
    rt_tick_t start = rt_tick_get(), timeout = rt_tick_from_millisecond(500);
    for (;;) {
        epd_resource_activity_snapshot_t state;
        if (epd_resource_activity_get(work->activity, &state) != EPD_RESOURCE_OK) break;
        if (state.state == EPD_RESOURCE_ADMITTED && state.credit && state.credit <= UINT32_MAX) {
            work->credit = (epd_memory_credit_t)state.credit;
            epd_memory_owner_set_class(work->credit, EPD_MEMORY_INFLIGHT);
            epd_memory_owner_leave_trusted(previous);
            return work;
        }
        if (state.state != EPD_RESOURCE_WAITING) break;
        rt_tick_t elapsed = rt_tick_get() - start;
        if (elapsed >= timeout || rt_sem_take(&work->changed, (rt_int32_t)(timeout - elapsed)) != RT_EOK) break;
    }
restore:
    epd_memory_owner_leave_trusted(previous);
failed:
    if (work && work->activity) { epd_activity_work_close(work); return NULL; }
    if (sem_ready) rt_sem_detach(&work->changed);
    work_release(work);
    epd_resource_runtime_close_owner(owner);
    epd_memory_owner_close_trusted(owner); epd_memory_owner_release_trusted(owner);
    return NULL;
}

static bool work_enter(epd_activity_work_t *work, epd_activity_guard_t *guard, bool trusted)
{
    if (!work || !guard || work->paused || epd_activity_work_ready(work) != 1) return false;
    guard->owner = trusted ? epd_memory_owner_enter_trusted(work->owner) : epd_memory_owner_enter(work->owner);
    if (guard->owner == EPD_MEMORY_OWNER_INVALID) return false;
    guard->credit = epd_resource_context_enter(work->activity);
    if (guard->credit == EPD_MEMORY_OWNER_INVALID) {
        if (trusted) epd_memory_owner_leave_trusted(guard->owner);
        else epd_memory_owner_leave(guard->owner);
        return false;
    }
    return true;
}
bool epd_activity_work_enter(epd_activity_work_t *work, epd_activity_guard_t *guard)
{ return work_enter(work, guard, false); }
bool epd_activity_work_enter_trusted(epd_activity_work_t *work, epd_activity_guard_t *guard)
{ return work_enter(work, guard, true); }
void epd_activity_work_leave(epd_activity_guard_t *guard)
{
    epd_resource_context_leave(guard->credit);
    epd_memory_owner_leave(guard->owner);
}
void epd_activity_work_leave_trusted(epd_activity_guard_t *guard)
{
    epd_resource_context_leave(guard->credit);
    epd_memory_owner_leave_trusted(guard->owner);
}
bool epd_activity_work_reserve(epd_activity_work_t *work, epd_app_memory_t region, size_t bytes)
{
    if (!work || work->closing || work->settled) return false;
    if (!bytes) return true;
    epd_memory_backend_t backend = region == EPD_APP_PSRAM ? work->psram : work->sram;
    epd_memory_credit_snapshot_t credit;
    if (backend == EPD_MEMORY_BACKEND_NONE || !epd_memory_credit_get(work->credit, &credit) ||
        credit.state != EPD_MEMORY_CREDIT_LIVE) return false;
    if (credit.unused[backend] >= bytes) return true;
    size_t additional[EPD_MEMORY_BACKEND_COUNT] = {0};
    additional[backend] = bytes - credit.unused[backend];
    return epd_memory_credit_expand(work->credit, additional) == EPD_MEMORY_OK;
}

bool epd_activity_work_replenish_to(epd_activity_work_t *work, size_t psram, size_t sram)
{
    epd_memory_credit_snapshot_t credit;
    if (!work || !work->resumable || work->paused ||
        (sram && work->sram == EPD_MEMORY_BACKEND_NONE) || epd_activity_work_ready(work) != 1 ||
        !epd_memory_credit_get(work->credit, &credit) || credit.state != EPD_MEMORY_CREDIT_LIVE)
        return false;
    size_t additional[EPD_MEMORY_BACKEND_COUNT] = {0};
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        if (credit.allocating[b]) return false;
        size_t peak = b == work->psram ? psram : b == work->sram ? sram : 0;
        size_t missing = peak > credit.physical[b] ? peak - credit.physical[b] : 0;
        additional[b] = missing > credit.unused[b] ? missing - credit.unused[b] : 0;
    }
    return epd_memory_credit_expand(work->credit, additional) == EPD_MEMORY_OK;
}

bool epd_activity_work_replenish(epd_activity_work_t *work)
{
    return work && epd_activity_work_replenish_to(work, work->psram_peak, work->sram_peak);
}

bool epd_activity_work_trim(epd_activity_work_t *work)
{
    epd_memory_credit_snapshot_t credit;
    if (!work || work->paused || epd_activity_work_ready(work) != 1 ||
        !epd_memory_credit_get(work->credit, &credit) || credit.state != EPD_MEMORY_CREDIT_LIVE)
        return false;
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
        if (credit.allocating[b]) return false;
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
        if (credit.unused[b] && epd_memory_credit_trim(work->credit,
                (epd_memory_backend_t)b, credit.unused[b]) != EPD_MEMORY_OK) return false;
    return true;
}

static bool can_wait(void)
{
    epd_operation_executor_t execute, deliver, cleanup;
    return rt_thread_self() && !rt_interrupt_get_nest() && !rt_critical_level() &&
        epd_operation_runtime_executors(&execute, &deliver, &cleanup) &&
        rt_thread_self() != execute.thread && rt_thread_self() != deliver.thread;
}
static bool wait_state(epd_activity_work_t *work, epd_resource_activity_state_t wanted,
                       uint64_t revision)
{
    rt_tick_t start = rt_tick_get(), timeout = rt_tick_from_millisecond(500);
    for (;;) {
        epd_resource_activity_snapshot_t state;
        if (epd_resource_activity_get(work->activity, &state) != EPD_RESOURCE_OK) return false;
        if (state.state == wanted && (!revision || state.effective_revision == revision))
            return state.credit == work->credit;
        if (state.state != EPD_RESOURCE_PAUSING && state.state != EPD_RESOURCE_RESUMING) return false;
        rt_tick_t elapsed = rt_tick_get() - start;
        if (elapsed >= timeout || rt_sem_take(&work->changed, (rt_int32_t)(timeout - elapsed)) != RT_EOK)
            return false;
    }
}
bool epd_activity_work_pause(epd_activity_work_t *work)
{
    if (!work || !work->resumable || work->closing || !can_wait()) return false;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(work->owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return false;
    int result = epd_resource_activity_pause(work->activity);
    if (result != EPD_RESOURCE_OK && result != EPD_RESOURCE_ACCEPTED && result != EPD_RESOURCE_PENDING) {
        epd_memory_owner_leave_trusted(previous); return false;
    }
    work->paused = true;
    epd_resource_runtime_task_state(work->owner, work->owner, false);
    bool paused = wait_state(work, EPD_RESOURCE_PAUSED, 0);
    if (paused) epd_memory_owner_set_class(work->credit, EPD_MEMORY_ACTIVE);
    epd_memory_owner_leave_trusted(previous);
    return paused;
}
bool epd_activity_work_resume(epd_activity_work_t *work)
{
    if (!work || !work->resumable || work->closing || !can_wait()) return false;
    epd_memory_credit_snapshot_t credit;
    if (!epd_memory_credit_get(work->credit, &credit)) return false;
    if (!work->paused) return credit.state == EPD_MEMORY_CREDIT_LIVE;
    if (credit.state != EPD_MEMORY_CREDIT_SUSPENDED) return false;
    size_t unused[EPD_MEMORY_BACKEND_COUNT] = {0};
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
        if (credit.allocating[b]) return false;
    unused[work->psram] = work->psram_peak > credit.physical[work->psram] ?
        work->psram_peak - credit.physical[work->psram] : 0;
    if (work->sram != EPD_MEMORY_BACKEND_NONE)
        unused[work->sram] = work->sram_peak > credit.physical[work->sram] ?
            work->sram_peak - credit.physical[work->sram] : 0;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(work->owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return false;
    bool resumed = false;
    if (epd_resource_runtime_task_state(work->owner, work->owner, true) == EPD_RESOURCE_OK) {
        epd_resource_context_request_t request = {EPD_RESOURCE_CONTINUOUS, work->psram,
            unused[work->psram], unused[work->psram], (uint64_t)epd_memory_now_ms() + 500u, 0, {0}};
        uint64_t revision;
        if (epd_resource_context_resume(work->activity, &request, unused, &revision) == EPD_RESOURCE_ACCEPTED)
            resumed = wait_state(work, EPD_RESOURCE_ADMITTED, revision);
    }
    if (resumed) {
        epd_memory_owner_set_class(work->credit, EPD_MEMORY_INFLIGHT);
        work->paused = false;
    } else {
        epd_resource_activity_pause(work->activity);
        epd_resource_runtime_task_state(work->owner, work->owner, false);
    }
    epd_memory_owner_leave_trusted(previous);
    return resumed;
}
#else
bool epd_activity_work_demand(epd_activity_work_t *work, epd_activity_demand_t *out)
{ (void)work; (void)out; return false; }
int epd_activity_work_capacity(epd_resource_kind_t kind, epd_resource_activity_t parent,
    size_t psram, size_t sram, epd_activity_capacity_t *out)
{
    (void)kind; (void)parent; (void)psram; (void)sram;
    if (out) memset(out, 0, sizeof(*out));
    return EPD_RESOURCE_INVALID;
}
struct epd_activity_work { unsigned unused; };
static epd_activity_work_t disabled;
epd_activity_work_t *epd_activity_work_queue(epd_resource_kind_t kind,
    epd_resource_activity_t parent, size_t psram, size_t sram, void (*wake)(void *), void *context)
{ (void)kind; (void)parent; (void)psram; (void)sram; (void)wake; (void)context; return &disabled; }
int epd_activity_work_ready(epd_activity_work_t *work) { return work ? 1 : -1; }
int epd_activity_work_prepare(epd_activity_work_t *work, size_t psram, size_t sram)
{ (void)psram; (void)sram; return work ? 1 : -1; }
epd_resource_activity_t epd_activity_work_activity(const epd_activity_work_t *work) { (void)work; return 0; }
bool epd_activity_work_settle(epd_activity_work_t *work) { (void)work; return true; }
bool epd_activity_work_retain(epd_activity_work_t *work) { return work != NULL; }
epd_activity_work_t *epd_activity_work_open(size_t psram, size_t sram)
{ (void)psram; (void)sram; return &disabled; }
bool epd_activity_work_enter(epd_activity_work_t *work, epd_activity_guard_t *guard)
{ (void)work; (void)guard; return true; }
void epd_activity_work_leave(epd_activity_guard_t *guard) { (void)guard; }
bool epd_activity_work_reserve(epd_activity_work_t *work, epd_app_memory_t region, size_t bytes)
{ (void)work; (void)region; (void)bytes; return true; }
bool epd_activity_work_pause(epd_activity_work_t *work) { (void)work; return true; }
bool epd_activity_work_replenish(epd_activity_work_t *work) { return work != NULL; }
bool epd_activity_work_replenish_to(epd_activity_work_t *work, size_t psram, size_t sram)
{ (void)psram; (void)sram; return work != NULL; }
bool epd_activity_work_trim(epd_activity_work_t *work) { return work != NULL; }
bool epd_activity_work_resume(epd_activity_work_t *work) { (void)work; return true; }
void epd_activity_work_close(epd_activity_work_t *work) { (void)work; }
#endif
RTM_EXPORT(epd_activity_work_open);
RTM_EXPORT(epd_activity_work_queue);
RTM_EXPORT(epd_activity_work_ready);
RTM_EXPORT(epd_activity_work_prepare);
RTM_EXPORT(epd_activity_work_capacity);
RTM_EXPORT(epd_activity_work_demand);
RTM_EXPORT(epd_activity_work_activity);
RTM_EXPORT(epd_activity_work_settle);
RTM_EXPORT(epd_activity_work_retain);
RTM_EXPORT(epd_activity_work_enter);
RTM_EXPORT(epd_activity_work_leave);
RTM_EXPORT(epd_activity_work_reserve);
RTM_EXPORT(epd_activity_work_replenish);
RTM_EXPORT(epd_activity_work_replenish_to);
RTM_EXPORT(epd_activity_work_trim);
RTM_EXPORT(epd_activity_work_pause);
RTM_EXPORT(epd_activity_work_resume);
RTM_EXPORT(epd_activity_work_close);
