/* The real memory core and background state machine use a single simulated
 * thread context. Production callbacks still perform their own scope guards. */
static epd_memory_owner_t host_context;
static unsigned rt_interrupt_get_nest(void) { return 0; }
static unsigned rt_critical_level(void) { return 0; }
static epd_memory_owner_t rt_thread_mem_context_get(rt_thread_t thread)
{ (void)thread; return host_context; }
static rt_err_t rt_thread_mem_context_set(rt_thread_t thread, epd_memory_owner_t owner)
{
    (void)thread;
    if (!epd_memory_owner_retain_trusted(owner)) return -RT_EINVAL;
    epd_memory_owner_t old = host_context;
    host_context = owner;
    epd_memory_owner_release_trusted(old);
    return RT_EOK;
}
#include "memory_core.inc"
#include "../platform/epd_resource_runtime.h"
#include "../platform/epd_cold_runtime.h"
/* Service-policy fixtures have no event subscribers. Subscription delivery is
 * exercised separately with the actual operation and resource runtimes. */
void app_background_events_publish(const epd_app_background_event_t *event) { (void)event; }
void app_background_events_forget(epd_memory_owner_t owner, uint32_t generation)
{ (void)owner; (void)generation; }
#include "background.inc"

#ifndef HOST_REAL_COLD_RUNTIME
static epd_cold_runtime_snapshot_t host_cold;
static epd_memory_result_t host_cold_result = EPD_MEMORY_WAITING;
static unsigned host_cold_calls;
static bool host_cold_edit, host_cold_foreground;
bool epd_cold_runtime_discardable(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{ (void)owner; (void)policy; (void)dirty; return false; }
bool epd_cold_runtime_get(epd_memory_owner_t owner, epd_cold_runtime_snapshot_t *out)
{ (void)owner; CHECK(!interrupt_depth); *out = host_cold; return host_cold.registered; }
epd_memory_result_t epd_cold_runtime_checkpoint(epd_memory_owner_t owner,
                                               uint32_t policy_revision, uint32_t dirty_revision)
{
    CHECK(!interrupt_depth && host_cold.registered);
    ++host_cold_calls;
    if (host_cold_edit) CHECK(app_background_dirty(owner, NULL) == EPD_MEMORY_OK);
    if (host_cold_foreground) CHECK(app_background_foreground(owner, true) == EPD_MEMORY_OK);
    host_cold.captured_policy_revision = policy_revision;
    host_cold.captured_dirty_revision = dirty_revision;
    return host_cold_result;
}
#endif

static int rt_mutex_init(struct rt_mutex *mutex, const char *name, unsigned flags)
{ (void)name; (void)flags; CHECK(global_object_depth); ++global_mutexes;
  mutex->held = false; return RT_EOK; }
static int rt_mutex_take(struct rt_mutex *mutex, int wait)
{ (void)wait; CHECK(!interrupt_depth && !mutex->held); mutex->held = true; return RT_EOK; }
static int rt_mutex_release(struct rt_mutex *mutex)
{ CHECK(!interrupt_depth && mutex->held); mutex->held = false; return RT_EOK; }
static struct {
    epd_memory_owner_t owner;
    uint64_t generation;
    bool running;
} host_runtime[128];
static bool host_runtime_busy, host_runtime_reject;
static unsigned host_runtime_true, host_runtime_false, host_runtime_close;
static epd_memory_owner_t host_ui_dispatch = EPD_MEMORY_OWNER_INVALID;
epd_memory_owner_t epd_resource_runtime_ui_enter(epd_memory_owner_t service)
{
    CHECK(!interrupt_depth);
    epd_memory_owner_t previous = host_ui_dispatch;
    host_ui_dispatch = service;
    return previous;
}
void epd_resource_runtime_ui_leave(epd_memory_owner_t previous)
{ CHECK(!interrupt_depth); host_ui_dispatch = previous; }
static int host_runtime_state(epd_memory_owner_t owner, uint64_t generation, bool running,
                              epd_memory_scope_kind_t kind)
{
    CHECK(!interrupt_depth && generation == (uint64_t)owner);
    if (host_runtime_reject) return EPD_RESOURCE_STOPPED;
    epd_memory_owner_snapshot_t state;
    CHECK(epd_memory_owner_get(owner, &state) && state.kind == kind);
    if (state.closed) return EPD_RESOURCE_STOPPED;
    for (unsigned i = 0; i < 128; ++i) {
        if (!host_runtime[i].owner || host_runtime[i].owner == owner) {
            if (!host_runtime[i].owner) CHECK(epd_memory_owner_retain_trusted(owner));
            host_runtime[i].owner = owner;
            host_runtime[i].generation = generation;
            host_runtime[i].running = running;
            if (running) ++host_runtime_true; else ++host_runtime_false;
            return EPD_RESOURCE_OK;
        }
    }
    return EPD_RESOURCE_CAPACITY;
}
int epd_resource_runtime_service_state(epd_memory_owner_t owner, uint64_t generation, bool running)
{ return host_runtime_state(owner, generation, running, EPD_MEMORY_SCOPE_SERVICE); }
int epd_resource_runtime_task_state(epd_memory_owner_t owner, uint64_t generation, bool running)
{ return host_runtime_state(owner, generation, running, EPD_MEMORY_SCOPE_TASK); }
int epd_resource_runtime_close_owner(epd_memory_owner_t owner)
{
    CHECK(!interrupt_depth);
    ++host_runtime_close;
    for (unsigned i = 0; i < 128; ++i)
        if (host_runtime[i].owner == owner) {
            epd_memory_owner_release_trusted(owner);
            memset(&host_runtime[i], 0, sizeof(host_runtime[i]));
        }
    epd_memory_owner_close_trusted(owner);
    return EPD_RESOURCE_OK;
}
bool epd_resource_runtime_owner_busy(epd_memory_owner_t owner)
{ (void)owner; return host_runtime_busy; }

static struct { char id[64]; epd_memory_owner_t owner; } host_modules[32];
static bool host_loader_missing;
static bool host_loader_reclaim_pending;
static bool app_module_loaded(const char *id)
{
    epd_memory_owner_snapshot_t state;
    for (unsigned i = 0; i < 32; ++i)
        if (!strcmp(host_modules[i].id, id)) return epd_memory_owner_get(host_modules[i].owner, &state);
    return false;
}
static epd_memory_result_t app_module_request_stop(const char *id)
{
    if (!app_module_loaded(id)) return EPD_MEMORY_OK;
    for (unsigned i = 0; i < 32; ++i)
        if (!strcmp(host_modules[i].id, id)) return app_background_request_stop(host_modules[i].owner);
    return EPD_MEMORY_INVALID;
}
static bool app_modules_reclaim(const char *foreground)
{ CHECK(foreground); return host_loader_reclaim_pending; }
static epd_memory_owner_t app_module_owner_acquire(const char *id)
{
    if (host_loader_missing) return EPD_MEMORY_OWNER_INVALID;
    for (unsigned i = 0; i < 32; ++i) {
        if (!host_modules[i].id[0]) {
            strcpy(host_modules[i].id, id);
            host_modules[i].owner = epd_memory_owner_create(id);
        }
        if (!strcmp(host_modules[i].id, id)) {
            return epd_memory_owner_retain_trusted(host_modules[i].owner) ? host_modules[i].owner :
                   EPD_MEMORY_OWNER_INVALID;
        }
    }
    return EPD_MEMORY_OWNER_INVALID;
}
