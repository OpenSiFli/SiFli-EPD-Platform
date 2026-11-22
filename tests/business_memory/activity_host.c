#include "epd_activity_work.h"
#include "epd_resource_runtime.h"
#include "epd_operation_runtime.h"
#include "../../src/services/app_background.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#ifndef CAPACITY
#define CAPACITY 1048576u
#endif
struct rt_memheap app_image_psram_memheap;
static unsigned sram_key;
static struct rt_thread actors[4];
static rt_thread_t actor = &actors[2];
static unsigned irq, mutexes, wakes, semaphores, blocked, fail_alloc;
static rt_tick_t ticks;
static epd_memory_backend_t backends[2];
static epd_memory_owner_t app;
static epd_operation_runtime_client_t clients[4];
/* Synchronous activity tests never register an application background group. */
epd_memory_result_t app_background_group_create(epd_memory_owner_t owner, epd_app_task_group_t *out)
{ (void)owner; (void)out; CHECK(false); return EPD_MEMORY_INVALID; }
epd_memory_result_t app_background_task_begin(epd_app_task_group_t group, epd_memory_owner_t *out)
{ (void)group; (void)out; CHECK(false); return EPD_MEMORY_INVALID; }
epd_memory_result_t app_background_group_default(epd_app_task_group_t group, epd_app_background_mode_t mode)
{ (void)group; (void)mode; CHECK(false); return EPD_MEMORY_INVALID; }
bool app_background_task_end(epd_memory_owner_t task) { (void)task; CHECK(false); return false; }
bool app_background_group_accepting(epd_app_task_group_t group) { (void)group; CHECK(false); return false; }
epd_memory_result_t app_background_group_release(epd_app_task_group_t group)
{ (void)group; CHECK(false); return EPD_MEMORY_INVALID; }
static size_t physical[2];
bool epd_memory_app_backend(epd_memory_region_t region, epd_memory_backend_t *out) {
    *out = backends[region == EPD_MEMORY_REGION_HCPU_PSRAM]; return true;
}
static struct { void *ptr; size_t bytes; unsigned region; epd_memory_owner_t owner; } blocks[128];
rt_thread_t rt_thread_self(void) { return actor; }
rt_tick_t rt_tick_get(void) { return ticks; }
rt_tick_t rt_tick_from_millisecond(int ms) { return (rt_tick_t)(((uint64_t)ms * RT_TICK_PER_SECOND + 999) / 1000); }
unsigned rt_interrupt_get_nest(void) { return 0; }
unsigned rt_critical_level(void) { return 0; }
rt_base_t rt_hw_interrupt_disable(void) { return (rt_base_t)irq++; }
void rt_hw_interrupt_enable(rt_base_t level) { CHECK(irq > (unsigned)level); irq = (unsigned)level; }
uint32_t rt_thread_mem_context_get(rt_thread_t t) { return (t ? t : actor)->context; }
int rt_thread_mem_context_set(rt_thread_t t, uint32_t token) {
    t = t ? t : actor; epd_memory_owner_t old = t->context;
    if (!epd_memory_owner_retain_trusted(token)) return -1;
    t->context = token; epd_memory_owner_release_trusted(old); return 0;
}
int rt_mutex_init(struct rt_mutex *m, const char *n, int f) { (void)n; (void)f; m->held = 0; return 0; }
int rt_mutex_take(struct rt_mutex *m, int t) { (void)t; CHECK(!irq && !m->held); m->held = 1; ++mutexes; return 0; }
int rt_mutex_release(struct rt_mutex *m) { CHECK(m->held); m->held = 0; --mutexes; return 0; }
int rt_mutex_detach(struct rt_mutex *m) { CHECK(!m->held); return 0; }
int rt_sem_init(struct rt_semaphore *s, const char *n, int v, int f) { (void)n; (void)f; s->value = v; s->live = true; ++semaphores; return 0; }
int rt_sem_release(struct rt_semaphore *s) { CHECK(s->live); ++s->value; return 0; }
int rt_sem_detach(struct rt_semaphore *s) { CHECK(s->live); s->live = false; --semaphores; return 0; }
void epd_power_notify(void) {}
void epd_operation_runtime_wake(void) { ++wakes; }
bool epd_operation_runtime_executors(epd_operation_executor_t *e, epd_operation_executor_t *d, epd_operation_executor_t *c) {
    *e = (epd_operation_executor_t){&actors[1], NULL, 0}; *c = *e;
    *d = (epd_operation_executor_t){&actors[0], NULL, 0}; return true;
}
bool epd_operation_runtime_register(const epd_operation_runtime_client_t *c) {
    for (unsigned i = 0; i < 4; ++i) {
        if (clients[i].key == c->key) return clients[i].process == c->process;
        if (!clients[i].key) { clients[i] = *c; return true; }
    }
    return false;
}
static void drain(void) {
    rt_thread_t previous = actor;
    for (unsigned pass = 0; pass < 100; ++pass) {
        size_t processed = 0; unsigned before = wakes;
        actor = &actors[1];
        for (unsigned i = 0; i < 4; ++i) if (clients[i].key) processed += clients[i].process(32, clients[i].user);
        actor = &actors[0]; processed += epd_resource_runtime_ui(32);
        if (!processed && before == wakes) { actor = previous; return; }
    }
    CHECK(!"worker did not quiesce");
}
int rt_sem_take(struct rt_semaphore *s, int timeout) {
    CHECK(s->live && !irq && !mutexes && (actor == &actors[2] || actor == &actors[3])); ++blocked;
    if (!s->value) drain();
    if (s->value) { --s->value; return 0; }
    ticks += timeout; drain(); return -1;
}
void *epd_app_alloc(size_t size, epd_app_memory_t region) {
    CHECK(!irq && !mutexes);
    if (fail_alloc) { --fail_alloc; return NULL; }
    unsigned i; for (i = 0; i < 128 && blocks[i].ptr; ++i) {} CHECK(i < 128);
    size_t bytes = size + 32;
    epd_memory_change_t change = {0}; change.kind = EPD_MEMORY_CHANGE_ALLOC;
    change.token = epd_memory_credit_current();
    if (change.token == EPD_MEMORY_OWNER_INVALID) change.token = epd_memory_owner_current();
    change.backend = backends[region];
    change.size = size; change.physical = bytes; change.available_before = CAPACITY - physical[region];
    change.available_after = change.available_before - bytes;
    if (epd_memory_try_change(&change) != EPD_MEMORY_OK) return NULL;
    void *p = malloc(size); CHECK(p); epd_memory_notify(&change);
    blocks[i].ptr = p; blocks[i].bytes = bytes; blocks[i].owner = change.token; blocks[i].region = region;
    physical[region] += bytes; epd_memory_dispatch_changes(); return p;
}
void epd_app_free(void *ptr) {
    if (!ptr) return; CHECK(!irq && !mutexes);
    unsigned i; for (i = 0; i < 128 && blocks[i].ptr != ptr; ++i) {} CHECK(i < 128);
    unsigned region = blocks[i].region; epd_memory_change_t change = {0};
    change.kind = EPD_MEMORY_CHANGE_FREE; change.backend = backends[region]; change.token = blocks[i].owner;
    change.old_size = blocks[i].bytes - 32; change.old_physical = blocks[i].bytes;
    change.available_before = CAPACITY - physical[region]; physical[region] -= blocks[i].bytes;
    change.available_after = CAPACITY - physical[region]; epd_memory_notify(&change);
    free(ptr); blocks[i].ptr = NULL; epd_memory_dispatch_changes();
}
static epd_memory_owner_t block_owner(void *p) {
    for (unsigned i = 0; i < 128; ++i) if (blocks[i].ptr == p) return blocks[i].owner;
    CHECK(false); return 0;
}
epd_memory_result_t epd_memory_block_get(const void *ptr, epd_memory_block_snapshot_t *out) {
    for (unsigned i = 0; i < 128; ++i) if (blocks[i].ptr == ptr) {
        epd_memory_owner_snapshot_t owner; CHECK(epd_memory_owner_get(blocks[i].owner, &owner));
        *out = (epd_memory_block_snapshot_t){blocks[i].owner, owner.account,
                 backends[blocks[i].region], blocks[i].bytes - 32, blocks[i].bytes};
        return EPD_MEMORY_OK;
    }
    return EPD_MEMORY_INVALID;
}
epd_memory_result_t epd_memory_block_transfer(void *ptr, epd_memory_owner_t to) {
    for (unsigned i = 0; i < 128; ++i) if (blocks[i].ptr == ptr) {
        epd_memory_result_t result = epd_memory_transfer(blocks[i].owner, to, backends[blocks[i].region],
                                                        blocks[i].bytes - 32, blocks[i].bytes);
        if (result == EPD_MEMORY_OK) blocks[i].owner = to;
        return result;
    }
    return EPD_MEMORY_INVALID;
}
static void setup(void) {
    CHECK(epd_memory_init() == EPD_MEMORY_OK);
    epd_memory_backend_config_t b = {EPD_MEMORY_REGION_SRAM, CAPACITY, 0, EPD_MEMORY_BACKEND_NONE};
    CHECK(epd_memory_backend_register(&sram_key, &b, &backends[0]) == EPD_MEMORY_OK);
    CHECK(epd_memory_backend_set_name(backends[0], "heap"));
    b.region = EPD_MEMORY_REGION_HCPU_PSRAM;
    CHECK(epd_memory_backend_register(&app_image_psram_memheap, &b, &backends[1]) == EPD_MEMORY_OK);
    app = epd_memory_owner_create("activity-test"); CHECK(app != EPD_MEMORY_OWNER_INVALID);
    CHECK(rt_thread_mem_context_set(NULL, app) == 0);
    CHECK(epd_resource_runtime_init(NULL) == EPD_RESOURCE_OK); drain();
}
static void finish(void) {
    drain(); CHECK(!semaphores && !physical[0] && !physical[1]);
    epd_memory_account_snapshot_t s; CHECK(epd_memory_scope_snapshot(app, &s));
    CHECK(!s.backend[backends[0]].unused && !s.backend[backends[1]].unused);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); setup();
    if (!strcmp(argv[1], "lifecycle")) {
        epd_activity_work_t *w = epd_activity_work_open(4096, 1024); CHECK(w && blocked);
        CHECK(epd_memory_owner_current() == app);
        epd_activity_guard_t g; CHECK(epd_activity_work_enter(w, &g));
        void *a = epd_app_alloc(100, EPD_APP_PSRAM), *b = epd_app_alloc(50, EPD_APP_SRAM); CHECK(a && b);
        epd_memory_owner_t token = block_owner(a); CHECK(token == block_owner(b));
        epd_memory_owner_snapshot_t s; CHECK(epd_memory_owner_get(token, &s)); CHECK(s.account == app);
        epd_memory_account_snapshot_t usage; CHECK(epd_memory_scope_snapshot(token, &usage));
        CHECK(usage.backend[backends[1]].physical[EPD_MEMORY_INFLIGHT] == 132);
        CHECK(epd_activity_work_reserve(w, EPD_APP_PSRAM, 8192));
        CHECK(!epd_activity_work_reserve(w, EPD_APP_PSRAM, CAPACITY * 2));
        epd_activity_work_leave(&g); CHECK(epd_memory_owner_current() == app);
        epd_activity_work_close(w); drain(); CHECK(!semaphores);
        CHECK(epd_memory_scope_snapshot(token, &usage)); CHECK(usage.backend[backends[1]].physical[EPD_MEMORY_ACTIVE] == 132);
        CHECK(!usage.backend[backends[1]].unused && !usage.backend[backends[0]].unused);
        epd_app_free(a); epd_app_free(b);
    } else if (!strcmp(argv[1], "timeout")) {
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
        void *occupied = epd_app_alloc(CAPACITY * 3 / 4, EPD_APP_PSRAM); CHECK(occupied);
        epd_memory_owner_leave_trusted(previous);
        CHECK(!epd_activity_work_open(CAPACITY / 2, 0)); CHECK(blocked); CHECK(ticks == rt_tick_from_millisecond(500));
        epd_app_free(occupied);
    } else if (!strcmp(argv[1], "sram_reject")) {
        CHECK(!epd_activity_work_open(4096, CAPACITY * 2)); CHECK(blocked);
    } else if (!strcmp(argv[1], "closed")) {
        CHECK(epd_memory_owner_close_trusted(app)); CHECK(!epd_activity_work_open(4096, 0));
    } else if (!strcmp(argv[1], "seed_failure")) {
        fail_alloc = 1;
        epd_activity_work_t *w = epd_activity_work_open(4096, 0);
        CHECK(w && fail_alloc == 1);
        fail_alloc = 0;
        epd_activity_work_close(w); drain(); CHECK(!semaphores);
    } else if (!strcmp(argv[1], "restricted_lanes")) {
        actor = &actors[0]; CHECK(!epd_activity_work_open(4096, 0));
        actor = &actors[1]; CHECK(!epd_activity_work_open(4096, 0)); actor = &actors[2];
    } else if (!strcmp(argv[1], "parent_closed_live")) {
        epd_activity_work_t *w = epd_activity_work_open(4096, 1024); CHECK(w);
        CHECK(epd_memory_owner_close_trusted(app));
        epd_activity_guard_t g;
        if (epd_activity_work_enter(w, &g)) { CHECK(!epd_app_alloc(100, EPD_APP_PSRAM)); epd_activity_work_leave(&g); }
        epd_activity_work_close(w);
    } else CHECK(false);
    finish(); printf("PASS %s\n", argv[1]); return 0;
}
