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
#include "platform/epd_resource_runtime.h"
#include "platform/app_catalog.h"
#include "platform/app_memory_pressure.h"

struct epd_service
{
    struct epd_service *next;
    char id[EPD_SERVICE_ID_SIZE];
    char app_id[EPD_SERVICE_ID_SIZE], parent_id[EPD_SERVICE_ID_SIZE];
    epd_background_t definition;
    bool (*release_tf)(epd_service_t *, uint32_t);
    uint32_t tf_release_ack;
    struct rt_thread worker;
    void *worker_stack;
    struct rt_event wakeup;
    uint32_t subscription;
    void *module;
    storage_volume_t volume;
    uint32_t storage_session;
    bool stopping, worker_done, ui_done;
    void (*stop_notify)(void *);
    void *stop_context;
    bool ui_requested;
    bool ui_ran_this_iteration;
    uint32_t ui_deadline, ui_events;
    unsigned callers;
    bool background, reclaimable, reclaim_reserved, dirty;
    bool schedule_revoked;
    bool schedule_hold, schedule_pending;
    unsigned reclaim_holds, cleanup_holds;
    bool flush_active, cleanup_retry;
    uint32_t cleanup_retry_deadline, cleanup_retry_ms;
    uint32_t reclaim_flushed, reclaim_work_revision;
    uint32_t saved_work_revision;
    epd_memory_owner_t memory_owner, worker_owner, cleanup_owner;
    epd_memory_owner_t activity_owner;
    epd_app_task_group_t activity_group;
    unsigned runtime_users;
    bool runtime_bound, runtime_running, runtime_closing;
    sleep_guard_owner_t sleep_owner, cleanup_sleep_owner;
    char summary[96];
};
static epd_service_t *services;
/* The UI owner advances this cursor before callbacks and repairs it on removal. */
static epd_service_t *ui_cursor;
static epd_service_t *reclaim_cursor;
static unsigned ui_remaining;
static bool ui_processing;
/* Check after each callback: a single step may exceed this cooperative budget. */
#define SERVICE_UI_BUDGET_MS 5u
static uint32_t summary_revision;
static struct rt_mutex activity_lock;
static bool activity_lock_ready;
static void service_stop(const char *id, bool preserve_schedule);

static bool service_schedule_hold(epd_service_t *service)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    bool live = !epd_service_cancelled(service) && !service->worker_done;
    bool held = live && service->schedule_hold;
    if (held) service->schedule_pending = true;
    rt_hw_interrupt_enable(guard);
    if (!live || held) return held;
    if (!epd_service_reclaim_hold(service)) return false;
    guard = rt_hw_interrupt_disable();
    live = !epd_service_cancelled(service) && !service->worker_done;
    if (live) {
        service->schedule_hold = service->schedule_pending = true;
    }
    rt_hw_interrupt_enable(guard);
    if (!live) epd_service_reclaim_release(service);
    return live;
}

static void service_schedule_release(epd_service_t *service, bool finished)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    bool release = service->schedule_hold && (finished || !service->schedule_pending);
    if (release) service->schedule_hold = service->schedule_pending = false;
    rt_hw_interrupt_enable(guard);
    if (release) epd_service_reclaim_release(service);
}

bool epd_service_schedule(epd_service_t *service, rt_int32_t delay_ticks, bool require_network)
{
    bool accepted = false;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service && item->module && !item->parent_id[0] && !item->schedule_revoked &&
            !item->worker_done && !item->runtime_closing &&
            (!item->stopping || item->reclaim_reserved)) {
            accepted = app_background_schedule(item->memory_owner, delay_ticks,
                                               require_network, (uint32_t)rt_tick_get());
            break;
        }
    rt_hw_interrupt_enable(guard);
    return accepted;
}
RTM_EXPORT(epd_service_schedule);

static void service_scheduled_process(void)
{
    if (epd_power_sleeping() || app_memory_pressure_background_blocked() ||
        system_reset_blocks_apps() || storage_changing()) return;
    char id[EPD_SERVICE_ID_SIZE];
    uint32_t generation;
    if (!app_background_schedule_due((uint32_t)rt_tick_get(), network_ready(),
                                     id, sizeof(id), &generation)) return;
    epd_service_t *service = NULL;
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->id, id)) { service = item; break; }
    if (service && (service->stopping || service->worker_done)) return;
    if (!app_background_schedule_claim(id, generation, (uint32_t)rt_tick_get())) return;
    if (service) {
        if (!service_schedule_hold(service)) return;
        rt_base_t guard = rt_hw_interrupt_disable();
        if (!service->stopping && !service->worker_done) {
            epd_service_wake(service);
            app_background_schedule_ack(id, generation);
        }
        rt_hw_interrupt_enable(guard);
    }
    else app_catalog_background_resume();
}

/* Called with a live caller/hold or runtime_users pin, outside interrupt guards.
 * Serialize publication and sample the latest counts to avoid a delayed false
 * notification withdrawing eligibility from a newly accepted operation. */
static bool service_runtime_sync(epd_service_t *service)
{
    if (rt_mutex_take(&activity_lock, RT_WAITING_FOREVER) != RT_EOK) return false;
    rt_base_t guard = rt_hw_interrupt_disable();
    bool running = service->callers || service->reclaim_holds || service->cleanup_holds;
    bool publish = running != service->runtime_running || (!service->runtime_bound && running);
    rt_hw_interrupt_enable(guard);
    int result = EPD_RESOURCE_OK;
    if (publish) {
        result = epd_resource_runtime_service_state(service->memory_owner,
                                                 (uint64_t)service->memory_owner, running);
        if (result == EPD_RESOURCE_OK) {
            service->runtime_bound = true;
            service->runtime_running = running;
        }
    }
    rt_mutex_release(&activity_lock);
    return result == EPD_RESOURCE_OK;
}

static void service_runtime_unpin(epd_service_t *service)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    RT_ASSERT(service->runtime_users);
    --service->runtime_users;
    rt_hw_interrupt_enable(guard);
    epd_power_notify();
}

static epd_memory_owner_t service_enter(epd_service_t *service, bool cleanup)
{
    return epd_memory_owner_enter_trusted(cleanup ? service->cleanup_owner : service->memory_owner);
}

static void service_release_owners(epd_service_t *service)
{
    epd_memory_owner_t owners[] = {service->worker_owner, service->cleanup_owner, service->memory_owner};
    for (unsigned i = 0; i < sizeof(owners) / sizeof(owners[0]); ++i)
        if (owners[i] != EPD_MEMORY_OWNER_INVALID) {
            epd_memory_owner_close_trusted(owners[i]);
            epd_memory_owner_release_trusted(owners[i]);
        }
}

static bool service_create_owners(epd_service_t *service, bool module, const char *parent_id)
{
    epd_memory_owner_t root = EPD_MEMORY_OWNER_SYSTEM;
    if (module) root = app_module_owner_acquire(service->app_id);
    else if (parent_id) {
        root = EPD_MEMORY_OWNER_INVALID;
        for (epd_service_t *item = services; item; item = item->next)
            if (!strcmp(item->id, parent_id)) { root = item->memory_owner; break; }
        if (!epd_memory_owner_retain_trusted(root)) root = EPD_MEMORY_OWNER_INVALID;
    } else if (!epd_memory_owner_retain_trusted(root)) root = EPD_MEMORY_OWNER_INVALID;
    if (root == EPD_MEMORY_OWNER_INVALID) return false;
    service->memory_owner = epd_memory_scope_create_trusted(root, EPD_MEMORY_SCOPE_SERVICE);
    epd_memory_owner_release_trusted(root);
    if (service->memory_owner == EPD_MEMORY_OWNER_INVALID) return false;
    service->cleanup_owner = epd_memory_scope_create_trusted(service->memory_owner, EPD_MEMORY_SCOPE_CLEANUP);
    if (service->definition.run)
        service->worker_owner = epd_memory_scope_create_trusted(service->memory_owner, EPD_MEMORY_SCOPE_TASK);
    return service->cleanup_owner != EPD_MEMORY_OWNER_INVALID &&
           (!service->definition.run || service->worker_owner != EPD_MEMORY_OWNER_INVALID);
}

static bool service_background_busy(epd_service_t *service)
{
    epd_app_background_snapshot_t state;
    return app_background_snapshot(service->memory_owner, &state) == EPD_MEMORY_OK &&
           (state.tasks || state.dirty_revision != state.saved_revision);
}

/* A burst of accepted calls/holds is one task; an idle worker is not. The
 * publication guard serializes admission with policy changes and teardown. */
static bool service_activity_begin(epd_service_t *service)
{
    epd_app_background_snapshot_t state;
    if (app_background_snapshot(service->memory_owner, &state) != EPD_MEMORY_OK)
        return true; /* Resident services have no dynamic-app policy. */
    if (service->activity_group) return app_background_group_accepting(service->activity_group);
    epd_app_task_group_t group;
    if (app_background_group_create(service->memory_owner, &group) != EPD_MEMORY_OK) return false;
    epd_memory_owner_t task;
    if (app_background_task_begin(group, &task) != EPD_MEMORY_OK) {
        app_background_group_release(group);
        return false;
    }
    epd_memory_result_t policy = app_background_group_default(group, EPD_APP_BG_RELEASE);
    if (policy != EPD_MEMORY_OK && policy != EPD_MEMORY_BUSY) {
        app_background_task_end(task);
        app_background_group_release(group);
        return false;
    }
    service->activity_group = group;
    service->activity_owner = task;
    return true;
}

static void service_activity_end(epd_service_t *service)
{
    if (!service->callers && !service->reclaim_holds && service->activity_group) {
        app_background_task_end(service->activity_owner);
        app_background_group_release(service->activity_group);
        service->activity_group = EPD_APP_TASK_GROUP_NONE;
        service->activity_owner = EPD_MEMORY_OWNER_INVALID;
    }
}

/* Page exit can preserve active TASKS, while shutdown and quiesce must drain.
 * Persistence is still acknowledged by the service's flush callback. */
static bool service_page_task_retained(epd_service_t *service)
{
    epd_app_background_snapshot_t state;
    return !service->stopping && !service->reclaim_reserved &&
           app_background_snapshot(service->memory_owner, &state) == EPD_MEMORY_OK &&
           !state.stopping && state.effective == EPD_APP_BG_TASKS && state.tasks;
}

/* Callback runs without the publication guard. Newly accepted work invalidates
 * the snapshot even when a flush reports success after starting another write. */
static bool service_flush(epd_service_t *service, bool leaving_page)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    if (service->worker_done && service->ui_done && !service->dirty &&
        !service->callers && !service->reclaim_holds && !service->cleanup_holds) {
        rt_hw_interrupt_enable(guard);
        return true;
    }
    if (service->flush_active || service->cleanup_holds) {
        rt_hw_interrupt_enable(guard);
        return false;
    }
    uint32_t revision = service->reclaim_work_revision;
    service->flush_active = true;
    rt_hw_interrupt_enable(guard);
    epd_memory_owner_t previous = service_enter(service, true);
    if (previous == EPD_MEMORY_OWNER_INVALID) {
        guard = rt_hw_interrupt_disable();
        service->flush_active = false;
        rt_hw_interrupt_enable(guard);
        return false;
    }
    bool saved = service->definition.flush ? service->definition.flush() : !service->dirty;
    epd_memory_owner_leave_trusted(previous);
    guard = rt_hw_interrupt_disable();
    service->flush_active = false;
    saved = saved && revision == service->reclaim_work_revision && !service->cleanup_holds &&
            ((!service->callers && !service->reclaim_holds) ||
             (leaving_page && service_page_task_retained(service)));
    if (saved) {
        service->saved_work_revision = revision;
        service->dirty = false;
        service->cleanup_retry = false;
        service->cleanup_retry_ms = 0;
    }
    rt_hw_interrupt_enable(guard);
    return saved;
}

/* Caller holds the publication guard. Newly accepted work invalidates every
 * completed pressure persistence barrier in the owning application. */
static void reclaim_work_started(epd_service_t *service)
{
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->app_id, service->app_id))
        {
            item->reclaim_flushed = 0;
            ++item->reclaim_work_revision;
        }
}

bool epd_service_mark_dirty(epd_service_t *service)
{
    bool found = false;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service && !item->runtime_closing) {
            reclaim_work_started(item);
            item->dirty = true;
            if (!item->cleanup_holds) item->cleanup_retry = false;
            found = true;
            break;
        }
    rt_hw_interrupt_enable(guard);
    if (found) epd_power_notify();
    return found;
}
RTM_EXPORT(epd_service_mark_dirty);

epd_memory_owner_t epd_service_owner_acquire(epd_service_t *service, bool cleanup)
{
    epd_memory_owner_t owner = EPD_MEMORY_OWNER_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service) {
            epd_memory_owner_t candidate = cleanup ? item->cleanup_owner : item->memory_owner;
            if (!item->runtime_closing && (cleanup || !epd_service_cancelled(item)) &&
                epd_memory_owner_retain_trusted(candidate))
                owner = candidate;
            break;
        }
    rt_hw_interrupt_enable(guard);
    return owner;
}
RTM_EXPORT(epd_service_owner_acquire);

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
    reclaim_work_started(service);
    ++summary_revision;
    rt_hw_interrupt_enable(level);
    app_service_ui_signal(EPD_SERVICE_UI_LIFECYCLE | EPD_SERVICE_UI_SUMMARY);
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
        if (reclaim_cursor == service) reclaim_cursor = service->next ? service->next : services;
        if (!services) ui_remaining = 0;
        ++summary_revision;
    }
    rt_hw_interrupt_enable(level);
    app_service_ui_signal(EPD_SERVICE_UI_LIFECYCLE | EPD_SERVICE_UI_SUMMARY);
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
    if (changed) {
        app_service_ui_signal(EPD_SERVICE_UI_SUMMARY);
        epd_power_notify();
    }
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
    bool stopping = service->stopping || service->reclaim_reserved;
    rt_hw_interrupt_enable(level);
    return stopping || (service->module && !storage_session_valid(service->volume, service->storage_session));
}
RTM_EXPORT(epd_service_cancelled);

bool epd_service_background_cancelled(epd_service_t *service)
{
    bool cancelled = true;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service) {
            epd_app_background_snapshot_t state;
            cancelled = item->stopping || item->reclaim_reserved || item->runtime_closing ||
                (app_background_snapshot(item->memory_owner, &state) == EPD_MEMORY_OK &&
                 (state.stopping || (!state.foreground && state.explicit_policy &&
                                     state.requested.mode != EPD_APP_BG_TASKS)));
            break;
        }
    rt_hw_interrupt_enable(guard);
    return cancelled;
}
RTM_EXPORT(epd_service_background_cancelled);

void app_service_background_changed(epd_memory_owner_t owner)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next) {
        epd_memory_owner_snapshot_t memory;
        if (!item->runtime_closing && epd_memory_owner_get(item->memory_owner, &memory) &&
            memory.account == owner) {
            if (!item->worker_done) rt_event_send(&item->wakeup, EPD_SERVICE_WAKE);
            if (!item->ui_done) {
                item->ui_requested = true;
                item->ui_deadline = (uint32_t)rt_tick_get();
            }
        }
    }
    rt_hw_interrupt_enable(guard);
    epd_power_notify();
}

uint32_t epd_service_wait(epd_service_t *service, rt_int32_t ticks)
{
    uint32_t events = 0;
    bool worker = rt_thread_self() == &service->worker;
    /* Keep an undispatched wake protected across this wait. A consumed wake
     * owns one worker pass, ending when that worker next becomes idle. */
    if (worker) service_schedule_release(service, false);
    if (epd_service_cancelled(service)) return EPD_SERVICE_STOP;
    rt_event_recv(&service->wakeup, EPD_SERVICE_WAKE | EPD_SERVICE_NETWORK | EPD_SERVICE_STOP,
                   RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, ticks, &events);
    if (worker) {
        rt_base_t guard = rt_hw_interrupt_disable();
        service->schedule_pending = false;
        rt_hw_interrupt_enable(guard);
    }
    return events;
}
RTM_EXPORT(epd_service_wait);

void epd_service_wake(epd_service_t *service)
{
    if (service) rt_event_send(&service->wakeup, EPD_SERVICE_WAKE);
    epd_power_notify();
}
RTM_EXPORT(epd_service_wake);

bool epd_service_on_stop(epd_service_t *service, void (*notify)(void *), void *context)
{
    bool accepted = false;
    bool call = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            if (!notify)
            {
                item->stop_notify = NULL;
                item->stop_context = NULL;
                accepted = true;
            }
            else if (!epd_service_cancelled(item) && !item->worker_done)
            {
                item->stop_notify = notify;
                item->stop_context = context;
                accepted = true;
            }
            else if (!item->runtime_closing) {
                ++item->runtime_users;
                call = true;
            }
            break;
        }
    rt_hw_interrupt_enable(level);
    if (call) {
        epd_memory_owner_t previous = service_enter(service, true);
        if (previous != EPD_MEMORY_OWNER_INVALID) {
            notify(context);
            epd_memory_owner_leave_trusted(previous);
        }
        service_runtime_unpin(service);
    }
    return accepted;
}
RTM_EXPORT(epd_service_on_stop);

static void stop_notify(epd_service_t *service)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    void (*notify)(void *) = service->stop_notify;
    void *context = service->stop_context;
    service->stop_notify = NULL;
    service->stop_context = NULL;
    if (notify) ++service->runtime_users;
    rt_hw_interrupt_enable(guard);
    if (notify) {
        epd_memory_owner_t previous = service_enter(service, true);
        if (previous != EPD_MEMORY_OWNER_INVALID) {
            notify(context);
            epd_memory_owner_leave_trusted(previous);
        }
        service_runtime_unpin(service);
    }
}

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
    if (item && (!service->ui_done || service->stopping) &&
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

void epd_service_ui_watch(epd_service_t *service, uint32_t events)
{
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service) { item->ui_events = events; break; }
    rt_hw_interrupt_enable(level);
}
RTM_EXPORT(epd_service_ui_watch);

void app_service_ui_signal(uint32_t events)
{
    uint32_t now = (uint32_t)rt_tick_get();
    bool wake = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next) {
        if (!(item->ui_events & events) || item->ui_done ||
            (item->ui_requested && (int32_t)(item->ui_deadline - now) <= 0))
            continue;
        item->ui_requested = true;
        item->ui_deadline = now;
        wake = true;
    }
    rt_hw_interrupt_enable(level);
    if (wake) epd_power_notify();
    app_ui_signal(events);
}

uint32_t app_service_ui_delay(uint32_t default_ms)
{
    /* Finish visiting an interrupted batch before the main loop goes to sleep. */
    if (ui_remaining) return 0;
    uint32_t now = (uint32_t)rt_tick_get();
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *service = services; service; service = service->next)
    {
        if (service->stopping && service->worker_done && !service->callers && !service->reclaim_holds && !service->cleanup_holds &&
            !service->ui_requested)
        {
            default_ms = 0;
            break;
        }
        if (!service->ui_requested || (service->ui_done && !service->stopping) ||
            (service->stopping && (!service->worker_done || service->callers || service->reclaim_holds || service->cleanup_holds))) continue;
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
        if (!epd_service_cancelled(item) && item->callers != UINT32_MAX &&
            item->definition.command && !strcmp(item->id, id))
        {
            if (!service_activity_begin(item)) break;
            service = item;
            ++service->callers;
            reclaim_work_started(service);
            break;
        }
    rt_hw_interrupt_enable(level);
    if (!service) return -RT_ENOSYS;
    epd_memory_owner_t previous = service_runtime_sync(service) ? service_enter(service, false) :
                                  EPD_MEMORY_OWNER_INVALID;
    int result = -RT_EBUSY;
    if (previous != EPD_MEMORY_OWNER_INVALID) {
        result = service->definition.command(argc, argv);
        epd_memory_owner_leave_trusted(previous);
    }
    epd_service_release(service);
    return result;
}

static void run(void *parameter)
{
    epd_service_t *service = parameter;
    rt_base_t dispatch_guard = rt_hw_interrupt_disable();
    service->schedule_pending = false;
    rt_hw_interrupt_enable(dispatch_guard);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(service->worker_owner);
    if (previous != EPD_MEMORY_OWNER_INVALID) {
        service->definition.run(service);
        epd_memory_owner_leave_trusted(previous);
    }
    rt_base_t guard = rt_hw_interrupt_disable();
    if (!service->stopping) ++summary_revision;
    service->stopping = true;
    if (!service->parent_id[0]) app_background_service_stopped(service->memory_owner);
    service->ui_requested = false;
    rt_hw_interrupt_enable(guard);
    service_schedule_release(service, true);
    stop_notify(service);
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

void epd_service_set_reclaimable(epd_service_t *service, bool reclaimable)
{
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service && !item->parent_id[0])
        {
            item->reclaimable = reclaimable;
            break;
        }
    rt_hw_interrupt_enable(level);
}
RTM_EXPORT(epd_service_set_reclaimable);

bool epd_service_reclaim_hold(epd_service_t *service)
{
    bool held = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            if (!epd_service_cancelled(item) && item->reclaim_holds != UINT32_MAX &&
                service_activity_begin(item))
            {
                ++item->reclaim_holds;
                reclaim_work_started(item);
                held = true;
            }
            break;
        }
    rt_hw_interrupt_enable(level);
    if (held && !service_runtime_sync(service)) {
        epd_service_reclaim_release(service);
        held = false;
    }
    return held;
}
RTM_EXPORT(epd_service_reclaim_hold);

void epd_service_reclaim_release(epd_service_t *service)
{
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            RT_ASSERT(item->reclaim_holds);
            ++item->runtime_users;
            found = true;
            --item->reclaim_holds;
            service_activity_end(item);
            break;
        }
    rt_hw_interrupt_enable(level);
    if (found) {
        service_runtime_sync(service);
        service_runtime_unpin(service);
    }
}
RTM_EXPORT(epd_service_reclaim_release);

bool epd_service_cleanup_hold(epd_service_t *service)
{
    bool held = false;
    rt_base_t guard = rt_hw_interrupt_disable();
    uint32_t now = (uint32_t)rt_tick_get();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service) {
            if (!item->runtime_closing && item->cleanup_holds != UINT32_MAX &&
                epd_memory_owner_current() == item->cleanup_owner &&
                (item->dirty || item->flush_active) &&
                (!item->cleanup_retry || (int32_t)(now - item->cleanup_retry_deadline) >= 0) &&
                sleep_guard_acquire_drain(&item->cleanup_sleep_owner)) {
                ++item->cleanup_holds;
                reclaim_work_started(item);
                item->cleanup_retry = true;
                item->cleanup_retry_ms = item->cleanup_retry_ms ?
                    (item->cleanup_retry_ms < 2500u ? item->cleanup_retry_ms * 2u : 5000u) : 500u;
                item->cleanup_retry_deadline = now + (uint32_t)
                    (((uint64_t)item->cleanup_retry_ms * RT_TICK_PER_SECOND + 999u) / 1000u);
                held = true;
            }
            break;
        }
    rt_hw_interrupt_enable(guard);
    if (held && !service_runtime_sync(service)) {
        epd_service_cleanup_release(service);
        held = false;
    }
    return held;
}
RTM_EXPORT(epd_service_cleanup_hold);

void epd_service_cleanup_release(epd_service_t *service)
{
    bool found = false;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service) {
            RT_ASSERT(item->cleanup_holds);
            ++item->runtime_users;
            --item->cleanup_holds;
            if (!item->cleanup_holds) sleep_guard_release(&item->cleanup_sleep_owner);
            found = true;
            break;
        }
    rt_hw_interrupt_enable(guard);
    if (found) {
        service_runtime_sync(service);
        service_runtime_unpin(service);
    }
}
RTM_EXPORT(epd_service_cleanup_release);

void app_service_set_background(const char *app_id, bool background)
{
    if (!app_id) return;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (!item->parent_id[0] && !strcmp(item->app_id, app_id)) item->background = background;
    rt_hw_interrupt_enable(level);
}

bool app_service_app_stopping(const char *app_id)
{
    if (!app_id) return false;
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->app_id, app_id) && (item->stopping || item->reclaim_reserved)) return true;
    return false;
}

size_t app_service_module_pins(const char *app_id)
{
    size_t count = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item->module && !strcmp(item->app_id, app_id)) ++count;
    rt_hw_interrupt_enable(level);
    return count;
}

app_service_reclaim_result_t app_service_reclaim_step(const char *foreground_id,
                                                      uint32_t generation)
{
    if (!foreground_id || !*foreground_id || !generation || ui_processing ||
        epd_power_sleeping() || system_reset_blocks_apps() || storage_changing())
        return APP_SERVICE_RECLAIM_WAITING;
    if (app_modules_reclaim(foreground_id)) return APP_SERVICE_RECLAIM_WAITING;
    if (!services) return APP_SERVICE_RECLAIM_NONE;
    if (!reclaim_cursor) reclaim_cursor = services;
    epd_service_t *first = reclaim_cursor, *root = NULL;
    rt_base_t level = rt_hw_interrupt_disable();
    do
    {
        epd_service_t *item = reclaim_cursor;
        reclaim_cursor = item->next ? item->next : services;
        if (!item->parent_id[0] && item->definition.run && item->background && item->reclaimable &&
            strcmp(item->app_id, foreground_id))
        {
            root = item;
            break;
        }
    } while (reclaim_cursor != first);
    if (!root)
    {
        rt_hw_interrupt_enable(level);
        return APP_SERVICE_RECLAIM_NONE;
    }
    epd_service_t *flush = NULL;
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->app_id, root->app_id))
        {
            if (!item->definition.run || item->stopping || item->callers || item->reclaim_holds || item->cleanup_holds ||
                service_background_busy(item))
            {
                rt_hw_interrupt_enable(level);
                return APP_SERVICE_RECLAIM_WAITING;
            }
            if (!flush && (item->definition.flush || item->dirty) && item->reclaim_flushed != generation)
                flush = item;
        }
    if (flush)
    {
        uint32_t work_revision = flush->reclaim_work_revision;
        rt_hw_interrupt_enable(level);
        ui_processing = true;
        bool saved = service_flush(flush, false);
        ui_processing = false;
        level = rt_hw_interrupt_disable();
        /* Work admitted during the callback must complete before cancellation.
         * Leave the barrier invalid until a later, idle invocation. */
        bool held = false;
        for (epd_service_t *item = services; item; item = item->next)
            if (!strcmp(item->app_id, root->app_id) && (item->reclaim_holds || item->cleanup_holds || item->callers)) held = true;
        if (saved && !held && work_revision == flush->reclaim_work_revision)
            flush->reclaim_flushed = generation;
        rt_hw_interrupt_enable(level);
        return APP_SERVICE_RECLAIM_WAITING;
    }
    /* Reserve the whole family atomically against new holds, shell calls and
     * asynchronous retains. Notifications run outside this publication guard. */
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->app_id, root->app_id)) item->reclaim_reserved = true;
    rt_hw_interrupt_enable(level);
    service_stop(root->id, true);
    return APP_SERVICE_RECLAIM_PROGRESS;
}

bool epd_service_retain(epd_service_t *service)
{
    bool retained = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            if (!epd_service_cancelled(item) && item->callers != UINT32_MAX &&
                service_activity_begin(item))
            {
                ++item->callers;
                reclaim_work_started(item);
                retained = true;
            }
            break;
        }
    rt_hw_interrupt_enable(level);
    if (retained && !service_runtime_sync(service)) {
        epd_service_release(service);
        retained = false;
    }
    return retained;
}

void epd_service_release(epd_service_t *service)
{
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (item == service)
        {
            RT_ASSERT(item->callers != 0);
            ++item->runtime_users;
            found = true;
            --item->callers;
            service_activity_end(item);
            rt_event_send(&item->wakeup, EPD_SERVICE_WAKE);
            break;
        }
    rt_hw_interrupt_enable(level);
    if (found) {
        service_runtime_sync(service);
        service_runtime_unpin(service);
    }
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

static epd_service_t *service_start_scoped(const char *id, const epd_background_t *definition,
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
    if (!activity_lock_ready) {
        stage = "activity_lock";
#ifdef RT_USING_MODULE_OBJECT_CONTEXT
        struct rt_object_global_scope scope = {0};
        error = rt_object_global_scope_enter(&scope);
        if (error != RT_EOK) goto rejected;
#endif
        error = rt_mutex_init(&activity_lock, "svc_work", RT_IPC_FLAG_PRIO);
#ifdef RT_USING_MODULE_OBJECT_CONTEXT
        rt_object_global_scope_leave(&scope);
#endif
        if (error != RT_EOK) goto rejected;
        activity_lock_ready = true;
    }
    epd_service_t initial = {0};
    epd_service_t *service = &initial;
    service->memory_owner = service->worker_owner = service->cleanup_owner = EPD_MEMORY_OWNER_INVALID;
    strcpy(service->id, id);
    strcpy(service->app_id, app_id);
    if (parent_id) strcpy(service->parent_id, parent_id);
    service->sleep_owner.id = service->id;
    service->cleanup_sleep_owner.id = service->id;
    service->definition = *definition;
    stage = "memory_owner";
    error = -RT_ENOMEM;
    if (!service_create_owners(service, module != NULL, parent_id)) {
        service_release_owners(service);
        goto rejected;
    }
    epd_memory_owner_t previous = service_enter(service, false);
    if (previous == EPD_MEMORY_OWNER_INVALID) {
        service_release_owners(service);
        goto rejected;
    }
    stage = "service_alloc";
    service = rt_calloc(1, sizeof(*service));
    if (!service) {
        epd_memory_owner_leave_trusted(previous);
        service_release_owners(&initial);
        goto rejected;
    }
    *service = initial;
    service->sleep_owner.id = service->id;
    service->cleanup_sleep_owner.id = service->id;
    service->worker_done = definition->run == NULL;
    service->ui_done = definition->ui_process == NULL;
    stage = "event_init";
    error = rt_event_init(&service->wakeup, "app_evt", RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        start_failed(id, definition, stage, error);
        epd_memory_owner_leave_trusted(previous);
        service_release_owners(service);
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
        if (!parent_id && service->volume == STORAGE_FLASH)
            service->release_tf = (bool (*)(epd_service_t *, uint32_t))
                                  dlsym(service->module, "epd_app_release_tf");
    }
    if (!definition->run)
    {
        service_add(service);
        if (module && !parent_id) app_background_schedule_ack(app_id, 0);
        epd_memory_owner_leave_trusted(previous);
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
    /* Catalog discovery runs in a later UI iteration. Its accepted generation
     * resides in the background scheduler until this worker owns the work. */
    uint32_t dispatch_generation = module && !parent_id ?
        app_background_schedule_claimed(app_id) : 0;
    if (dispatch_generation) {
        stage = "scheduled_work";
        error = -RT_ENOMEM;
        if (!service_schedule_hold(service)) goto startup_failed;
    }
    stage = "thread_startup";
    error = rt_thread_startup(thread);
    if (error == RT_EOK) {
        if (dispatch_generation) app_background_schedule_ack(app_id, dispatch_generation);
        epd_memory_owner_leave_trusted(previous);
        return service;
    }
startup_failed:
    start_failed(id, definition, stage, error);
    service_schedule_release(service, true);
    service_remove(service);
    thread->cleanup = NULL;
    rt_thread_detach(thread);
    goto cleanup;
failed:
    start_failed(id, definition, stage, error);
cleanup:
    network_unsubscribe(service->subscription);
    if (service->runtime_bound) epd_resource_runtime_close_owner(service->memory_owner);
    if (service->module) app_module_close(service->module);
    rt_event_detach(&service->wakeup);
    epd_app_free(service->worker_stack);
    epd_memory_owner_leave_trusted(previous);
    service_release_owners(service);
    rt_free(service);
    return NULL;
rejected:
    start_failed(id, definition, stage, error);
    return NULL;
}

static epd_service_t *service_start(const char *id, const epd_background_t *definition,
                                   void *module, const char *app_id, const char *parent_id)
{
#ifdef RT_USING_MODULE_OBJECT_CONTEXT
    struct rt_object_global_scope scope = {0};
    bool shared = !module && !parent_id;
    if (shared) {
        int error = rt_object_global_scope_enter(&scope);
        if (error != RT_EOK) {
            start_failed(id, definition, "object_scope", error);
            return NULL;
        }
    }
#endif
    epd_service_t *service = service_start_scoped(id, definition, module, app_id, parent_id);
#ifdef RT_USING_MODULE_OBJECT_CONTEXT
    if (shared) rt_object_global_scope_leave(&scope);
#endif
    return service;
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

static void service_stop(const char *id, bool preserve_schedule)
{
    if (!preserve_schedule) app_background_schedule_cancel(id);
    for (epd_service_t *service = services; service; service = service->next)
    {
        if (strcmp(service->id, id) && strcmp(service->parent_id, id)) continue;
        rt_base_t level = rt_hw_interrupt_disable();
        if (!preserve_schedule) service->schedule_revoked = true;
        if (service->stopping)
        {
            rt_hw_interrupt_enable(level);
            continue;
        }
        ++summary_revision;
        service->stopping = true;
        if (!service->parent_id[0]) app_background_service_stopped(service->memory_owner);
        service->ui_requested = false;
        rt_hw_interrupt_enable(level);
        stop_notify(service);
        /* Pressure cancellation only signals workers here. Their normal exit
         * path performs owner cleanup, which may take speech/system mutexes. */
        if (!service->reclaim_reserved)
        {
            speech_owner_stop(service);
            epd_system_service_stopped(service);
        }
        rt_event_send(&service->wakeup, EPD_SERVICE_STOP);
        epd_power_notify();
    }
}

void app_service_stop(const char *id)
{
    service_stop(id, false);
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
                   (!stopping || (worker_done && !callers && !service->reclaim_holds && !service->cleanup_holds)) &&
                   (!maintenance || stopping || due) && (!requested || due) &&
                   (!once || !service->ui_ran_this_iteration);
    if (call_ui)
    {
        service->ui_requested = false;
        service->ui_ran_this_iteration = true;
    }
    rt_hw_interrupt_enable(level);
    if (stopping && service->ui_done && requested && !due) return false;
    if (stopping && worker_done && !callers && !service->reclaim_holds && !service->cleanup_holds && service->dirty) {
        if ((requested && !due) || !service_flush(service, false)) {
            epd_service_ui_request(service, 10);
            return false;
        }
    }
    if (call_ui)
    {
        epd_memory_owner_t previous = service_enter(service, stopping);
        bool done = false;
        if (previous != EPD_MEMORY_OWNER_INVALID) {
            bool interactive = !maintenance && !stopping && !epd_power_sleeping() &&
                               !storage_changing() && !system_reset_blocks_apps();
            epd_memory_owner_t dispatch = EPD_MEMORY_OWNER_INVALID;
            if (interactive) dispatch = epd_resource_runtime_ui_enter(service->memory_owner);
            done = service->definition.ui_process(service, stopping);
            if (interactive) epd_resource_runtime_ui_leave(dispatch);
            epd_memory_owner_leave_trusted(previous);
        }
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
    level = rt_hw_interrupt_disable();
    bool retire = service->worker_done && service->ui_done && !service->callers &&
                  !service->reclaim_holds && !service->cleanup_holds && !service->runtime_users && !service->dirty;
    bool close_runtime = retire && !service->runtime_closing;
    if (retire) service->runtime_closing = true;
    rt_hw_interrupt_enable(level);
    if (close_runtime) epd_resource_runtime_close_owner(service->memory_owner);
    if (retire && epd_resource_runtime_owner_busy(service->memory_owner)) {
        epd_service_ui_request(service, 10);
        return call_ui;
    }
    if (retire)
    {
        service_remove(service);
        rt_event_detach(&service->wakeup);
        if (service->module) app_module_close(service->module);
        sleep_guard_release(&service->sleep_owner);
        epd_app_free(service->worker_stack);
        service_release_owners(service);
        if (service->reclaim_reserved)
            rt_kprintf("svc %.*s: pressure released\n", (int)EPD_SERVICE_ID_SIZE - 1, service->id);
        rt_free(service);
    }
    return call_ui;
}

static void process(bool maintenance, bool once)
{
    if (ui_processing) return;
    if (!maintenance) service_scheduled_process();
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

static epd_service_t *tf_export_keeper(const char *app_id)
{
    epd_service_t *keeper = NULL;
    for (epd_service_t *service = services; service; service = service->next) {
        if (strcmp(service->app_id, app_id)) continue;
        if (keeper || !service->release_tf || service->stopping || service->worker_done) return NULL;
        keeper = service;
    }
    return keeper;
}

bool app_service_stays_for_tf_export(const char *app_id, uint32_t release_id)
{
    epd_service_t *keeper = release_id ? tf_export_keeper(app_id) : NULL;
    return keeper && keeper->tf_release_ack == release_id;
}

static bool flush_services(uint32_t release_id)
{
    if (ui_processing) return false;
    /* Local account changes can run while the HTTP service is offline. */
    bool ok = portal_release_storage();
    if (!ui_settings_flush()) ok = false;
    ui_processing = true;
    for (epd_service_t *service = services; service; service = service->next) {
        if (release_id && service->release_tf && service == tf_export_keeper(service->app_id)) {
            epd_memory_owner_t previous = service_enter(service, true);
            if (previous == EPD_MEMORY_OWNER_INVALID) { ok = false; continue; }
            bool ready = service->release_tf(service, release_id);
            epd_memory_owner_leave_trusted(previous);
            service->tf_release_ack = ready ? release_id : 0;
            if (!ready) ok = false;
        } else if (!service_flush(service, false)) ok = false;
    }
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *service = services; service; service = service->next)
        if (!app_service_stays_for_tf_export(service->app_id, release_id) &&
            (service->callers || service->reclaim_holds || service->cleanup_holds ||
             service->saved_work_revision != service->reclaim_work_revision)) ok = false;
    rt_hw_interrupt_enable(guard);
    ui_processing = false;
    return ok;
}

bool app_service_flush(void) { return flush_services(0); }

bool app_service_flush_for_tf_export(uint32_t release_id)
{
    return release_id && flush_services(release_id);
}

bool app_service_flush_app(const char *app_id)
{
    if (!app_id || !*app_id || ui_processing) return false;
    bool ok = true;
    ui_processing = true;
    for (epd_service_t *service = services; service; service = service->next)
        if (!strcmp(service->app_id, app_id) && !service_flush(service, true)) ok = false;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *service = services; service; service = service->next)
        if (!strcmp(service->app_id, app_id) &&
            (((service->callers || service->reclaim_holds) && !service_page_task_retained(service)) ||
             service->cleanup_holds || service->dirty ||
             service->saved_work_revision != service->reclaim_work_revision)) ok = false;
    rt_hw_interrupt_enable(guard);
    ui_processing = false;
    return ok;
}

epd_memory_result_t app_service_quiesce(const char *app_id)
{
    if (!app_id || !*app_id) return EPD_MEMORY_INVALID;
    if (ui_processing) return EPD_MEMORY_WAITING;
    bool found = false, stopping = false;
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->app_id, app_id)) {
            found = true;
            stopping |= item->stopping;
        }
    if (!found) return EPD_MEMORY_OK;
    if (stopping || !app_service_flush_app(app_id)) return EPD_MEMORY_WAITING;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->app_id, app_id) &&
            (item->callers || item->reclaim_holds || item->cleanup_holds || item->stopping ||
             item->saved_work_revision != item->reclaim_work_revision)) {
            rt_hw_interrupt_enable(guard);
            return EPD_MEMORY_WAITING;
        }
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->app_id, app_id)) item->reclaim_reserved = true;
    rt_hw_interrupt_enable(guard);
    for (epd_service_t *item = services; item; item = item->next)
        if (!strcmp(item->app_id, app_id)) service_stop(item->id, true);
    return EPD_MEMORY_WAITING;
}

epd_memory_result_t app_service_terminate(const char *app_id)
{
    if (!app_id || !*app_id) return EPD_MEMORY_INVALID;
    epd_memory_result_t result = app_module_request_stop(app_id);
    if (result != EPD_MEMORY_OK) return result;
    app_background_schedule_cancel(app_id);
    result = app_service_quiesce(app_id);
    if (result != EPD_MEMORY_OK) return result;
    return app_module_loaded(app_id) ? EPD_MEMORY_WAITING : EPD_MEMORY_OK;
}

void app_service_stop_modules(void)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->module) app_service_stop(service->id);
}

void app_service_stop_tf_modules(uint32_t release_id)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->module && !app_service_stays_for_tf_export(service->app_id, release_id))
            app_service_stop(service->id);
}

bool app_service_modules_running(void)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->module) return true;
    return false;
}

bool app_service_tf_modules_running(uint32_t release_id)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->module && !app_service_stays_for_tf_export(service->app_id, release_id)) return true;
    return false;
}
