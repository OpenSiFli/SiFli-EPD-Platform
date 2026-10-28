#include "app_service_internal.h"
#include "platform/epd_system_internal.h"
#include "app_service.h"
#include "system_reset_guard.h"
#include "net/network.h"
#include <dlmodule.h>
#include <dlfcn.h>
#include <rtm.h>
#include <rthw.h>
#include <string.h>
#include "platform/app_module.h"
#include "platform/epd_system.h"
#include "storage_file.h"
#include "sleep_guard.h"
#include "portal/portal.h"
#include "platform/epd_power.h"
#include "platform/epd_memory.h"
#include "speech/speech_internal.h"
#include "ui_settings_internal.h"

struct epd_service
{
    struct epd_service *next;
    char id[EPD_SERVICE_ID_SIZE];
    char app_id[EPD_SERVICE_ID_SIZE], parent_id[EPD_SERVICE_ID_SIZE];
    epd_background_t definition;
    struct rt_thread worker;
    void *worker_stack;
    struct rt_event wakeup;
    uint32_t subscription;
    void *module;
    storage_volume_t volume;
    uint32_t storage_session;
    bool stopping, worker_done, ui_done;
    bool ui_requested;
    bool ui_ran_this_iteration;
    uint32_t ui_deadline;
    unsigned callers;
    sleep_guard_owner_t sleep_owner;
    char summary[96];
};
static epd_service_t *services;
/* The UI owner advances this cursor before callbacks and repairs it on removal. */
static epd_service_t *ui_cursor;
static unsigned ui_remaining;
static bool ui_processing;
/* Check after each callback: a single step may exceed this cooperative budget. */
#define SERVICE_UI_BUDGET_MS 5u
static uint32_t summary_revision;

bool epd_service_sleep_acquire(epd_service_t *service)
{
    bool accepted = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            accepted = !epd_service_cancelled(item) && sleep_guard_acquire(&item->sleep_owner);
            break;
        }
    rt_hw_interrupt_enable(level);
    return accepted;
}
RTM_EXPORT(epd_service_sleep_acquire);

void epd_service_sleep_release(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            /* Stop owns the remaining lifetime, including UI resource cleanup. */
            if (!item->stopping) sleep_guard_release(&item->sleep_owner);
            break;
        }
    rt_hw_interrupt_enable(level);
}
RTM_EXPORT(epd_service_sleep_release);

/* UI thread owns list changes; shell lookups hold an interrupt guard. */
static void service_add(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    service->next = services;
    services = service;
    ++summary_revision;
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

static void service_remove(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    epd_service_t **link = &services;
    while (*link && *link != service) link = &(*link)->next;
    if (*link)
    {
        *link = service->next;
        if (ui_cursor == service) ui_cursor = service->next ? service->next : services;
        if (!services) ui_remaining = 0;
        ++summary_revision;
    }
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

void epd_service_publish_summary(epd_service_t *service, const char *text)
{
    if (!service) return;
    char snapshot[sizeof(service->summary)];
    rt_strncpy(snapshot, text ? text : "", sizeof(snapshot) - 1);
    snapshot[sizeof(snapshot) - 1] = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    bool changed = !service->stopping && strcmp(service->summary, snapshot);
    if (changed)
    {
        strcpy(service->summary, snapshot);
        ++summary_revision;
    }
    rt_hw_interrupt_enable(level);
    if (changed) epd_power_notify();
}
RTM_EXPORT(epd_service_publish_summary);

uint32_t epd_app_summary_revision(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t revision = summary_revision;
    rt_hw_interrupt_enable(level);
    return revision;
}
RTM_EXPORT(epd_app_summary_revision);

bool epd_app_summary_read(const char *id, char *text, size_t capacity)
{
    if (!text || !capacity) return false;
    text[0] = 0;
    if (!id) return false;
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *service = services; service; service = service->next)
        if (!service->stopping && !strcmp(service->id, id))
        {
            size_t length = strlen(service->summary);
            if (length >= capacity) length = capacity - 1;
            memcpy(text, service->summary, length);
            text[length] = 0;
            found = true;
            break;
        }
    rt_hw_interrupt_enable(level);
    return found;
}
RTM_EXPORT(epd_app_summary_read);

bool epd_service_cancelled(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool stopping = service->stopping;
    rt_hw_interrupt_enable(level);
    return stopping || (service->module && !storage_session_valid(service->volume, service->storage_session));
}
RTM_EXPORT(epd_service_cancelled);

uint32_t epd_service_wait(epd_service_t *service, rt_int32_t ticks)
{
    uint32_t events = 0;
    if (epd_service_cancelled(service)) return EPD_SERVICE_STOP;
    rt_event_recv(&service->wakeup, EPD_SERVICE_WAKE | EPD_SERVICE_NETWORK | EPD_SERVICE_STOP,
                   RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, ticks, &events);
    return events;
}
RTM_EXPORT(epd_service_wait);

void epd_service_wake(epd_service_t *service)
{
    if (service) rt_event_send(&service->wakeup, EPD_SERVICE_WAKE);
    epd_power_notify();
}
RTM_EXPORT(epd_service_wake);

void epd_service_ui_request(epd_service_t *service, uint32_t delay_ms)
{
    if (!service) return;
    uint64_t ticks = ((uint64_t)delay_ms * RT_TICK_PER_SECOND + 999u) / 1000u;
    if (ticks > INT32_MAX) ticks = INT32_MAX;
    uint32_t now = (uint32_t)rt_tick_get();
    uint32_t deadline = now + (uint32_t)ticks;
    rt_base_t level = rt_hw_interrupt_disable();
    bool changed = false;
    epd_service_t *item = services;
    while (item && item != service) item = item->next;
    if (item && !service->ui_done &&
        (!service->ui_requested ||
         ((int32_t)(service->ui_deadline - now) > 0 &&
          (uint32_t)ticks < service->ui_deadline - now)))
    {
        service->ui_deadline = deadline;
        service->ui_requested = true;
        changed = true;
    }
    rt_hw_interrupt_enable(level);
    if (changed) epd_power_notify();
}
RTM_EXPORT(epd_service_ui_request);

uint32_t app_service_ui_delay(uint32_t default_ms)
{
    /* Finish visiting an interrupted batch before the main loop goes to sleep. */
    if (ui_remaining) return 0;
    uint32_t now = (uint32_t)rt_tick_get();
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *service = services; service; service = service->next)
    {
        if (service->stopping && service->worker_done && !service->callers &&
            !service->ui_requested)
        {
            default_ms = 0;
            break;
        }
        if (!service->ui_requested || service->ui_done ||
            (service->stopping && (!service->worker_done || service->callers))) continue;
        int32_t ticks = (int32_t)(service->ui_deadline - now);
        uint64_t ms = ticks > 0 ? ((uint64_t)ticks * 1000u + RT_TICK_PER_SECOND - 1u) /
                                  RT_TICK_PER_SECOND : 0;
        if (ms < default_ms) default_ms = (uint32_t)ms;
    }
    rt_hw_interrupt_enable(level);
    return default_ms;
}

int app_service_command(const char *id, int argc, char **argv)
{
    if (system_reset_blocks_apps() || storage_changing()) return -RT_EBUSY;
    epd_service_t *service = NULL;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (!epd_service_cancelled(item) &&
            item->definition.command && !strcmp(item->id, id))
        {
            service = item;
            ++service->callers;
            break;
        }
    rt_hw_interrupt_enable(level);
    if (!service) return -RT_ENOSYS;
    int result = service->definition.command(argc, argv);
    level = rt_hw_interrupt_disable();
    --service->callers;
    /* Send while protected: the worker may publish done immediately afterward. */
    rt_event_send(&service->wakeup, EPD_SERVICE_WAKE);
    rt_hw_interrupt_enable(level);
    epd_power_notify();
    return result;
}

static void run(void *parameter)
{
    epd_service_t *service = parameter;
    service->definition.run(service);
    rt_base_t guard = rt_hw_interrupt_disable();
    if (!service->stopping) ++summary_revision;
    service->stopping = true;
    service->ui_requested = false;
    rt_hw_interrupt_enable(guard);
    speech_owner_stop(service);
    epd_system_service_stopped(service);
    for (;;)
    {
        guard = rt_hw_interrupt_disable();
        bool idle = service->callers == 0;
        rt_hw_interrupt_enable(guard);
        if (idle) break;
        uint32_t events;
        rt_event_recv(&service->wakeup, EPD_SERVICE_WAKE, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &events);
    }
    network_unsubscribe(service->subscription);
    service->subscription = 0;
}

static void worker_cleanup(rt_thread_t thread)
{
    epd_service_t *service = thread->parameter;
    /* SDK idle invokes cleanup after the worker has stopped, with scheduling
     * locked until the static thread object is detached. The UI owns frees. */
    rt_base_t level = rt_hw_interrupt_disable();
    service->worker_done = true;
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

bool app_service_running(const char *id)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (!strcmp(service->id, id)) return true;
    return false;
}

bool epd_service_identity(epd_service_t *service, char *id, size_t capacity)
{
    if (!service || !id || !capacity) return false;
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
    {
        if (item != service) continue;
        size_t length = strlen(item->app_id);
        if (!epd_service_cancelled(item) && length < capacity)
        {
            memcpy(id, item->app_id, length + 1);
            found = true;
        }
        break;
    }
    rt_hw_interrupt_enable(level);
    return found;
}

bool epd_service_is_worker(epd_service_t *service)
{
    if (!service) return false;
    bool worker = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
    {
        if (item != service) continue;
        worker = item->definition.run && !item->worker_done &&
                 rt_thread_self() == &item->worker;
        break;
    }
    rt_hw_interrupt_enable(level);
    return worker;
}
RTM_EXPORT(epd_service_is_worker);

bool epd_service_retain(epd_service_t *service)
{
    bool retained = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            if (!epd_service_cancelled(item) && item->callers != UINT32_MAX)
            {
                ++item->callers;
                retained = true;
            }
            break;
        }
    rt_hw_interrupt_enable(level);
    return retained;
}

void epd_service_release(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            RT_ASSERT(item->callers != 0);
            --item->callers;
            rt_event_send(&item->wakeup, EPD_SERVICE_WAKE);
            break;
        }
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

static void start_failed(const char *id, const epd_background_t *definition,
                         const char *stage, int error)
{
    rt_uint32_t total = 0, used = 0;
    rt_memory_info(&total, &used, NULL);
    /* Keep each line within the 128-byte console buffer. */
    rt_kprintf("svc %.*s: start failed stage=%s rc=%d\n",
               (int)EPD_SERVICE_ID_SIZE - 1, id ? id : "(null)", stage, error);
    rt_kprintf("svc mem: obj=%lu stack=%lu heap_free=%lu psram_free=%lu\n",
               (unsigned long)sizeof(epd_service_t),
               (unsigned long)(definition && definition->run ? definition->stack_size : 0),
               (unsigned long)(total >= used ? total - used : 0),
               (unsigned long)epd_app_psram_available());
}

static epd_service_t *service_start(const char *id, const epd_background_t *definition,
                                   void *module, const char *app_id, const char *parent_id)
{
    const char *stage = "reset";
    int error = -RT_EBUSY;
    if (system_reset_blocks_apps()) goto rejected;
    stage = "definition";
    error = -RT_EINVAL;
    if (!id || !*id || strlen(id) >= EPD_SERVICE_ID_SIZE || !definition ||
        (!definition->run && !definition->ui_process)) goto rejected;
    stage = "identity";
    if (!app_id || !*app_id || strlen(app_id) >= EPD_SERVICE_ID_SIZE ||
        (parent_id && strlen(parent_id) >= EPD_SERVICE_ID_SIZE) ||
        (module && strlen(app_id) >= RT_NAME_MAX)) goto rejected;
    stage = "already_running";
    error = -RT_EBUSY;
    if (app_service_running(id)) goto rejected;
    stage = "worker_config";
    error = -RT_EINVAL;
    if (definition->run && (definition->stack_size < 2048 || definition->stack_size > 32768 ||
        definition->priority < 20 || definition->priority >= RT_THREAD_PRIORITY_MAX)) goto rejected;
    stage = "network_config";
    if (!definition->run && definition->network) goto rejected;
    stage = "service_alloc";
    error = -RT_ENOMEM;
    epd_service_t *service = rt_calloc(1, sizeof(*service));
    if (!service) goto rejected;
    strcpy(service->id, id);
    strcpy(service->app_id, app_id);
    if (parent_id) strcpy(service->parent_id, parent_id);
    service->sleep_owner.id = service->id;
    service->definition = *definition;
    service->worker_done = definition->run == NULL;
    service->ui_done = definition->ui_process == NULL;
    stage = "event_init";
    error = rt_event_init(&service->wakeup, "app_evt", RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        start_failed(id, definition, stage, error);
        rt_free(service);
        return NULL;
    }
    error = -RT_ERROR;
    if (definition->network)
    {
        stage = "network_subscribe";
        service->subscription = network_subscribe(&service->wakeup, EPD_SERVICE_NETWORK);
        if (!service->subscription) goto failed;
    }
    if (module)
    {
        stage = "module_storage";
        if (!app_module_storage(app_id, &service->volume, &service->storage_session)) goto failed;
        stage = "storage_session";
        if (!storage_session_valid(service->volume, service->storage_session)) goto failed;
        char path[96], name[RT_NAME_MAX + 4];
        rt_snprintf(name, sizeof(name), "%s.so", app_id);
        stage = "module_path";
        if (!storage_app_path(path, sizeof(path), app_id, STORAGE_APP_CODE, name)) goto failed;
        stage = "module_pin";
        service->module = app_module_open(path, app_id); /* SDK nref pins code past ONSTOP. */
        if (!service->module) goto failed;
    }
    if (!definition->run)
    {
        service_add(service);
        return service;
    }
    stage = "worker_stack";
    error = -RT_ENOMEM;
    service->worker_stack = epd_app_alloc(definition->stack_size, EPD_APP_PSRAM);
    if (!service->worker_stack) goto failed;
    stage = "thread_init";
    rt_thread_t thread = &service->worker;
    error = rt_thread_init(thread, id, run, service, service->worker_stack,
                           definition->stack_size, definition->priority, 10);
    if (error != RT_EOK) goto failed;
    thread->cleanup = worker_cleanup;
    service_add(service);
    stage = "thread_startup";
    error = rt_thread_startup(thread);
    if (error == RT_EOK) return service;
    start_failed(id, definition, stage, error);
    service_remove(service);
    thread->cleanup = NULL;
    rt_thread_detach(thread);
    goto cleanup;
failed:
    start_failed(id, definition, stage, error);
cleanup:
    network_unsubscribe(service->subscription);
    if (service->module) app_module_close(service->module);
    rt_event_detach(&service->wakeup);
    epd_app_free(service->worker_stack);
    rt_free(service);
    return NULL;
rejected:
    start_failed(id, definition, stage, error);
    return NULL;
}

epd_service_t *app_service_start(const char *id, const epd_background_t *definition, void *module)
{
    return service_start(id, definition, module, id, NULL);
}

epd_service_t *app_service_start_auxiliary(epd_service_t *owner, const char *name,
                                          const epd_background_t *definition)
{
    for (epd_service_t *item = services; item; item = item->next)
        if (item == owner && !epd_service_cancelled(item) && !item->parent_id[0])
        {
            char id[EPD_SERVICE_ID_SIZE];
            if (!name || !*name || strlen(item->id) + strlen(name) + 1 >= sizeof(id))
            {
                start_failed(item->id, definition, "auxiliary_name", -RT_EINVAL);
                return NULL;
            }
            rt_snprintf(id, sizeof(id), "%s.%s", item->id, name);
            return service_start(id, definition, item->module, item->app_id, item->id);
        }
    start_failed(name, definition, "auxiliary_owner", -RT_EINVAL);
    return NULL;
}
RTM_EXPORT(app_service_start_auxiliary);

void app_service_stop(const char *id)
{
    for (epd_service_t *service = services; service; service = service->next)
    {
        if (strcmp(service->id, id) && strcmp(service->parent_id, id)) continue;
        rt_base_t level = rt_hw_interrupt_disable();
        if (service->stopping)
        {
            rt_hw_interrupt_enable(level);
            continue;
        }
        ++summary_revision;
        service->stopping = true;
        service->ui_requested = false;
        rt_hw_interrupt_enable(level);
        speech_owner_stop(service);
        epd_system_service_stopped(service);
        rt_event_send(&service->wakeup, EPD_SERVICE_STOP);
        epd_power_notify();
    }
}

/* The caller must not retain service after this step: cleanup may free it. */
static bool service_ui_step(epd_service_t *service, bool maintenance, bool once)
{
    if (!service->stopping && epd_service_cancelled(service)) app_service_stop(service->id);
    rt_base_t level = rt_hw_interrupt_disable();
    bool stopping = service->stopping;
    unsigned callers = service->callers;
    bool worker_done = service->worker_done;
    bool requested = service->ui_requested;
    bool due = requested && (int32_t)(service->ui_deadline - (uint32_t)rt_tick_get()) <= 0;
    bool call_ui = service->definition.ui_process && !service->ui_done &&
                   (!stopping || (worker_done && !callers)) &&
                   (!maintenance || stopping || due) && (!requested || due) &&
                   (!once || !service->ui_ran_this_iteration);
    if (call_ui)
    {
        service->ui_requested = false;
        service->ui_ran_this_iteration = true;
    }
    rt_hw_interrupt_enable(level);
    if (call_ui)
    {
        bool done = service->definition.ui_process(service, stopping);
        RT_ASSERT(!done || stopping);
        if (done)
        {
            level = rt_hw_interrupt_disable();
            service->stopping = service->ui_done = true;
            service->ui_requested = false;
            rt_hw_interrupt_enable(level);
        }
        else if (stopping && !service->ui_requested)
            epd_service_ui_request(service, 10);
    }
    if (worker_done && service->ui_done && !callers)
    {
        service_remove(service);
        rt_event_detach(&service->wakeup);
        if (service->module) app_module_close(service->module);
        sleep_guard_release(&service->sleep_owner);
        epd_app_free(service->worker_stack);
        rt_free(service);
    }
    return call_ui;
}

static void process(bool maintenance, bool once)
{
    if (ui_processing) return;
    ui_processing = true;
    uint32_t started = (uint32_t)rt_tick_get();
    const uint32_t budget = (uint32_t)(((uint64_t)SERVICE_UI_BUDGET_MS * RT_TICK_PER_SECOND + 999u) / 1000u);
    /* Retain the batch length across calls so ordinary polling can sleep once
     * the whole batch has had its turn. New arrivals do not extend the batch. */
    if (!ui_remaining)
        for (epd_service_t *service = services; service; service = service->next) ++ui_remaining;
    if (!ui_cursor) ui_cursor = services;
    while (ui_remaining)
    {
        epd_service_t *service = ui_cursor;
        ui_cursor = service->next ? service->next : services;
        --ui_remaining;
        bool call_ui = service_ui_step(service, maintenance, once);
        if (call_ui && (uint32_t)((uint32_t)rt_tick_get() - started) >= budget)
            break;
    }
    ui_processing = false;
}

void app_service_process(void)
{
    process(epd_power_sleeping(), false);
}

void app_service_maintenance_process(void)
{
    process(true, false);
}

void app_service_ui_begin_iteration(void)
{
    if (ui_processing) return;
    for (epd_service_t *service = services; service; service = service->next)
        service->ui_ran_this_iteration = false;
}

void app_service_ui_process_foreground(const char *app_id)
{
    if (ui_processing || !app_id || epd_power_sleeping()) return;
    ui_processing = true;
    for (epd_service_t *service = services; service; service = service->next)
        if (!service->parent_id[0] && !strcmp(service->id, app_id))
        {
            service_ui_step(service, false, true);
            break;
        }
    ui_processing = false;
}

void app_service_ui_process_deferred(void)
{
    process(epd_power_sleeping(), true);
}

void app_service_resume(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *service = services; service; service = service->next)
        if (!service->worker_done && !epd_service_cancelled(service))
            rt_event_send(&service->wakeup, EPD_SERVICE_WAKE);
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

bool app_service_stopping(void)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->stopping) return true;
    return false;
}

bool app_service_flush(void)
{
    /* Local account changes can run while the HTTP service is offline. */
    bool ok = portal_release_storage();
    if (!ui_settings_flush()) ok = false;
    for (epd_service_t *service = services; service; service = service->next)
    {
        if ((!service->worker_done || !service->ui_done) && service->definition.flush &&
            !service->definition.flush()) ok = false;
    }
    return ok;
}

void app_service_stop_modules(void)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->module) app_service_stop(service->id);
}

bool app_service_modules_running(void)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->module) return true;
    return false;
}
