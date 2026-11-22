#include "epd_resource_runtime.h"
#include "epd_operation_runtime.h"
#include "reader_page_resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CAPACITY ((size_t)1048576)
struct rt_memheap app_image_psram_memheap;
static struct rt_thread actors[2];
static rt_thread_t actor = &actors[0];
static unsigned irq, mutexes, worker_wakes, ui_wakes, loaded, freed;
static uint32_t ticks;
static size_t physical;
static bool delay_load;
static epd_resource_t pending_resource;
static epd_resource_credit_t pending_credit;
static epd_memory_backend_t backend;
static epd_memory_owner_t app, pages[2], service;
static epd_operation_runtime_client_t client;
static struct { void *pointer; size_t bytes; epd_memory_owner_t token; } blocks[128];
static void *control_memory;
void *epd_app_realloc(void *old, size_t bytes, int region)
{
    CHECK(!old && region == 1 && epd_memory_owner_current() == EPD_MEMORY_OWNER_SYSTEM);
    CHECK(!irq && !mutexes && !control_memory && bytes > 4096 && bytes < 8192);
    control_memory = calloc(1, bytes); CHECK(control_memory); return control_memory;
}

rt_thread_t rt_thread_self(void) { return actor; }
rt_tick_t rt_tick_get(void) { return ticks; }
unsigned rt_interrupt_get_nest(void) { return 0; }
unsigned rt_critical_level(void) { return 0; }
rt_base_t rt_hw_interrupt_disable(void) { return (rt_base_t)irq++; }
void rt_hw_interrupt_enable(rt_base_t level) { CHECK(irq > (unsigned)level); irq = (unsigned)level; }
uint32_t rt_thread_mem_context_get(rt_thread_t thread) { return (thread ? thread : actor)->context; }
int rt_thread_mem_context_set(rt_thread_t thread, uint32_t token)
{
    rt_thread_t t = thread ? thread : actor;
    epd_memory_owner_t old = t->context;
    if (!epd_memory_owner_retain_trusted(token)) return -1;
    t->context = token; epd_memory_owner_release_trusted(old); return 0;
}
int rt_mutex_init(struct rt_mutex *m, const char *name, int flags)
{ (void)name; (void)flags; m->held = 0; return 0; }
int rt_mutex_take(struct rt_mutex *m, int timeout)
{ (void)timeout; CHECK(!m->held && !irq); m->held = 1; ++mutexes; return 0; }
int rt_mutex_release(struct rt_mutex *m)
{ CHECK(m->held && mutexes); m->held = 0; --mutexes; return 0; }
int rt_mutex_detach(struct rt_mutex *m) { CHECK(!m->held); return 0; }
void epd_power_notify(void) { CHECK(!mutexes); ++ui_wakes; }
/* The core notifier may send the kernel event while the resource mutex is held.
 * It performs no allocation, mutex acquisition or application callback. */
void epd_operation_runtime_wake(void) { ++worker_wakes; }
bool epd_operation_runtime_register(const epd_operation_runtime_client_t *c)
{ CHECK(!client.key); client = *c; epd_operation_runtime_wake(); return true; }
bool epd_operation_runtime_executors(epd_operation_executor_t *work,
                                     epd_operation_executor_t *ui, epd_operation_executor_t *cleanup)
{
    *work = (epd_operation_executor_t){&actors[1], NULL, 0};
    *cleanup = *work; *ui = (epd_operation_executor_t){&actors[0], NULL, 0}; return true;
}
bool epd_memory_app_backend(epd_memory_region_t region, epd_memory_backend_t *out)
{
    if (!out || region != EPD_MEMORY_REGION_HCPU_PSRAM) return false;
    return epd_memory_backend_find(&app_image_psram_memheap, out);
}
epd_memory_result_t epd_memory_block_get(const void *ptr, epd_memory_block_snapshot_t *out)
{
    for (unsigned i = 0; i < 128; ++i) if (blocks[i].pointer == ptr) {
        epd_memory_owner_snapshot_t owner;
        CHECK(epd_memory_owner_get(blocks[i].token, &owner));
        out->owner = blocks[i].token; out->account = owner.account; out->backend = backend;
        out->size = out->physical = blocks[i].bytes; return EPD_MEMORY_OK;
    }
    return EPD_MEMORY_INVALID;
}
epd_memory_result_t epd_memory_block_transfer(void *ptr, epd_memory_owner_t to)
{
    for (unsigned i = 0; i < 128; ++i) if (blocks[i].pointer == ptr) {
        epd_memory_result_t result = epd_memory_transfer(blocks[i].token, to, backend,
                                                       blocks[i].bytes, blocks[i].bytes);
        if (result == EPD_MEMORY_OK) blocks[i].token = to;
        return result;
    }
    return EPD_MEMORY_INVALID;
}
static void *allocate(size_t bytes)
{
    unsigned i;
    epd_memory_change_t change = {0};
    void *data;
    for (i = 0; i < 128 && blocks[i].pointer; ++i) {}
    CHECK(i < 128 && bytes <= CAPACITY - physical);
    change.kind = EPD_MEMORY_CHANGE_ALLOC; change.backend = backend;
    change.token = epd_memory_credit_current();
    if (change.token == EPD_MEMORY_OWNER_INVALID) change.token = epd_memory_owner_current();
    change.size = change.physical = bytes;
    change.available_before = CAPACITY - physical; change.available_after = change.available_before - bytes;
    data = malloc(bytes); CHECK(data);
    if (epd_memory_try_change(&change) != EPD_MEMORY_OK) { free(data); return NULL; }
    epd_memory_notify(&change); blocks[i].pointer = data; blocks[i].bytes = bytes; blocks[i].token = change.token;
    physical += bytes; return data;
}
static void dispose(void *data)
{
    epd_memory_change_t change = {0}; unsigned i;
    CHECK(!mutexes && !irq);
    for (i = 0; i < 128 && blocks[i].pointer != data; ++i) {}
    CHECK(i < 128);
    change.kind = EPD_MEMORY_CHANGE_FREE; change.token = blocks[i].token; change.backend = backend;
    change.old_size = change.old_physical = blocks[i].bytes;
    change.available_before = CAPACITY - physical;
    physical -= blocks[i].bytes; change.available_after = CAPACITY - physical;
    free(data); blocks[i].pointer = NULL; epd_memory_notify(&change); ++freed;
}
static void drain(void)
{
    unsigned i;
    for (i = 0; i < 100; ++i) {
        size_t n;
        actor = &actors[1]; n = client.process(32, client.user);
        actor = &actors[0]; n += epd_resource_runtime_ui(32);
        if (!n) return;
    }
    CHECK(!"runtime failed to quiesce");
}
static void setup(void)
{
    epd_memory_backend_config_t config = {EPD_MEMORY_REGION_HCPU_PSRAM, CAPACITY, 0, EPD_MEMORY_BACKEND_NONE};
    actors[0].id = 1; actors[1].id = 2;
    CHECK(epd_memory_backend_register(&app_image_psram_memheap, &config, &backend) == EPD_MEMORY_OK);
    CHECK(epd_memory_init() == EPD_MEMORY_OK);
    app = epd_memory_owner_create("same-policy-for-every-app"); CHECK(app != EPD_MEMORY_OWNER_INVALID);
    pages[0] = epd_memory_scope_create_trusted(app, EPD_MEMORY_SCOPE_PAGE);
    pages[1] = epd_memory_scope_create_trusted(app, EPD_MEMORY_SCOPE_PAGE);
    service = epd_memory_scope_create_trusted(app, EPD_MEMORY_SCOPE_SERVICE);
    CHECK(epd_resource_runtime_init(NULL) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_page_state(pages[0], 1, true) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_page_state(pages[1], 2, true) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_task_state(service, 3, true) == EPD_RESOURCE_OK);
}
static epd_resource_activity_t begin(epd_memory_owner_t owner, epd_resource_kind_t kind, size_t bytes, uint64_t deadline)
{
    epd_resource_context_request_t request = {0}; epd_resource_activity_t activity;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    CHECK(previous != EPD_MEMORY_OWNER_INVALID);
    request.kind = kind; request.backend = backend; request.minimum_bytes = request.target_bytes = bytes; request.deadline = deadline;
    CHECK(epd_resource_context_begin(&request, NULL, &activity) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(previous); return activity;
}
static int load(void *ctx, epd_resource_t resource, const epd_resource_descriptor_t *d, epd_resource_credit_t credit)
{
    void *data;
    (void)ctx; CHECK(actor == &actors[1] && !mutexes && !irq);
    CHECK(epd_memory_credit_current() == credit);
    if (delay_load) { pending_resource = resource; pending_credit = credit; ++loaded; return EPD_RESOURCE_PENDING; }
    data = allocate(d->estimated_bytes); CHECK(data); ++loaded;
    CHECK(epd_resource_load_complete(resource, EPD_RESOURCE_OK, data, d->estimated_bytes) == EPD_RESOURCE_OK);
    return EPD_RESOURCE_OK;
}
static int destroy(void *ctx, epd_resource_t resource, void *data, size_t bytes)
{ (void)ctx; (void)resource; (void)bytes; CHECK(actor == &actors[0]); dispose(data); return 0; }
static epd_resource_provider_t provider(epd_memory_owner_t page)
{
    epd_resource_provider_spec_t p = {0}; epd_resource_provider_t out;
    p.code_owner = app; p.lifetime_owner = page; p.implementation_version = 1;
    p.load_lane = EPD_RESOURCE_WORKER; p.destroy_lane = EPD_RESOURCE_UI; p.load = load; p.destroy = destroy;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(page);
    CHECK(epd_resource_provider_register(&p, &out) == EPD_RESOURCE_OK);
    epd_memory_owner_leave_trusted(previous); return out;
}
static epd_resource_ref_t acquire(epd_resource_activity_t activity, epd_resource_provider_t p, epd_memory_owner_t owner)
{
    epd_resource_descriptor_t d = {0}; epd_resource_ref_t ref;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    d.backend = backend; d.estimated_bytes = 64; d.key.bytes[0] = 1; d.key.length = 1; d.key.version = 1; d.discardable = true;
    CHECK(epd_resource_acquire(activity, p, &d, NULL, NULL, EPD_RESOURCE_UI, &ref) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(previous); return ref;
}
static void close_all(void)
{
    epd_memory_metadata_snapshot_t metadata; epd_resource_stats_t stats;
    CHECK(epd_resource_runtime_close_owner(app) == EPD_RESOURCE_ACCEPTED); drain();
    CHECK(!epd_resource_runtime_owner_busy(app));
    epd_memory_owner_release_trusted(pages[0]); epd_memory_owner_release_trusted(pages[1]); epd_memory_owner_release_trusted(service); epd_memory_owner_release_trusted(app);
    epd_memory_metadata_snapshot(&metadata); epd_resource_stats(&stats);
    CHECK(!physical && !mutexes && !irq && !metadata.protocol_errors);
    CHECK(!stats.activities && !stats.providers && !stats.resources && !stats.references);
    CHECK(metadata.descriptors_used == 2); /* SYSTEM plus resident shared scope. */
}
static void exact_page_close(void)
{
    epd_resource_activity_t a, b, c; epd_resource_provider_t p;
    epd_resource_ref_t ref; epd_resource_activity_snapshot_t s; void *data;
    setup(); a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 0, 0);
    b = begin(pages[1], EPD_RESOURCE_INTERACTIVE, 0, 0); c = begin(service, EPD_RESOURCE_CONTINUOUS, 0, 0);
    p = provider(pages[0]); ref = acquire(a, p, pages[0]); drain();
    CHECK(loaded == 1 && physical == 64 && epd_resource_pin(ref, &data) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_close_owner(pages[0]) == EPD_RESOURCE_ACCEPTED); drain();
    CHECK(epd_resource_runtime_owner_busy(pages[0]) && !freed);
    CHECK(epd_resource_activity_get(b, &s) == 0 && s.state == EPD_RESOURCE_ADMITTED);
    CHECK(epd_resource_activity_get(c, &s) == 0 && s.state == EPD_RESOURCE_ADMITTED);
    CHECK(epd_resource_unpin(ref) == EPD_RESOURCE_OK); drain();
    CHECK(!epd_resource_runtime_owner_busy(pages[0]) && freed == 1);
    close_all();
}
static void pending_does_not_spin(void)
{
    epd_memory_credit_spec_t all = {0}; epd_memory_credit_t held;
    epd_resource_activity_t a; epd_resource_activity_snapshot_t s; unsigned wakes;
    setup(); all.bytes[backend] = CAPACITY;
    CHECK(epd_memory_credit_request(service, &all, &held) == EPD_MEMORY_OK);
    a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 100);
    drain(); CHECK(epd_resource_activity_get(a, &s) == 0 && s.state == EPD_RESOURCE_WAITING);
    wakes = worker_wakes; actor = &actors[1];
    CHECK(client.process(32, NULL) == 0 && worker_wakes == wakes);
    CHECK(client.next_timeout_ticks(UINT32_MAX, NULL) == (100u * RT_TICK_PER_SECOND + 999u) / 1000u);
    ticks = (100u * RT_TICK_PER_SECOND + 999u) / 1000u;
    drain(); CHECK(epd_resource_activity_get(a, &s) == 0 && s.state == EPD_RESOURCE_REJECTED);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held); close_all();
}
static void expired_pending_with_loading(bool resume)
{
    epd_memory_credit_spec_t all = {0}; epd_memory_credit_t held;
    epd_resource_activity_snapshot_t snapshot;
    epd_resource_context_request_t request = {0};
    size_t unused[EPD_MEMORY_BACKEND_COUNT] = {0};
    epd_memory_metadata_snapshot_t before, after;
    epd_memory_owner_t previous;
    uint64_t revision;
    setup(); delay_load = true;
    epd_resource_activity_t loading = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 0);
    drain(); epd_resource_provider_t p = provider(app);
    epd_resource_ref_t ref = acquire(loading, p, pages[0]); drain();
    CHECK(pending_resource && loaded == 1 && !physical);
    epd_resource_activity_t waiting = 0;
    if (resume) {
        waiting = begin(pages[1], EPD_RESOURCE_INTERACTIVE, 64, 0); drain();
        previous = epd_memory_owner_enter_trusted(pages[1]);
        CHECK(epd_resource_activity_pause(waiting) == EPD_RESOURCE_ACCEPTED);
        epd_memory_owner_leave_trusted(previous); drain();
        CHECK(epd_resource_activity_get(waiting, &snapshot) == 0 && snapshot.state == EPD_RESOURCE_PAUSED);
    }
    all.bytes[backend] = CAPACITY - 64;
    CHECK(epd_memory_credit_request(service, &all, &held) == EPD_MEMORY_OK);
    epd_memory_metadata_snapshot(&before);
    if (resume) {
        request.kind = EPD_RESOURCE_INTERACTIVE; request.backend = backend;
        request.minimum_bytes = request.target_bytes = unused[backend] = 64;
        request.deadline = 100;
        previous = epd_memory_owner_enter_trusted(pages[1]);
        CHECK(epd_resource_context_resume(waiting, &request, unused, &revision) == EPD_RESOURCE_ACCEPTED);
        epd_memory_owner_leave_trusted(previous);
    } else waiting = begin(pages[1], EPD_RESOURCE_INTERACTIVE, 64, 100);
    drain(); CHECK(epd_resource_activity_get(waiting, &snapshot) == 0);
    CHECK(snapshot.state == (resume ? EPD_RESOURCE_RESUMING : EPD_RESOURCE_WAITING));
    ticks = (100u * RT_TICK_PER_SECOND + 999u) / 1000u;
    /* A started asynchronous loader must remain asleep after an unrelated timeout. */
    drain(); CHECK(epd_resource_activity_get(waiting, &snapshot) == 0);
    CHECK(snapshot.state == (resume ? EPD_RESOURCE_PAUSED : EPD_RESOURCE_REJECTED));
    epd_memory_metadata_snapshot(&after);
    CHECK(after.descriptors_used == before.descriptors_used);
    unsigned wakes = worker_wakes; actor = &actors[1];
    CHECK(client.next_timeout_ticks(UINT32_MAX, NULL) == UINT32_MAX);
    CHECK(client.process(32, NULL) == 0 && worker_wakes == wakes && loaded == 1);
    CHECK(epd_resource_load_complete(pending_resource, EPD_RESOURCE_DENIED, NULL, 0) == EPD_RESOURCE_OK);
    drain(); CHECK(epd_resource_release(ref) == EPD_RESOURCE_OK);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held); close_all();
}
static void expired_wait_does_not_spin(void) { expired_pending_with_loading(false); }
static void expired_resume_does_not_spin(void) { expired_pending_with_loading(true); }
static void helper_proof_and_demotion(void)
{
    epd_resource_context_request_t request = {0}; epd_resource_activity_t a, fake;
    epd_resource_activity_snapshot_t s; epd_memory_owner_t previous, invented;
    setup(); request.kind = EPD_RESOURCE_INTERACTIVE; request.backend = backend;
    invented = epd_memory_scope_create_trusted(app, EPD_MEMORY_SCOPE_PAGE);
    previous = epd_memory_owner_enter_trusted(invented);
    CHECK(epd_resource_context_begin(&request, NULL, &fake) == EPD_RESOURCE_DENIED); epd_memory_owner_leave_trusted(previous);
    previous = epd_memory_owner_enter_trusted(service);
    CHECK(epd_resource_context_begin(&request, NULL, &fake) == EPD_RESOURCE_DENIED); epd_memory_owner_leave_trusted(previous);
    a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 0, 0); drain();
    CHECK(epd_resource_runtime_page_state(pages[0], 1, false) == 0); drain();
    CHECK(epd_resource_activity_get(a, &s) == 0 && s.effective.kind == EPD_RESOURCE_OPTIONAL);
    previous = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(epd_resource_context_begin(&request, NULL, &fake) == EPD_RESOURCE_DENIED); epd_memory_owner_leave_trusted(previous);
    epd_memory_owner_close_trusted(invented); epd_memory_owner_release_trusted(invented); close_all();
}
static void real_reader_provider(void)
{
    reader_page_cache_t cache = {0}; epd_reader_page_t *source; reader_cached_page_t *p;
    epd_memory_owner_t previous; epd_resource_stats_t stats;
    setup(); cache.allocate = allocate; cache.release = dispose; cache.budget = 16384;
    reader_page_resource_attach(&cache); source = calloc(1, sizeof(*source)); CHECK(source);
    source->done = source->collect = 1; source->bytes = 4; memcpy(source->text, "page", 4);
    previous = epd_memory_owner_enter_trusted(pages[0]);
    p = reader_page_cache_put(&cache, 1, 7, source); CHECK(p && p->resource);
    p = reader_page_cache_put(&cache, 20, 7, source); CHECK(p && p->resource);
    epd_memory_owner_leave_trusted(previous); drain();
    epd_resource_stats(&stats); CHECK(stats.resources == 2 && stats.physical_bytes == physical);
    CHECK(epd_resource_reclaim(backend, 1, 1) > 0); drain(); CHECK(cache.count == 1 && freed == 1);
    previous = epd_memory_owner_enter_trusted(pages[0]); reader_page_cache_close(&cache); epd_memory_owner_leave_trusted(previous);
    drain(); CHECK(!cache.first && !cache.count && !physical && freed == 2);
    free(source); close_all();
}
static void full_credit_share(void)
{
    epd_resource_activity_t a, b; epd_resource_provider_t p; epd_resource_ref_t ar, br;
    epd_resource_activity_snapshot_t snapshot; epd_memory_credit_snapshot_t credit; void *data;
    setup(); CHECK(epd_memory_owner_set_limit(app, backend, CAPACITY) == EPD_MEMORY_OK);
    a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, CAPACITY, 0);
    b = begin(pages[1], EPD_RESOURCE_INTERACTIVE, 0, 0); drain();
    CHECK(epd_resource_activity_get(a, &snapshot) == 0 && snapshot.state == EPD_RESOURCE_ADMITTED);
    p = provider(app); ar = acquire(a, p, pages[0]); br = acquire(b, p, pages[1]); drain();
    CHECK(loaded == 1 && physical == 64 && epd_resource_pin(br, &data) == 0);
    CHECK(epd_memory_credit_get((epd_memory_credit_t)snapshot.credit, &credit) && credit.unused[backend] == CAPACITY - 64);
    CHECK(epd_resource_activity_dispose(a) == EPD_RESOURCE_ACCEPTED); drain();
    CHECK(epd_resource_pin(br, &data) == 0 && physical == 64);
    CHECK(epd_resource_unpin(br) == 0 && epd_resource_unpin(br) == 0);
    (void)ar; close_all();
}
static void default_replace_delta(void)
{
    epd_resource_activity_t a; epd_resource_activity_snapshot_t old, next;
    epd_resource_context_request_t request = {0}; uint64_t revision;
    epd_memory_owner_t previous;
    setup(); a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, CAPACITY - 64, 0); drain();
    CHECK(epd_resource_activity_get(a, &old) == 0 && old.state == EPD_RESOURCE_ADMITTED);
    request.kind = EPD_RESOURCE_INTERACTIVE; request.backend = backend;
    request.minimum_bytes = request.target_bytes = CAPACITY;
    previous = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(epd_resource_context_update(a, &request, &revision) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(previous); drain();
    CHECK(epd_resource_activity_get(a, &next) == 0 && next.effective_revision == revision && next.credit != old.credit);
    old = next; request.minimum_bytes = request.target_bytes = CAPACITY + 1;
    previous = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(epd_resource_context_update(a, &request, &revision) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(previous); drain();
    CHECK(epd_resource_activity_get(a, &next) == 0 && next.credit == old.credit && next.effective_revision == old.effective_revision);
    close_all();
}
static void ordinary_free_wakes_waiter(void)
{
    setup();
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(service);
    void *block = allocate(CAPACITY); CHECK(block); epd_memory_owner_leave_trusted(previous);
    epd_resource_activity_t activity = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 1000);
    drain(); epd_resource_activity_snapshot_t snapshot;
    CHECK(epd_resource_activity_get(activity, &snapshot) == 0 && snapshot.state == EPD_RESOURCE_WAITING);
    unsigned wakes = worker_wakes; uint32_t epoch = epd_memory_capacity_epoch();
    dispose(block); epd_memory_dispatch_changes();
    CHECK(worker_wakes > wakes && epd_memory_capacity_epoch() != epoch);
    CHECK(ticks == 0); drain();
    CHECK(epd_resource_activity_get(activity, &snapshot) == 0 && snapshot.state == EPD_RESOURCE_ADMITTED);
    CHECK(epd_resource_context_enter(activity) == EPD_MEMORY_OWNER_INVALID);
    previous = epd_memory_owner_enter_trusted(pages[0]);
    epd_memory_owner_t token = epd_resource_context_enter(activity);
    CHECK(token != EPD_MEMORY_OWNER_INVALID && epd_memory_credit_current() == snapshot.credit);
    block = allocate(64); CHECK(block);
    epd_resource_context_leave(token); epd_memory_owner_leave_trusted(previous);
    dispose(block); epd_memory_dispatch_changes(); close_all();
}
static void account_usage(epd_memory_owner_t owner, size_t physical_bytes, size_t associations)
{
    epd_memory_account_snapshot_t account;
    CHECK(epd_memory_account_snapshot(owner, &account));
    size_t used=0; for(unsigned i=0;i<EPD_MEMORY_CLASS_COUNT;++i) used+=account.backend[backend].physical[i];
    CHECK(used == physical_bytes && account.associated[backend] == associations);
}
static epd_resource_descriptor_t shared_descriptor(void)
{
    epd_resource_descriptor_t d = {0}; d.backend = backend; d.estimated_bytes = 64;
    d.key.bytes[0] = 1; d.key.length = 1; d.key.version = 1; d.discardable = true; return d;
}
static void account_dedup_ready_hit(void)
{
    setup(); epd_resource_activity_t a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 0);
    epd_resource_activity_t b = begin(pages[1], EPD_RESOURCE_INTERACTIVE, 0, 0); drain();
    epd_resource_provider_t p = provider(app); epd_resource_ref_t ar = acquire(a,p,pages[0]); drain();
    epd_resource_ref_t br = acquire(b,p,pages[1]); drain(); account_usage(app,0,64);
    CHECK(epd_resource_release(ar) == 0); account_usage(app,0,64);
    epd_memory_owner_t other = epd_memory_owner_create("second-account");
    epd_memory_owner_t page = epd_memory_scope_create_trusted(other, EPD_MEMORY_SCOPE_PAGE);
    CHECK(epd_resource_runtime_page_state(page, 7, true) == 0);
    epd_resource_activity_t c = begin(page, EPD_RESOURCE_INTERACTIVE,0,0); drain();
    CHECK(epd_memory_owner_set_limit(other,backend,32)==EPD_MEMORY_OK);
    epd_resource_descriptor_t d=shared_descriptor(); epd_resource_ref_t cr=0;
    epd_memory_owner_t old=epd_memory_owner_enter_trusted(page);
    CHECK(epd_resource_acquire(c,p,&d,NULL,NULL,EPD_RESOURCE_UI,&cr)==EPD_RESOURCE_DENIED && !cr);
    account_usage(other,0,0); account_usage(app,0,64);
    CHECK(epd_memory_owner_set_limit(other,backend,64)==EPD_MEMORY_OK);
    CHECK(epd_resource_acquire(c,p,&d,NULL,NULL,EPD_RESOURCE_UI,&cr)==EPD_RESOURCE_ACCEPTED);
    CHECK(allocate(1)==NULL); epd_memory_owner_leave_trusted(old); account_usage(other,0,64);
    void *data; CHECK(epd_resource_pin(cr,&data)==0);
    CHECK(epd_resource_runtime_close_owner(other)==EPD_RESOURCE_ACCEPTED); drain();
    account_usage(other,0,64); CHECK(epd_resource_runtime_owner_busy(other));
    CHECK(epd_resource_unpin(cr)==0); drain(); account_usage(other,0,0);
    CHECK(!epd_resource_runtime_owner_busy(other)); epd_memory_owner_release_trusted(page); epd_memory_owner_release_trusted(other);
    CHECK(epd_resource_release(br)==0); account_usage(app,0,0); close_all();
}
static void adopt_accounting(bool shared)
{
    setup(); epd_resource_activity_t a=begin(pages[0],EPD_RESOURCE_INTERACTIVE,0,0); drain();
    epd_memory_owner_t old=epd_memory_owner_enter_trusted(shared ? EPD_MEMORY_OWNER_SYSTEM : pages[0]);
    void *data=allocate(64); CHECK(data); epd_memory_owner_leave_trusted(old);
    epd_resource_provider_t p=provider(app); epd_resource_ref_t ref=0;
    epd_resource_descriptor_t d=shared_descriptor();
    CHECK(epd_memory_owner_set_limit(app,backend,shared ? 32 : 64)==EPD_MEMORY_OK);
    old=epd_memory_owner_enter_trusted(pages[0]);
    if(shared) {
        CHECK(epd_resource_adopt(a,p,&d,data,64,&ref)==EPD_RESOURCE_DENIED && !ref);
        account_usage(app,0,0); CHECK(epd_memory_owner_set_limit(app,backend,64)==EPD_MEMORY_OK);
    }
    CHECK(epd_resource_adopt(a,p,&d,data,64,&ref)==EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(old); account_usage(app,shared ? 0 : 64,shared ? 64 : 0);
    CHECK(epd_resource_release(ref)==0); account_usage(app,shared ? 0 : 64,0); close_all();
}
static void adopt_private_account(void){adopt_accounting(false);}
static void adopt_shared_account(void){adopt_accounting(true);}
static void loading_settlement(bool oversize)
{
    setup(); delay_load=true;
    CHECK(epd_memory_owner_set_limit(app,backend,64)==EPD_MEMORY_OK);
    epd_resource_activity_t a=begin(pages[0],EPD_RESOURCE_INTERACTIVE,64,0); drain();
    epd_resource_provider_t p=provider(app); epd_resource_ref_t ref=acquire(a,p,pages[0]); drain();
    account_usage(app,0,64); CHECK(pending_resource && !physical);
    epd_memory_owner_t old=epd_memory_owner_enter_trusted((epd_memory_owner_t)pending_credit);
    size_t size=oversize?128:32;
    if(oversize) {
        CHECK(allocate(size)==NULL);
        size_t extra[EPD_MEMORY_BACKEND_COUNT]={0}; extra[backend]=64;
        CHECK(epd_memory_credit_expand((epd_memory_credit_t)pending_credit,extra)==EPD_MEMORY_OK);
    }
    void *data=allocate(size); CHECK(data);
    CHECK(epd_resource_load_complete(pending_resource,0,data,size)==0); epd_memory_owner_leave_trusted(old); drain();
    epd_resource_snapshot_t snapshot; CHECK(epd_resource_get(pending_resource,&snapshot)==0);
    CHECK(snapshot.state==(oversize?EPD_RESOURCE_LOAD_FAILED:EPD_RESOURCE_READY));
    account_usage(app,0,oversize?64:32);
    CHECK(epd_resource_pin(ref,&data)==(oversize?EPD_RESOURCE_DENIED:EPD_RESOURCE_OK));
    if(!oversize) CHECK(epd_resource_unpin(ref)==0);
    CHECK(epd_resource_release(ref)==0); account_usage(app,0,0); close_all();
}
static void actual_shared_size(void){loading_settlement(false);}
static void oversized_shared_rejected(void){loading_settlement(true);}
static void full_budget_join(bool loading)
{
    setup(); delay_load=loading;
    epd_resource_activity_t a=begin(pages[0],EPD_RESOURCE_INTERACTIVE,64,0); drain();
    epd_resource_provider_t p=provider(app); epd_resource_ref_t ar=acquire(a,p,pages[0]); drain();
    epd_memory_owner_t other=epd_memory_owner_create("full-budget-consumer");
    epd_memory_owner_t page=epd_memory_scope_create_trusted(other,EPD_MEMORY_SCOPE_PAGE);
    CHECK(epd_resource_runtime_page_state(page,7,true)==0);
    CHECK(epd_memory_owner_set_limit(other,backend,64)==EPD_MEMORY_OK);
    epd_resource_activity_t c=begin(page,EPD_RESOURCE_INTERACTIVE,64,0); drain();
    epd_resource_activity_snapshot_t activity; CHECK(epd_resource_activity_get(c,&activity)==0 && activity.state==EPD_RESOURCE_ADMITTED);
    epd_resource_ref_t cr=acquire(c,p,page); CHECK(cr); account_usage(other,0,64);
    epd_memory_credit_snapshot_t credit; CHECK(epd_memory_credit_get((epd_memory_credit_t)activity.credit,&credit) && !credit.unused[backend]);
    if(loading) {
        CHECK(epd_resource_release(ar)==0); account_usage(app,0,0);
        epd_memory_owner_t old=epd_memory_owner_enter_trusted((epd_memory_owner_t)pending_credit);
        void *data=allocate(64); CHECK(data);
        CHECK(epd_resource_load_complete(pending_resource,0,data,64)==0); epd_memory_owner_leave_trusted(old); drain();
    }
    void *data; CHECK(epd_resource_pin(cr,&data)==0); CHECK(epd_resource_unpin(cr)==0);
    CHECK(epd_resource_runtime_close_owner(other)==EPD_RESOURCE_ACCEPTED); drain(); account_usage(other,0,0);
    CHECK(!epd_resource_runtime_owner_busy(other)); epd_memory_owner_release_trusted(page); epd_memory_owner_release_trusted(other); close_all();
}
static void full_budget_ready_hit(void){full_budget_join(false);}
static void full_budget_late_waiter(void){full_budget_join(true);}
static void initial_full_accounts(void)
{
    setup(); CHECK(epd_memory_owner_set_limit(app,backend,64)==EPD_MEMORY_OK);
    epd_memory_owner_t other=epd_memory_owner_create("joint-consumer");
    epd_memory_owner_t page=epd_memory_scope_create_trusted(other,EPD_MEMORY_SCOPE_PAGE);
    CHECK(epd_resource_runtime_page_state(page,9,true)==0);
    CHECK(epd_memory_owner_set_limit(other,backend,64)==EPD_MEMORY_OK);
    epd_resource_activity_t a=begin(pages[0],EPD_RESOURCE_INTERACTIVE,64,0);
    epd_resource_activity_t b=begin(page,EPD_RESOURCE_INTERACTIVE,64,0); drain();
    epd_resource_provider_t p=provider(app);
    epd_resource_ref_t ar=acquire(a,p,pages[0]), br=acquire(b,p,page); drain();
    void *first,*second; CHECK(loaded==1 && physical==64);
    CHECK(epd_resource_pin(ar,&first)==0 && epd_resource_pin(br,&second)==0 && first==second);
    account_usage(app,0,64); account_usage(other,0,64);
    epd_resource_activity_snapshot_t state; epd_memory_credit_snapshot_t credit;
    CHECK(epd_resource_activity_get(a,&state)==0 && epd_memory_credit_get((epd_memory_credit_t)state.credit,&credit));
    CHECK(!credit.unused[backend]);
    CHECK(epd_resource_activity_get(b,&state)==0 && epd_memory_credit_get((epd_memory_credit_t)state.credit,&credit));
    CHECK(!credit.unused[backend]);
    CHECK(epd_resource_unpin(ar)==0 && epd_resource_release(ar)==0); account_usage(app,0,0);
    CHECK(epd_resource_unpin(br)==0);
    CHECK(epd_resource_runtime_close_owner(other)==EPD_RESOURCE_ACCEPTED); drain(); account_usage(other,0,0);
    CHECK(!epd_resource_runtime_owner_busy(other)); epd_memory_owner_release_trusted(page); epd_memory_owner_release_trusted(other); close_all();
}
static int cache_shrink(void *context, uint32_t backend_id, size_t target, size_t *freed_bytes)
{ (void)context; (void)backend_id; (void)target; *freed_bytes = 0; return EPD_RESOURCE_OK; }
typedef struct { epd_resource_cache_t cache; void *data; unsigned calls; } reclaim_fixture_t;
static int reclaim_payload(void *context, uint32_t backend_id, size_t target, size_t *freed_bytes)
{
    reclaim_fixture_t *f = context;
    CHECK(actor == &actors[1] && !irq && !mutexes && backend_id == backend && target);
    CHECK(f->data); ++f->calls;
    epd_memory_block_snapshot_t block;
    CHECK(epd_memory_block_get(f->data, &block) == EPD_MEMORY_OK);
    *freed_bytes = block.physical; dispose(f->data); f->data = NULL;
    CHECK(epd_resource_cache_update(&f->cache, &app_image_psram_memheap, 0, 0, 1, 1) == 0);
    return EPD_RESOURCE_OK;
}
static void waiting_reclaims_cache(void)
{
    setup(); reclaim_fixture_t f = {0};
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(app);
    CHECK(epd_resource_cache_open(&f.cache, EPD_RESOURCE_WORKER, reclaim_payload, &f) == 0);
    f.data = allocate(CAPACITY * 3 / 4); CHECK(f.data);
    CHECK(epd_resource_cache_adopt(&f.cache, f.data) == 0);
    CHECK(epd_resource_cache_update(&f.cache, &app_image_psram_memheap, CAPACITY*3/4, CAPACITY*3/4, 1, 1) == 0);
    epd_memory_owner_leave_trusted(old);
    epd_resource_activity_t a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, CAPACITY/2, 100);
    drain(); epd_resource_activity_snapshot_t s;
    CHECK(epd_resource_activity_get(a, &s) == 0 && s.state == EPD_RESOURCE_ADMITTED);
    CHECK(f.calls == 1 && !f.data && !physical);
    CHECK(epd_resource_cache_close(&f.cache) == 0); close_all();
}
static void waiting_budget_keeps_cache(void)
{
    setup(); reclaim_fixture_t f = {0};
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    CHECK(epd_resource_cache_open(&f.cache, EPD_RESOURCE_WORKER, reclaim_payload, &f) == 0);
    f.data = allocate(CAPACITY * 3 / 4); CHECK(f.data);
    CHECK(epd_resource_cache_adopt(&f.cache, f.data) == 0);
    CHECK(epd_resource_cache_update(&f.cache, &app_image_psram_memheap, CAPACITY*3/4, CAPACITY*3/4, 1, 1) == 0);
    epd_memory_owner_leave_trusted(old);
    CHECK(epd_memory_owner_set_limit(app, backend, 64) == EPD_MEMORY_OK);
    epd_resource_activity_t a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 128, 0); drain();
    epd_resource_activity_snapshot_t s; CHECK(epd_resource_activity_get(a, &s) == 0);
    CHECK(!f.calls && f.data);
    dispose(f.data); CHECK(epd_resource_cache_update(&f.cache, &app_image_psram_memheap, 0, 0, 1, 1) == 0);
    CHECK(epd_resource_cache_close(&f.cache) == 0); close_all();
}
static void cache_completed_ownership(void)
{
    setup(); epd_resource_cache_t cache = {0};
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(app);
    CHECK(epd_resource_cache_open(&cache, EPD_RESOURCE_WORKER, cache_shrink, NULL) == 0);
    epd_memory_owner_leave_trusted(old);
    epd_memory_metadata_snapshot_t initial, final;
    epd_memory_metadata_snapshot(&initial);
    void *data[80];
    for (unsigned i = 0; i < 80; ++i) {
        epd_memory_owner_t task = epd_memory_scope_create_trusted(pages[0], EPD_MEMORY_SCOPE_TASK);
        epd_memory_credit_spec_t spec = {0}; epd_memory_credit_t credit;
        spec.bytes[backend] = 64;
        CHECK(epd_memory_credit_request(task, &spec, &credit) == EPD_MEMORY_OK);
        old = epd_memory_owner_enter_trusted(credit); data[i] = allocate(64); CHECK(data[i]);
        CHECK(epd_resource_cache_adopt(&cache, data[i]) == 0);
        epd_memory_owner_leave_trusted(old);
        epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit);
        epd_memory_owner_close_trusted(task); epd_memory_owner_release_trusted(task);
        epd_memory_metadata_snapshot(&final);
        CHECK(final.descriptors_used == initial.descriptors_used);
    }
    CHECK(epd_resource_cache_update(&cache, &app_image_psram_memheap, 5120, 5120, 1, 1) == 0);
    CHECK(epd_resource_cache_close(&cache) == EPD_RESOURCE_PENDING && cache.provider && cache.owner);
    for (unsigned i = 0; i < 80; ++i) dispose(data[i]);
    CHECK(epd_resource_cache_update(&cache, &app_image_psram_memheap, 0, 0, 1, 1) == 0);
    drain();
    CHECK(epd_resource_cache_close(&cache) == 0 && !cache.provider);
    close_all();
}
static void cache_adopt_budget(void)
{
    setup(); epd_resource_cache_t cache = {0};
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(app);
    CHECK(epd_resource_cache_open(&cache, EPD_RESOURCE_WORKER, cache_shrink, NULL) == 0);
    epd_memory_owner_leave_trusted(old);
    CHECK(epd_memory_owner_set_limit(app, backend, 32) == EPD_MEMORY_OK);
    old = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    void *data = allocate(64); CHECK(data); epd_memory_owner_leave_trusted(old);
    CHECK(epd_resource_cache_adopt(&cache, data) == EPD_RESOURCE_DENIED);
    CHECK(epd_memory_owner_set_limit(app, backend, 64) == EPD_MEMORY_OK);
    CHECK(epd_resource_cache_adopt(&cache, data) == 0);
    epd_memory_block_snapshot_t block; CHECK(epd_memory_block_get(data, &block) == EPD_MEMORY_OK);
    CHECK(block.owner == cache.owner && block.account == app);
    dispose(data); CHECK(epd_resource_cache_close(&cache) == 0); close_all();
}
static void cache_auto_close(void)
{
    setup(); epd_resource_cache_t cache = {0};
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(app);
    CHECK(epd_resource_cache_open(&cache, EPD_RESOURCE_WORKER, cache_shrink, NULL) == 0);
    void *data = allocate(64); CHECK(data);
    CHECK(epd_resource_cache_adopt(&cache, data) == 0);
    epd_memory_owner_leave_trusted(old);
    CHECK(epd_resource_cache_update(&cache, &app_image_psram_memheap, 64, 64, 1, 1) == 0);
    CHECK(epd_resource_cache_close(&cache) == EPD_RESOURCE_PENDING);
    dispose(data);
    CHECK(epd_resource_cache_update(&cache, &app_image_psram_memheap, 0, 0, 1, 1) == 0);
    /* Application shutdown must drain the provider without a second cache close. */
    close_all();
    epd_memory_owner_snapshot_t owner;
    CHECK(!epd_memory_owner_get(cache.owner, &owner));
}
static void runtime_prefetch_headroom(void)
{
    epd_resource_activity_t parent, nearby, continuous, foreground;
    epd_resource_activity_snapshot_t snapshot;
    epd_resource_context_request_t request = {0};
    epd_memory_owner_t previous;
    setup();
    CHECK(epd_memory_backend_set_headroom(backend, CAPACITY / 4));
    parent = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 0, 0); drain();
    request.kind = EPD_RESOURCE_NEARBY; request.interactive_parent = parent;
    request.backend = backend;
    request.minimum_bytes = request.target_bytes = CAPACITY * 3 / 4 + 1;
    previous = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(epd_resource_context_begin(&request, NULL, &nearby) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(previous); drain();
    CHECK(epd_resource_activity_get(nearby, &snapshot) == 0);
    CHECK(snapshot.state == EPD_RESOURCE_REJECTED);
    CHECK(epd_resource_activity_dispose(nearby) == EPD_RESOURCE_ACCEPTED); drain();
    continuous = begin(service, EPD_RESOURCE_CONTINUOUS, CAPACITY * 3 / 4 + 1, 0); drain();
    CHECK(epd_resource_activity_get(continuous, &snapshot) == 0 && snapshot.state == EPD_RESOURCE_ADMITTED);
    CHECK(epd_resource_activity_dispose(continuous) == EPD_RESOURCE_ACCEPTED); drain();
    request.minimum_bytes = request.target_bytes = CAPACITY * 3 / 4;
    previous = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(epd_resource_context_begin(&request, NULL, &nearby) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(previous); drain();
    CHECK(epd_resource_activity_get(nearby, &snapshot) == 0 && snapshot.state == EPD_RESOURCE_ADMITTED);
    foreground = begin(pages[1], EPD_RESOURCE_INTERACTIVE, CAPACITY / 4, 0); drain();
    CHECK(epd_resource_activity_get(foreground, &snapshot) == 0 && snapshot.state == EPD_RESOURCE_ADMITTED);
    close_all();
}
static unsigned ending_notices, ended_notices;
static void observe_child(void *context, const epd_resource_activity_snapshot_t *snapshot)
{
    (void)context;
    if (snapshot->state == EPD_RESOURCE_ENDING) ++ending_notices;
    if (snapshot->state == EPD_RESOURCE_ENDED) ++ended_notices;
}

typedef enum { EXECUTION_UPDATE, EXECUTION_DEMOTE, EXECUTION_END,
               EXECUTION_PARENT_END, EXECUTION_BAD_LEAVE } execution_case_t;
static void execution_lease_case(execution_case_t scenario)
{
    setup();
    bool recursive = scenario == EXECUTION_PARENT_END;
    epd_resource_activity_t parent = 0, activity;
    if (recursive) {
        parent = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 0, 0);
        drain();
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(pages[0]);
        epd_resource_context_request_t request = {0};
        request.kind = EPD_RESOURCE_NEARBY; request.interactive_parent = parent;
        request.backend = backend; request.minimum_bytes = request.target_bytes = 64;
        epd_resource_observer_t observer = {.notify = observe_child, .lane = EPD_RESOURCE_UI};
        CHECK(epd_resource_context_begin(&request, &observer, &activity) == EPD_RESOURCE_ACCEPTED);
        epd_memory_owner_leave_trusted(previous);
    } else activity = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 0);
    drain();
    epd_resource_activity_snapshot_t first, pending, last;
    CHECK(epd_resource_activity_get(activity, &first) == EPD_RESOURCE_OK);
    epd_resource_activity_t sibling = 0;
    if (scenario == EXECUTION_UPDATE) {
        sibling = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 0);
        drain();
    }
    epd_memory_owner_t owner = epd_memory_owner_enter_trusted(pages[0]);
    epd_memory_owner_t scope = epd_resource_context_enter(activity);
    CHECK(scope != EPD_MEMORY_OWNER_INVALID && epd_memory_credit_current() == first.credit);
    if (sibling) {
        epd_resource_activity_snapshot_t sibling_snapshot;
        CHECK(epd_resource_activity_get(sibling, &sibling_snapshot) == EPD_RESOURCE_OK);
        CHECK(sibling_snapshot.credit != first.credit);
        epd_memory_owner_t inner = epd_resource_context_enter(sibling);
        CHECK(inner == first.credit && epd_memory_credit_current() == sibling_snapshot.credit);
        epd_resource_context_leave(inner);
        CHECK(epd_memory_credit_current() == first.credit);
        CHECK(epd_resource_activity_dispose(sibling) == EPD_RESOURCE_ACCEPTED);
        drain();
    }
    epd_memory_owner_t nested = epd_resource_context_enter(activity);
    CHECK(nested == first.credit && epd_memory_credit_current() == first.credit);
    bool end = recursive || scenario == EXECUTION_END;
    if (end) {
        CHECK(epd_resource_activity_end(recursive ? parent : activity) == EPD_RESOURCE_ACCEPTED);
    } else if (scenario == EXECUTION_DEMOTE) {
        CHECK(epd_resource_runtime_page_state(pages[0], 1, false) == EPD_RESOURCE_OK);
    } else {
        uint64_t revision;
        epd_resource_context_request_t request = {0};
        request.kind = EPD_RESOURCE_INTERACTIVE; request.backend = backend;
        request.minimum_bytes = request.target_bytes = 128;
        CHECK(epd_resource_context_update(activity, &request, &revision) == EPD_RESOURCE_ACCEPTED);
    }
    drain();
    CHECK(epd_resource_activity_get(activity, &pending) == EPD_RESOURCE_OK);
    int reserve = epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM, 128);
    CHECK(pending.credit == first.credit);
    epd_memory_owner_t rejected = epd_resource_context_enter(activity);
    CHECK(rejected == EPD_MEMORY_OWNER_INVALID);
    epd_resource_context_leave(rejected);
    CHECK(epd_memory_credit_current() == first.credit);
    if (scenario == EXECUTION_BAD_LEAVE) {
        epd_memory_owner_t foreign = epd_memory_owner_create("foreign-context-leave");
        CHECK(foreign != EPD_MEMORY_OWNER_INVALID && foreign != first.credit);
        epd_resource_context_leave(foreign);
        CHECK(epd_memory_credit_current() == first.credit);
        epd_memory_owner_close_trusted(foreign);
        epd_memory_owner_release_trusted(foreign);
    }
    epd_resource_context_leave(nested);
    drain();
    CHECK(epd_resource_activity_get(activity, &pending) == EPD_RESOURCE_OK &&
          pending.credit == first.credit);
    CHECK(reserve == EPD_RESOURCE_OK);
    if (recursive) CHECK(ending_notices == 1 && ended_notices == 0);
    if (end) CHECK(pending.state == EPD_RESOURCE_ENDING);
    epd_resource_context_leave(scope); epd_memory_owner_leave_trusted(owner);
    drain();
    CHECK(epd_resource_activity_get(activity, &last) == EPD_RESOURCE_OK);
    CHECK(last.credit != first.credit);
    if (end) {
        CHECK(last.state == EPD_RESOURCE_ENDED && !last.credit);
        CHECK(epd_resource_activity_forget(activity) == EPD_RESOURCE_OK);
        if (recursive) {
            CHECK(ended_notices == 1);
            CHECK(epd_resource_activity_forget(parent) == EPD_RESOURCE_OK);
        }
    }
    else CHECK(last.state == EPD_RESOURCE_ADMITTED &&
               last.effective.kind == (scenario == EXECUTION_DEMOTE ? EPD_RESOURCE_OPTIONAL : EPD_RESOURCE_INTERACTIVE));
    close_all();
}
static void execution_lease_update(void) { execution_lease_case(EXECUTION_UPDATE); }
static void execution_lease_demote(void) { execution_lease_case(EXECUTION_DEMOTE); }
static void execution_lease_end(void) { execution_lease_case(EXECUTION_END); }
static void execution_lease_bad_leave(void) { execution_lease_case(EXECUTION_BAD_LEAVE); }
static void execution_lease_parent_end(void) { execution_lease_case(EXECUTION_PARENT_END); }

static const struct { const char *name; void (*run)(void); } cases[] = {
    {"execution_lease_update", execution_lease_update},
    {"execution_lease_demote", execution_lease_demote},
    {"execution_lease_end", execution_lease_end},
    {"execution_lease_parent_end", execution_lease_parent_end},
    {"execution_lease_bad_leave", execution_lease_bad_leave},
    {"runtime_prefetch_headroom", runtime_prefetch_headroom},
    {"waiting_reclaims_cache", waiting_reclaims_cache},
    {"waiting_budget_keeps_cache", waiting_budget_keeps_cache},
    {"cache_auto_close", cache_auto_close},
    {"cache_completed_ownership", cache_completed_ownership}, {"cache_adopt_budget", cache_adopt_budget},
    {"initial_full_accounts",initial_full_accounts},
    {"full_budget_ready_hit",full_budget_ready_hit},{"full_budget_late_waiter",full_budget_late_waiter},
    {"account_dedup_ready_hit",account_dedup_ready_hit},
    {"adopt_private_account",adopt_private_account},{"adopt_shared_account",adopt_shared_account},
    {"actual_shared_size",actual_shared_size},{"oversized_shared_rejected",oversized_shared_rejected},
    {"runtime_exact_page_close", exact_page_close}, {"runtime_pending_no_spin", pending_does_not_spin},
    {"runtime_expired_wait_no_spin", expired_wait_does_not_spin},
    {"runtime_expired_resume_no_spin", expired_resume_does_not_spin},
    {"runtime_helper_proof", helper_proof_and_demotion}, {"reader_real_provider", real_reader_provider},
    {"ordinary_free_wakes_waiter", ordinary_free_wakes_waiter},
    {"default_full_credit_share", full_credit_share}, {"default_replace_delta", default_replace_delta}
};
int main(int argc, char **argv)
{
    size_t i; CHECK(argc == 2);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!strcmp(argv[1], "--list")) puts(cases[i].name);
        else if (!strcmp(argv[1], cases[i].name)) { cases[i].run(); printf("PASS %s\n", cases[i].name); return 0; }
    }
    return strcmp(argv[1], "--list") ? 2 : 0;
}
