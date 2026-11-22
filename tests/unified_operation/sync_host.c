/* Production operation/runtime, with host ports and observation-only counters. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
static CRITICAL_SECTION sync_irq;
static unsigned irq_entries, closed_queries, match_checks, task_publications;
static void before_system_enter(void);
#include "sync_support.inc"
#pragma warning(push)
/* Including separate production units exposes fixture-global shadowing. */
#pragma warning(disable: 4459)
#include "sync_operation.inc"
#define run runtime_worker_entry
#include "sync_runtime.inc"
#undef run
#pragma warning(pop)

static bool dwt_ready, dwt_fail;
static unsigned dwt_inits, cycle_reads;
static uint32_t cycle_value, clock_hz = 240000000;
static HANDLE snapshot_ready, close_done;
static bool pause_before_publish;
void __asan_poison_memory_region(void const volatile *address, size_t size);
void __asan_unpoison_memory_region(void const volatile *address, size_t size);

uint32_t HAL_DBG_DWT_GetCycles(void)
{ CHECK(irq_depth); ++cycle_reads; cycle_value += 100; return cycle_value; }
int HAL_DBG_DWT_IsInit(void) { return dwt_ready; }
void HAL_DBG_DWT_Init(void) { CHECK(!irq_depth); ++dwt_inits; dwt_ready = !dwt_fail; }
uint32_t HAL_RCC_GetHCLKFreq(int core) { CHECK(core == CORE_ID_HCPU); return clock_hz; }
void rt_kprintf(const char *format, ...)
{ CHECK(!irq_depth); va_list args; va_start(args, format); vprintf(format, args); va_end(args); }

static void before_system_enter(void)
{
    if (pause_before_publish && current == &runtime_thread) {
        pause_before_publish = false;
        CHECK(SetEvent(snapshot_ready));
        CHECK(WaitForSingleObject(close_done, 5000) == WAIT_OBJECT_0);
    }
}
static DWORD WINAPI close_after_snapshot(void *unused)
{
    (void)unused;
    current = &threads[3]; context = 1;
    CHECK(WaitForSingleObject(snapshot_ready, 5000) == WAIT_OBJECT_0);
    CHECK(epd_operation_scope_close(business));
    CHECK(SetEvent(close_done));
    return 0;
}
static void worker_sync(void) { current = &runtime_thread; sync_tasks(); current = &threads[0]; }
static unsigned fact_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY * 2; ++i) if (task_facts[i].owner) ++count;
    return count;
}
static epd_operation_t pending_task(void)
{
    CHECK(epd_operation_runtime_init()); kinds[1] = EPD_MEMORY_SCOPE_TASK;
    epd_operation_spec_t spec = setup(); spec.run = task_run;
    epd_operation_t id; CHECK(submit_as_owner(&spec, &id));
    CHECK(pump(1) == 0 && !runs); current = &threads[0];
    return id;
}
static void finish_task(epd_operation_t id)
{
    epd_operation_snapshot_t state; CHECK(epd_operation_get(id, &state));
    if (state.state == EPD_OPERATION_QUEUED) CHECK(pump(1) == 1);
    CHECK(epd_operation_complete(id, EPD_OPERATION_OK, 0, NULL));
    drain(); worker_sync();
    CHECK(epd_operation_release(id)); finish_scopes();
    CHECK(!fact_count());
}
static void empty_sync(void)
{
    CHECK(epd_operation_runtime_init()); current = &runtime_thread;
    unsigned before = irq_entries;
    sync_tasks();
    CHECK(irq_entries - before == 2 && !syncing_tasks && !fact_count() && !cycle_reads);
    current = &threads[0];
    task_yield_and_return();
    CHECK(!fact_count());
}
static void failure_then_resubmit(void)
{
    epd_operation_t id = pending_task(); task_hook_error = EPD_RESOURCE_CAPACITY;
    worker_sync(); worker_sync();
    CHECK(task_publications == 1 && !task_starts && fact_count() == 1);
    drain(); worker_sync();
    epd_operation_snapshot_t state; CHECK(epd_operation_get(id, &state));
    CHECK(!runs && state.status == EPD_OPERATION_FAILED && state.error == EPD_RESOURCE_CAPACITY);
    CHECK(!fact_count() && epd_operation_release(id)); finish_scopes();
    task_hook_error = 0; id = pending_task(); worker_sync();
    CHECK(task_publications == 2 && task_starts == 1 && task_running[1]);
    CHECK(pump(1) == 1 && runs == 1);
    finish_task(id); CHECK(task_ends == 1);
}
static void context_retry(void)
{
    epd_operation_t id = pending_task(); fail_enter = true;
    worker_sync(); CHECK(!task_publications && fact_count() == 1 && !syncing_tasks);
    fail_enter = false; worker_sync(); CHECK(task_starts == 1);
    finish_task(id); CHECK(task_ends == 1);
}
static void pending_cancel(void)
{
    epd_operation_t id = pending_task(); fail_enter = true; worker_sync(); fail_enter = false;
    CHECK(fact_count() == 1 && !task_publications);
    CHECK(epd_operation_cancel(id)); worker_sync();
    CHECK(!fact_count() && !task_publications && !task_starts && !task_ends);
    drain(); CHECK(epd_operation_release(id)); finish_scopes();
}
static void fact_without_task(void)
{
    CHECK(epd_operation_runtime_init()); kinds[1] = EPD_MEMORY_SCOPE_TASK;
    (void)setup();
    task_facts[EPD_OPERATION_CAPACITY * 2 - 1] = (task_fact_t){1, true, false, 1};
    worker_sync(); CHECK(!fact_count() && !task_publications);
    task_facts[EPD_OPERATION_CAPACITY * 2 - 1] = (task_fact_t){1, true, true, 0};
    task_running[1] = true;
    worker_sync(); CHECK(!fact_count() && task_publications == 1 && task_ends == 1);
    finish_scopes();
}
static void matched_prefix(void)
{
    CHECK(epd_operation_runtime_init());
    for (unsigned i = 0; i < 8; ++i) {
        operations[i].id = i + 1; operations[i].owner = i + 1;
        operations[i].managed_task = operations[i].task_intent = true;
        task_facts[i].owner = i + 1;
    }
    fail_enter = true; worker_sync(); fail_enter = false;
    CHECK(match_checks == 36 + 56 * 8);
    for (unsigned i = 0; i < 8; ++i) CHECK(task_facts[i].desired);
    memset(operations, 0, sizeof(operations)); memset(task_facts, 0, sizeof(task_facts));
}
static void concurrent_close(void)
{
    epd_operation_t id = pending_task();
    snapshot_ready = CreateEvent(NULL, TRUE, FALSE, NULL);
    close_done = CreateEvent(NULL, TRUE, FALSE, NULL);
    CHECK(snapshot_ready && close_done); pause_before_publish = true;
    HANDLE closer = CreateThread(NULL, 0, close_after_snapshot, NULL, 0, NULL); CHECK(closer);
    settings_pump(); CHECK(WaitForSingleObject(closer, 5000) == WAIT_OBJECT_0);
    CHECK(!runs && !fact_count() && task_starts == 1 && task_ends == 1);
    drain(); CHECK(!receives && epd_operation_release(id)); finish_scopes();
    CloseHandle(closer); CloseHandle(snapshot_ready); CloseHandle(close_done);
}
static void scope_lookup(bool failed, bool closed, bool result)
{
    epd_operation_spec_t spec = setup(); epd_operation_t id;
    CHECK(submit_as_owner(&spec, &id));
    CHECK(pump(1) == 1);
    void *data = result ? epd_app_alloc(8, EPD_APP_PSRAM) : NULL;
    CHECK(epd_operation_complete(id, failed ? EPD_OPERATION_FAILED : EPD_OPERATION_OK, 0, data));
    if (closed) CHECK(epd_operation_scope_close(business));
    current = &threads[3]; closed_queries = 0;
    CHECK(epd_operation_dispatch(1) == 0);
    CHECK(closed_queries == (failed && result ? 0u : 1u));
    drain();
    if (closed) CHECK(!receives);
    else CHECK(receives == 1 && received_status == (failed ? EPD_OPERATION_FAILED : EPD_OPERATION_OK));
    if (accepted_result) { epd_app_free(accepted_result); accepted_result = NULL; }
    CHECK(epd_operation_release(id)); finish_scopes();
}
static int diag(const char *action, const char *samples, const char *ms)
{
    char *args[] = {"op_sync_diag", (char *)action, (char *)samples, (char *)ms};
    return op_sync_diag(ms ? 4 : samples ? 3 : action ? 2 : 1, args);
}
static void diagnostic_samples(void)
{
    CHECK(epd_operation_runtime_init()); worker_sync(); CHECK(!cycle_reads);
    dwt_ready = true;
    CHECK(diag("on", "2", "1000") == 0 && !dwt_inits);
    worker_sync(); worker_sync(); worker_sync();
    CHECK(!sync_diagnostic.active && sync_diagnostic.exhausted && !sync_diagnostic.remaining);
    CHECK(sync_diagnostic.path[0].samples == 2 && sync_diagnostic.path[0].max_cycles == 100);
    CHECK(!sync_diagnostic.path[1].samples && cycle_reads == 4 && dwt_ready);
    CHECK(diag("off", NULL, NULL) == 0); worker_sync(); CHECK(cycle_reads == 4);
}
static void diagnostic_wrap(void)
{
    CHECK(epd_operation_runtime_init()); ticks = UINT32_MAX - 1;
    CHECK(diag("on", "10", "30") == 0 && dwt_inits == 1);
    cycle_value = UINT32_MAX - 150; worker_sync();
    CHECK(sync_diagnostic.path[0].max_cycles == 100);
    ticks += (30u * RT_TICK_PER_SECOND + 999u) / 1000u;
    CHECK(diag("show", NULL, NULL) == 0);
    CHECK(!sync_diagnostic.active && sync_diagnostic.timed_out && cycle_reads == 2);
    worker_sync(); CHECK(cycle_reads == 2 && dwt_ready);
}
static void diagnostic_guards(void)
{
    epd_operation_t id = pending_task(); CHECK(diag("on", "1000", "1000") == 0);
    worker_sync(); CHECK(sync_diagnostic.path[0].samples == 1 && sync_diagnostic.path[1].samples > 0);
    unsigned before = sync_diagnostic.path[0].samples;
    rt_base_t outer = rt_hw_interrupt_disable();
    current = &runtime_thread; syncing_tasks = true; sync_tasks(); syncing_tasks = false;
    CHECK(sync_diagnostic.path[0].samples == before + 1 && sync_diagnostic.path[0].entered_irq_off == 1);
    CHECK(diag("on", "10", "1000") == -RT_EBUSY);
    rt_hw_interrupt_enable(outer); current = &threads[0];
    CHECK(diag("off", NULL, NULL) == 0); finish_task(id);
}
static void diagnostic_invalid(void)
{
    CHECK(diag("on", "0", "100") == -RT_EINVAL);
    CHECK(diag("on", "1000001", "100") == -RT_EINVAL);
    CHECK(diag("on", "1", "60001") == -RT_EINVAL);
    CHECK(diag("on", "4294967296", "1") == -RT_EINVAL);
    CHECK(diag("on", "-1", "1") == -RT_EINVAL);
    CHECK(diag("on", "1x", "1") == -RT_EINVAL);
    CHECK(!cycle_reads && !dwt_inits);
    dwt_fail = true; CHECK(diag("on", "1", "1") == -RT_ERROR);
    CHECK(!sync_diagnostic.active); dwt_fail = false;
    CHECK(diag("on", "1", "1") == 0); CHECK(diag("off", NULL, NULL) == 0);
    CHECK(dwt_ready && !cycle_reads);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); InitializeCriticalSection(&sync_irq);
    if (!strcmp(argv[1], "diag_before_metadata")) {
        __asan_poison_memory_region(&sync_diagnostic, sizeof(sync_diagnostic));
        worker_sync(); CHECK(!cycle_reads && !sync_diagnostic_state);
        CHECK(diag("show", NULL, NULL) == -RT_EBUSY);
        CHECK(diag("on", "1", "100") == -RT_EBUSY);
        __asan_unpoison_memory_region(&sync_diagnostic, sizeof(sync_diagnostic));
    }
    budget_reset(); CHECK(!test_init_operation_metadata_init() && !test_init_cold_metadata_init() &&
        !test_init_operation_runtime_metadata_init() && !test_init_checkpoint_metadata_init() &&
        !test_init_background_metadata_init());
    const char *name = argv[1];
    if (!strcmp(name, "empty")) empty_sync();
    else if (!strcmp(name, "failure_then_resubmit")) failure_then_resubmit();
    else if (!strcmp(name, "context_retry")) context_retry();
    else if (!strcmp(name, "pending_cancel")) pending_cancel();
    else if (!strcmp(name, "fact_without_task")) fact_without_task();
    else if (!strcmp(name, "matched_prefix")) matched_prefix();
    else if (!strcmp(name, "concurrent_close")) concurrent_close();
    else if (!strcmp(name, "post_dispatch")) task_yield_and_return();
    else if (!strcmp(name, "terminal_failure")) task_start_failure();
    else if (!strncmp(name, "scope_", 6)) scope_lookup(strstr(name, "failed") != NULL,
        strstr(name, "closed") != NULL, strstr(name, "result") != NULL);
    else if (!strcmp(name, "diag_samples")) diagnostic_samples();
    else if (!strcmp(name, "diag_wrap")) diagnostic_wrap();
    else if (!strcmp(name, "diag_guards")) diagnostic_guards();
    else if (!strcmp(name, "diag_invalid")) diagnostic_invalid();
    else if (!strcmp(name, "diag_before_metadata")) CHECK(sync_diagnostic_state == 1 && !cycle_reads);
    else CHECK(false);
    CHECK(!irq_depth && !critical_depth && !context && !allocations && !global_scope && !syncing_tasks);
    for (unsigned i = 1; i < 16; ++i) CHECK(!refs[i]);
    CHECK(!fact_count() && !sleep_holds); DeleteCriticalSection(&sync_irq);
    printf("PASS %s tick=%u\n", name, RT_TICK_PER_SECOND); return 0;
}
