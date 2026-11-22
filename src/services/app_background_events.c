#include "app_background_events.h"
#include "platform/epd_operation_runtime.h"
#include <rthw.h>
#include <rtm.h>
#include <mem_section.h>
#include <string.h>

#define SUBSCRIPTION_COUNT 32u
typedef struct {
    epd_app_background_subscription_t id;
    epd_memory_owner_t owner, account;
    epd_operation_executor_t deliver;
    epd_app_background_notify_t notify;
    void *context;
    epd_app_background_event_t latest;
    epd_operation_t operation;
    epd_operation_scope_t business, cleanup;
    bool active, pending, scheduling, cancelled;
} subscription_t;
L2_RET_BSS_SECT_BEGIN(app_background_events)
static subscription_t subscriptions[SUBSCRIPTION_COUNT] L2_RET_BSS_SECT(app_background_events);
L2_RET_BSS_SECT_END
static uint32_t next_subscription;
static unsigned cursor;
static const uint8_t client_key = 0;

static int events_init(void)
{
    memset(subscriptions, 0, sizeof(subscriptions));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(events_init);

/* The owner tree is shallow and the memory manager snapshots are IRQ-safe. */
static bool live_owner(epd_memory_owner_t owner, epd_memory_owner_t account)
{
    for (unsigned depth = 0; depth < 17; ++depth) {
        epd_memory_owner_snapshot_t state;
        if (!epd_memory_owner_get(owner, &state) || state.closed || state.account != account) return false;
        if (owner == account) return true;
        owner = state.parent;
    }
    return false;
}
static bool valid(subscription_t *s)
{
    epd_app_background_event_t current;
    return s->active && live_owner(s->owner, s->account) &&
        app_background_event_snapshot(s->account, &current) &&
        current.instance_generation == s->latest.instance_generation;
}
static void deactivate(subscription_t *s)
{
    s->active = s->pending = false;
    s->notify = NULL; s->context = NULL;
}
static bool same_event(const epd_app_background_event_t *a, const epd_app_background_event_t *b)
{
    const epd_app_background_snapshot_t *x = &a->state, *y = &b->state;
    return a->instance_generation == b->instance_generation && a->result == b->result &&
        a->reason == b->reason && x->owner == y->owner &&
        x->requested.mode == y->requested.mode && x->requested.group == y->requested.group &&
        x->requested.after_tasks == y->requested.after_tasks && x->effective == y->effective &&
        x->actual == y->actual && x->revision == y->revision && x->applied_revision == y->applied_revision &&
        x->dirty_revision == y->dirty_revision && x->saved_revision == y->saved_revision &&
        x->tasks == y->tasks && x->foreground == y->foreground && x->stopping == y->stopping &&
        x->explicit_policy == y->explicit_policy && x->last_result == y->last_result;
}
void app_background_events_publish(const epd_app_background_event_t *event)
{
    bool changed = false;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < SUBSCRIPTION_COUNT; ++i) {
        subscription_t *s = &subscriptions[i];
        if (!s->active || s->account != event->state.owner) continue;
        if (s->latest.instance_generation != event->instance_generation || !live_owner(s->owner, s->account)) {
            deactivate(s); changed = true;
        } else if (!same_event(&s->latest, event)) {
            s->latest = *event; s->pending = true; changed = true;
        }
    }
    rt_hw_interrupt_enable(guard);
    /* The RT event wake is IRQ-safe and only schedules the resident worker. */
    if (changed) epd_operation_runtime_wake();
}
void app_background_events_forget(epd_memory_owner_t owner, uint32_t generation)
{
    bool changed = false;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < SUBSCRIPTION_COUNT; ++i) {
        subscription_t *s = &subscriptions[i];
        if (s->active && s->account == owner && s->latest.instance_generation == generation) {
            deactivate(s); changed = true;
        }
    }
    rt_hw_interrupt_enable(guard);
    if (changed) epd_operation_runtime_wake();
}
static void event_run(epd_operation_t operation, void *context)
{
    (void)context;
    epd_operation_complete(operation, EPD_OPERATION_OK, 0, NULL);
}
static bool event_receive(epd_operation_t operation, const epd_operation_snapshot_t *state,
                           void *result, void *context)
{
    (void)operation; (void)result;
    subscription_t *s = context;
    epd_app_background_notify_t notify = NULL;
    void *user = NULL;
    epd_app_background_event_t event;
    rt_base_t guard = rt_hw_interrupt_disable();
    if (!valid(s)) deactivate(s);
    /* Publication already captured the value. Even if admission rejects the
     * empty run, its scoped delivery can consume the snapshot without retrying. */
    else if (s->pending && (state->status == EPD_OPERATION_OK || state->status == EPD_OPERATION_FAILED) &&
             state->instance_generation == s->latest.instance_generation) {
        event = s->latest; notify = s->notify; user = s->context;
        s->pending = false;
    }
    rt_hw_interrupt_enable(guard);
    if (notify) notify(&event, user);
    return true;
}
static void release_scope(epd_operation_scope_t scope)
{
    if (scope && epd_operation_scope_close(scope)) epd_operation_scope_release(scope);
}
static bool schedule(subscription_t *s, epd_operation_executor_t worker)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    epd_memory_owner_t owner = s->owner;
    uint32_t generation = s->latest.instance_generation;
    epd_operation_executor_t deliver = s->deliver;
    rt_hw_interrupt_enable(guard);
    epd_operation_scope_t business = s->business, cleanup = s->cleanup;
    epd_operation_t operation = 0;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    if (previous != EPD_MEMORY_OWNER_INVALID) {
        if ((business || epd_operation_scope_open(owner, generation, &business)) &&
            (cleanup || epd_operation_scope_open(owner, generation, &cleanup))) {
            epd_operation_spec_t spec = {.scope = business, .cleanup_scope = cleanup,
                .execute = worker, .deliver = deliver, .cleanup = worker,
                .user = s, .run = event_run, .receive = event_receive};
            epd_operation_submit(&spec, &operation);
        }
        epd_memory_owner_leave_trusted(previous);
    }
    guard = rt_hw_interrupt_disable();
    /* Keep partial metadata only while a notification is pending. Rolling it
     * back on every full-pool retry would generate its own capacity wake loop. */
    s->operation = operation; s->business = business; s->cleanup = cleanup;
    s->scheduling = false;
    rt_hw_interrupt_enable(guard);
    return operation != 0;
}
static size_t events_process(size_t budget, void *context)
{
    (void)context;
    epd_operation_executor_t worker, gui, cleanup;
    if (!epd_operation_runtime_executors(&worker, &gui, &cleanup) || rt_thread_self() != worker.thread) return 0;
    size_t done = 0;
    for (unsigned n = 0; n < SUBSCRIPTION_COUNT && done < budget; ++n) {
        subscription_t *s = &subscriptions[cursor++ % SUBSCRIPTION_COUNT];
        /* Only this worker mutates operation/scope handles; registration cannot
         * reuse a slot while it has an operation or a submission in progress. */
        if (s->operation) {
            epd_operation_snapshot_t state;
            if (epd_operation_get(s->operation, &state) && state.state != EPD_OPERATION_FINISHED) {
                rt_base_t guard = rt_hw_interrupt_disable();
                bool closed = !valid(s);
                if (closed) deactivate(s);
                rt_hw_interrupt_enable(guard);
                if (closed && !s->cancelled) { s->cancelled = true; epd_operation_scope_close(s->business); }
                continue;
            }
            epd_operation_release(s->operation);
            release_scope(s->business); release_scope(s->cleanup);
            rt_base_t guard = rt_hw_interrupt_disable();
            s->operation = s->business = s->cleanup = 0;
            s->cancelled = false;
            rt_hw_interrupt_enable(guard);
            ++done;
        }
        rt_base_t guard = rt_hw_interrupt_disable();
        if (!s->id && !s->operation && !s->business && !s->cleanup && !s->scheduling) {
            rt_hw_interrupt_enable(guard);
            continue;
        }
        if (s->active && !valid(s)) deactivate(s);
        bool submit = s->active && s->pending && !s->scheduling;
        if (submit) s->scheduling = true;
        bool discard = !s->active && !s->scheduling;
        if (discard) s->scheduling = true;
        rt_hw_interrupt_enable(guard);
        if (discard) {
            release_scope(s->business); release_scope(s->cleanup);
            guard = rt_hw_interrupt_disable();
            s->business = s->cleanup = s->id = 0;
            s->scheduling = false;
            rt_hw_interrupt_enable(guard);
        }
        if (submit && schedule(s, worker)) ++done;
    }
    return done;
}
epd_memory_result_t epd_app_background_subscribe(epd_app_background_notify_t notify,
    void *context, epd_app_background_subscription_t *out)
{
    if (!notify || !out || !rt_thread_self() || rt_interrupt_get_nest() || rt_critical_level())
        return EPD_MEMORY_INVALID;
    *out = 0;
    epd_operation_executor_t worker, gui, cleanup;
    if (!epd_operation_runtime_executors(&worker, &gui, &cleanup)) return EPD_MEMORY_CLOSED;
    epd_memory_owner_t owner = epd_memory_owner_current();
    epd_memory_owner_snapshot_t memory;
    if (!epd_memory_owner_get(owner, &memory) || !live_owner(owner, memory.account)) return EPD_MEMORY_CLOSED;
    bool page = memory.kind == EPD_MEMORY_SCOPE_PAGE;
    if ((!page && memory.kind != EPD_MEMORY_SCOPE_TASK && memory.kind != EPD_MEMORY_SCOPE_SERVICE) ||
        rt_thread_self() != (page ? gui.thread : worker.thread)) return EPD_MEMORY_INVALID;
    epd_operation_runtime_client_t client = {&client_key, NULL, events_process, NULL};
    if (!epd_operation_runtime_register(&client)) return EPD_MEMORY_NO_METADATA;
    rt_base_t guard = rt_hw_interrupt_disable();
    epd_app_background_event_t event;
    epd_memory_result_t result = EPD_MEMORY_NO_METADATA;
    if (!live_owner(owner, memory.account) || !app_background_event_snapshot(owner, &event)) result = EPD_MEMORY_CLOSED;
    else if (next_subscription == UINT32_MAX) result = EPD_MEMORY_OVERFLOW;
    else for (unsigned i = 0; i < SUBSCRIPTION_COUNT; ++i) {
        subscription_t *s = &subscriptions[i];
        if (s->active && !valid(s)) deactivate(s);
        if (s->active || s->operation || s->scheduling || s->business || s->cleanup) continue;
        memset(s, 0, sizeof(*s));
        s->id = ++next_subscription; s->owner = owner; s->account = memory.account;
        s->deliver = page ? gui : worker;
        s->notify = notify; s->context = context; s->latest = event;
        s->latest.reason = EPD_APP_BG_EVENT_INITIAL;
        s->active = s->pending = true; *out = s->id; result = EPD_MEMORY_OK;
        break;
    }
    rt_hw_interrupt_enable(guard);
    if (result == EPD_MEMORY_OK) epd_operation_runtime_wake();
    return result;
}
epd_memory_result_t epd_app_background_unsubscribe(epd_app_background_subscription_t id)
{
    if (!id || !rt_thread_self() || rt_interrupt_get_nest() || rt_critical_level()) return EPD_MEMORY_INVALID;
    epd_memory_owner_snapshot_t caller;
    if (!epd_memory_owner_get(epd_memory_owner_current(), &caller)) return EPD_MEMORY_CLOSED;
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    rt_base_t guard = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < SUBSCRIPTION_COUNT; ++i) {
        subscription_t *s = &subscriptions[i];
        if (s->id != id || s->account != caller.account || s->deliver.thread != rt_thread_self()) continue;
        deactivate(s); result = EPD_MEMORY_OK; break;
    }
    rt_hw_interrupt_enable(guard);
    if (result == EPD_MEMORY_OK) epd_operation_runtime_wake();
    return result;
}
RTM_EXPORT(epd_app_background_subscribe);
RTM_EXPORT(epd_app_background_unsubscribe);
