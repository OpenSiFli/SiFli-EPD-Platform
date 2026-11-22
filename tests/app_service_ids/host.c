#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>
#include <crtdbg.h>
#include "app_service_internal.h"
#include "sleep_guard.h"

static unsigned checks, allocations, pins, subscriptions;
static char log_text[4096], pinned_app[64], pinned_path[96];
static bool reset_pending, storage_valid = true;
static bool sleeping;
static uint32_t now_ticks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define RT_ASSERT(x) CHECK(x)
#define EPD_APP_PSRAM 1
#define STORAGE_APP_CODE 0
typedef int storage_volume_t;

static unsigned interrupt_depth;
static unsigned global_object_depth, global_events, global_threads, global_mutexes;
static unsigned local_events, local_threads;
static int event_failure, thread_failure, startup_failure;
static rt_err_t rt_object_global_scope_enter(struct rt_object_global_scope *scope)
{ CHECK(!scope->active); scope->active = true; ++global_object_depth; return RT_EOK; }
static void rt_object_global_scope_leave(struct rt_object_global_scope *scope)
{ CHECK(scope->active && global_object_depth); scope->active = false; --global_object_depth; }
static rt_base_t rt_hw_interrupt_disable(void) { return (rt_base_t)interrupt_depth++; }
static void rt_hw_interrupt_enable(rt_base_t level) { interrupt_depth = (unsigned)level; }
static uint32_t rt_tick_get(void) { return now_ticks; }
static void epd_power_notify(void) {}
static bool epd_power_sleeping(void) { return sleeping; }
static bool system_reset_blocks_apps(void) { return reset_pending; }
#ifndef HOST_CUSTOM_SCHEDULE_PORTS
#ifndef HOST_REAL_PRESSURE
static bool host_network_ready = true;
static bool network_ready(void) { return host_network_ready; }
static bool host_pressure_blocked;
static unsigned host_schedule_resumes;
static bool app_memory_pressure_background_blocked(void) { return host_pressure_blocked; }
static void app_catalog_background_resume(void) { ++host_schedule_resumes; }
#else
static bool network_ready(void);
bool app_memory_pressure_background_blocked(void);
void app_catalog_background_resume(void);
#endif
#endif
static bool storage_handoff;
static bool storage_changing(void) { return storage_handoff; }
static bool storage_session_valid(storage_volume_t volume, uint32_t session)
{ (void)volume; (void)session; return storage_valid; }
static void *rt_calloc(size_t count, size_t size)
{ void *p = calloc(count, size); if (p) ++allocations; return p; }
static void rt_free(void *p) { if (p) { CHECK(allocations > 0); --allocations; free(p); } }
static void *epd_app_alloc(size_t size, int region) { (void)region; return rt_calloc(1, size); }
static void epd_app_free(void *p) { rt_free(p); }
static size_t epd_app_psram_available(void) { return 1000000; }
static void rt_memory_info(rt_uint32_t *total, rt_uint32_t *used, rt_uint32_t *peak)
{ *total = 100000; *used = 1000; (void)peak; }
static void rt_kprintf(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    CHECK(length > 0 && length < 128);
    CHECK(strlen(log_text) + (size_t)length < sizeof(log_text));
    strcat(log_text, line);
}
#define rt_strncpy strncpy
#define rt_snprintf snprintf
static rt_err_t rt_event_init(struct rt_event *e, const char *name, int flags)
{ (void)name; (void)flags; e->bits = 0;
  if (global_object_depth) ++global_events; else ++local_events;
  return event_failure; }
static void rt_event_detach(struct rt_event *e) { (void)e; }
static void rt_event_send(struct rt_event *e, uint32_t bits) { e->bits |= bits; }
static void rt_event_recv(struct rt_event *e, uint32_t bits, int flags, rt_int32_t ticks, uint32_t *out)
{ (void)flags; (void)ticks; *out = e->bits & bits; e->bits &= ~bits; }
static rt_err_t rt_thread_init(rt_thread_t t, const char *name, void (*entry)(void *), void *parameter,
                              void *stack, uint32_t size, uint8_t priority, uint32_t ticks)
{
    (void)stack; (void)size; (void)priority; (void)ticks;
    if (global_object_depth) ++global_threads; else ++local_threads;
    strncpy(t->name, name, RT_NAME_MAX);
    t->entry = entry; t->parameter = parameter;
    return thread_failure;
}
static rt_err_t rt_thread_startup(rt_thread_t t) { (void)t; return startup_failure; }
static void rt_thread_detach(rt_thread_t t) { (void)t; }
static rt_thread_t rt_thread_self(void) { return NULL; }
static uint32_t network_subscribe(struct rt_event *e, uint32_t bits)
{ (void)e; (void)bits; ++subscriptions; return 1; }
static void network_unsubscribe(uint32_t id)
{ if (id) { CHECK(subscriptions > 0); --subscriptions; } }
static bool app_module_storage(const char *app, storage_volume_t *volume, uint32_t *session)
{ CHECK(!strcmp(app, "books") || !strcmp(app, "music")); *volume = 1; *session = 1; return true; }
static bool storage_app_path(char *path, size_t capacity, const char *app, int kind, const char *file)
{ (void)kind; return snprintf(path, capacity, "/apps/%s/%s", app, file) < (int)capacity; }
static void *app_module_open(const char *path, const char *app)
{ strcpy(pinned_app, app); strcpy(pinned_path, path); ++pins; return (void *)1; }
static void app_module_close(void *module) { CHECK(module && pins); --pins; }
static void speech_owner_stop(epd_service_t *s) { (void)s; }
static void epd_system_service_stopped(epd_service_t *s) { (void)s; }
bool sleep_guard_acquire(sleep_guard_owner_t *o) { o->held = true; return true; }
bool sleep_guard_acquire_drain(sleep_guard_owner_t *o) { if (sleeping) return false; o->held = true; return true; }
void sleep_guard_release(sleep_guard_owner_t *o) { o->held = false; }
static bool portal_release_storage(void) { return true; }
static bool ui_settings_flush(void) { return true; }

#include "owner_host.h"
#include "service.inc"

static void idle_worker(epd_service_t *s) { CHECK(epd_service_cancelled(s)); }
static bool ui_process(epd_service_t *s, bool stopping) { (void)s; return stopping; }
static int command_a(int argc, char **argv) { (void)argc; (void)argv; return 11; }
static int command_b(int argc, char **argv) { (void)argc; (void)argv; return 22; }
static const epd_background_t parent_definition = { .ui_process = ui_process };
static const epd_background_t ai_definition = {
    .run = idle_worker, .stack_size = 24576, .priority = 24, .network = true,
    .ui_process = ui_process, .command = command_a
};
static const epd_background_t tts_definition = {
    .run = idle_worker, .stack_size = 8192, .priority = 25,
    .ui_process = ui_process, .command = command_b
};

static void retire_stopped(void)
{
    for (epd_service_t *s = services; s; s = s->next)
        if (s->stopping && !s->worker_done) {
            s->worker.entry(s->worker.parameter);
            s->worker.cleanup(&s->worker);
        }
    app_service_process();
}

static void companion_services(void)
{
    epd_service_t *parent = app_service_start("books", &parent_definition, (void *)1);
    CHECK(parent);
    epd_service_t *ai = app_service_start_auxiliary(parent, "ai", &ai_definition);
    epd_service_t *tts = app_service_start_auxiliary(parent, "tts", &tts_definition);
    CHECK(ai && tts && app_service_running("books.ai") && app_service_running("books.tts"));
    CHECK(!strcmp(ai->id, "books.ai") && !strcmp(tts->id, "books.tts"));
    CHECK(!strcmp(ai->parent_id, "books") && !strcmp(tts->parent_id, "books"));
    char app[RT_NAME_MAX];
    CHECK(epd_service_identity(ai, app, sizeof(app)) && !strcmp(app, "books"));
    CHECK(epd_service_identity(tts, app, sizeof(app)) && !strcmp(app, "books"));
    CHECK(pins == 3 && subscriptions == 1);
    CHECK(!strcmp(pinned_app, "books") && !strcmp(pinned_path, "/apps/books/books.so"));
    CHECK(!app_service_start_auxiliary(parent, "ai", &ai_definition));
    CHECK(strstr(log_text, "svc books.ai: start failed stage=already_running"));
    app_service_stop("books.ai");
    CHECK(ai->stopping && !tts->stopping && !parent->stopping);
    retire_stopped();
    CHECK(!app_service_running("books.ai") && app_service_running("books.tts"));
    app_service_stop("books");
    CHECK(parent->stopping && tts->stopping);
    retire_stopped();
    CHECK(!services && !allocations && !pins && !subscriptions);
}

static void truncated_thread_names(void)
{
    epd_service_t *parent = app_service_start("books", &parent_definition, NULL);
    epd_service_t *first = app_service_start_auxiliary(parent, "ai.one", &ai_definition);
    epd_service_t *second = app_service_start_auxiliary(parent, "ai.two", &tts_definition);
    CHECK(first && second && !memcmp(first->worker.name, second->worker.name, RT_NAME_MAX));
    CHECK(app_service_command("books.ai.one", 0, NULL) == 11);
    CHECK(app_service_command("books.ai.two", 0, NULL) == 22);
    epd_service_publish_summary(first, "first");
    epd_service_publish_summary(second, "second");
    char summary[32];
    CHECK(epd_app_summary_read("books.ai.one", summary, sizeof(summary)) && !strcmp(summary, "first"));
    CHECK(epd_app_summary_read("books.ai.two", summary, sizeof(summary)) && !strcmp(summary, "second"));
    app_service_stop("books.ai.one");
    CHECK(first->stopping && !second->stopping && !parent->stopping);
    retire_stopped();
    CHECK(!app_service_running("books.ai.one") && app_service_running("books.ai.two"));
    app_service_stop("books"); retire_stopped();
    CHECK(!services && !allocations && !subscriptions);
}

static void name_boundaries(void)
{
    char max_id[64], too_long[65], app[64];
    memset(max_id, 'a', 63); max_id[63] = 0;
    memset(too_long, 'b', 64); too_long[64] = 0;
    epd_service_t *full = app_service_start(max_id, &ai_definition, NULL);
    CHECK(full && !strcmp(full->id, max_id) && app_service_running(max_id));
    CHECK(epd_service_identity(full, app, sizeof(app)) && !strcmp(app, max_id));
    CHECK(!app_service_start(too_long, &ai_definition, NULL));
    CHECK(!app_service_start(max_id, &ai_definition, NULL));
    CHECK(strstr(log_text, max_id));
    app_service_stop(max_id); retire_stopped();
    CHECK(!services && !allocations && !subscriptions);

    char parent_id[56], child_id[64];
    memset(parent_id, 'p', 55); parent_id[55] = 0;
    epd_service_t *parent = app_service_start(parent_id, &parent_definition, NULL);
    CHECK(parent);
    epd_service_t *child = app_service_start_auxiliary(parent, "1234567", &tts_definition);
    snprintf(child_id, sizeof(child_id), "%s.1234567", parent_id);
    CHECK(child && !strcmp(child->id, child_id) && !strcmp(child->parent_id, parent_id));
    CHECK(!app_service_start_auxiliary(parent, "12345678", &tts_definition));
    CHECK(strstr(log_text, "stage=auxiliary_name"));
    CHECK(!app_service_start_auxiliary(child, "nested", &tts_definition));
    CHECK(strstr(log_text, "stage=auxiliary_owner"));
    app_service_stop(parent_id);
    CHECK(child->stopping);
    retire_stopped();
    CHECK(!services && !allocations);
}

static void invalid_owners(void)
{
    CHECK(!app_service_start_auxiliary(NULL, "ai", &ai_definition));
    epd_service_t *parent = app_service_start("books", &parent_definition, (void *)1);
    CHECK(parent);
    CHECK(!app_service_start_auxiliary(parent, NULL, &ai_definition));
    CHECK(!app_service_start_auxiliary(parent, "", &ai_definition));
    storage_valid = false;
    CHECK(!app_service_start_auxiliary(parent, "ai", &ai_definition));
    storage_valid = true;
    app_service_stop("books");
    CHECK(!app_service_start_auxiliary(parent, "ai", &ai_definition));
    retire_stopped();
    CHECK(!services && !allocations && !pins);
    CHECK(!app_service_start("long-app-id", &parent_definition, (void *)1));
    CHECK(strstr(log_text, "stage=identity"));
}

static void notify_counter(void *context) { ++*(unsigned *)context; }

static void stop_notification(void)
{
    unsigned calls = 0, unregistered = 0, late = 0;
    epd_service_t *service = app_service_start("books", &ai_definition, (void *)1);
    CHECK(service && pins == 1);
    CHECK(epd_service_on_stop(service, notify_counter, &unregistered));
    CHECK(epd_service_on_stop(service, NULL, NULL));
    CHECK(epd_service_on_stop(service, notify_counter, &calls));
    CHECK(!epd_service_on_stop((epd_service_t *)(uintptr_t)1, notify_counter, &late));
    app_service_stop("books");
    CHECK(calls == 1 && !unregistered && !late);
    CHECK(!service->worker_done && pins == 1 && service->stopping);
    app_service_stop("books");
    CHECK(calls == 1);
    CHECK(!epd_service_on_stop(service, notify_counter, &late));
    CHECK(late == 1);
    retire_stopped();
    CHECK(calls == 1 && late == 1 && !services && !allocations && !pins);
}

static void companion_stop_notification(void)
{
    unsigned calls = 0;
    epd_service_t *parent = app_service_start("books", &parent_definition, (void *)1);
    epd_service_t *child = app_service_start_auxiliary(parent, "layout", &tts_definition);
    CHECK(child && epd_service_on_stop(child, notify_counter, &calls));
    app_service_stop("books");
    CHECK(calls == 1 && !child->worker_done && pins == 2);
    retire_stopped();
    CHECK(calls == 1 && !services && !allocations && !pins);
}

static void stop_notification_unregistered(void)
{
    unsigned calls = 0;
    epd_service_t *service = app_service_start("books", &ai_definition, (void *)1);
    CHECK(service && epd_service_on_stop(service, notify_counter, &calls));
    CHECK(epd_service_on_stop(service, NULL, NULL));
    app_service_stop("books");
    retire_stopped();
    CHECK(!calls && !services && !allocations && !pins);
}

typedef struct probe probe_t;
struct probe {
    epd_service_t *service;
    unsigned calls, cleanup_calls, cleanup_retries;
    uint32_t cost;
    void (*hook)(probe_t *, bool);
};
static probe_t probes[16];
static unsigned probe_count, trace[256], trace_count;

static uint32_t ticks_for_ms(uint32_t ms)
{ return (uint32_t)(((uint64_t)ms * RT_TICK_PER_SECOND + 999u) / 1000u); }

static uint32_t ms_for_ticks(uint32_t ticks)
{ return (uint32_t)(((uint64_t)ticks * 1000u + RT_TICK_PER_SECOND - 1u) / RT_TICK_PER_SECOND); }

static bool probe_ui(epd_service_t *service, bool stopping)
{
    unsigned index;
    for (index = 0; index < probe_count; ++index)
        if (probes[index].service == service) break;
    CHECK(index < probe_count && trace_count < sizeof(trace) / sizeof(trace[0]));
    probe_t *p = &probes[index];
    trace[trace_count++] = index;
    if (stopping) ++p->cleanup_calls;
    else { ++p->calls; now_ticks += p->cost; }
    if (p->hook) p->hook(p, stopping);
    return stopping && p->cleanup_calls > p->cleanup_retries;
}

static probe_t *add_probe(const char *id, uint32_t cost, bool worker, void *module)
{
    CHECK(probe_count < sizeof(probes) / sizeof(probes[0]));
    probe_t *p = &probes[probe_count++];
    p->cost = cost;
    epd_background_t definition = {.ui_process = probe_ui};
    if (worker) {
        definition.run = idle_worker;
        definition.stack_size = 2048;
        definition.priority = 24;
    }
    p->service = app_service_start(id, &definition, module);
    CHECK(p->service);
    return p;
}

static probe_t *add_auxiliary(probe_t *parent, const char *name)
{
    CHECK(probe_count < sizeof(probes) / sizeof(probes[0]));
    probe_t *p = &probes[probe_count++];
    const epd_background_t definition = {.ui_process = probe_ui};
    p->service = app_service_start_auxiliary(parent->service, name, &definition);
    CHECK(p->service);
    return p;
}

static void finish_probes(void)
{
    for (unsigned i = 0; i < probe_count; ++i) {
        probes[i].hook = NULL;
        probes[i].cleanup_retries = 0;
    }
    for (epd_service_t *s = services; s; s = s->next) app_service_stop(s->id);
    for (unsigned i = 0; services && i < 32; ++i) {
        now_ticks += ticks_for_ms(100);
        retire_stopped();
    }
    CHECK(!services && !allocations && !pins && !subscriptions);
    CHECK(app_service_ui_delay(100) == 100);
}

static void foreground_selection(void)
{
    probe_t *books = add_probe("books", 0, false, NULL);
    probe_t *ai = add_auxiliary(books, "ai");
    probe_t *tts = add_auxiliary(books, "tts");
    probe_t *shelf = add_probe("bookshelf", 0, false, NULL);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground(NULL);
    app_service_ui_process_foreground("");
    app_service_ui_process_foreground("book");
    app_service_ui_process_foreground("missing");
    app_service_ui_process_foreground("books.ai");
    CHECK(trace_count == 0);
    app_service_ui_process_foreground("books");
    app_service_ui_process_foreground("books");
    CHECK(books->calls == 1 && !ai->calls && !tts->calls && !shelf->calls);
    app_service_ui_process_deferred();
    app_service_ui_process_deferred();
    CHECK(books->calls == 1 && ai->calls == 1 && tts->calls == 1 && shelf->calls == 1);
    CHECK(app_service_ui_delay(100) == 100);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_foreground("bookshelf");
    app_service_ui_process_deferred();
    CHECK(books->calls == 2 && shelf->calls == 2 && ai->calls == 2 && tts->calls == 2);
    finish_probes();
}

static void foreground_preserves_cursor(void)
{
    probe_t *a = add_probe("a", ticks_for_ms(5), false, NULL);
    probe_t *b = add_probe("b", ticks_for_ms(5), false, NULL);
    probe_t *c = add_probe("c", ticks_for_ms(5), false, NULL);
    app_service_process();
    CHECK(c->calls == 1 && !b->calls && !a->calls);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("a");
    app_service_ui_process_deferred();
    CHECK(trace_count == 3 && trace[0] == 2 && trace[1] == 0 && trace[2] == 1);
    app_service_ui_process_deferred();
    CHECK(trace_count == 3 && app_service_ui_delay(100) == 100);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("a");
    app_service_ui_process_deferred();
    CHECK(c->calls == 2 && b->calls == 1 && a->calls == 2);
    finish_probes();
}

static void deferred_fairness(void)
{
    probe_t *front = add_probe("books", ticks_for_ms(8), false, NULL);
    for (unsigned i = 0; i < 4; ++i) {
        char id[8];
        snprintf(id, sizeof(id), "bg%u", i);
        add_probe(id, ticks_for_ms(5), false, NULL);
    }
    for (unsigned round = 1; round <= 20; ++round) {
        app_service_ui_begin_iteration();
        app_service_ui_process_foreground("books");
        app_service_ui_process_deferred();
        CHECK(front->calls == round);
        for (unsigned i = 1; i < probe_count; ++i)
            CHECK(probes[i].calls >= round / 5);
    }
    finish_probes();
}

static void deferred_budget(void)
{
    uint32_t budget = ticks_for_ms(5);
    probe_t *a = add_probe("a", budget, false, NULL);
    probe_t *b = add_probe("b", 1, false, NULL);
    probe_t *c = add_probe("c", budget - 1, false, NULL);
    app_service_ui_begin_iteration();
    app_service_ui_process_deferred();
    CHECK(c->calls == 1 && b->calls == 1 && !a->calls);
    CHECK(now_ticks == budget && app_service_ui_delay(100) == 0);
    app_service_ui_begin_iteration();
    app_service_ui_process_deferred();
    CHECK(a->calls == 1 && b->calls == 1 && c->calls == 1);
    CHECK(app_service_ui_delay(100) == 100);
    finish_probes();
}

static void pending_deadline(void)
{
    probe_t *p = add_probe("books", 0, false, NULL);
    epd_service_ui_request(p->service, 50);
    epd_service_ui_request(p->service, 100);
    CHECK(app_service_ui_delay(1000) == ms_for_ticks(ticks_for_ms(50)));
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(!p->calls && app_service_ui_delay(1000) == ms_for_ticks(ticks_for_ms(50)));
    epd_service_ui_request(p->service, 30);
    now_ticks += ticks_for_ms(30) - 1;
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(!p->calls && app_service_ui_delay(1000) == ms_for_ticks(1));
    ++now_ticks;
    app_service_ui_process_deferred();
    CHECK(p->calls == 1 && app_service_ui_delay(1000) == 1000);
    app_service_ui_process_foreground("books");
    CHECK(p->calls == 1);
    finish_probes();
}

static void deadline_wraparound(void)
{
    now_ticks = UINT32_MAX - 1;
    pending_deadline();
}

static void request_again(probe_t *p, bool stopping)
{
    CHECK(!stopping);
    epd_service_ui_request(p->service, p->calls == 1 ? 0 : 30);
}

static void callback_requests(void)
{
    probe_t *p = add_probe("books", 0, false, NULL);
    p->hook = request_again;
    epd_service_ui_request(p->service, 0);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(p->calls == 1 && app_service_ui_delay(100) == 0);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(p->calls == 2 && app_service_ui_delay(100) == ms_for_ticks(ticks_for_ms(30)));
    p->hook = NULL;
    now_ticks += ticks_for_ms(30);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(p->calls == 3 && app_service_ui_delay(100) == 100);
    finish_probes();
}

static void stop_self(probe_t *p, bool stopping)
{ if (!stopping) app_service_stop(p->service->id); }

static void callback_stop(void)
{
    probe_t *p = add_probe("books", 0, false, (void *)1);
    p->hook = stop_self;
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(p->calls == 1 && !p->cleanup_calls && pins == 1);
    CHECK(app_service_running("books") && app_service_ui_delay(UINT32_MAX) == 0);
    sleeping = true;
    app_service_process();
    CHECK(p->cleanup_calls == 1 && !app_service_running("books") && !pins);
    sleeping = false;
    finish_probes();
}

static void cleanup_waits_for_caller(void)
{
    probe_t *p = add_probe("books", 0, false, (void *)1);
    CHECK(epd_service_retain(p->service));
    app_service_stop("books");
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    app_service_maintenance_process();
    CHECK(!p->calls && !p->cleanup_calls && pins == 1);
    CHECK(app_service_ui_delay(UINT32_MAX) == UINT32_MAX);
    epd_service_release(p->service);
    CHECK(app_service_ui_delay(UINT32_MAX) == 0);
    app_service_ui_process_deferred();
    CHECK(p->cleanup_calls == 1 && !app_service_running("books") && !pins);
    finish_probes();
}

static void cleanup_waits_for_worker(void)
{
    probe_t *p = add_probe("books", 0, true, (void *)1);
    app_service_stop("books");
    p->service->worker.entry(p->service->worker.parameter);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(!p->cleanup_calls && pins == 1 && allocations == 2);
    CHECK(app_service_ui_delay(UINT32_MAX) == UINT32_MAX);
    p->service->worker.cleanup(&p->service->worker);
    CHECK(app_service_ui_delay(UINT32_MAX) == 0);
    app_service_ui_process_deferred();
    CHECK(p->cleanup_calls == 1 && !pins && !allocations);
    finish_probes();
}

static void cleanup_retry(void)
{
    probe_t *p = add_probe("books", 0, false, (void *)1);
    p->cleanup_retries = 1;
    app_service_stop("books");
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(p->cleanup_calls == 1 && pins == 1);
    CHECK(app_service_ui_delay(UINT32_MAX) == ms_for_ticks(ticks_for_ms(10)));
    app_service_maintenance_process();
    CHECK(p->cleanup_calls == 1);
    now_ticks += ticks_for_ms(10);
    app_service_ui_process_deferred();
    CHECK(p->cleanup_calls == 1 && app_service_ui_delay(UINT32_MAX) == 0);
    app_service_maintenance_process();
    CHECK(p->cleanup_calls == 2 && !pins && !app_service_running("books"));
    finish_probes();
}

static void request_cleanup_retry(probe_t *p, bool stopping)
{ CHECK(stopping); if (p->cleanup_calls == 1) epd_service_ui_request(p->service, 30); }

static void cleanup_explicit_deadline(void)
{
    probe_t *p = add_probe("books", 0, false, NULL);
    p->cleanup_retries = 1;
    p->hook = request_cleanup_retry;
    app_service_stop("books");
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(app_service_ui_delay(UINT32_MAX) == ms_for_ticks(ticks_for_ms(30)));
    now_ticks += ticks_for_ms(10);
    app_service_maintenance_process();
    CHECK(p->cleanup_calls == 1);
    now_ticks += ticks_for_ms(20);
    app_service_ui_begin_iteration();
    app_service_ui_process_deferred();
    CHECK(p->cleanup_calls == 2 && !services);
    finish_probes();
}

static void nested_process(probe_t *p, bool stopping)
{
    CHECK(!stopping);
    unsigned count = trace_count;
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground(p->service->id);
    app_service_ui_process_foreground("other");
    app_service_ui_process_deferred();
    app_service_process();
    app_service_maintenance_process();
    CHECK(trace_count == count);
}

static void reentry_guard(void)
{
    probe_t *p = add_probe("books", 0, false, NULL);
    probe_t *other = add_probe("other", 0, false, NULL);
    p->hook = nested_process;
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    app_service_ui_process_foreground("books");
    CHECK(p->calls == 1 && other->calls == 1);
    app_service_ui_begin_iteration();
    app_service_ui_process_deferred();
    CHECK(p->calls == 2 && other->calls == 2);
    finish_probes();
}

static void legacy_independent_calls(void)
{
    probe_t *p = add_probe("books", 0, false, NULL);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_process();
    app_service_process();
    CHECK(p->calls == 3);
    app_service_maintenance_process();
    CHECK(p->calls == 3);
    epd_service_ui_request(p->service, 0);
    app_service_maintenance_process();
    epd_service_ui_request(p->service, 0);
    app_service_maintenance_process();
    CHECK(p->calls == 5);
    app_service_ui_process_deferred();
    CHECK(p->calls == 5 && app_service_ui_delay(100) == 100);
    finish_probes();
}

static void sleeping_maintenance(void)
{
    probe_t *p = add_probe("books", 0, false, NULL);
    sleeping = true;
    epd_service_ui_request(p->service, 30);
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_process();
    CHECK(!p->calls && app_service_ui_delay(UINT32_MAX) == ms_for_ticks(ticks_for_ms(30)));
    now_ticks += ticks_for_ms(30);
    app_service_ui_process_foreground("books");
    CHECK(!p->calls);
    app_service_process();
    app_service_process();
    CHECK(p->calls == 1);
    epd_service_ui_request(p->service, 0);
    app_service_process();
    CHECK(p->calls == 2);
    app_service_stop("books");
    app_service_process();
    CHECK(p->cleanup_calls == 1 && !services);
    sleeping = false;
    finish_probes();
}

static bool expect_ui_dispatch;
static void check_ui_dispatch(probe_t *p, bool stopping)
{
    CHECK(host_ui_dispatch == (expect_ui_dispatch && !stopping ?
          p->service->memory_owner : EPD_MEMORY_OWNER_INVALID));
}
static void dispatch_lifetime(void)
{
    probe_t *p = add_probe("dispatch", 0, false, NULL);
    p->hook = check_ui_dispatch;
    expect_ui_dispatch = true; app_service_process();
    CHECK(p->calls == 1 && host_ui_dispatch == EPD_MEMORY_OWNER_INVALID);
    expect_ui_dispatch = false;
    epd_service_ui_request(p->service, 0); app_service_maintenance_process();
    sleeping = true; epd_service_ui_request(p->service, 0); app_service_process(); sleeping = false;
    storage_handoff = true; epd_service_ui_request(p->service, 0); app_service_process(); storage_handoff = false;
    reset_pending = true; epd_service_ui_request(p->service, 0); app_service_process(); reset_pending = false;
    CHECK(p->calls == 5 && host_ui_dispatch == EPD_MEMORY_OWNER_INVALID);
    finish_probes();
}

static void foreground_removes_cursor(void)
{
    probe_t *a = add_probe("a", ticks_for_ms(5), false, NULL);
    probe_t *b = add_probe("b", ticks_for_ms(5), false, NULL);
    probe_t *c = add_probe("c", ticks_for_ms(5), false, NULL);
    app_service_process();
    CHECK(c->calls == 1 && !a->calls && !b->calls);
    app_service_stop("b");
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("b");
    CHECK(b->cleanup_calls == 1 && !app_service_running("b"));
    app_service_ui_process_deferred();
    CHECK(a->calls == 1 && c->calls == 1);
    app_service_ui_process_deferred();
    CHECK(c->calls == 2 && app_service_ui_delay(100) == 100);
    finish_probes();
}

static probe_t *command_probe;
static int stop_from_command(int argc, char **argv)
{
    (void)argc; (void)argv;
    app_service_stop("books");
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    app_service_maintenance_process();
    CHECK(!command_probe->cleanup_calls && pins == 1);
    return 33;
}

static void command_lifetime(void)
{
    CHECK(probe_count == 0);
    command_probe = &probes[probe_count++];
    const epd_background_t definition = {.ui_process = probe_ui, .command = stop_from_command};
    command_probe->service = app_service_start("books", &definition, (void *)1);
    CHECK(command_probe->service);
    app_service_ui_begin_iteration();
    CHECK(app_service_command("books", 0, NULL) == 33);
    CHECK(app_service_ui_delay(UINT32_MAX) == 0);
    app_service_ui_process_deferred();
    CHECK(command_probe->cleanup_calls == 1 && !pins && !services);
    finish_probes();
}

static void invalid_storage_cleanup(void)
{
    probe_t *p = add_probe("books", 0, false, (void *)1);
    storage_valid = false;
    app_service_ui_begin_iteration();
    app_service_ui_process_foreground("books");
    app_service_ui_process_deferred();
    CHECK(!p->calls && p->cleanup_calls == 1 && !pins && !services);
    storage_valid = true;
    finish_probes();
}

static void idle_stop_preserves_other_module(void)
{
    epd_service_t *books = app_service_start("books", &ai_definition, (void *)1);
    epd_service_t *music = app_service_start("music", &ai_definition, (void *)1);
    CHECK(books && music && pins == 2 && subscriptions == 2);
    CHECK(epd_service_retain(books));
    app_service_stop("books");
    app_service_process();
    CHECK(pins == 2 && books->callers == 1 && !music->stopping);
    CHECK(!app_service_start("books", &ai_definition, (void *)1));
    CHECK(app_service_command("music", 0, NULL) == 11);
    epd_service_release(books);
    retire_stopped();
    CHECK(!app_service_running("books") && app_service_running("music"));
    CHECK(pins == 1 && subscriptions == 1 && !music->stopping);
    books = app_service_start("books", &ai_definition, (void *)1);
    CHECK(books && pins == 2 && !books->stopping);
    app_service_stop("books"); retire_stopped();
    CHECK(pins == 1 && !music->stopping);
    app_service_stop("music"); retire_stopped();
    CHECK(!services && !allocations && !pins && !subscriptions);
}

static const struct { const char *name; void (*run)(void); } cases[] = {
    {"dispatch_lifetime", dispatch_lifetime},
    {"idle_stop_preserves_other_module", idle_stop_preserves_other_module},
    {"companion_services", companion_services},
    {"truncated_thread_names", truncated_thread_names},
    {"name_boundaries", name_boundaries},
    {"invalid_owners", invalid_owners},
    {"stop_notification", stop_notification},
    {"companion_stop_notification", companion_stop_notification},
    {"stop_notification_unregistered", stop_notification_unregistered},
    {"foreground_selection", foreground_selection},
    {"foreground_preserves_cursor", foreground_preserves_cursor},
    {"deferred_fairness", deferred_fairness},
    {"deferred_budget", deferred_budget},
    {"pending_deadline", pending_deadline},
    {"deadline_wraparound", deadline_wraparound},
    {"callback_requests", callback_requests},
    {"callback_stop", callback_stop},
    {"cleanup_waits_for_caller", cleanup_waits_for_caller},
    {"cleanup_waits_for_worker", cleanup_waits_for_worker},
    {"cleanup_retry", cleanup_retry},
    {"cleanup_explicit_deadline", cleanup_explicit_deadline},
    {"reentry_guard", reentry_guard},
    {"legacy_independent_calls", legacy_independent_calls},
    {"sleeping_maintenance", sleeping_maintenance},
    {"foreground_removes_cursor", foreground_removes_cursor},
    {"command_lifetime", command_lifetime},
    {"invalid_storage_cleanup", invalid_storage_cleanup},
};

int main(int argc, char **argv)
{
    CHECK(epd_memory_init() == EPD_MEMORY_OK);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    CHECK(RT_NAME_MAX == 8 && EPD_SERVICE_ID_SIZE == 64);
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) puts(cases[i].name);
        return 0;
    }
    if (argc == 2)
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
            if (!strcmp(argv[1], cases[i].name)) {
                cases[i].run();
                printf("PASS %s (%u checks; ticks/s=%u)\n", cases[i].name, checks,
                       (unsigned)RT_TICK_PER_SECOND);
                return 0;
            }
    fprintf(stderr, "Usage: %s --list|CASE\n", argv[0]);
    return 2;
}
