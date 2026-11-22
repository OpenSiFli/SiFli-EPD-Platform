#include "events_support.inc"
#include "background.inc"
#include "../../src/services/app_background_events.c"
#include "epd_operation_runtime_internal.h"

static void *control_memory;
static unsigned notifications, service_notifications;
static epd_memory_owner_t subscriber_owner;
static rt_thread_t subscriber_thread;
static epd_app_background_subscription_t subscription;
static epd_app_background_event_t received;
static bool remove_in_callback, change_in_callback, close_in_callback;
static HANDLE callback_entered, concurrent_done;
static bool concurrent_close;
static epd_memory_result_t quiesce_result = EPD_MEMORY_OK;
static int notification_admission;
static epd_cold_runtime_snapshot_t cold_state;
static bool cold_optional, cold_stale;
void *epd_app_realloc(void *old, size_t bytes, epd_app_memory_t region)
{
    CHECK(!old && !control_memory && region == EPD_APP_PSRAM);
    control_memory = calloc(1, bytes); CHECK(control_memory); return control_memory;
}
int test_init_operation_metadata_init(void);
int epd_operation_runtime_task_prepare(epd_memory_owner_t owner)
{ CHECK(!irq && !mutexes && epd_memory_owner_get(owner, &(epd_memory_owner_snapshot_t){0})); return notification_admission; }
int rt_event_send(struct rt_event *event, unsigned bits)
{ (void)bits; ++event->sends; return 0; }
void app_service_background_changed(epd_memory_owner_t owner)
{ (void)owner; ++service_notifications; }
epd_memory_result_t app_service_quiesce(const char *id)
{ CHECK(id && !irq && !mutexes); return quiesce_result; }
bool epd_cold_runtime_get(epd_memory_owner_t owner, epd_cold_runtime_snapshot_t *out)
{ (void)owner; CHECK(!irq); *out = cold_state; return out->registered; }
epd_memory_result_t epd_cold_runtime_checkpoint(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{
    (void)owner; CHECK(!irq && cold_state.registered);
    cold_state.captured_policy_revision = policy;
    cold_state.captured_dirty_revision = cold_stale ? dirty - 1 : dirty;
    return cold_state.result;
}
bool epd_cold_runtime_discardable(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{
    (void)owner; CHECK(!irq);
    return cold_optional && cold_state.captured_policy_revision == policy &&
        cold_state.captured_dirty_revision == dirty;
}

static void policy(epd_app_background_mode_t mode)
{
    epd_app_background_policy_t value = {mode, 0, EPD_APP_BG_RELEASE};
    CHECK(epd_app_background_set(&value, NULL) == EPD_MEMORY_OK);
}
static void notify(const epd_app_background_event_t *event, void *context)
{
    CHECK(context == &notifications && !irq && !mutexes && actor == subscriber_thread);
    CHECK(epd_memory_owner_current() == subscriber_owner);
    epd_memory_owner_snapshot_t owner;
    CHECK(epd_memory_owner_get(subscriber_owner, &owner) && !owner.closed && owner.account == test_app);
    CHECK(event->state.owner == test_app && event->instance_generation);
    received = *event; ++notifications;
    if (callback_entered && notifications == 1) {
        epd_app_background_event_t stable = *event;
        CHECK(SetEvent(callback_entered));
        CHECK(WaitForSingleObject(concurrent_done, 5000) == WAIT_OBJECT_0);
        CHECK(!memcmp(event, &stable, sizeof(stable)));
    }
    if (remove_in_callback) CHECK(epd_app_background_unsubscribe(subscription) == EPD_MEMORY_OK);
    if (change_in_callback) {
        change_in_callback = false;
        epd_app_background_event_t stable = *event;
        policy(EPD_APP_BG_STATE);
        CHECK(!memcmp(event, &stable, sizeof(stable)));
    }
    if (close_in_callback) epd_operation_close_owner(subscriber_owner);
}
static size_t worker_pass(void)
{
    rt_thread_t before = actor; actor = &actors[1];
    size_t done = epd_operation_dispatch(128);
    for (unsigned i = 0; i < 4; ++i) if (clients[i].key) done += clients[i].process(128, clients[i].user);
    actor = before; return done;
}
static void pump(void)
{
    rt_thread_t before = actor;
    for (unsigned pass = 0; pass < 100; ++pass) {
        unsigned wake_before = wakes;
        size_t done = worker_pass();
        actor = &actors[0]; done += epd_operation_dispatch(128); done += epd_resource_runtime_ui(128);
        if (!done && wake_before == wakes) { actor = before; return; }
    }
    CHECK(!"event worker did not quiesce");
}
static void subscribe(void)
{
    CHECK(epd_app_background_subscribe(notify, &notifications, &subscription) == EPD_MEMORY_OK);
}
static void check_idle(void)
{
    unsigned before = wakes;
    for (unsigned i = 0; i < 20; ++i) CHECK(worker_pass() == 0);
    CHECK(before == wakes);
    epd_operation_task_snapshot_t tasks[EPD_OPERATION_CAPACITY];
    CHECK(epd_operation_tasks(tasks) == 0);
}
static void occupy_run(epd_operation_t operation, void *context) { (void)operation; (void)context; }
static bool occupy_receive(epd_operation_t operation, const epd_operation_snapshot_t *state, void *result, void *context)
{ (void)operation; (void)state; (void)result; (void)context; return true; }
static void metadata_capacity(bool scopes_only, bool finished)
{
    epd_operation_scope_t scopes[EPD_OPERATION_SCOPE_CAPACITY];
    unsigned count = scopes_only ? EPD_OPERATION_SCOPE_CAPACITY : 2;
    for (unsigned i = 0; i < count; ++i) CHECK(epd_operation_scope_open(subscriber_owner, 1, &scopes[i]));
    epd_operation_t operations[EPD_OPERATION_CAPACITY];
    if (!scopes_only) {
        epd_operation_spec_t spec = {.scope = scopes[0], .cleanup_scope = scopes[1],
            .execute = {&actors[1], NULL, 0}, .deliver = {&actors[0], NULL, 0},
            .cleanup = {&actors[1], NULL, 0}, .run = occupy_run, .receive = occupy_receive};
        for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) CHECK(epd_operation_submit(&spec, &operations[i]));
    }
    subscribe(); pump(); CHECK(!notifications); check_idle();
    if (finished) {
        for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i)
            CHECK(epd_operation_complete(operations[i], EPD_OPERATION_OK, 0, NULL));
        pump(); CHECK(!notifications);
        for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) {
            epd_operation_snapshot_t state;
            CHECK(epd_operation_get(operations[i], &state) && state.state == EPD_OPERATION_FINISHED);
        }
    }
    unsigned before = wakes;
    if (scopes_only) {
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(epd_operation_scope_close(scopes[i]) && epd_operation_scope_release(scopes[i])); scopes[i] = 0;
        }
    } else {
        if (!finished) CHECK(epd_operation_complete(operations[0], EPD_OPERATION_OK, 0, NULL));
        CHECK(epd_operation_release(operations[0])); operations[0] = 0;
    }
    CHECK(wakes > before); pump(); CHECK(notifications == 1);
    if (!scopes_only) for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) if (operations[i]) {
        if (!finished) CHECK(epd_operation_complete(operations[i], EPD_OPERATION_OK, 0, NULL));
        CHECK(epd_operation_release(operations[i]));
    }
    pump();
    for (unsigned i = 0; i < count; ++i) if (scopes[i])
        CHECK(epd_operation_scope_close(scopes[i]) && epd_operation_scope_release(scopes[i]));
}
static void partial_cancel(void)
{
    epd_operation_scope_t occupied[EPD_OPERATION_SCOPE_CAPACITY - 1];
    for (unsigned i = 0; i < EPD_OPERATION_SCOPE_CAPACITY - 1; ++i)
        CHECK(epd_operation_scope_open(subscriber_owner, 1, &occupied[i]));
    epd_memory_owner_snapshot_t before, after;
    CHECK(epd_memory_owner_get(subscriber_owner, &before));
    subscribe(); pump(); CHECK(!notifications); check_idle();
    CHECK(epd_memory_owner_get(subscriber_owner, &after) && after.references == before.references + 1);
    CHECK(epd_app_background_unsubscribe(subscription) == EPD_MEMORY_OK);
    pump(); CHECK(!notifications);
    CHECK(epd_memory_owner_get(subscriber_owner, &after) && after.references == before.references);
    for (unsigned i = 0; i < EPD_OPERATION_SCOPE_CAPACITY - 1; ++i)
        CHECK(epd_operation_scope_close(occupied[i]) && epd_operation_scope_release(occupied[i]));
    subscribe(); pump(); CHECK(notifications == 1);
}
static DWORD WINAPI concurrent_change(void *unused)
{
    (void)unused; actor = &actors[2];
    CHECK(rt_thread_mem_context_set(NULL, test_app) == 0);
    CHECK(WaitForSingleObject(callback_entered, 5000) == WAIT_OBJECT_0);
    CHECK(epd_app_background_unsubscribe(subscription) == EPD_MEMORY_INVALID);
    if (concurrent_close) {
        epd_operation_close_owner(subscriber_owner);
        CHECK(!epd_operation_owner_drained(subscriber_owner));
    } else policy(EPD_APP_BG_STATE);
    CHECK(SetEvent(concurrent_done));
    return 0;
}
static void checkpoint_event(const char *name)
{
    bool durable = !strcmp(name, "cold_durable");
    bool recovery = !strcmp(name, "cold_recovery");
    bool coalesced = !strcmp(name, "cold_coalesced");
    cold_optional = strcmp(name, "cold_required") && !recovery;
    cold_stale = !strcmp(name, "cold_stale");
    cold_state = (epd_cold_runtime_snapshot_t){.registered = true, .durable = durable,
        .failed = !durable, .result = durable ? EPD_MEMORY_OK : EPD_MEMORY_NO_CAPACITY};
    subscribe(); pump(); CHECK(notifications == 1);
    policy(EPD_APP_BG_STATE);
    CHECK(app_background_foreground(test_app, false) == EPD_MEMORY_OK);
    uint32_t dirty;
    CHECK(app_background_dirty(test_app, &dirty) == EPD_MEMORY_OK && dirty);
    epd_app_background_state_t terminal = EPD_APP_BG_FOREGROUND;
    bool discard = cold_optional && !cold_stale && !durable;
    CHECK(app_background_prepare_close(test_app, &terminal) ==
        (discard || durable ? EPD_MEMORY_OK : EPD_MEMORY_NO_CAPACITY));
    epd_app_background_snapshot_t state;
    CHECK(app_background_snapshot(test_app, &state) == EPD_MEMORY_OK);
    CHECK(state.saved_revision == (durable ? dirty : 0));
    CHECK(state.last_result == cold_state.result);
    if (!coalesced) {
        pump(); CHECK(notifications == 2 && received.reason == EPD_APP_BG_EVENT_PERSISTENCE);
        CHECK(received.result == cold_state.result && received.state.saved_revision == state.saved_revision);
    }
    if (recovery) {
        cold_state.failed = false; cold_state.durable = true; cold_state.result = EPD_MEMORY_OK;
        CHECK(app_background_prepare_close(test_app, &terminal) == EPD_MEMORY_OK);
        pump(); CHECK(notifications == 3 && received.result == EPD_MEMORY_OK);
        CHECK(received.state.saved_revision == dirty); durable = true;
    }
    if (discard || durable) {
        CHECK(terminal == (durable ? EPD_APP_BG_COLD : EPD_APP_BG_RELEASED));
        if (discard) CHECK(app_background_report(test_app, state.revision, EPD_APP_BG_COLD) == EPD_MEMORY_BUSY);
        app_background_stopped(test_app);
        app_background_service_stopped(test_app);
        CHECK(app_background_report(test_app, state.revision, terminal) == EPD_MEMORY_OK);
        pump(); CHECK(received.reason == EPD_APP_BG_EVENT_ACTUAL && received.state.actual == terminal);
        CHECK(received.result == cold_state.result && received.state.last_result == cold_state.result);
        CHECK(received.state.saved_revision == (durable ? dirty : 0));
        CHECK(received.state.dirty_revision == dirty);
        unsigned before = notifications;
        CHECK(app_background_unbind(test_app) == EPD_MEMORY_OK); pump(); CHECK(notifications == before);
    } else {
        CHECK(!state.stopping && state.actual == EPD_APP_BG_RETAINED);
        CHECK(app_background_unbind(test_app) == EPD_MEMORY_BUSY);
    }
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); InitializeCriticalSection(&host_irq_lock);
    setup(); CHECK(test_init_operation_metadata_init() == 0 && events_init() == 0);
    CHECK(app_background_bind("activity-test", test_app) == EPD_MEMORY_OK);
    subscriber_thread = !strcmp(argv[1], "managed_worker") || !strcmp(argv[1], "transport_rejected") ? &actors[1] : &actors[0];
    actor = subscriber_thread; CHECK(rt_thread_mem_context_set(NULL, test_app) == 0);
    subscriber_owner = epd_memory_scope_create(test_app, actor == &actors[0] ? EPD_MEMORY_SCOPE_PAGE : EPD_MEMORY_SCOPE_SERVICE);
    CHECK(subscriber_owner != EPD_MEMORY_OWNER_INVALID);
    CHECK(rt_thread_mem_context_set(NULL, subscriber_owner) == 0);
    epd_memory_owner_snapshot_t baseline; CHECK(epd_memory_owner_get(subscriber_owner, &baseline));
    if (!strcmp(argv[1], "empty_slots") || !strcmp(argv[1], "empty_then_subscribe")) {
        subscription_t before_slots[SUBSCRIPTION_COUNT];
        memcpy(before_slots, subscriptions, sizeof(before_slots));
        actor = &actors[1];
        unsigned before_irq = irq_entries;
        CHECK(events_process(128, NULL) == 0);
        CHECK(irq_entries - before_irq == SUBSCRIPTION_COUNT);
        CHECK(!memcmp(before_slots, subscriptions, sizeof(before_slots)));
        actor = subscriber_thread;
        if (!strcmp(argv[1], "empty_then_subscribe")) {
            subscribe(); pump(); CHECK(notifications == 1);
            CHECK(epd_app_background_unsubscribe(subscription) == EPD_MEMORY_OK);
            pump(); CHECK(notifications == 1);
            subscription = 0;
            for (unsigned i = 0; i < SUBSCRIPTION_COUNT; ++i)
                CHECK(!subscriptions[i].id && !subscriptions[i].business && !subscriptions[i].cleanup);
            subscribe(); pump(); CHECK(notifications == 2);
        }
    } else if (!strcmp(argv[1], "transport_rejected")) {
        notification_admission = EPD_RESOURCE_CAPACITY;
        subscribe(); pump(); CHECK(notifications == 1 && received.reason == EPD_APP_BG_EVENT_INITIAL);
        check_idle(); notification_admission = 0;
        policy(EPD_APP_BG_STATE); pump(); CHECK(notifications == 2 && received.state.effective == EPD_APP_BG_STATE);
    } else if (!strncmp(argv[1], "cold_", 5)) {
        checkpoint_event(argv[1]);
    } else if (!strcmp(argv[1], "race_publish") || !strcmp(argv[1], "race_close")) {
        concurrent_close = !strcmp(argv[1], "race_close");
        callback_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
        concurrent_done = CreateEvent(NULL, TRUE, FALSE, NULL);
        CHECK(callback_entered && concurrent_done);
        subscribe();
        HANDLE publisher = CreateThread(NULL, 0, concurrent_change, NULL, 0, NULL); CHECK(publisher);
        pump(); CHECK(WaitForSingleObject(publisher, 5000) == WAIT_OBJECT_0);
        CHECK(notifications == (concurrent_close ? 1u : 2u));
        if (concurrent_close) CHECK(epd_operation_owner_drained(subscriber_owner));
        else CHECK(received.state.effective == EPD_APP_BG_STATE);
        CloseHandle(publisher); CloseHandle(callback_entered); CloseHandle(concurrent_done);
        callback_entered = concurrent_done = NULL;
    } else if (!strcmp(argv[1], "operation_capacity") || !strcmp(argv[1], "scope_capacity") ||
               !strcmp(argv[1], "operation_finished_capacity")) {
        metadata_capacity(!strcmp(argv[1], "scope_capacity"), !strcmp(argv[1], "operation_finished_capacity"));
    } else if (!strcmp(argv[1], "partial_cancel")) {
        partial_cancel();
    } else if (!strcmp(argv[1], "runtime_not_ready")) {
        executors_ready = false;
        CHECK(epd_app_background_subscribe(notify, NULL, &subscription) == EPD_MEMORY_CLOSED);
        executors_ready = true; subscribe(); pump(); CHECK(notifications == 1);
    } else if (!strcmp(argv[1], "registration")) {
        CHECK(epd_app_background_subscribe(NULL, NULL, &subscription) == EPD_MEMORY_INVALID);
        CHECK(epd_app_background_subscribe(notify, NULL, NULL) == EPD_MEMORY_INVALID);
        for (unsigned i = 0; i < 4; ++i) if (!clients[i].key) {
            clients[i].key = &clients[i]; clients[i].process = NULL;
        }
        CHECK(epd_app_background_subscribe(notify, NULL, &subscription) == EPD_MEMORY_NO_METADATA);
        for (unsigned i = 0; i < 4; ++i) if (clients[i].key == &clients[i]) memset(&clients[i], 0, sizeof(clients[i]));
        subscribe(); pump(); CHECK(notifications == 1);
    } else if (!strcmp(argv[1], "generation_overflow")) {
        background_next_instance = UINT32_MAX;
        CHECK(app_background_bind("activity-test", test_app) == EPD_MEMORY_OK);
        epd_memory_owner_t other = epd_memory_owner_create("other");
        CHECK(app_background_bind("other", other) == EPD_MEMORY_OVERFLOW);
        CHECK(!app_background_event_snapshot(other, &received));
        epd_memory_owner_close_trusted(other); epd_memory_owner_release_trusted(other);
        next_subscription = UINT32_MAX;
        CHECK(epd_app_background_subscribe(notify, NULL, &subscription) == EPD_MEMORY_OVERFLOW);
    } else {
        subscribe(); CHECK(!notifications);
        epd_memory_owner_snapshot_t state; CHECK(epd_memory_owner_get(subscriber_owner, &state));
        CHECK(state.references == baseline.references);
        if (!strcmp(argv[1], "close_after_submit")) { worker_pass(); CHECK(!notifications); }
        if (!strcmp(argv[1], "unsubscribe_ready") || !strcmp(argv[1], "close_ready")) {
            worker_pass(); worker_pass(); CHECK(!notifications);
        }
        if (!strcmp(argv[1], "unsubscribe_queued") || !strcmp(argv[1], "unsubscribe_ready")) {
            CHECK(epd_app_background_unsubscribe(subscription) == EPD_MEMORY_OK); pump(); CHECK(!notifications);
        } else if (!strcmp(argv[1], "close_queued") || !strcmp(argv[1], "close_ready") ||
                   !strcmp(argv[1], "close_after_submit") || !strcmp(argv[1], "root_close")) {
            epd_operation_close_owner(!strcmp(argv[1], "root_close") ? test_app : subscriber_owner);
            pump(); CHECK(!notifications && epd_operation_owner_drained(subscriber_owner));
        } else {
            remove_in_callback = !strcmp(argv[1], "self_remove");
            change_in_callback = !strcmp(argv[1], "publish_in_callback");
            close_in_callback = !strcmp(argv[1], "close_in_callback");
            pump(); CHECK(notifications == (!strcmp(argv[1], "publish_in_callback") ? 2u : 1u));
            if (!strcmp(argv[1], "self_remove")) {
                policy(EPD_APP_BG_STATE); pump(); CHECK(notifications == 1);
            } else if (!strcmp(argv[1], "worker")) {
                actor = &actors[2]; CHECK(rt_thread_mem_context_set(NULL, test_app) == 0);
                policy(EPD_APP_BG_STATE); CHECK(notifications == 1); pump(); CHECK(notifications == 2);
            } else if (!strcmp(argv[1], "coalesce")) {
                policy(EPD_APP_BG_STATE); policy(EPD_APP_BG_RELEASE); policy(EPD_APP_BG_STATE);
                CHECK(notifications == 1); pump(); CHECK(notifications == 2 && received.state.revision == 4);
                CHECK(received.reason == EPD_APP_BG_EVENT_POLICY && received.state.effective == EPD_APP_BG_STATE);
            } else if (!strcmp(argv[1], "duplicate")) {
                CHECK(app_background_foreground(test_app, true) == EPD_MEMORY_OK); pump(); unsigned delivered = notifications;
                for (unsigned i = 0; i < 20; ++i) CHECK(app_background_foreground(test_app, true) == EPD_MEMORY_OK);
                pump(); CHECK(notifications == delivered);
            } else if (!strcmp(argv[1], "foreign") || !strcmp(argv[1], "bad_lane")) {
                epd_memory_owner_t other = epd_memory_owner_create("foreign"); CHECK(other != EPD_MEMORY_OWNER_INVALID);
                if (!strcmp(argv[1], "foreign")) CHECK(rt_thread_mem_context_set(NULL, other) == 0);
                else { actor = &actors[2]; CHECK(rt_thread_mem_context_set(NULL, subscriber_owner) == 0); }
                CHECK(epd_app_background_unsubscribe(subscription) == EPD_MEMORY_INVALID);
                epd_app_background_subscription_t denied;
                CHECK(epd_app_background_subscribe(notify, NULL, &denied) == EPD_MEMORY_INVALID);
                actor = subscriber_thread; CHECK(rt_thread_mem_context_set(NULL, subscriber_owner) == 0);
                epd_memory_owner_close_trusted(other); epd_memory_owner_release_trusted(other);
                policy(EPD_APP_BG_STATE); pump(); CHECK(notifications == 2);
            } else if (!strcmp(argv[1], "old_handle")) {
                epd_app_background_subscription_t old = subscription;
                CHECK(epd_app_background_unsubscribe(old) == EPD_MEMORY_OK); pump(); subscribe(); CHECK(subscription != old);
                CHECK(epd_app_background_unsubscribe(old) == EPD_MEMORY_INVALID); pump(); CHECK(notifications == 2);
            } else if (!strcmp(argv[1], "quota")) {
                epd_app_background_subscription_t all[SUBSCRIPTION_COUNT]; all[0] = subscription;
                for (unsigned i = 1; i < SUBSCRIPTION_COUNT; ++i)
                    CHECK(epd_app_background_subscribe(notify, &notifications, &all[i]) == EPD_MEMORY_OK);
                epd_app_background_subscription_t denied;
                CHECK(epd_app_background_subscribe(notify, NULL, &denied) == EPD_MEMORY_NO_METADATA);
                for (unsigned i = 0; i < SUBSCRIPTION_COUNT; ++i) CHECK(epd_app_background_unsubscribe(all[i]) == EPD_MEMORY_OK);
                pump(); subscribe(); pump(); CHECK(notifications == 2);
            } else if (!strcmp(argv[1], "sequence")) {
                background_app_t *bound = background_find_owner(test_app);
                bound->state.event_revision = UINT32_MAX; bound->event_sequence = UINT64_MAX;
                policy(EPD_APP_BG_STATE); pump(); CHECK(notifications == 2 && received.sequence == UINT64_MAX);
                policy(EPD_APP_BG_RELEASE); pump(); CHECK(notifications == 3 && received.state.effective == EPD_APP_BG_RELEASE);
            } else if (!strcmp(argv[1], "rejected")) {
                epd_app_background_policy_t invalid = {EPD_APP_BG_TASKS, 999, EPD_APP_BG_STATE};
                CHECK(epd_app_background_set(&invalid, NULL) == EPD_MEMORY_INVALID);
                pump(); CHECK(notifications == 2 && received.state.revision == 1 && received.reason == EPD_APP_BG_EVENT_REJECTED);
            } else if (!strcmp(argv[1], "stale_report")) {
                policy(EPD_APP_BG_STATE); pump(); CHECK(notifications == 2);
                CHECK(app_background_report(test_app, 1, EPD_APP_BG_RELEASED) == EPD_MEMORY_INVALID);
                pump(); CHECK(notifications == 2 && received.state.actual == EPD_APP_BG_FOREGROUND);
            } else if (!strcmp(argv[1], "persist")) {
                uint32_t dirty; CHECK(app_background_dirty(test_app, &dirty) == EPD_MEMORY_OK); pump();
                CHECK(received.reason == EPD_APP_BG_EVENT_PERSISTENCE && received.state.dirty_revision == dirty);
                CHECK(app_background_saved(test_app, dirty)); pump(); CHECK(received.state.saved_revision == dirty);
                policy(EPD_APP_BG_STATE); CHECK(app_background_foreground(test_app, false) == EPD_MEMORY_OK);
                epd_app_background_state_t terminal;
                CHECK(app_background_prepare_close(test_app, &terminal) == EPD_MEMORY_OK); pump();
                CHECK(received.state.actual == EPD_APP_BG_STOPPING && terminal == EPD_APP_BG_RELEASED);
            } else if (!strcmp(argv[1], "tasks")) {
                epd_app_task_group_t group; epd_memory_owner_t task;
                CHECK(epd_app_task_group_create(&group) == EPD_MEMORY_OK && epd_app_task_begin(group, &task) == EPD_MEMORY_OK);
                epd_app_background_policy_t value = {EPD_APP_BG_TASKS, group, EPD_APP_BG_STATE};
                CHECK(epd_app_background_set(&value, NULL) == EPD_MEMORY_OK); pump(); CHECK(received.state.tasks == 1);
                CHECK(epd_app_task_end(task)); pump(); CHECK(received.state.tasks == 0 && received.state.effective == EPD_APP_BG_STATE);
                CHECK(epd_app_task_group_release(group) == EPD_MEMORY_OK);
            } else if (!strcmp(argv[1], "rebind")) {
                uint32_t generation = received.instance_generation;
                app_background_stopped(test_app);
                epd_app_background_snapshot_t background_state;
                CHECK(app_background_snapshot(test_app, &background_state) == EPD_MEMORY_OK);
                CHECK(app_background_report(test_app, background_state.revision, EPD_APP_BG_RELEASED) == EPD_MEMORY_OK);
                CHECK(app_background_unbind(test_app) == EPD_MEMORY_OK); pump(); CHECK(notifications == 1);
                CHECK(app_background_should_start("activity-test", true));
                CHECK(app_background_bind("activity-test", test_app) == EPD_MEMORY_OK); subscribe(); pump();
                CHECK(notifications == 2 && received.instance_generation != generation);
            }
        }
    }
    actor = subscriber_thread; CHECK(rt_thread_mem_context_set(NULL, subscriber_owner) == 0);
    if (subscription) epd_app_background_unsubscribe(subscription);
    pump(); check_idle();
    epd_memory_owner_snapshot_t final; CHECK(epd_memory_owner_get(subscriber_owner, &final));
    /* Other simulated threads may have acquired a context reference. */
    for (unsigned i = 0; i < 4; ++i) if (&actors[i] != subscriber_thread)
        CHECK(rt_thread_mem_context_set(&actors[i], EPD_MEMORY_OWNER_SYSTEM) == 0);
    CHECK(epd_memory_owner_get(subscriber_owner, &final) && final.references == baseline.references);
    epd_operation_close_owner(subscriber_owner); pump(); CHECK(epd_operation_owner_drained(subscriber_owner));
    epd_memory_metadata_snapshot_t metadata; epd_memory_metadata_snapshot(&metadata);
    CHECK(!metadata.protocol_errors && !irq && !mutexes);
    printf("PASS background events %s (%u ticks/s)\n", argv[1], RT_TICK_PER_SECOND);
    return 0;
}
