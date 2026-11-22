/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_background.h"
#include "app_background_events.h"
#include <rtthread.h>
#include <rthw.h>
#include <rtm.h>
#include <string.h>
#include "platform/epd_power.h"
#include "platform/epd_resource_runtime.h"
#include "platform/epd_cold_runtime.h"
#include "app_service_internal.h"
#ifndef EPD_MEMORY_HOST
#include "mem_section.h"
#endif

#define BG_APPS 64u
#define BG_GROUPS 64u
#define BG_TASKS 128u
#define BG_ID_SIZE 64u

typedef struct {
    char id[BG_ID_SIZE];
    epd_app_background_snapshot_t state;
    bool allowed, next_enter, close_prepared, cold_prepared, discard_prepared;
    bool scheduled, require_network, schedule_dispatched;
    uint8_t schedule_attempt;
    uint32_t deadline, schedule_generation;
    uint32_t instance_generation;
    uint64_t event_sequence;
    epd_app_background_reason_t event_reason;
    epd_memory_result_t event_result;
} background_app_t;
typedef struct {
    epd_app_task_group_t handle;
    epd_memory_owner_t owner;
    uint32_t members;
    bool ended, component_default;
    epd_app_background_mode_t after;
} background_group_t;
typedef struct {
    epd_memory_owner_t owner;
    epd_app_task_group_t group;
} background_task_t;
#ifndef EPD_MEMORY_HOST
L2_RET_BSS_SECT_BEGIN(app_background)
static background_app_t background_apps[BG_APPS] L2_RET_BSS_SECT(app_background);
static background_group_t background_groups[BG_GROUPS] L2_RET_BSS_SECT(app_background);
static background_task_t background_tasks[BG_TASKS] L2_RET_BSS_SECT(app_background);
L2_RET_BSS_SECT_END
#else
static background_app_t background_apps[BG_APPS];
static background_group_t background_groups[BG_GROUPS];
static background_task_t background_tasks[BG_TASKS];
#endif
static uint32_t background_next_group;
/* Survives record retirement: a delayed claim/ack must not match a reinstall,
 * including when the same ID moves to a different free slot. Never wrap. */
static uint32_t background_next_schedule;
static uint32_t background_next_instance;
static unsigned background_schedule_cursor;

#ifndef EPD_MEMORY_HOST
static int background_metadata_init(void)
{
    memset(background_apps, 0, sizeof(background_apps));
    memset(background_groups, 0, sizeof(background_groups));
    memset(background_tasks, 0, sizeof(background_tasks));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(background_metadata_init);
#endif

static background_app_t *background_find_id(const char *id)
{
    for (unsigned i = 0; i < BG_APPS; ++i)
        if (background_apps[i].id[0] && !strcmp(background_apps[i].id, id))
            return &background_apps[i];
    return NULL;
}
static background_app_t *background_find_owner(epd_memory_owner_t owner)
{
    epd_memory_owner_snapshot_t memory;
    if (owner == EPD_MEMORY_OWNER_SYSTEM || owner == EPD_MEMORY_OWNER_INVALID ||
        !epd_memory_owner_get(owner, &memory)) return NULL;
    for (unsigned i = 0; i < BG_APPS; ++i)
        if (background_apps[i].id[0] && background_apps[i].state.owner == memory.account)
            return &background_apps[i];
    return NULL;
}
static background_group_t *background_find_group(epd_app_task_group_t handle)
{
    if (!handle) return NULL;
    for (unsigned i = 0; i < BG_GROUPS; ++i)
        if (background_groups[i].handle == handle) return &background_groups[i];
    return NULL;
}
static void background_changed(background_app_t *app)
{
    if (app->state.event_revision != UINT32_MAX) ++app->state.event_revision;
}
static void background_event_copy(const background_app_t *app, epd_app_background_event_t *event)
{
    *event = (epd_app_background_event_t){.sequence = app->event_sequence,
        .instance_generation = app->instance_generation, .reason = app->event_reason,
        .result = app->event_result, .state = app->state};
}
/* Called after the complete state transaction, under its publication guard. */
static void background_publish(background_app_t *app, epd_app_background_reason_t reason,
                               epd_memory_result_t result)
{
    /* A permitted discard keeps its save failure through coalesced teardown. */
    if (app->discard_prepared && result == EPD_MEMORY_OK) result = app->state.last_result;
    if (app->event_sequence != UINT64_MAX) ++app->event_sequence;
    app->event_reason = reason; app->event_result = result;
    epd_app_background_event_t event;
    background_event_copy(app, &event);
    app_background_events_publish(&event);
}
bool app_background_event_snapshot(epd_memory_owner_t owner, epd_app_background_event_t *out)
{
    if (!out) return false;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    if (app) background_event_copy(app, out);
    rt_hw_interrupt_enable(guard);
    return app != NULL;
}
static bool background_next_revision(background_app_t *app)
{
    if (app->state.revision == UINT32_MAX) return false;
    ++app->state.revision;
    background_changed(app);
    return true;
}
static bool background_live(background_app_t *app)
{
    epd_memory_owner_snapshot_t memory;
    return app && !app->state.stopping &&
           epd_memory_owner_get(app->state.owner, &memory) && !memory.closed;
}
static void background_effective(background_app_t *app)
{
    uint32_t selected = 0, all = 0;
    for (unsigned i = 0; i < BG_GROUPS; ++i) {
        background_group_t *group = &background_groups[i];
        if (!group->handle || group->owner != app->state.owner) continue;
        all += group->members;
        if ((app->state.explicit_policy && group->handle == app->state.requested.group) ||
            (!app->state.explicit_policy && group->component_default)) selected += group->members;
    }
    app->state.tasks = all;
    epd_app_background_policy_t *policy = &app->state.requested;
    app->state.effective = policy->mode;
    if (policy->mode == EPD_APP_BG_TASKS && !selected) {
        policy->mode = policy->after_tasks;
        policy->group = EPD_APP_TASK_GROUP_NONE;
        app->state.effective = policy->mode;
    }
}

bool app_background_should_start(const char *id, bool explicit_enter)
{
    if (!id || !*id || strlen(id) >= BG_ID_SIZE) return false;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_id(id);
    if (!app) {
        for (unsigned i = 0; i < BG_APPS; ++i)
            if (!background_apps[i].id[0]) {
                app = &background_apps[i];
                strcpy(app->id, id);
                app->state.owner = EPD_MEMORY_OWNER_INVALID;
                app->state.actual = EPD_APP_BG_RELEASED;
                app->allowed = true;
                break;
            }
    }
    bool allowed = false;
    if (app) {
        if (explicit_enter) {
            if (app->state.stopping && app->state.owner != EPD_MEMORY_OWNER_INVALID)
                app->next_enter = true;
            else app->allowed = true;
        }
        allowed = app->allowed && !app->state.stopping;
    }
    rt_hw_interrupt_enable(guard);
    return allowed;
}

epd_memory_result_t app_background_bind(const char *id, epd_memory_owner_t owner)
{
    if (!id || !*id || strlen(id) >= BG_ID_SIZE) return EPD_MEMORY_INVALID;
    epd_memory_owner_snapshot_t memory;
    if (!epd_memory_owner_get(owner, &memory) || memory.closed ||
        memory.kind != EPD_MEMORY_SCOPE_APP || memory.account != owner || !owner)
        return EPD_MEMORY_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < BG_APPS; ++i)
        if (background_apps[i].id[0] && background_apps[i].state.owner == owner &&
            strcmp(background_apps[i].id, id)) {
            rt_hw_interrupt_enable(guard);
            return EPD_MEMORY_INVALID;
        }
    background_app_t *app = background_find_id(id);
    if (app && app->state.owner == owner) {
        rt_hw_interrupt_enable(guard);
        return EPD_MEMORY_OK;
    }
    if (!app_background_should_start(id, false)) {
        rt_hw_interrupt_enable(guard);
        return EPD_MEMORY_CLOSED;
    }
    app = background_find_id(id);
    epd_memory_result_t result = EPD_MEMORY_BUSY;
    if (app->state.owner == owner) result = EPD_MEMORY_OK;
    else if (background_next_instance == UINT32_MAX) result = EPD_MEMORY_OVERFLOW;
    else if (app->state.owner == EPD_MEMORY_OWNER_INVALID && app->allowed &&
             epd_memory_owner_retain_trusted(owner)) {
        memset(&app->state, 0, sizeof(app->state));
        app->state.owner = owner;
        app->state.revision = app->state.event_revision = 1;
        app->state.foreground = true;
        app->state.actual = EPD_APP_BG_FOREGROUND;
        app->close_prepared = false;
        app->cold_prepared = false;
        app->discard_prepared = false;
        app->instance_generation = ++background_next_instance;
        app->event_sequence = 1;
        app->event_reason = EPD_APP_BG_EVENT_INITIAL;
        app->event_result = EPD_MEMORY_OK;
        result = EPD_MEMORY_OK;
    }
    rt_hw_interrupt_enable(guard);
    return result;
}

epd_memory_result_t app_background_unbind(epd_memory_owner_t owner)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (app && owner == app->state.owner) {
        bool groups = false;
        for (unsigned i = 0; i < BG_GROUPS; ++i)
            if (background_groups[i].handle && background_groups[i].owner == owner &&
                background_groups[i].members) groups = true;
        bool discarded = app->close_prepared && app->discard_prepared && app->state.stopping &&
                         app->state.actual == EPD_APP_BG_RELEASED;
        if (groups || (app->state.dirty_revision != app->state.saved_revision && !discarded) ||
            (app->state.actual != EPD_APP_BG_RELEASED && app->state.actual != EPD_APP_BG_COLD))
            result = EPD_MEMORY_BUSY;
        else {
            app_background_events_forget(owner, app->instance_generation);
            for (unsigned i = 0; i < BG_GROUPS; ++i)
                if (background_groups[i].handle && background_groups[i].owner == owner)
                    memset(&background_groups[i], 0, sizeof(background_groups[i]));
            app->state.owner = EPD_MEMORY_OWNER_INVALID;
            app->state.stopping = false;
            app->allowed = app->next_enter;
            app->next_enter = false;
            epd_memory_owner_release_trusted(owner);
            result = EPD_MEMORY_OK;
        }
    }
    rt_hw_interrupt_enable(guard);
    return result;
}

bool app_background_forget(const char *id)
{
    if (!id) return false;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_id(id);
    bool removed = app && app->state.owner == EPD_MEMORY_OWNER_INVALID &&
                   !app->state.tasks && !app->scheduled && !app->schedule_dispatched;
    if (removed) memset(app, 0, sizeof(*app));
    rt_hw_interrupt_enable(guard);
    return removed;
}

epd_memory_result_t app_background_snapshot(epd_memory_owner_t owner,
                                           epd_app_background_snapshot_t *out)
{
    if (!out) return EPD_MEMORY_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    if (app) *out = app->state;
    rt_hw_interrupt_enable(guard);
    return app ? EPD_MEMORY_OK : EPD_MEMORY_INVALID;
}
epd_memory_result_t epd_app_background_get(epd_app_background_snapshot_t *out)
{
    return app_background_snapshot(epd_memory_owner_current(), out);
}
RTM_EXPORT(epd_app_background_get);

epd_memory_result_t epd_app_background_set(const epd_app_background_policy_t *policy,
                                         uint32_t *revision_out)
{
    if (!policy || policy->mode > EPD_APP_BG_TASKS || policy->mode < EPD_APP_BG_RELEASE ||
        policy->after_tasks > EPD_APP_BG_STATE || policy->after_tasks < EPD_APP_BG_RELEASE ||
        (policy->mode != EPD_APP_BG_TASKS && policy->group)) return EPD_MEMORY_INVALID;
    epd_memory_owner_t notify_owner = EPD_MEMORY_OWNER_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(epd_memory_owner_current());
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (app) {
        background_group_t *group = background_find_group(policy->group);
        if (!background_live(app)) result = EPD_MEMORY_CLOSED;
        else if (policy->mode == EPD_APP_BG_TASKS &&
                 (!group || group->owner != app->state.owner || !group->members || group->ended))
            result = EPD_MEMORY_INVALID;
        else if (!background_next_revision(app)) result = EPD_MEMORY_OVERFLOW;
        else {
            app->state.requested = *policy;
            app->state.explicit_policy = true;
            if (policy->mode == EPD_APP_BG_RELEASE) {
                app->scheduled = app->schedule_dispatched = false;
            }
            background_effective(app);
            notify_owner = app->state.owner;
            if (revision_out) *revision_out = app->state.revision;
            result = EPD_MEMORY_OK;
        }
        app->state.last_result = result;
        background_publish(app, result == EPD_MEMORY_OK ? EPD_APP_BG_EVENT_POLICY : EPD_APP_BG_EVENT_REJECTED, result);
    }
    rt_hw_interrupt_enable(guard);
    if (result == EPD_MEMORY_OK) app_service_background_changed(notify_owner);
    return result;
}
RTM_EXPORT(epd_app_background_set);

epd_memory_result_t app_background_foreground(epd_memory_owner_t owner, bool foreground)
{
    epd_memory_owner_t notify_owner = EPD_MEMORY_OWNER_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (app) {
        if (!background_live(app)) result = EPD_MEMORY_CLOSED;
        else if (app->state.foreground != foreground && !background_next_revision(app))
            result = EPD_MEMORY_OVERFLOW;
        else {
            if (app->state.foreground != foreground) notify_owner = app->state.owner;
            app->state.foreground = foreground;
            if (foreground) app->state.actual = EPD_APP_BG_FOREGROUND;
            background_changed(app);
            result = EPD_MEMORY_OK;
            background_publish(app, EPD_APP_BG_EVENT_FOREGROUND, result);
        }
    }
    rt_hw_interrupt_enable(guard);
    if (notify_owner != EPD_MEMORY_OWNER_INVALID) app_service_background_changed(notify_owner);
    else if (result == EPD_MEMORY_OK) epd_power_notify();
    return result;
}

epd_memory_result_t app_background_report(epd_memory_owner_t owner, uint32_t revision,
                                         epd_app_background_state_t actual)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (app && revision == app->state.revision) {
        bool clean = app->state.dirty_revision == app->state.saved_revision;
        bool valid = (actual == EPD_APP_BG_FOREGROUND && app->state.foreground && !app->state.stopping) ||
                     (actual == EPD_APP_BG_RUNNING && !app->state.foreground && !app->state.stopping &&
                      app->state.effective == EPD_APP_BG_TASKS && app->state.tasks) ||
                     (actual == EPD_APP_BG_RETAINED && !app->state.foreground && !app->state.tasks &&
                      !app->state.stopping && app->state.effective == EPD_APP_BG_STATE) ||
                     (actual == EPD_APP_BG_STOPPING) ||
                     ((actual == EPD_APP_BG_RELEASED || actual == EPD_APP_BG_COLD) &&
                      app->state.stopping && !app->state.tasks &&
                      (clean || (actual == EPD_APP_BG_RELEASED && app->close_prepared && app->discard_prepared)) &&
                      (actual != EPD_APP_BG_COLD || !app->discard_prepared));
        if (valid) {
            app->state.actual = actual;
            app->state.applied_revision = revision;
            if (actual == EPD_APP_BG_STOPPING) {
                app->state.stopping = true;
                app->allowed = false;
            }
            background_changed(app);
            result = EPD_MEMORY_OK;
        } else result = EPD_MEMORY_BUSY;
        background_publish(app, result == EPD_MEMORY_OK ? EPD_APP_BG_EVENT_ACTUAL : EPD_APP_BG_EVENT_REJECTED, result);
    }
    rt_hw_interrupt_enable(guard);
    return result;
}

void app_background_stopped(epd_memory_owner_t owner)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    if (app) {
        app->state.stopping = true;
        app->state.actual = EPD_APP_BG_STOPPING;
        app->allowed = false;
        background_changed(app);
        background_publish(app, EPD_APP_BG_EVENT_STOP, EPD_MEMORY_OK);
    }
    rt_hw_interrupt_enable(guard);
}
void app_background_service_stopped(epd_memory_owner_t owner)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    if (app) {
        app->allowed = false;
        background_changed(app);
        background_publish(app, EPD_APP_BG_EVENT_STOP, EPD_MEMORY_OK);
    }
    rt_hw_interrupt_enable(guard);
    if (app) epd_power_notify();
}
epd_memory_result_t app_background_request_stop(epd_memory_owner_t owner)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (app) {
        if (app->state.foreground) result = EPD_MEMORY_WAITING;
        else if (app->close_prepared) result = EPD_MEMORY_OK;
        else if (!app->state.stopping && !background_next_revision(app)) result = EPD_MEMORY_OVERFLOW;
        else {
            app->state.requested = (epd_app_background_policy_t){EPD_APP_BG_RELEASE, 0, EPD_APP_BG_RELEASE};
            app->state.explicit_policy = true;
            app->state.stopping = true;
            app->state.actual = EPD_APP_BG_STOPPING;
            app->allowed = app->next_enter = false;
            app->scheduled = app->schedule_dispatched = false;
            background_effective(app);
            background_changed(app);
            result = EPD_MEMORY_OK;
            background_publish(app, EPD_APP_BG_EVENT_STOP, result);
        }
    }
    rt_hw_interrupt_enable(guard);
    if (result == EPD_MEMORY_OK) app_service_background_changed(owner);
    return result;
}
bool app_background_accepting(epd_memory_owner_t owner)
{
    if (owner == EPD_MEMORY_OWNER_SYSTEM) return true;
    rt_base_t guard = rt_hw_interrupt_disable();
    bool result = background_live(background_find_owner(owner));
    rt_hw_interrupt_enable(guard);
    return result;
}

bool app_background_schedule(epd_memory_owner_t owner, int32_t delay_ticks,
                             bool require_network, uint32_t now)
{
    if (delay_ticks < -1) return false;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    bool accepted = app && !app->close_prepared &&
        (!app->state.explicit_policy || app->state.requested.mode != EPD_APP_BG_RELEASE) &&
        background_next_schedule != UINT32_MAX;
    if (accepted) {
        app->schedule_generation = ++background_next_schedule;
        app->schedule_dispatched = false;
        app->schedule_attempt = 0;
        app->scheduled = delay_ticks >= 0;
        app->deadline = now + (uint32_t)(delay_ticks >= 0 ? delay_ticks : 0);
        app->require_network = require_network;
    }
    rt_hw_interrupt_enable(guard);
    return accepted;
}

void app_background_schedule_cancel(const char *id)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = id ? background_find_id(id) : NULL;
    if (app) {
        /* Revoke discovery authorized by a dispatch even if no worker exists. */
        if (app->schedule_dispatched) app->allowed = false;
        app->scheduled = app->schedule_dispatched = false;
    }
    rt_hw_interrupt_enable(guard);
}

bool app_background_schedule_due(uint32_t now, bool network_ready,
                                 char *id, size_t capacity, uint32_t *generation)
{
    if (!id || !capacity || !generation) return false;
    bool due = false;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (unsigned n = 0; n < BG_APPS; ++n) {
        background_app_t *app = &background_apps[background_schedule_cursor];
        background_schedule_cursor = (background_schedule_cursor + 1u) % BG_APPS;
        if (!app->scheduled || app->state.stopping ||
            (app->require_network && !network_ready) ||
            (int32_t)(now - app->deadline) < 0 || strlen(app->id) >= capacity) continue;
        strcpy(id, app->id);
        *generation = app->schedule_generation;
        due = true;
        break;
    }
    rt_hw_interrupt_enable(guard);
    return due;
}

bool app_background_schedule_claim(const char *id, uint32_t generation, uint32_t now)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = id ? background_find_id(id) : NULL;
    bool claimed = app && app->scheduled && !app->state.stopping &&
                   app->schedule_generation == generation && (int32_t)(now - app->deadline) >= 0;
    if (claimed) {
        /* Keep responsibility until a live worker accepts the wake/start. */
        uint32_t seconds = app->schedule_attempt == 0 ? 30u :
                           app->schedule_attempt == 1 ? 300u : 1800u;
        if (app->schedule_attempt < 2) ++app->schedule_attempt;
        app->deadline = now + seconds * RT_TICK_PER_SECOND;
        app->schedule_dispatched = true;
        app->allowed = true;
    }
    rt_hw_interrupt_enable(guard);
    return claimed;
}

uint32_t app_background_schedule_claimed(const char *id)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = id ? background_find_id(id) : NULL;
    uint32_t generation = app && app->scheduled && app->schedule_dispatched &&
        app->allowed && !app->state.stopping ? app->schedule_generation : 0;
    rt_hw_interrupt_enable(guard);
    return generation;
}

void app_background_schedule_ack(const char *id, uint32_t generation)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = id ? background_find_id(id) : NULL;
    if (app && app->scheduled && app->schedule_dispatched &&
        (!generation || app->schedule_generation == generation))
        app->scheduled = app->schedule_dispatched = false;
    rt_hw_interrupt_enable(guard);
}

epd_memory_result_t app_background_group_create(epd_memory_owner_t owner, epd_app_task_group_t *out)
{
    if (!out) return EPD_MEMORY_INVALID;
    *out = 0;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    epd_memory_result_t result = EPD_MEMORY_CLOSED;
    if (background_live(app)) {
        result = EPD_MEMORY_NO_METADATA;
        for (unsigned i = 0; i < BG_GROUPS; ++i)
            if (!background_groups[i].handle && background_next_group != UINT32_MAX) {
                background_group_t *group = &background_groups[i];
                memset(group, 0, sizeof(*group));
                group->handle = ++background_next_group;
                group->owner = app->state.owner;
                *out = group->handle;
                result = EPD_MEMORY_OK;
                break;
            }
    }
    rt_hw_interrupt_enable(guard);
    return result;
}

epd_memory_result_t app_background_task_begin(epd_app_task_group_t handle, epd_memory_owner_t *out)
{
    if (!out) return EPD_MEMORY_INVALID;
    *out = EPD_MEMORY_OWNER_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_group_t *group = background_find_group(handle);
    background_app_t *app = group ? background_find_owner(group->owner) : NULL;
    epd_memory_result_t result = EPD_MEMORY_CLOSED;
    if (app_background_group_accepting(handle)) {
        result = EPD_MEMORY_NO_METADATA;
        for (unsigned i = 0; i < BG_TASKS; ++i)
            if (!background_tasks[i].group) {
                epd_memory_owner_t task = epd_memory_scope_create_trusted(app->state.owner, EPD_MEMORY_SCOPE_TASK);
                if (task == EPD_MEMORY_OWNER_INVALID) break;
                background_tasks[i].group = handle;
                background_tasks[i].owner = task;
                ++group->members;
                background_effective(app);
                background_changed(app);
                background_publish(app, EPD_APP_BG_EVENT_TASKS, EPD_MEMORY_OK);
                *out = task;
                result = EPD_MEMORY_OK;
                break;
            }
    }
    rt_hw_interrupt_enable(guard);
    return result;
}

bool app_background_group_accepting(epd_app_task_group_t handle)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_group_t *group = background_find_group(handle);
    background_app_t *app = group ? background_find_owner(group->owner) : NULL;
    bool allowed = background_live(app) && !group->ended &&
        (app->state.foreground || (!app->state.explicit_policy && app->allowed) ||
         (app->state.effective == EPD_APP_BG_TASKS && app->state.requested.group == handle));
    rt_hw_interrupt_enable(guard);
    return allowed;
}

epd_memory_result_t epd_app_task_group_create(epd_app_task_group_t *out)
{
    return app_background_group_create(epd_memory_owner_current(), out);
}
RTM_EXPORT(epd_app_task_group_create);

epd_memory_result_t epd_app_task_begin(epd_app_task_group_t handle, epd_memory_owner_t *out)
{
    if (!out) return EPD_MEMORY_INVALID;
    *out = EPD_MEMORY_OWNER_INVALID;
    epd_memory_owner_t task = EPD_MEMORY_OWNER_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_group_t *group = background_find_group(handle);
    background_app_t *app = background_find_owner(epd_memory_owner_current());
    epd_memory_result_t result = group && app && group->owner == app->state.owner ?
        app_background_task_begin(handle, &task) : EPD_MEMORY_INVALID;
    rt_hw_interrupt_enable(guard);
    if (result == EPD_MEMORY_OK) {
        int published = epd_resource_runtime_task_state(task, (uint64_t)task, true);
        if (published == EPD_RESOURCE_OK) *out = task;
        else {
            epd_resource_runtime_close_owner(task);
            app_background_task_end(task);
            result = published == EPD_RESOURCE_CAPACITY ? EPD_MEMORY_NO_METADATA : EPD_MEMORY_CLOSED;
        }
    }
    return result;
}
RTM_EXPORT(epd_app_task_begin);

bool epd_app_task_end(epd_memory_owner_t task)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(epd_memory_owner_current());
    bool retained = app && background_find_owner(task) == app && epd_memory_owner_retain_trusted(task);
    bool result = retained && app_background_task_end(task);
    rt_hw_interrupt_enable(guard);
    if (result) epd_resource_runtime_close_owner(task);
    if (retained) epd_memory_owner_release_trusted(task);
    return result;
}
RTM_EXPORT(epd_app_task_end);

epd_memory_result_t epd_app_task_group_release(epd_app_task_group_t handle)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_group_t *group = background_find_group(handle);
    background_app_t *app = background_find_owner(epd_memory_owner_current());
    epd_memory_result_t result = group && app && group->owner == app->state.owner ?
        app_background_group_release(handle) : EPD_MEMORY_INVALID;
    rt_hw_interrupt_enable(guard);
    return result;
}
RTM_EXPORT(epd_app_task_group_release);

bool app_background_task_end(epd_memory_owner_t task)
{
    bool ended = false;
    epd_memory_owner_t notify_owner = EPD_MEMORY_OWNER_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < BG_TASKS; ++i)
        if (background_tasks[i].group && background_tasks[i].owner == task) {
            background_group_t *group = background_find_group(background_tasks[i].group);
            background_app_t *app = background_find_owner(group->owner);
            background_tasks[i].group = 0;
            if (!--group->members) group->ended = true;
            epd_memory_owner_close_trusted(task);
            epd_memory_owner_release_trusted(task);
            epd_app_background_mode_t before = app->state.requested.mode;
            background_effective(app);
            if (app->state.explicit_policy && before != app->state.requested.mode)
                notify_owner = app->state.owner;
            background_next_revision(app);
            background_publish(app, EPD_APP_BG_EVENT_TASKS, EPD_MEMORY_OK);
            ended = true;
            break;
        }
    rt_hw_interrupt_enable(guard);
    if (notify_owner != EPD_MEMORY_OWNER_INVALID) app_service_background_changed(notify_owner);
    else if (ended) epd_power_notify();
    return ended;
}

epd_memory_result_t app_background_group_release(epd_app_task_group_t handle)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_group_t *group = background_find_group(handle);
    epd_memory_result_t result = !group ? EPD_MEMORY_INVALID : EPD_MEMORY_BUSY;
    if (group && !group->members) {
        memset(group, 0, sizeof(*group));
        result = EPD_MEMORY_OK;
    }
    rt_hw_interrupt_enable(guard);
    return result;
}

epd_memory_result_t app_background_group_default(epd_app_task_group_t handle,
                                                 epd_app_background_mode_t after)
{
    if (after != EPD_APP_BG_RELEASE && after != EPD_APP_BG_STATE) return EPD_MEMORY_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_group_t *group = background_find_group(handle);
    background_app_t *app = group ? background_find_owner(group->owner) : NULL;
    epd_memory_result_t result = EPD_MEMORY_CLOSED;
    if (background_live(app) && group->members && !group->ended) {
        if (app->state.explicit_policy) result = EPD_MEMORY_BUSY;
        else if (!background_next_revision(app)) result = EPD_MEMORY_OVERFLOW;
        else {
            group->component_default = true;
            group->after = after;
            app->state.requested.mode = EPD_APP_BG_TASKS;
            app->state.requested.group = 0; /* Union of registered component groups. */
            /* STATE survives the end of one component until all groups finish. */
            if (after == EPD_APP_BG_STATE) app->state.requested.after_tasks = EPD_APP_BG_STATE;
            background_effective(app);
            result = EPD_MEMORY_OK;
            background_publish(app, EPD_APP_BG_EVENT_TASKS, result);
        }
    }
    rt_hw_interrupt_enable(guard);
    if (result == EPD_MEMORY_OK) epd_power_notify();
    return result;
}

epd_memory_result_t app_background_dirty(epd_memory_owner_t owner, uint32_t *out)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (app) {
        if (app->close_prepared) result = EPD_MEMORY_CLOSED;
        else if (app->state.dirty_revision == UINT32_MAX) result = EPD_MEMORY_OVERFLOW;
        else {
            ++app->state.dirty_revision;
            if (out) *out = app->state.dirty_revision;
            background_changed(app);
            result = EPD_MEMORY_OK;
            background_publish(app, EPD_APP_BG_EVENT_PERSISTENCE, result);
        }
    }
    rt_hw_interrupt_enable(guard);
    return result;
}
bool app_background_saved(epd_memory_owner_t owner, uint32_t revision)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    bool saved = app && app->state.dirty_revision == revision;
    if (saved) {
        app->state.saved_revision = revision;
        background_changed(app);
        background_publish(app, EPD_APP_BG_EVENT_PERSISTENCE, EPD_MEMORY_OK);
    }
    rt_hw_interrupt_enable(guard);
    return saved;
}
bool app_background_clean(epd_memory_owner_t owner)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    bool clean = app && app->state.dirty_revision == app->state.saved_revision;
    rt_hw_interrupt_enable(guard);
    return clean;
}

epd_memory_result_t app_background_prepare_close(epd_memory_owner_t owner,
                                                epd_app_background_state_t *terminal)
{
    if (!terminal) return EPD_MEMORY_INVALID;
    char id[BG_ID_SIZE];
    epd_app_background_snapshot_t before;
    rt_base_t guard = rt_hw_interrupt_disable();
    background_app_t *app = background_find_owner(owner);
    if (!app || app->state.owner != owner) {
        rt_hw_interrupt_enable(guard);
        return EPD_MEMORY_INVALID;
    }
    if (app->close_prepared) {
        *terminal = app->cold_prepared ? EPD_APP_BG_COLD : EPD_APP_BG_RELEASED;
        rt_hw_interrupt_enable(guard);
        return EPD_MEMORY_OK;
    }
    before = app->state;
    strcpy(id, app->id);
    rt_hw_interrupt_enable(guard);
    if (before.foreground || before.tasks || before.effective == EPD_APP_BG_TASKS)
        return EPD_MEMORY_WAITING;

    epd_memory_result_t result = app_service_quiesce(id);
    if (result != EPD_MEMORY_OK) return result;
    epd_cold_runtime_snapshot_t cold;
    bool registered = epd_cold_runtime_get(owner, &cold) && cold.registered;
    bool discarded = false;
    if (registered) {
        result = epd_cold_runtime_checkpoint(owner, before.revision, before.dirty_revision);
        bool current = epd_cold_runtime_get(owner, &cold) && cold.registered && !cold.busy &&
                       !cold.restoring && !cold.closing && cold.captured_policy_revision == before.revision &&
                       cold.captured_dirty_revision == before.dirty_revision;
        discarded = current && cold.failed && !cold.durable &&
                    epd_cold_runtime_discardable(owner, before.revision, before.dirty_revision);
        if (discarded) result = EPD_MEMORY_OK;
        else if (result == EPD_MEMORY_OK && (!current || !cold.durable || cold.failed)) result = EPD_MEMORY_WAITING;
    } else if (before.dirty_revision != before.saved_revision) {
        result = EPD_MEMORY_WAITING;
    }

    guard = rt_hw_interrupt_disable();
    app = background_find_owner(owner);
    if (!app || app->state.owner != owner) result = EPD_MEMORY_INVALID;
    else if (app->state.revision != before.revision || app->state.dirty_revision != before.dirty_revision ||
             app->state.foreground || app->state.tasks) result = EPD_MEMORY_WAITING;
    else {
        /* Closing optional data after a failed save is a release, not a commit. */
        app->state.last_result = discarded ? cold.result : result;
        if (result == EPD_MEMORY_OK) {
            if (registered && !discarded) app->state.saved_revision = before.dirty_revision;
            /* Clean state without a checkpoint is rebuilt by normal startup. */
            app->discard_prepared = discarded;
            app->cold_prepared = registered && !discarded && before.effective == EPD_APP_BG_STATE;
            app->close_prepared = true;
            app->state.stopping = true;
            app->allowed = false;
            app->state.actual = EPD_APP_BG_STOPPING;
            app->state.applied_revision = before.revision;
            *terminal = app->cold_prepared ? EPD_APP_BG_COLD : EPD_APP_BG_RELEASED;
            background_changed(app);
        } else if (before.effective == EPD_APP_BG_STATE && !before.stopping) {
            app->state.actual = EPD_APP_BG_RETAINED;
            app->state.applied_revision = before.revision;
        }
        background_publish(app, EPD_APP_BG_EVENT_PERSISTENCE, app->state.last_result);
    }
    rt_hw_interrupt_enable(guard);
    return result;
}
