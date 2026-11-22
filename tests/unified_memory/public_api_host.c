/* Reuse the real resource/core runtime and its allocator/RTOS host ports. */
#define main resource_fixture_main
#include "../unified_resource/runtime_host.c"
#undef main

int epd_operation_runtime_task_prepare(epd_memory_owner_t owner)
{ (void)owner; CHECK(false); return -1; } /* PAGE dispatch needs no TASK binding. */
int rt_event_send(struct rt_event *event, unsigned bits)
{ (void)event; (void)bits; CHECK(false); return 0; }

static void setup_id(const char *id)
{
    epd_memory_backend_config_t config = {EPD_MEMORY_REGION_HCPU_PSRAM, CAPACITY, 0, EPD_MEMORY_BACKEND_NONE};
    CHECK(epd_memory_backend_register(&app_image_psram_memheap, &config, &backend) == EPD_MEMORY_OK);
    CHECK(epd_memory_init() == EPD_MEMORY_OK);
    app = epd_memory_owner_create(id); CHECK(app != EPD_MEMORY_OWNER_INVALID);
    pages[0] = epd_memory_scope_create_trusted(app, EPD_MEMORY_SCOPE_PAGE);
    pages[1] = epd_memory_scope_create_trusted(app, EPD_MEMORY_SCOPE_PAGE);
    service = epd_memory_scope_create_trusted(app, EPD_MEMORY_SCOPE_SERVICE);
    CHECK(epd_resource_runtime_init(NULL) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_page_state(pages[0], 1, true) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_page_state(pages[1], 2, false) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_service_state(service, 3, true) == EPD_RESOURCE_OK);
}

static void scope_is_not_eligibility(void)
{
    epd_resource_context_request_t request = {0};
    epd_resource_activity_t denied = 0;
    epd_memory_owner_snapshot_t before, after;
    epd_memory_owner_t foreign = epd_memory_owner_create("foreign");
    epd_memory_owner_t shared = epd_memory_scope_create_trusted(EPD_MEMORY_OWNER_SYSTEM, EPD_MEMORY_SCOPE_SHARED);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(app);
    CHECK(previous != EPD_MEMORY_OWNER_INVALID);
    CHECK(epd_memory_owner_set_limit(app, backend, 64) == EPD_MEMORY_OK);
    CHECK(!allocate(128));
    epd_memory_owner_t targets[] = {EPD_MEMORY_OWNER_SYSTEM, shared, foreign};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(epd_memory_owner_get(targets[i], &before));
        CHECK(epd_memory_scope_create(targets[i], EPD_MEMORY_SCOPE_SHARED) == EPD_MEMORY_OWNER_INVALID);
        CHECK(epd_memory_owner_enter(targets[i]) == EPD_MEMORY_OWNER_INVALID);
        CHECK(!epd_memory_owner_retain(targets[i]));
        CHECK(!epd_memory_owner_close(targets[i]));
        epd_memory_owner_release(targets[i]);
        epd_memory_owner_leave(targets[i]);
        epd_resource_context_leave(targets[i]);
        CHECK(epd_memory_owner_get(targets[i], &after));
        CHECK(before.references == after.references && before.closed == after.closed);
        CHECK(epd_memory_owner_current() == app && !allocate(128));
    }
    CHECK(epd_memory_scope_create(app, EPD_MEMORY_SCOPE_APP) == EPD_MEMORY_OWNER_INVALID);
    CHECK(epd_memory_scope_create(app, EPD_MEMORY_SCOPE_CREDIT) == EPD_MEMORY_OWNER_INVALID);
    for (unsigned kind = EPD_MEMORY_SCOPE_PAGE; kind <= EPD_MEMORY_SCOPE_SHARED; ++kind) {
        epd_memory_owner_t child = epd_memory_scope_create(app, (epd_memory_scope_kind_t)kind);
        CHECK(child != EPD_MEMORY_OWNER_INVALID);
        CHECK(epd_memory_owner_get(child, &after) && after.account == app);
        CHECK(epd_memory_owner_retain(child)); epd_memory_owner_release(child);
        epd_memory_owner_t guard = epd_memory_owner_enter(child);
        CHECK(guard == app && !allocate(128));
        for (unsigned business = EPD_RESOURCE_OPTIONAL; business <= EPD_RESOURCE_INTERACTIVE; ++business) {
            request.kind = (epd_resource_kind_t)business; request.backend = backend;
            CHECK(epd_resource_context_begin(&request, NULL, &denied) == EPD_RESOURCE_DENIED && !denied);
        }
        CHECK(epd_memory_owner_close(child));
        /* Closed callback/guard cleanup remains legal within the same account. */
        epd_memory_owner_t nested = epd_memory_owner_enter(app);
        CHECK(nested == child); epd_memory_owner_leave(nested);
        CHECK(epd_memory_owner_current() == child);
        epd_memory_owner_leave(guard); epd_memory_owner_release(child);
        CHECK(!epd_memory_owner_get(child, &after));
        CHECK(epd_memory_owner_enter(child) == EPD_MEMORY_OWNER_INVALID);
        CHECK(epd_memory_scope_create(child, EPD_MEMORY_SCOPE_TASK) == EPD_MEMORY_OWNER_INVALID);
    }
    /* Resident service attribution may cross accounts without changing limits. */
    epd_memory_owner_t guard = epd_memory_owner_enter_trusted(shared);
    CHECK(guard == app); void *data = allocate(128); CHECK(data);
    dispose(data); epd_memory_owner_leave_trusted(guard);
    CHECK(epd_memory_owner_current() == app && !allocate(128));
    epd_memory_owner_leave_trusted(previous);
    epd_memory_owner_close_trusted(shared); epd_memory_owner_release_trusted(shared);
    epd_memory_owner_close_trusted(foreign); epd_memory_owner_release_trusted(foreign);
}

static unsigned run_count, disposed_count, retired_count;
static epd_memory_owner_t cleanup_owner;
static void operation_run(epd_operation_t id, void *user)
{
    CHECK(!irq && !mutexes && !user && actor == &actors[1]);
    CHECK(epd_memory_owner_current() == pages[0]); ++run_count;
    void *data = allocate(32); CHECK(data);
    CHECK(epd_operation_complete(id, EPD_OPERATION_OK, 0, data));
}
static bool operation_receive(epd_operation_t id, const epd_operation_snapshot_t *s, void *data, void *user)
{ (void)id; (void)s; (void)data; (void)user; CHECK(false); return true; }
static void operation_dispose(void *data, void *user)
{
    CHECK(!irq && !mutexes && !user && actor == &actors[1]);
    CHECK(epd_memory_owner_current() == cleanup_owner); ++disposed_count; dispose(data);
}
static void operation_retire(void *user)
{ CHECK(!user && epd_memory_owner_current() == cleanup_owner); ++retired_count; }

static void dispatch_boundary(void)
{
    epd_operation_scope_t business = 0, cleanup = 0, foreign = 0, denied = 0;
    epd_operation_t operation = 0;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    CHECK(epd_operation_scope_open(EPD_MEMORY_OWNER_SYSTEM, 1, &foreign));
    epd_memory_owner_leave_trusted(previous);
    previous = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(!epd_operation_scope_open(EPD_MEMORY_OWNER_SYSTEM, 1, &denied) && !denied);
    cleanup_owner = epd_memory_scope_create(pages[0], EPD_MEMORY_SCOPE_CLEANUP);
    CHECK(cleanup_owner != EPD_MEMORY_OWNER_INVALID);
    CHECK(epd_operation_scope_open(pages[0], 1, &business));
    CHECK(epd_operation_scope_open(cleanup_owner, 1, &cleanup));
    epd_operation_spec_t spec = {0};
    spec.scope = business; spec.cleanup_scope = foreign;
    spec.execute.thread = spec.cleanup.thread = &actors[1]; spec.deliver.thread = &actors[0];
    spec.run = operation_run; spec.receive = operation_receive;
    spec.dispose_result = operation_dispose; spec.dispose_user = operation_retire;
    CHECK(!epd_operation_submit(&spec, &operation) && !operation);
    spec.scope = foreign; spec.cleanup_scope = cleanup;
    CHECK(!epd_operation_submit(&spec, &operation) && !operation);
    spec.scope = business;
    CHECK(epd_operation_submit(&spec, &operation));
    epd_resource_provider_spec_t provider_spec = {0}; epd_resource_provider_t provider_id = 0;
    provider_spec.code_owner = EPD_MEMORY_OWNER_SYSTEM;
    provider_spec.load = load; provider_spec.destroy = destroy;
    CHECK(epd_resource_provider_register(&provider_spec, &provider_id) == EPD_RESOURCE_STOPPED && !provider_id);
    epd_memory_owner_leave_trusted(previous);
    actor = &actors[1]; CHECK(epd_operation_dispatch(1) == 1 && run_count == 1);
    CHECK(epd_memory_owner_current() == EPD_MEMORY_OWNER_SYSTEM);
    epd_operation_close_owner(pages[0]);
    for (unsigned i = 0; i < 4; ++i) epd_operation_dispatch(1);
    CHECK(disposed_count == 1 && retired_count == 1 && !physical);
    CHECK(epd_memory_owner_current() == EPD_MEMORY_OWNER_SYSTEM);
    CHECK(epd_operation_owner_drained(pages[0]) && epd_operation_release(operation));
    actor = &actors[0];
    CHECK(epd_operation_scope_close(foreign) && epd_operation_scope_release(foreign));
    epd_memory_owner_release_trusted(cleanup_owner);
}

static void direct_activity_checks_binding(void)
{
    epd_resource_activity_spec_t spec = {0};
    epd_resource_activity_snapshot_t live;
    epd_resource_activity_t denied = 0, parent;
    uint64_t revision = 0;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(pages[0]);
    spec.kind = EPD_RESOURCE_INTERACTIVE; spec.backend = backend;
    CHECK(epd_resource_activity_begin(&spec, NULL, &denied) == EPD_RESOURCE_DENIED);
    epd_memory_owner_leave_trusted(previous);
    parent = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 0); drain();
    CHECK(epd_resource_activity_get(parent, &live) == EPD_RESOURCE_OK && live.state == EPD_RESOURCE_ADMITTED);
    spec = live.requested;
    previous = epd_memory_owner_enter_trusted(pages[1]);
    CHECK(epd_resource_activity_begin(&spec, NULL, &denied) == EPD_RESOURCE_DENIED);
    CHECK(epd_resource_activity_update(parent, &spec, &revision) == EPD_RESOURCE_DENIED);
    epd_memory_owner_leave_trusted(previous);
    previous = epd_memory_owner_enter_trusted(pages[0]);
    spec.kind = EPD_RESOURCE_CONTINUOUS;
    CHECK(epd_resource_activity_begin(&spec, NULL, &denied) == EPD_RESOURCE_DENIED);
    CHECK(epd_resource_activity_update(parent, &spec, &revision) == EPD_RESOURCE_DENIED);
    spec.kind = EPD_RESOURCE_NEARBY; spec.interactive_parent = 0;
    CHECK(epd_resource_activity_begin(&spec, NULL, &denied) == EPD_RESOURCE_DENIED);
    epd_memory_owner_leave_trusted(previous);
    CHECK(epd_resource_runtime_page_state(pages[0], 1, false) == EPD_RESOURCE_OK); drain();
    previous = epd_memory_owner_enter_trusted(pages[0]);
    spec.kind = EPD_RESOURCE_INTERACTIVE;
    CHECK(epd_resource_activity_begin(&spec, NULL, &denied) == EPD_RESOURCE_DENIED);
    CHECK(epd_resource_activity_update(parent, &spec, &revision) == EPD_RESOURCE_DENIED);
    epd_memory_owner_leave_trusted(previous);
    CHECK(!denied && !revision);
}

static void normal_activities(void)
{
    epd_resource_activity_t foreground, continuous, nearby;
    epd_resource_activity_snapshot_t snapshot;
    epd_resource_context_request_t request = {0};
    foreground = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 0);
    continuous = begin(service, EPD_RESOURCE_CONTINUOUS, 64, 0); drain();
    request.kind = EPD_RESOURCE_NEARBY; request.interactive_parent = foreground;
    request.backend = backend; request.minimum_bytes = request.target_bytes = 64;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(epd_resource_context_begin(&request, NULL, &nearby) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(previous); drain();
    epd_resource_activity_t activities[] = {foreground, continuous, nearby};
    epd_memory_owner_t owners[] = {pages[0], service, pages[0]};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(epd_resource_activity_get(activities[i], &snapshot) == EPD_RESOURCE_OK);
        CHECK(snapshot.state == EPD_RESOURCE_ADMITTED && snapshot.guaranteed_bytes == 64);
        previous = epd_memory_owner_enter_trusted(owners[i]);
        epd_memory_owner_t guard = epd_resource_context_enter(activities[i]);
        CHECK(guard != EPD_MEMORY_OWNER_INVALID);
        void *buffer = allocate(64); CHECK(buffer);
        epd_resource_context_leave(guard); epd_memory_owner_leave_trusted(previous);
        dispose(buffer);
    }
}

static void private_grant_counterexample(void)
{
    epd_memory_credit_spec_t spec = {0};
    epd_memory_credit_t credit;
    epd_memory_credit_snapshot_t snapshot;
    CHECK(epd_memory_backend_set_headroom(backend, CAPACITY / 4));
    spec.bytes[backend] = CAPACITY * 3 / 4 + 1;
    spec.priority = EPD_MEMORY_PRIORITY_OPTIONAL;
    CHECK(epd_memory_credit_request(pages[1], &spec, &credit) == EPD_MEMORY_NO_CAPACITY);
    spec.priority = EPD_MEMORY_PRIORITY_INTERACTIVE;
    CHECK(epd_memory_credit_request(pages[1], &spec, &credit) == EPD_MEMORY_OK);
    CHECK(epd_memory_credit_get(credit, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_LIVE);
    CHECK(snapshot.unused[backend] == spec.bytes[backend]);
    CHECK(epd_memory_credit_end(credit)); epd_memory_owner_release_trusted(credit);
    epd_resource_context_request_t request = {0}; epd_resource_activity_t denied = 0;
    request.kind = EPD_RESOURCE_INTERACTIVE; request.backend = backend;
    request.minimum_bytes = request.target_bytes = spec.bytes[backend];
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(pages[1]);
    CHECK(epd_resource_context_begin(&request, NULL, &denied) == EPD_RESOURCE_DENIED);
    epd_memory_owner_leave_trusted(previous);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3); setup_id(argv[1]);
    if (!strcmp(argv[2], "scope")) scope_is_not_eligibility();
    else if (!strcmp(argv[2], "activity")) direct_activity_checks_binding();
    else if (!strcmp(argv[2], "normal")) normal_activities();
    else if (!strcmp(argv[2], "private")) private_grant_counterexample();
    else if (!strcmp(argv[2], "dispatch")) dispatch_boundary();
    else return 2;
    close_all(); free(control_memory); control_memory = NULL;
    printf("PASS public boundary %s %s\n", argv[1], argv[2]); return 0;
}
