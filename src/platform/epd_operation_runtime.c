/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_operation_runtime.h"
#include "app_service_internal.h"
#include "epd_operation_runtime_internal.h"
#include "epd_resource_runtime.h"
#include "epd_cold_state.h"
#include "epd_cold_runtime.h"
#include "epd_memory.h"
#include "epd_shared_service.h"
#include "platform/epd_power.h"
#include <rthw.h>
#include <rtm.h>
#include <limits.h>
#include <mem_section.h>
#include <string.h>

#define RUNTIME_WAKE 1u
#define RUNTIME_OPERATION_BATCH 8u
#define RUNTIME_STACK_BYTES 12288u
static struct rt_event wake_event;
static rt_thread_t worker_thread, gui_thread;
static bool starting, initialized;
typedef struct {
    epd_memory_owner_t owner;
    bool desired, published;
    int result;
} task_fact_t;
/* One retiring and one current set, each bounded by accepted operation slots. */
L2_RET_BSS_SECT_BEGIN(epd_operation_runtime)
static epd_operation_runtime_client_t clients[EPD_OPERATION_RUNTIME_CLIENTS] L2_RET_BSS_SECT(epd_operation_runtime);
static task_fact_t task_facts[EPD_OPERATION_CAPACITY * 2u] L2_RET_BSS_SECT(epd_operation_runtime);
L2_RET_BSS_SECT_END
static bool syncing_tasks;

/* The shell probe shares the existing DWT clock, but measures whole sync guards. */
#ifndef EPD_OPERATION_SYNC_DIAGNOSTIC
#ifdef SOC_BF0_HCPU
#define EPD_OPERATION_SYNC_DIAGNOSTIC 1
#else
#define EPD_OPERATION_SYNC_DIAGNOSTIC 0
#endif
#endif
#if EPD_OPERATION_SYNC_DIAGNOSTIC
#include <bf0_hal.h>
#include <finsh.h>
typedef struct {
    bool active, exhausted, timed_out;
    uint32_t remaining, deadline, frequency;
    epd_memory_diagnostic_path_snapshot_t path[2];
} sync_diagnostic_t;
L2_RET_BSS_SECT_BEGIN(epd_operation_runtime)
static sync_diagnostic_t sync_diagnostic L2_RET_BSS_SECT(epd_operation_runtime);
L2_RET_BSS_SECT_END
/* 0: PSRAM metadata unavailable; 1: ready/off; 2: sampling. */
static uint8_t sync_diagnostic_state;

static void sync_diagnostic_expire(void)
{
    if (sync_diagnostic_state == 2 && (int32_t)((uint32_t)rt_tick_get() - sync_diagnostic.deadline) >= 0) {
        sync_diagnostic_state = 1;
        sync_diagnostic.active = false;
        sync_diagnostic.timed_out = true;
    }
}
__STATIC_FORCEINLINE rt_base_t sync_lock(uint32_t *started)
{
    rt_base_t level = rt_hw_interrupt_disable();
    *started = 0;
    if (sync_diagnostic_state == 2) {
        *started = HAL_DBG_DWT_GetCycles();
        sync_diagnostic_expire();
    }
    return level;
}
static void sync_diagnostic_record(rt_base_t level, uint32_t started, unsigned path)
{
    epd_memory_diagnostic_path_snapshot_t *p = &sync_diagnostic.path[path];
    ++p->samples;
    if (level) ++p->entered_irq_off;
    if (!--sync_diagnostic.remaining) {
        sync_diagnostic_state = 1;
        sync_diagnostic.active = false;
        sync_diagnostic.exhausted = true;
    }
    uint32_t elapsed = HAL_DBG_DWT_GetCycles() - started;
    if (elapsed > p->max_cycles) p->max_cycles = elapsed;
}
__STATIC_FORCEINLINE void sync_unlock(rt_base_t level, uint32_t started, unsigned path)
{
    if (sync_diagnostic_state == 2) sync_diagnostic_record(level, started, path);
    rt_hw_interrupt_enable(level);
}
static bool sync_diagnostic_number(const char *text, uint32_t *out)
{
    uint32_t value = 0;
    if (!text || !*text) return false;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || value > (UINT32_MAX - (unsigned)(*text - '0')) / 10u)
            return false;
        value = value * 10u + (unsigned)(*text - '0');
    }
    *out = value;
    return true;
}
static int op_sync_diag(int argc, char **argv)
{
    uint32_t samples = 10000, ms = 2000;
    bool start = argc >= 2 && !strcmp(argv[1], "on");
    bool stop = argc == 2 && !strcmp(argv[1], "off");
    if ((start && (argc > 4 || (argc > 2 && !sync_diagnostic_number(argv[2], &samples)) ||
         (argc > 3 && !sync_diagnostic_number(argv[3], &ms)) || !samples || samples > 1000000u ||
         !ms || ms > 60000u)) || (!start && !stop && !(argc == 1 ||
         (argc == 2 && !strcmp(argv[1], "show"))))) {
        rt_kprintf("op_sync_diag [show|off|on [samples<=1000000] [ms<=60000]]\n");
        return -RT_EINVAL;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    if (level || rt_interrupt_get_nest() || rt_critical_level() || !sync_diagnostic_state) {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    if (start || stop) { sync_diagnostic_state = 1; sync_diagnostic.active = false; }
    rt_hw_interrupt_enable(level);
    if (start) {
        if (!HAL_DBG_DWT_IsInit()) HAL_DBG_DWT_Init();
        uint32_t frequency = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
        if (!HAL_DBG_DWT_IsInit() || !frequency) return -RT_ERROR;
        level = rt_hw_interrupt_disable();
        memset(&sync_diagnostic, 0, sizeof(sync_diagnostic));
        sync_diagnostic.remaining = samples;
        sync_diagnostic.frequency = frequency;
        sync_diagnostic.deadline = (uint32_t)rt_tick_get() +
            (uint32_t)(((uint64_t)ms * RT_TICK_PER_SECOND + 999u) / 1000u);
        sync_diagnostic.active = true;
        sync_diagnostic_state = 2;
        rt_hw_interrupt_enable(level);
    }
    level = rt_hw_interrupt_disable();
    sync_diagnostic_expire();
    sync_diagnostic_t snapshot = sync_diagnostic;
    rt_hw_interrupt_enable(level);
    rt_kprintf("op_sync active=%u remaining=%u exhausted=%u timeout=%u start_hz=%u\n",
        snapshot.active, snapshot.remaining, snapshot.exhausted, snapshot.timed_out, snapshot.frequency);
    for (unsigned i = 0; i < 2; ++i)
        rt_kprintf("%s samples=%u max_cycles=%u entered_irq_off=%u\n", i ? "fact" : "snapshot",
            snapshot.path[i].samples, snapshot.path[i].max_cycles, snapshot.path[i].entered_irq_off);
    return RT_EOK;
}
MSH_CMD_EXPORT(op_sync_diag, bounded operation task synchronization timing);
#else
#define sync_lock(started) ((void)(started), rt_hw_interrupt_disable())
#define sync_unlock(level, started, path) rt_hw_interrupt_enable(level)
#endif

static int operation_runtime_metadata_init(void)
{
    memset(clients, 0, sizeof(clients));
    memset(task_facts, 0, sizeof(task_facts));
#if EPD_OPERATION_SYNC_DIAGNOSTIC
    memset(&sync_diagnostic, 0, sizeof(sync_diagnostic));
    sync_diagnostic_state = 1;
#endif
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(operation_runtime_metadata_init);

static void sync_tasks(void)
{
    epd_operation_task_snapshot_t active[EPD_OPERATION_CAPACITY];
    uint32_t started;
    rt_base_t level = sync_lock(&started);
    if (syncing_tasks || !initialized || rt_thread_self() != worker_thread) {
        sync_unlock(level, started, 0); return;
    }
    syncing_tasks = true;
    /* Snapshot and withdrawal share the same guard as operation start intent.
     * A new executor either contributes to this snapshot or waits for publication. */
    size_t count = epd_operation_tasks(active);
    if (!count) {
        bool has_fact = false;
        for (unsigned i = 0; i < EPD_OPERATION_CAPACITY * 2u; ++i)
            if (task_facts[i].owner) { has_fact = true; break; }
        if (!has_fact) {
            syncing_tasks = false;
            sync_unlock(level, started, 0);
            return;
        }
    }
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY * 2u; ++i) {
        task_fact_t *fact = &task_facts[i];
        fact->desired = false;
        for (size_t j = 0; j < count; ++j)
            if (fact->owner && fact->owner == active[j].owner) {
                fact->desired = true;
                break;
            }
        if (!fact->desired) fact->result = 1;
    }
    for (size_t j = 0; j < count; ++j) {
        unsigned at, empty = EPD_OPERATION_CAPACITY * 2u;
        for (at = 0; at < EPD_OPERATION_CAPACITY * 2u; ++at) {
            if (task_facts[at].owner == active[j].owner) break;
            if (!task_facts[at].owner) empty = at;
        }
        if (at == EPD_OPERATION_CAPACITY * 2u && empty < EPD_OPERATION_CAPACITY * 2u)
            task_facts[empty] = (task_fact_t){active[j].owner, true, false, 1};
    }
    sync_unlock(level, started, 0);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    if (previous == EPD_MEMORY_OWNER_INVALID) {
        level = sync_lock(&started); syncing_tasks = false; sync_unlock(level, started, 1);
        return;
    }
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY * 2u; ++i) {
        level = sync_lock(&started);
        task_fact_t fact = task_facts[i];
        sync_unlock(level, started, 1);
        if (!fact.owner) continue;
        if (!fact.desired || (!fact.published && fact.result == 1)) {
            int result = EPD_RESOURCE_OK;
            if (fact.desired || fact.published)
                result = epd_resource_runtime_task_state(fact.owner, fact.owner, fact.desired);
            level = sync_lock(&started);
            if (!fact.desired) task_facts[i] = (task_fact_t){0};
            else {
                task_facts[i].published = result == EPD_RESOURCE_OK;
                task_facts[i].result = result == EPD_RESOURCE_OK ? 0 : result < 0 ? result : EPD_RESOURCE_FAILED;
            }
            sync_unlock(level, started, 1);
            epd_operation_task_wake(fact.owner);
        }
    }
    level = sync_lock(&started);
    syncing_tasks = false;
    sync_unlock(level, started, 1);
    epd_memory_owner_leave_trusted(previous);
}
int epd_operation_runtime_task_prepare(epd_memory_owner_t owner)
{
    sync_tasks();
    rt_base_t level = rt_hw_interrupt_disable();
    int result = initialized ? 1 : EPD_RESOURCE_STOPPED;
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY * 2u; ++i)
        if (task_facts[i].owner == owner) { result = task_facts[i].result; break; }
    rt_hw_interrupt_enable(level);
    if (result == 1) epd_operation_runtime_wake();
    return result;
}

bool epd_operation_runtime_register(const epd_operation_runtime_client_t *client)
{
    if (!client || !client->key || !client->process) return false;
    bool ok = false;
    rt_base_t level = rt_hw_interrupt_disable();
    unsigned empty = EPD_OPERATION_RUNTIME_CLIENTS;
    for (unsigned i = 0; i < EPD_OPERATION_RUNTIME_CLIENTS; ++i) {
        if (!clients[i].key) { if (empty == EPD_OPERATION_RUNTIME_CLIENTS) empty = i; }
        else if (clients[i].key == client->key) {
            ok = clients[i].process == client->process && clients[i].next_timeout_ticks == client->next_timeout_ticks &&
                 clients[i].user == client->user;
            empty = EPD_OPERATION_RUNTIME_CLIENTS;
            goto done;
        }
    }
    if (empty < EPD_OPERATION_RUNTIME_CLIENTS) { clients[empty] = *client; ok = true; }
done:
    rt_hw_interrupt_enable(level);
    if (ok) epd_operation_runtime_wake();
    return ok;
}
static epd_operation_runtime_client_t client_at(unsigned index)
{
    rt_base_t level = rt_hw_interrupt_disable();
    epd_operation_runtime_client_t client = clients[index];
    rt_hw_interrupt_enable(level);
    return client;
}
uint32_t epd_operation_runtime_next_timeout_ticks(uint32_t maximum)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = initialized && rt_thread_self() == worker_thread;
    rt_hw_interrupt_enable(level);
    if (!ready) return maximum;
    uint32_t timeout = epd_operation_next_timeout(maximum);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    if (previous == EPD_MEMORY_OWNER_INVALID) return timeout;
    for (unsigned i = 0; i < EPD_OPERATION_RUNTIME_CLIENTS; ++i) {
        epd_operation_runtime_client_t client = client_at(i);
        if (client.key && client.next_timeout_ticks) {
            uint32_t next = client.next_timeout_ticks(timeout, client.user);
            if (next < timeout) timeout = next;
        }
    }
    epd_memory_owner_leave_trusted(previous);
    return timeout;
}

void epd_operation_runtime_close_owner(epd_memory_owner_t owner)
{
    epd_operation_close_owner(owner);
    epd_operation_runtime_wake();
}
bool epd_operation_runtime_owner_drained(epd_memory_owner_t owner)
{
    return epd_operation_owner_drained(owner);
}

void epd_operation_runtime_wake(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = initialized;
    rt_hw_interrupt_enable(level);
    if (ready) rt_event_send(&wake_event, RUNTIME_WAKE);
}
bool epd_operation_runtime_executors(epd_operation_executor_t *execute,
                                     epd_operation_executor_t *deliver,
                                     epd_operation_executor_t *cleanup)
{
    if (!execute || !deliver || !cleanup) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = initialized;
    if (ready) {
        *execute = (epd_operation_executor_t){worker_thread, &wake_event, RUNTIME_WAKE};
        *cleanup = *execute;
        *deliver = (epd_operation_executor_t){gui_thread, NULL, 0};
    }
    rt_hw_interrupt_enable(level);
    return ready;
}
size_t epd_operation_runtime_ui_pump(size_t budget)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = initialized && rt_thread_self() == gui_thread;
    rt_hw_interrupt_enable(level);
    if (!ready) return 0;
    size_t count = epd_operation_dispatch(budget);
    if (count) app_service_ui_signal(EPD_SERVICE_UI_OPERATION);
    count += epd_cold_runtime_ui_pump(budget);
    /* Events coalesce. A full pass must not leave ready callbacks asleep. */
    if (budget && count >= budget) epd_power_notify();
    return count;
}
size_t epd_operation_runtime_worker_pump(size_t operation_budget, size_t cold_budget)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = initialized && rt_thread_self() == worker_thread;
    rt_hw_interrupt_enable(level);
    if (!ready) return 0;
    sync_tasks();
    size_t count = epd_operation_dispatch(operation_budget);
    count += epd_cold_runtime_worker_pump(cold_budget);
    size_t cold_count = epd_cold_state_dispatch(cold_budget);
    count += cold_count;
    count += epd_cold_runtime_worker_pump(cold_budget);
    sync_tasks();
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    if (previous != EPD_MEMORY_OWNER_INVALID) {
        for (unsigned i = 0; i < EPD_OPERATION_RUNTIME_CLIENTS; ++i) {
            epd_operation_runtime_client_t client = client_at(i);
            if (client.key && operation_budget) {
                size_t processed = client.process(operation_budget, client.user);
                if (processed > SIZE_MAX - count) count = SIZE_MAX;
                else count += processed;
            }
        }
        epd_memory_owner_leave_trusted(previous);
    }
    return count;
}
static void run(void *unused)
{
    (void)unused;
    for (;;) {
        size_t actions = epd_operation_runtime_worker_pump(RUNTIME_OPERATION_BATCH, 1);
        if (actions >= RUNTIME_OPERATION_BATCH) { rt_thread_yield(); continue; }
        uint32_t timeout = epd_operation_runtime_next_timeout_ticks(UINT32_MAX);
        rt_int32_t wait = timeout == UINT32_MAX ? RT_WAITING_FOREVER :
                          timeout > INT32_MAX ? INT32_MAX : (rt_int32_t)timeout;
        rt_uint32_t bits;
        rt_event_recv(&wake_event, RUNTIME_WAKE, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, wait, &bits);
    }
}
bool epd_operation_runtime_init(void)
{
    rt_thread_t gui = rt_thread_self();
    if (!gui) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    if (initialized || starting) {
        bool ok = initialized && gui_thread == gui;
        rt_hw_interrupt_enable(level);
        return ok;
    }
    starting = true;
    rt_hw_interrupt_enable(level);
    epd_shared_service_guard_t guard = {0};
    if (!epd_shared_service_enter(&guard)) {
        level = rt_hw_interrupt_disable(); starting = false; rt_hw_interrupt_enable(level);
        return false;
    }
    bool event_ready = rt_event_init(&wake_event, "opwake", RT_IPC_FLAG_FIFO) == RT_EOK;
    rt_thread_t thread = event_ready ? epd_thread_create_psram("epd_ops", run, NULL,
                                                    RUNTIME_STACK_BYTES, 23, 10) : NULL;
    /* Scheduling cannot enter run before publication. No user callback or I/O
     * occurs in this bounded kernel startup section. */
    rt_enter_critical();
    bool ok = thread && rt_thread_startup(thread) == RT_EOK;
    if (ok) {
        epd_operation_executor_t executor = {thread, &wake_event, RUNTIME_WAKE};
        ok = epd_cold_state_init(&executor);
    }
    level = rt_hw_interrupt_disable();
    if (ok) { worker_thread = thread; gui_thread = gui; initialized = true; }
    starting = false;
    rt_hw_interrupt_enable(level);
    if (!ok) {
        if (thread) rt_thread_delete(thread);
        if (event_ready) rt_event_detach(&wake_event);
    }
    rt_exit_critical();
    (void)epd_shared_service_leave(&guard);
    if (ok) epd_operation_runtime_wake();
    return ok;
}
RTM_EXPORT(epd_operation_runtime_executors);
