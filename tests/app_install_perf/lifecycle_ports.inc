/* Real owner and background cores; controlled external teardown completions. */
static __declspec(thread) epd_memory_owner_t host_context;
static rt_thread_t rt_thread_self(void) { return &host_context; }
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
static epd_cold_runtime_snapshot_t host_cold;
static epd_memory_result_t host_cold_result = EPD_MEMORY_WAITING;
static epd_memory_owner_t host_cold_owner, host_cleanup_scope;
static bool host_ops_busy, host_resources_busy;
static unsigned host_cold_calls, host_cleanup_calls, host_lv_cancel;
static size_t host_service_pins;
static bool host_quiesce_pending;
static unsigned host_quiesce_calls;
static void (*host_quiesce_hook)(void);
void app_service_background_changed(epd_memory_owner_t owner) { (void)owner; }
epd_memory_result_t app_service_quiesce(const char *id)
{
    (void)id; ++host_quiesce_calls;
    if (host_quiesce_hook) host_quiesce_hook();
    return host_service_pins || host_quiesce_pending ? EPD_MEMORY_WAITING : EPD_MEMORY_OK;
}
size_t app_service_module_pins(const char *id) { (void)id; return host_service_pins; }
bool epd_cold_runtime_get(epd_memory_owner_t owner, epd_cold_runtime_snapshot_t *out)
{
    if (host_cold_owner != owner || !host_cold.registered) return false;
    *out = host_cold; return true;
}
epd_memory_result_t epd_cold_runtime_checkpoint(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{
    assert(owner == host_cold_owner && host_cold.registered && !irq_depth);
    ++host_cold_calls;
    host_cold.captured_policy_revision = policy;
    host_cold.captured_dirty_revision = dirty;
    return host_cold_result;
}
/* The simulated checkpoint does not model optional failed-save discard. */
bool epd_cold_runtime_discardable(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{ (void)owner; (void)policy; (void)dirty; return false; }
void epd_cold_runtime_close_owner(epd_memory_owner_t owner)
{ if (owner == host_cold_owner && host_cold.registered) host_cold.closing = true; }
bool epd_cold_runtime_owner_busy(epd_memory_owner_t owner)
{ return owner == host_cold_owner && host_cold.registered; }
void epd_operation_runtime_close_owner(epd_memory_owner_t owner)
{ assert(!epd_cold_runtime_owner_busy(owner)); epd_memory_owner_close(owner); }
bool epd_operation_runtime_owner_drained(epd_memory_owner_t owner)
{ (void)owner; return !host_ops_busy; }
#ifndef APP_ACTIVITY_HOST
int epd_resource_runtime_close_owner(epd_memory_owner_t owner)
{ epd_memory_owner_close(owner); return EPD_RESOURCE_OK; }
bool epd_resource_runtime_owner_busy(epd_memory_owner_t owner)
{ (void)owner; return host_resources_busy; }
int epd_resource_runtime_task_state(epd_memory_owner_t owner, uint64_t generation, bool active)
{ (void)owner; (void)generation; (void)active; return EPD_RESOURCE_OK; }
#endif
void lv_execution_context_cancel(uintptr_t owner) { (void)owner; ++host_lv_cancel; }
static epd_memory_owner_t host_input_pin;
static unsigned host_input_cleanups;
void epd_input_cleanup_closed(void)
{
    epd_memory_owner_snapshot_t state;
    if (host_input_pin && epd_memory_owner_get(host_input_pin, &state) && state.closed) {
        ++host_input_cleanups;
        epd_memory_owner_release_trusted(host_input_pin);
        host_input_pin = 0;
    }
}
void app_memory_context_process(void) {}
/* This fixture has no background event subscribers. */
void app_background_events_publish(const epd_app_background_event_t *event) { (void)event; }
void app_background_events_forget(epd_memory_owner_t owner, uint32_t generation)
{ (void)owner; (void)generation; }
#include "background.inc"
