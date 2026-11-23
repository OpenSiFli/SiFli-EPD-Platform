#define HOST_REAL_PRESSURE
#define main service_test_main
#include "service_host.inc"
#undef main
#include "app_memory_pressure.h"
#include "epd_resource.h"
#define RT_USING_FINSH
#define MSH_CMD_EXPORT(command, description)

#include "heap_fixture.h"
#include "memory_capacity.inc"

void app_catalog_background_resume(void);
void app_ui_signal(uint32_t events) { (void)events; }
#include "pressure.inc"

static epd_resource_reclaim_snapshot_t resource_snapshot;
static epd_memory_backend_t resource_test_backend;
static size_t resource_candidate;
static size_t resource_requested;
static unsigned resource_selects;
size_t epd_resource_reclaim(uint32_t backend, size_t wanted, size_t maximum)
{
    CHECK(!interrupt_depth && backend == resource_test_backend && wanted && maximum == 1);
    ++resource_selects;
    resource_requested = wanted;
    size_t selected = resource_candidate;
    resource_candidate = 0;
    if (selected) {
        resource_snapshot.pending_objects = 1;
        resource_snapshot.pending_bytes = selected;
    }
    return selected;
}
void epd_resource_reclaim_status(uint32_t backend, epd_resource_reclaim_snapshot_t *snapshot)
{
    CHECK(!interrupt_depth && backend == resource_test_backend);
    *snapshot = resource_snapshot;
}
static void resource_backend_register(void)
{
    epd_memory_backend_config_t config = {0};
    config.region = EPD_MEMORY_REGION_HCPU_PSRAM;
    config.capacity = 1000000;
    config.parent = EPD_MEMORY_BACKEND_NONE;
    CHECK(epd_memory_backend_register(&app_image_psram_memheap, &config,
                                     &resource_test_backend) == EPD_MEMORY_OK);
}

static unsigned background_revision = UINT32_MAX, test_catalog_revision;
static unsigned discoveries, automatic_starts;
#define STORAGE_FLASH 0
#define STORAGE_COUNT 1
typedef struct { int index; } DIR;
struct dirent { char d_name[RT_NAME_MAX]; };
typedef struct { char id[RT_NAME_MAX]; bool background, resources_only; } epd_app_entry_t;
typedef struct { unsigned entry_func, user_data; } app_entity_info;
static int app_recovery_check_dynamic(void) { return RT_EOK; }
static bool app_installer_busy(void) { return false; }
static uint32_t app_catalog_revision(void) { return test_catalog_revision; }
static bool app_installer_volume_ready(int v) { (void)v; return true; }
static bool storage_apps_enabled(int v) { (void)v; return true; }
static const char *storage_app_directory(int v) { (void)v; return "/apps"; }
static void storage_lock(void) {}
static void storage_unlock(void) {}
static DIR *opendir(const char *name) { static DIR d; (void)name; d.index = 0; ++discoveries; return &d; }
static struct dirent *readdir(DIR *d) { static struct dirent e = {"weather"}; return d->index++ ? NULL : &e; }
static void closedir(DIR *d) { (void)d; }
static bool app_package_read(const char *directory, const char *id, epd_app_entry_t *entry)
{ (void)directory; strcpy(entry->id, id); entry->background = true; entry->resources_only = false; return true; }
static bool storage_app_locate(const char *id, storage_volume_t *v) { (void)id; *v = STORAGE_FLASH; return true; }
static void *array_grow(void *items, unsigned *capacity, unsigned count, size_t size)
{ CHECK(!items && !count); *capacity = 1; return epd_app_alloc(size, EPD_APP_PSRAM); }
#define LOG_W(...) ((void)0)
static epd_service_t *reclaimable(const char *id, const epd_background_t *definition)
{
    epd_service_t *s = app_service_start(id, definition, (void *)1);
    CHECK(s);
    app_service_set_background(id, true);
    epd_service_set_reclaimable(s, true);
    return s;
}
static bool module_open_requested(const char *id, app_entity_info *info, bool explicit_enter)
{
    (void)info;
    if (!app_background_should_start(id, explicit_enter)) return false;
    epd_memory_owner_t root = app_module_owner_acquire(id);
    CHECK(root != EPD_MEMORY_OWNER_INVALID);
    CHECK(app_background_bind(id, root) == EPD_MEMORY_OK);
    CHECK(app_background_foreground(root, false) == EPD_MEMORY_OK);
    epd_memory_owner_release(root);
    reclaimable(id, &ai_definition);
    ++automatic_starts;
    return true;
}
static bool module_close(const char *id, const app_entity_info *info) { (void)id; (void)info; return true; }
#include "catalog.inc"

#define GUI_APP_ID_MAX_LEN 16
typedef struct { uint32_t session, state, paths[6]; } app_package_revision_t;
static app_package_revision_t estimate_revision;
static bool estimate_revision_valid = true, estimate_resident;
static uint32_t estimate_storage_revision;
static uint32_t storage_revision(void) { return estimate_storage_revision; }
static bool builtin_id(const char *id) { return !strcmp(id, "setting"); }
static void *dlmodule_find(const char *id) { (void)id; return estimate_resident ? (void *)1 : NULL; }
static bool app_package_revision(const char *root, const char *id, const char *library,
                                 app_package_revision_t *out)
{
    CHECK(root && id && !library);
    *out = estimate_revision;
    return estimate_revision_valid;
}
#include "../cold_static_psram/host_sections.h"
#include "load_estimate.inc"

static void advance_ms(uint32_t ms) { now_ticks += pressure_ticks(ms); }
static void low_memory(void) { available_bytes = 100; }
static void recovered(void) { available_bytes = 1000000; }
static void stop_all(void)
{
    for (epd_service_t *s = services; s; s = s->next) app_service_stop(s->id);
    retire_stopped();
    CHECK(!services && !allocations && !pins && !subscriptions);
}

static void no_pressure(void)
{
    epd_service_t *s = reclaimable("weather", &ai_definition);
    CHECK(!app_memory_pressure_request(1000));
    app_memory_pressure_process("books", false);
    CHECK(!app_memory_pressure_query().generation && !s->stopping);
    CHECK(!app_memory_pressure_background_blocked());
    low_memory();
    CHECK(app_memory_pressure_request(200));
    recovered();
    app_memory_pressure_process("books", false);
    CHECK(app_memory_pressure_query().result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    stop_all();
}

static void merged_cancel(void)
{
    unsigned cancelled = 0;
    epd_service_t *s = reclaimable("weather", &ai_definition);
    CHECK(epd_service_on_stop(s, notify_counter, &cancelled));
    low_memory();
    uint32_t generation = app_memory_pressure_request(500);
    for (unsigned i = 0; i < 1000; ++i) CHECK(app_memory_pressure_request(1000) == generation);
    CHECK(app_memory_pressure_query().min_available == 1000);
    app_memory_pressure_process("books", false);
    CHECK(cancelled == 1 && s->stopping && pins == 1 && s->worker_stack);
    CHECK(!epd_service_reclaim_hold(s) && !epd_service_retain(s));
    CHECK(app_service_command("weather", 0, NULL) == -RT_ENOSYS);
    for (unsigned i = 0; i < 5; ++i) app_memory_pressure_process("books", false);
    CHECK(cancelled == 1 && pins == 1 && app_memory_pressure_query().pending);
    CHECK(!speech_stops && !system_stops && !log_text[0]);
    retire_stopped();
    CHECK(!services && !pins && !allocations && !subscriptions);
    CHECK(speech_stops == 1 && system_stops == 1);
    CHECK(strstr(log_text, "svc weather: pressure released\n"));
    recovered();
    app_memory_pressure_process("books", false);
    CHECK(!app_memory_pressure_query().pending);
    CHECK(app_memory_pressure_query().result == APP_MEMORY_PRESSURE_SATISFIED);
    CHECK(strstr(log_text, "mem pressure gen=1 satisfied"));
    size_t logged = strlen(log_text);
    for (unsigned i = 0; i < 100; ++i) app_memory_pressure_process("books", false);
    CHECK(strlen(log_text) == logged);
}

static void protected_apps(void)
{
    epd_service_t *front = reclaimable("weather", &ai_definition);
    epd_service_t *music = app_service_start("music", &ai_definition, (void *)1);
    epd_service_t *unknown = app_service_start("books", &parent_definition, (void *)1);
    CHECK(music && unknown);
    app_service_set_background("music", true);
    epd_service_set_reclaimable(unknown, true);
    app_service_set_background("books", true); /* UI-only owners stay protected. */
    low_memory();
    app_memory_pressure_request(1000);
    app_memory_pressure_process(NULL, false);
    CHECK(app_memory_pressure_query().pending && !front->stopping);
    app_memory_pressure_process("weather", false);
    CHECK(app_memory_pressure_query().result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(!front->stopping && !music->stopping && !unknown->stopping);
    CHECK(app_service_command("music", 0, NULL) == 11);
    stop_all();
}

static unsigned flush_calls;
static bool saved;
static epd_service_t *saving_service;
static bool admit_during_flush;
static bool save_step(void)
{
    ++flush_calls;
    if (admit_during_flush)
    {
        CHECK(epd_service_reclaim_hold(saving_service));
        epd_service_reclaim_release(saving_service);
    }
    return saved;
}
static const epd_background_t saving_definition = {
    .run = idle_worker, .stack_size = 8192, .priority = 24,
    .ui_process = ui_process, .flush = save_step, .command = command_a
};

static void save_and_auxiliary(void)
{
    epd_service_t *root = reclaimable("weather", &saving_definition);
    epd_service_t *aux = app_service_start_auxiliary(root, "save", &tts_definition);
    CHECK(aux && epd_service_reclaim_hold(aux));
    low_memory(); app_memory_pressure_request(1000);
    app_memory_pressure_process("books", false);
    CHECK(!flush_calls && !root->stopping && !aux->stopping);
    epd_service_reclaim_release(aux);
    CHECK(epd_service_retain(aux));
    app_memory_pressure_process("books", false);
    CHECK(!flush_calls && !root->stopping);
    epd_service_release(aux);
    app_memory_pressure_process("books", false);
    CHECK(flush_calls == 1 && !root->stopping); /* Pending or failed save. */
    saved = true;
    app_memory_pressure_process("books", false);
    CHECK(flush_calls == 2 && !root->stopping); /* Exactly one callback per step. */
    app_memory_pressure_process("books", false);
    CHECK(root->stopping && aux->stopping);
    retire_stopped();
    CHECK(!services && !pins && !allocations && !subscriptions);
}

static void flush_race_and_navigation(void)
{
    saving_service = reclaimable("weather", &saving_definition);
    epd_service_t *music = app_service_start("music", &ai_definition, (void *)1);
    CHECK(music);
    CHECK(!app_service_flush_app("weather"));
    CHECK(!saving_service->stopping && !music->stopping);
    saved = true; admit_during_flush = true;
    low_memory(); app_memory_pressure_request(1000);
    app_memory_pressure_process("books", false);
    app_memory_pressure_process("books", false);
    CHECK(flush_calls == 3 && !saving_service->stopping);
    admit_during_flush = false;
    app_memory_pressure_process("books", false);
    CHECK(flush_calls == 4);
    /* New work after successful flush invalidates the barrier as well. */
    CHECK(epd_service_reclaim_hold(saving_service));
    epd_service_reclaim_release(saving_service);
    app_memory_pressure_process("books", false);
    CHECK(flush_calls == 5 && !saving_service->stopping);
    CHECK(epd_service_retain(saving_service));
    epd_service_release(saving_service);
    app_memory_pressure_process("books", false);
    CHECK(flush_calls == 6 && !saving_service->stopping);
    CHECK(app_service_command("weather", 0, NULL) == 11);
    app_memory_pressure_process("books", false);
    CHECK(flush_calls == 7 && !saving_service->stopping);
    app_memory_pressure_process("weather", false); /* Foreground changed meanwhile. */
    CHECK(!saving_service->stopping && !music->stopping);
    stop_all();
}

static void deadline_and_cooldown(void)
{
    epd_service_t *s = reclaimable("weather", &saving_definition);
    low_memory();
    uint32_t generation = app_memory_pressure_request(1000);
    app_memory_pressure_process("books", false);
    advance_ms(4990);
    CHECK(app_memory_pressure_request(2000) == generation);
    advance_ms(10);
    app_memory_pressure_process("books", false);
    CHECK(app_memory_pressure_query().result == APP_MEMORY_PRESSURE_TIMED_OUT && !s->stopping);
    for (unsigned i = 0; i < 1000; ++i) CHECK(app_memory_pressure_request(1000) == generation);
    CHECK(!app_memory_pressure_query().pending);
    advance_ms(1000);
    CHECK(app_memory_pressure_request(1000) != generation);
    CHECK(app_memory_pressure_query().pending);
    stop_all();
}

static void restart_hysteresis(void)
{
    app_catalog_process();
    CHECK(automatic_starts == 1 && discoveries == 1);
    low_memory(); app_memory_pressure_request(1000);
    ++test_catalog_revision;
    app_catalog_process();
    CHECK(automatic_starts == 1 && discoveries == 1); /* Even before first UI step. */
    app_memory_pressure_process("books", false);
    retire_stopped();
    app_catalog_process();
    CHECK(!services && automatic_starts == 1);
    recovered(); app_memory_pressure_process("books", false);
    advance_ms(10000); app_memory_pressure_process("books", false);
    app_catalog_process(); CHECK(!services);
    available_bytes = 1000 + PRESSURE_HEADROOM - 1;
    app_memory_pressure_process("Main", true);
    advance_ms(10000); app_memory_pressure_process("Main", true);
    CHECK(app_memory_pressure_background_blocked());
    recovered(); app_memory_pressure_process("Main", true);
    advance_ms(4990); app_memory_pressure_process("Main", true);
    CHECK(app_memory_pressure_background_blocked());
    low_memory(); app_memory_pressure_process("Main", true); /* Restart stability timer. */
    recovered(); app_memory_pressure_process("Main", true);
    advance_ms(5000); app_memory_pressure_process("Main", true);
    CHECK(!app_memory_pressure_background_blocked());
    app_catalog_process();
    CHECK(automatic_starts == 1 && discoveries == 2 && !app_service_running("weather"));
    app_entity_info info;
    CHECK(module_open_requested("weather", &info, true));
    CHECK(automatic_starts == 2 && app_service_running("weather"));
    stop_all();
}

static void fair_candidates(void)
{
    epd_service_t *first = reclaimable("weather", &ai_definition);
    epd_service_t *blocked = reclaimable("books", &saving_definition);
    CHECK(epd_service_reclaim_hold(blocked));
    low_memory(); app_memory_pressure_request(1000);
    app_memory_pressure_process("Main", false);
    app_memory_pressure_process("Main", false);
    CHECK(first->stopping && !blocked->stopping);
    retire_stopped();
    epd_service_reclaim_release(blocked);
    stop_all();
}

static void clock_wrap(void)
{
    now_ticks = UINT32_MAX - pressure_ticks(1000);
    deadline_and_cooldown();
}

#include "weather_fixture.h"

static void catalog_load_estimate(void)
{
    CHECK(!app_catalog_load_required(NULL) && !app_catalog_load_required("unknown"));
    remember_load_required("/apps/weather", "weather", 12345, &estimate_revision);
    CHECK(app_catalog_load_required("weather") == 12345);
    estimate_resident = true;
    CHECK(!app_catalog_load_required("weather"));
    estimate_resident = false;
    CHECK(app_catalog_load_required("weather") == 12345);
    ++estimate_revision.paths[0];
    CHECK(!app_catalog_load_required("weather"));
    remember_load_required("/apps/weather", "weather", 23456, &estimate_revision);
    CHECK(app_catalog_load_required("weather") == 23456);
    ++test_catalog_revision;
    CHECK(!app_catalog_load_required("weather"));
    remember_load_required("/apps/weather", "weather", 23456, &estimate_revision);
    ++estimate_storage_revision;
    CHECK(!app_catalog_load_required("weather"));
    remember_load_required("/apps/weather", "weather", 23456, &estimate_revision);
    estimate_revision_valid = false;
    CHECK(!app_catalog_load_required("weather"));
    estimate_revision_valid = true;
    app_package_revision_t previous = estimate_revision;
    ++estimate_revision.session;
    remember_load_required("/apps/other", "other", 98765, &previous);
    CHECK(!app_catalog_load_required("other"));
    remember_load_required("/apps/setting", "setting", 98765, &estimate_revision);
    CHECK(!app_catalog_load_required("setting"));
    CHECK(!discoveries && !allocations && !automatic_starts);
}

static void diagnostic_read_and_request(void)
{
    char *query[] = {"memory_pressure"};
    CHECK(memory_pressure(1, query) == RT_EOK);
    CHECK(!pressure.generation && !background_blocked && !allocations);
    CHECK(strstr(log_text, "available=1000000 blocked=0"));
    epd_service_t *s = reclaimable("weather", &ai_definition);
    unsigned allocated = allocations;
    low_memory();
    char *request[] = {"memory_pressure", "200"};
    CHECK(memory_pressure(2, request) == RT_EOK);
    CHECK(pressure.generation == 1 && pressure.pending && pressure.min_available == 200);
    CHECK(!s->stopping && allocations == allocated); /* Shell only publishes. */
    uint32_t started = pressure_started;
    advance_ms(1000);
    CHECK(memory_pressure(2, request) == RT_EOK);
    CHECK(pressure.generation == 1 && pressure_started == started);
    CHECK(memory_pressure(1, query) == RT_EOK);
    CHECK(!s->stopping && allocations == allocated);
    CHECK(strstr(log_text, "available=100 blocked=1"));
    stop_all();
}

static void diagnostic_invalid_numbers(void)
{
    const char *invalid[] = {"", "-1", "+1", " 123", "123 ", "0x100", "123x",
                             "18446744073709551616", "999999999999999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        char *args[] = {"memory_pressure", (char *)invalid[i]};
        CHECK(memory_pressure(2, args) == -RT_EINVAL);
        CHECK(!pressure.generation && !pressure.pending && !background_blocked && !allocations);
    }
    char *extra[] = {"memory_pressure", "123", "456"};
    CHECK(memory_pressure(3, extra) == -RT_EINVAL);
    char *zero[] = {"memory_pressure", "0"};
    CHECK(memory_pressure(2, zero) == RT_EOK && !pressure.generation);
    char maximum[32];
    snprintf(maximum, sizeof(maximum), "%zu", SIZE_MAX);
    char *max_args[] = {"memory_pressure", maximum};
    CHECK(memory_pressure(2, max_args) == RT_EOK);
    app_memory_pressure_status_t snapshot = app_memory_pressure_query();
    CHECK(snapshot.min_available == SIZE_MAX && !snapshot.pending && !allocations);
    CHECK(snapshot.generation && snapshot.result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(!app_memory_pressure_background_blocked());
}

static void impossible_api_then_normal(void)
{
    epd_service_t *s = reclaimable("weather", &ai_definition);
    low_memory();
    unsigned allocated = allocations;
    size_t capacity = epd_app_psram_capacity();
    CHECK(capacity == 1000000u);
    uint32_t rejected = app_memory_pressure_request(capacity + 1);
    app_memory_pressure_status_t status = app_memory_pressure_query();
    CHECK(rejected && status.generation == rejected && !status.pending);
    CHECK(status.result == APP_MEMORY_PRESSURE_EXHAUSTED && !background_blocked);
    for (unsigned i = 0; i < 100; ++i)
        CHECK(app_memory_pressure_request(capacity + 1) == rejected);
    app_memory_pressure_process("books", false);
    CHECK(!s->stopping && allocations == allocated && !background_blocked);
    CHECK(!app_memory_pressure_request(available_bytes));
    uint32_t normal = app_memory_pressure_request(1000);
    CHECK(normal && normal != rejected && app_memory_pressure_query().pending);
    CHECK(app_memory_pressure_query().generation == normal);
    app_memory_pressure_process("books", false);
    CHECK(s->stopping);
    retire_stopped();
    recovered(); app_memory_pressure_process("Main", true);
    CHECK(app_memory_pressure_query().result == APP_MEMORY_PRESSURE_SATISFIED);
    advance_ms(5000); app_memory_pressure_process("Main", true);
    CHECK(!background_blocked && !services && !allocations && !pins);
}

static void impossible_does_not_poison_live_request(void)
{
    epd_service_t *s = reclaimable("weather", &ai_definition);
    low_memory();
    uint32_t normal = app_memory_pressure_request(1000);
    uint32_t started = pressure_started;
    advance_ms(500);
    CHECK(app_memory_pressure_request(SIZE_MAX) != normal);
    CHECK(app_memory_pressure_query().result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(pressure.pending && pressure.generation == normal && pressure.min_available == 1000);
    CHECK(pressure_started == started && !s->stopping);
    CHECK(app_memory_pressure_request(1000) == normal);
    CHECK(app_memory_pressure_query().pending && app_memory_pressure_query().generation == normal);
    app_memory_pressure_process("books", false);
    CHECK(s->stopping);
    retire_stopped();
    recovered(); app_memory_pressure_process("Main", true);
    uint32_t stable_since = resume_started;
    advance_ms(1000);
    app_memory_pressure_request(SIZE_MAX);
    app_memory_pressure_process("Main", true);
    CHECK(resume_tracking && resume_started == stable_since);
    advance_ms(4000); app_memory_pressure_process("Main", true);
    CHECK(!background_blocked && !services && !allocations && !pins);
}

static void diagnostic_over_capacity_then_normal(void)
{
    epd_service_t *s = reclaimable("weather", &ai_definition);
    unsigned allocated = allocations;
    low_memory();
    char above_capacity[32];
    snprintf(above_capacity, sizeof(above_capacity), "%zu", epd_app_psram_capacity() + 1);
    char *bad[] = {"memory_pressure", above_capacity};
    CHECK(memory_pressure(2, bad) == RT_EOK);
    app_memory_pressure_status_t status = app_memory_pressure_query();
    CHECK(status.generation && !status.pending && status.result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(strstr(log_text, "pending=0 result=3") && strstr(log_text, "blocked=0 capacity=1000000"));
    app_memory_pressure_process("books", false);
    CHECK(!s->stopping && allocations == allocated);
    char *normal[] = {"memory_pressure", "200"};
    CHECK(memory_pressure(2, normal) == RT_EOK);
    CHECK(app_memory_pressure_query().pending && app_memory_pressure_query().generation != status.generation);
    app_memory_pressure_process("books", false);
    CHECK(s->stopping);
    retire_stopped();
    CHECK(!services && !allocations && !pins);
}

static void capacity_and_recovery_boundary(void)
{
    size_t pool = app_image_psram_memheap.pool_size;
    app_image_psram_memheap.pool_size = 0;
    available_bytes = 0;
    CHECK(!epd_app_psram_capacity());
    CHECK(app_memory_pressure_request(1));
    CHECK(!app_memory_pressure_query().pending && !background_blocked);
    app_image_psram_memheap.pool_size = 2u * RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    CHECK(!epd_app_psram_capacity());
    app_image_psram_memheap.pool_size = pool;
    epd_service_t *s = reclaimable("weather", &ai_definition);
    size_t capacity = epd_app_psram_capacity();
    CHECK(capacity == 1000000u);
    CHECK(app_memory_pressure_request(capacity));
    CHECK(app_memory_pressure_query().pending);
    available_bytes = capacity;
    app_memory_pressure_process("Main", true);
    CHECK(app_memory_pressure_query().result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    advance_ms(5000); app_memory_pressure_process("Main", true);
    CHECK(!background_blocked);
    stop_all();
}

static void successful_reclaim_then_new_shortage(void)
{
    epd_service_t *s = reclaimable("weather", &ai_definition);
    low_memory();
    uint32_t first = app_memory_pressure_request(1000);
    recovered();
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    low_memory();
    uint32_t second = app_memory_pressure_request(2000);
    CHECK(second != first && pressure.pending && pressure.min_available == 2000);
    app_memory_pressure_process("books", false);
    CHECK(s->stopping && pressure.pending);
    retire_stopped();
    recovered();
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !allocations && !pins);
}

static void fragmented_release(void)
{
    epd_service_t *s = reclaimable("weather", &ai_definition);
    CHECK(!app_memory_pressure_request(690000));
    uint32_t gen = app_memory_pressure_allocation_failed(690000);
    CHECK(gen && pressure.pending && pressure.min_available == 690000);
    app_memory_pressure_process("books", false);
    CHECK(s->stopping && pins == 1 && pressure.pending);
    for (unsigned i = 0; i < 100; ++i) {
        app_memory_pressure_process("books", false);
        CHECK(pressure.pending && pins == 1);
    }
    retire_stopped();
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !pins && !allocations);
    CHECK(app_memory_pressure_allocation_failed(690000) != gen && pressure.pending);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED);
    available_bytes = 690000 + PRESSURE_HEADROOM;
    app_memory_pressure_process("Main", true);
    advance_ms(5000); app_memory_pressure_process("Main", true);
    CHECK(!background_blocked); /* Uses real demand, not previous free + demand. */
}
static void fragmented_protected(void)
{
    epd_service_t *front = reclaimable("weather", &ai_definition);
    epd_service_t *music = app_service_start("music", &ai_definition, (void *)1);
    app_service_set_background("music", true);
    CHECK(app_memory_pressure_allocation_failed(500));
    app_memory_pressure_process("weather", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(!front->stopping && !music->stopping);
    stop_all();
}
static void fragmented_merge_timeout(void)
{
    epd_service_t *s = reclaimable("weather", &saving_definition);
    low_memory(); uint32_t normal = app_memory_pressure_request(1000);
    recovered(); app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    uint32_t forced = app_memory_pressure_allocation_failed(500);
    CHECK(forced != normal && pressure.pending); /* Normal cooldown cannot skip reclamation. */
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && flush_calls == 1 && !s->stopping);
    advance_ms(4990);
    CHECK(app_memory_pressure_allocation_failed(2000) == forced);
    advance_ms(10); app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_TIMED_OUT && !s->stopping);
    CHECK(pressure.min_available == 2000 && flush_calls == 1);
    stop_all();
}
static void fragmented_merge_stopping(void)
{
    epd_service_t *s = reclaimable("weather", &ai_definition);
    low_memory(); uint32_t generation = app_memory_pressure_request(1000);
    app_memory_pressure_process("books", false);
    CHECK(s->stopping && pressure.pending);
    CHECK(app_memory_pressure_allocation_failed(500) == generation);
    recovered(); app_memory_pressure_process("books", false);
    CHECK(pressure.pending && pins == 1);
    retire_stopped(); app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !pins);
}

static void resource_completion_barrier(void)
{
    resource_backend_register();
    epd_service_t *s = reclaimable("weather", &ai_definition);
    low_memory();
    resource_candidate = 900;
    CHECK(app_memory_pressure_request(1000));
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && resource_selects == 1 && !s->stopping);
    for (unsigned i = 0; i < 10; ++i) app_memory_pressure_process("books", false);
    CHECK(pressure.pending && resource_selects == 1 && !s->stopping);
    resource_snapshot.pending_objects = 0;
    resource_snapshot.pending_bytes = 0;
    resource_snapshot.reclaimed_bytes = 900;
    available_bytes = 1000;
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    stop_all();
}
static void resource_failure_fallback(void)
{
    resource_backend_register();
    epd_service_t *s = reclaimable("weather", &ai_definition);
    low_memory(); resource_candidate = 900;
    CHECK(app_memory_pressure_request(1000));
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && !s->stopping);
    resource_snapshot.pending_objects = 0;
    resource_snapshot.pending_bytes = 0;
    resource_snapshot.failed_objects = 1;
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && s->stopping && !resource_snapshot.reclaimed_bytes);
    retire_stopped(); recovered();
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !allocations && !pins);
}
static void resource_fragmentation_completion(void)
{
    resource_backend_register();
    epd_service_t *s = reclaimable("weather", &ai_definition);
    resource_candidate = 100;
    CHECK(app_memory_pressure_allocation_failed(500));
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && !s->stopping);
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && !s->stopping);
    resource_snapshot.pending_objects = 0;
    resource_snapshot.pending_bytes = 0;
    resource_snapshot.reclaimed_bytes = 100;
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    stop_all();
}

#include "block_cases.inc"

static const struct { const char *name; void (*run)(void); } pressure_cases[] = {
    {"successful_reclaim_then_new_shortage", successful_reclaim_then_new_shortage},
    {"block_scan_late_fit", block_scan_late_fit},
    {"block_span_boundary", block_span_boundary},
    {"block_empty_heap", block_empty_heap},
    {"block_normal_no_scan", block_normal_no_scan},
    {"block_heap_busy", block_heap_busy},
    {"block_merge_deadline", block_merge_deadline},
    {"block_merge_clock_wrap", block_merge_clock_wrap},
    {"block_merge_during_scan", block_merge_during_scan},
    {"block_total_during_scan", block_total_during_scan},
    {"block_generation_during_scan", block_generation_during_scan},
    {"block_resource_chain", block_resource_chain},
    {"block_resource_then_service", block_resource_then_service},
    {"block_services_exhausted", block_services_exhausted},
    {"block_success_new_failure", block_success_new_failure},
    {"block_exhausted_cooldown", block_exhausted_cooldown},
    {"block_foreground_tasks", block_foreground_tasks},
    {"block_over_capacity", block_over_capacity},
    {"resource_completion_barrier", resource_completion_barrier},
    {"resource_failure_fallback", resource_failure_fallback},
    {"resource_fragmentation_completion", resource_fragmentation_completion},
    {"fragmented_release", fragmented_release}, {"fragmented_protected", fragmented_protected},
    {"fragmented_merge_timeout", fragmented_merge_timeout},
    {"fragmented_merge_stopping", fragmented_merge_stopping},
    {"no_pressure", no_pressure}, {"merged_cancel", merged_cancel},
    {"protected_apps", protected_apps}, {"save_and_auxiliary", save_and_auxiliary},
    {"flush_race_and_navigation", flush_race_and_navigation},
    {"deadline_and_cooldown", deadline_and_cooldown},
    {"restart_hysteresis", restart_hysteresis}, {"fair_candidates", fair_candidates},
    {"clock_wrap", clock_wrap}, {"weather_queued_save", weather_queued_save},
    {"weather_abandoned_job", weather_abandoned_job},
    {"catalog_load_estimate", catalog_load_estimate},
    {"diagnostic_read_and_request", diagnostic_read_and_request},
    {"diagnostic_invalid_numbers", diagnostic_invalid_numbers},
    {"impossible_api_then_normal", impossible_api_then_normal},
    {"impossible_does_not_poison_live_request", impossible_does_not_poison_live_request},
    {"diagnostic_over_capacity_then_normal", diagnostic_over_capacity_then_normal},
    {"capacity_and_recovery_boundary", capacity_and_recovery_boundary}
};

int main(int argc, char **argv)
{
    CHECK(epd_memory_init() == EPD_MEMORY_OK);
    CHECK(host_init_app_load_estimates_init() == RT_EOK);
    heap_spans(NULL, 0);
    if (argc == 2 && !strcmp(argv[1], "--list"))
    {
        for (unsigned i = 0; i < sizeof(pressure_cases) / sizeof(pressure_cases[0]); ++i)
            puts(pressure_cases[i].name);
        return 0;
    }
    for (unsigned i = 0; argc == 2 && i < sizeof(pressure_cases) / sizeof(pressure_cases[0]); ++i)
        if (!strcmp(argv[1], pressure_cases[i].name))
        {
            pressure_cases[i].run();
            CHECK(!pressure_heap_locked && !app_image_psram_memheap.lock.held && !interrupt_depth);
            CHECK(heap_sem_acquires == heap_sem_releases && !heap_after_scan);
            printf("PASS %s (%u checks; ticks/s=%u)\n", argv[1], checks, RT_TICK_PER_SECOND);
            return 0;
        }
    return 2;
}
