#include "host.h"
#include "ui_scale.h"
#include "epd_operation_runtime.h"
#include "epd_cold_runtime.h"
#include "epd_resource_runtime.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ExitProcess(1); } } while (0)
static CRITICAL_SECTION disk_lock, runtime_lock;
static HANDLE entered, proceed, barrier_entered, completed;
static DWORD ui_thread;
static volatile LONG gate_write, fail_write, fail_one_write, held, writes, reads, worker_count, notifications, dismissals;
static bool change_during_read;
static bool sleeping, reset_blocked, handing_off, power_pending, write_allowed = true, available = true, fail_start;
static unsigned active_writers, path_revision = 1, volume_revision = 1;
static bool bt = true, wifi = true;
static unsigned refresh = 10, scale = 2;
static unsigned char disk[128];
static size_t disk_size, cursor;
struct host_thread { HANDLE handle; void (*entry)(void *); void *arg; };
static __declspec(thread) struct host_thread local_thread;
static __declspec(thread) rt_thread_t self;
rt_base_t rt_hw_interrupt_disable(void) { EnterCriticalSection(&runtime_lock);return 0; }
void rt_hw_interrupt_enable(rt_base_t previous) { (void)previous;LeaveCriticalSection(&runtime_lock); }
void rt_enter_critical(void) { EnterCriticalSection(&runtime_lock); }
void rt_exit_critical(void) { LeaveCriticalSection(&runtime_lock); }
rt_thread_t rt_thread_self(void) { return self ? self : &local_thread; }
void rt_thread_yield(void) { SwitchToThread(); }
int rt_event_init(struct rt_event *event,const char *name,int flags)
{
    (void)name;(void)flags;InitializeCriticalSection(&event->lock);event->bits=0;
    event->handle=CreateEvent(NULL,TRUE,FALSE,NULL);return event->handle ? 0 : -1;
}
int rt_event_detach(struct rt_event *event)
{ CloseHandle(event->handle);DeleteCriticalSection(&event->lock);return 0; }
int rt_event_send(struct rt_event *event,uint32_t bits)
{
    EnterCriticalSection(&event->lock);event->bits|=bits;SetEvent(event->handle);
    LeaveCriticalSection(&event->lock);return 0;
}
int rt_event_recv(struct rt_event *event,uint32_t bits,int flags,int32_t wait,uint32_t *out)
{
    CHECK(flags==(RT_EVENT_FLAG_OR|RT_EVENT_FLAG_CLEAR));
    for(;;) {
        EnterCriticalSection(&event->lock);*out=event->bits&bits;event->bits&=~*out;
        if(!event->bits) ResetEvent(event->handle);
        LeaveCriticalSection(&event->lock);
        if(*out) return 0;
        if(WaitForSingleObject(event->handle,wait<0?INFINITE:(DWORD)wait)!=WAIT_OBJECT_0) return -1;
    }
}
/* SYSTEM-only memory and unrelated cold/resource ports. Settings operations and
 * the shared worker loop are the production implementations compiled by run.py. */
epd_memory_owner_t epd_memory_owner_current(void) { return EPD_MEMORY_OWNER_SYSTEM; }
bool epd_memory_owner_get(epd_memory_owner_t owner,epd_memory_owner_snapshot_t *out)
{ if(owner) return false;memset(out,0,sizeof(*out));out->owner=owner;return true; }
bool epd_memory_owner_retain(epd_memory_owner_t owner) { return owner==EPD_MEMORY_OWNER_SYSTEM; }
void epd_memory_owner_release(epd_memory_owner_t owner) { CHECK(!owner); }
bool epd_memory_owner_close(epd_memory_owner_t owner) { return owner!=EPD_MEMORY_OWNER_SYSTEM; }
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner) { CHECK(!owner);return 0; }
void epd_memory_owner_leave(epd_memory_owner_t previous) { CHECK(!previous); }
bool epd_cold_state_init(const epd_operation_executor_t *executor) { CHECK(executor->thread);return true; }
size_t epd_cold_state_dispatch(size_t budget) { (void)budget;return 0; }
size_t epd_cold_runtime_worker_pump(size_t budget) { (void)budget;return 0; }
size_t epd_cold_runtime_ui_pump(size_t budget) { (void)budget;return 0; }
int epd_resource_runtime_task_state(epd_memory_owner_t owner,uint64_t generation,bool running)
{ (void)owner;(void)generation;(void)running;CHECK(false);return EPD_RESOURCE_FAILED; }
int rt_mutex_init(struct rt_mutex *m, const char *n, int f) { (void)n; (void)f; InitializeCriticalSection(&m->lock); return 0; }
int rt_mutex_detach(struct rt_mutex *m) { DeleteCriticalSection(&m->lock); return 0; }
int rt_mutex_take(struct rt_mutex *m, int t) { (void)t; EnterCriticalSection(&m->lock); return 0; }
int rt_mutex_release(struct rt_mutex *m) { LeaveCriticalSection(&m->lock); return 0; }
int rt_sem_init(struct rt_semaphore *s, const char *n, unsigned v, int f)
{ (void)f; s->handle = CreateSemaphore(NULL, (LONG)v, 10000, NULL); s->barrier = !strcmp(n, "uisetd"); return s->handle ? 0 : -1; }
int rt_sem_detach(struct rt_semaphore *s) { CloseHandle(s->handle); return 0; }
int rt_sem_take(struct rt_semaphore *s, int t)
{ (void)t; if (s->barrier) SetEvent(barrier_entered); return WaitForSingleObject(s->handle, INFINITE) == WAIT_OBJECT_0 ? 0 : -1; }
int rt_sem_release(struct rt_semaphore *s) { CHECK(ReleaseSemaphore(s->handle, 1, NULL)); return 0; }
static DWORD WINAPI thread_main(void *arg) { struct host_thread *t = arg; self=t;t->entry(t->arg); return 0; }
rt_thread_t epd_thread_create_psram(const char *name, void (*entry)(void *), void *arg, unsigned stack, unsigned char priority, unsigned tick)
{
    (void)name; (void)stack; (void)priority; (void)tick;
    if (fail_start) return NULL;
    struct host_thread *t = calloc(1, sizeof(*t)); CHECK(t); t->entry = entry; t->arg = arg;
    t->handle = CreateThread(NULL, 0, thread_main, t, CREATE_SUSPENDED, NULL); CHECK(t->handle);
    InterlockedIncrement(&worker_count); return t;
}
int rt_thread_startup(rt_thread_t t) { CHECK(ResumeThread(t->handle) != (DWORD)-1); return 0; }
int rt_thread_delete(rt_thread_t t) { CloseHandle(t->handle); free(t); return 0; }
bool sleep_guard_acquire(sleep_guard_owner_t *owner)
{ if (sleeping) return false; if (!owner->held) { owner->held = true; InterlockedIncrement(&held); } return true; }
void sleep_guard_release(sleep_guard_owner_t *owner)
{ if (owner->held) { owner->held = false; InterlockedDecrement(&held); } }
void epd_power_notify(void) { SetEvent(completed); }
void storage_lock(void) { EnterCriticalSection(&disk_lock); }
void storage_unlock(void) { LeaveCriticalSection(&disk_lock); }
bool storage_path_available(const char *path) { (void)path; return available; }
bool storage_file_recover(const char *path) { (void)path; return available; }
uint32_t storage_path_revision(const char *p) { (void)p; return path_revision; }
uint32_t storage_volume_revision(int v) { (void)v; return volume_revision; }
uint32_t storage_file_writers(const char *p) { (void)p; return active_writers; }
bool system_reset_write_allowed(void) { return write_allowed; }
bool system_reset_blocks_apps(void) { return reset_blocked; }
bool storage_changing(void) { return handing_off; }
bool board_power_pending(void) { return power_pending; }
uint32_t storage_crc32(const void *data, size_t size)
{
    const unsigned char *p = data; uint32_t crc = UINT32_MAX;
    while (size--) { crc ^= *p++; for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ~crc;
}
bool storage_file_replace(const char *path, const void *data, size_t size)
{
    (void)path; CHECK(size <= sizeof(disk));
    if (worker_count) CHECK(GetCurrentThreadId() != ui_thread && held == 1);
    InterlockedIncrement(&writes);
    if (InterlockedCompareExchange(&gate_write, 0, 1) == 1)
    { SetEvent(entered); CHECK(WaitForSingleObject(proceed, 10000) == WAIT_OBJECT_0); }
    if (InterlockedCompareExchange(&fail_one_write, 0, 1) == 1 || fail_write || !write_allowed || !available) return false;
    memcpy(disk, data, size); disk_size = size; ++path_revision; ++volume_revision; return true;
}
int host_open(const char *p, int flags)
{ (void)p; (void)flags; InterlockedIncrement(&reads); cursor = 0; if (!disk_size) { errno = ENOENT; return -1; } return 1; }
int host_fstat(int fd, struct host_stat *st) { (void)fd; st->st_mode = 1; st->st_size = (int64_t)disk_size; return 0; }
int host_read(int fd, void *out, size_t size)
{ (void)fd; if (size > disk_size - cursor) size = disk_size - cursor; memcpy(out, disk + cursor, size); cursor += size;
  if (change_during_read) { change_during_read = false; ++path_revision; } return (int)size; }
int host_close(int fd) { (void)fd; return 0; }
int btpan_get_state(void) { return bt ? 1 : 0; }
int btpan_enable(bool value) { bt = value; return 0; }
int wlan_service_enable(bool value) { wifi = value; return 0; }
bool reading_font_get(unsigned id, reading_font_info_t *out) { (void)id; strcpy(out->name, "font"); return true; }
unsigned reading_font_selected(void) { return 0; }
void epd_wave_set_part_times(unsigned n) { refresh = n; }
unsigned epd_wave_get_part_times(void) { return refresh; }
bool ui_scale_prepare(unsigned n) { return n < 5; }
void ui_scale_apply(unsigned n) { scale = n; }
bool epd_notification_publish_resident(const epd_notification_t *notice)
{ CHECK(GetCurrentThreadId() == ui_thread); CHECK(!strcmp(notice->app_id, "settings") && !strcmp(notice->id, "settings-save") && !notice->ongoing && notice->text[0]); ++notifications; return true; }
bool epd_notification_dismiss(const char *app, const char *id)
{ CHECK(!strcmp(app, "settings") && !strcmp(id, "settings-save")); ++dismissals; return true; }

typedef struct epd_service {
    struct epd_service *next;
    bool worker_done, ui_done, dirty, flush_active, cleanup_retry;
    unsigned callers, reclaim_holds, cleanup_holds;
    uint32_t cleanup_retry_ms;
    uint32_t saved_work_revision, reclaim_work_revision;
    struct { bool (*flush)(void); } definition;
} epd_service_t;
static epd_service_t *services;
static bool ui_processing;
static epd_memory_owner_t service_enter(epd_service_t *service,bool cleanup)
{ (void)service;(void)cleanup;return EPD_MEMORY_OWNER_SYSTEM; }
static unsigned portal_polls, other_polls;
static bool other_pending;
static bool portal_release_storage(void) { ++portal_polls; return true; }
static bool other_flush(void) { ++other_polls; return !other_pending; }
#include "flush.inc"

typedef enum { POWER_IDLE, POWER_SAVE, POWER_STOP } power_phase_t;
typedef enum { BOARD_POWER_ERROR_NONE, BOARD_POWER_ERROR_BUSY, BOARD_POWER_ERROR_SAVE } board_power_error_t;
#define POWER_SAVE_TIMEOUT_MS 35000u
static power_phase_t power_phase;
static board_power_error_t power_error;
static rt_tick_t power_save_started, now;
static unsigned stop_modules;
static bool app_installer_busy(void) { return false; }
rt_tick_t rt_tick_get(void) { return now; }
static rt_tick_t rt_tick_from_millisecond(unsigned ms) { return ms; }
static void power_fail(board_power_error_t error) { power_error = error; power_phase = POWER_IDLE; }
static void app_service_stop_modules(void)
{ CHECK(!held && ui_settings_save_status() != UI_SETTINGS_SAVE_PENDING); ++stop_modules; }
static void board_save_step(void)
{
    power_phase_t phase = power_phase;
#include "board_save.inc"
}

static void wait_event(HANDLE event) { CHECK(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0); }
static void settle(ui_settings_save_status_t expected)
{
    ULONGLONG start = GetTickCount64();
    while (ui_settings_save_status() == UI_SETTINGS_SAVE_PENDING)
    { CHECK(GetTickCount64() - start < 5000); (void)WaitForSingleObject(completed, 20); }
    CHECK(ui_settings_save_status() == expected && held == 0);
}
static void block_next_write(void) { ResetEvent(entered); ResetEvent(proceed); InterlockedExchange(&gate_write, 1); }
static unsigned stored(unsigned tag)
{
    storage_lock();
    for (size_t i = 16; i + 3 < disk_size; i += 4)
        if (((unsigned)disk[i] | (unsigned)disk[i + 1] << 8) == tag)
        { unsigned value = (unsigned)disk[i + 2] | (unsigned)disk[i + 3] << 8; storage_unlock(); return value; }
    storage_unlock(); CHECK(false); return 0;
}
static void values(uint8_t out[UI_SETTING_COUNT])
{ for (unsigned i = 0; i < UI_SETTING_COUNT; ++i) out[i] = (uint8_t)ui_settings_index((ui_setting_id_t)i); }

static void merge_test(void)
{
    ui_settings_init(); block_next_write();
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); wait_event(entered);
    CHECK(ui_settings_index(UI_SETTING_TIMEOUT) == 1 && held == 1 && !ui_settings_flush());
    LONG before = reads;
    for (unsigned i = 0; i < 100; ++i) CHECK(ui_settings_set(UI_SETTING_TIMEOUT, i % 6));
    CHECK(ui_settings_set(UI_SETTING_MARGIN, 4));
    CHECK(reads == before && writes == 1 && ui_settings_index(UI_SETTING_TIMEOUT) == 3);
    epd_service_t service = {0}; service.definition.flush = other_flush; services = &service;
    CHECK(!app_service_flush() && portal_polls && other_polls);
    SetEvent(proceed); settle(UI_SETTINGS_SAVE_OK);
    CHECK(writes == 2 && worker_count == 1 && stored(2) == 10 && stored(12) == 20);
    CHECK(app_service_flush());
    CHECK(ui_settings_reload() == UI_SETTINGS_LOAD_OK && ui_settings_index(UI_SETTING_TIMEOUT) == 3);
}
static void failure_test(void)
{
    ui_settings_init(); fail_write = 1;
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 4)); settle(UI_SETTINGS_SAVE_FAILED);
    CHECK(ui_settings_index(UI_SETTING_TIMEOUT) == 4 && app_service_flush());
    LONG attempts = writes;
    for (unsigned i = 0; i < 100; ++i) { CHECK(ui_settings_flush()); ui_settings_process(); }
    CHECK(writes == attempts && notifications == 1 && !held);
    CHECK(ui_settings_save_status() == UI_SETTINGS_SAVE_FAILED);
    CHECK(ui_settings_reload() == UI_SETTINGS_LOAD_IO_ERROR && ui_settings_index(UI_SETTING_TIMEOUT) == 4);
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 3)); settle(UI_SETTINGS_SAVE_FAILED);
    ui_settings_process(); CHECK(notifications == 2);
    fail_write = 0; CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 4)); settle(UI_SETTINGS_SAVE_OK);
    ui_settings_process(); CHECK(dismissals == 1 && stored(2) == 15 && ui_settings_flush());
    /* A failed older write cannot discard or fail the newer queued snapshot. */
    block_next_write(); fail_one_write = 1;
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); wait_event(entered);
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 2));
    SetEvent(proceed); settle(UI_SETTINGS_SAVE_OK);
    ui_settings_process();
    CHECK(ui_settings_index(UI_SETTING_TIMEOUT) == 2 && stored(2) == 5 && notifications == 2);
}
static DWORD WINAPI reset_call(void *arg) { *(bool *)arg = ui_settings_reset_network(); return 0; }
static DWORD WINAPI reload_call(void *arg) { *(ui_settings_load_result_t *)arg = ui_settings_reload(); return 0; }
static void reset_test(void)
{
    ui_settings_init(); CHECK(ui_settings_set(UI_SETTING_BLUETOOTH, 0)); CHECK(ui_settings_set(UI_SETTING_WIFI, 0)); settle(UI_SETTINGS_SAVE_OK);
    block_next_write(); CHECK(ui_settings_set(UI_SETTING_MARGIN, 4)); wait_event(entered);
    bool ok = false; ResetEvent(barrier_entered);
    HANDLE thread = CreateThread(NULL, 0, reset_call, &ok, 0, NULL); CHECK(thread); wait_event(barrier_entered);
    uint8_t candidate[UI_SETTING_COUNT]; values(candidate); candidate[UI_SETTING_MARGIN] = 0;
    CHECK(!ui_settings_store_save(candidate) && !ui_settings_flush() && held == 1);
    SetEvent(proceed); wait_event(thread); CloseHandle(thread);
    CHECK(ok && ui_settings_flush() && bt && wifi && stored(4) == 1 && stored(5) == 1 && stored(12) == 20);
    CHECK(ui_settings_index(UI_SETTING_MARGIN) == 4 && !held);
    CHECK(ui_settings_set(UI_SETTING_WIFI, 0)); settle(UI_SETTINGS_SAVE_OK);
    fail_write = 1; CHECK(!ui_settings_reset_network());
    CHECK(!wifi && ui_settings_flush() && !held && ui_settings_save_status() == UI_SETTINGS_SAVE_FAILED);
}
static void reload_test(void)
{
    ui_settings_init(); block_next_write(); CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 0)); wait_event(entered);
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 5));
    ui_settings_load_result_t result = UI_SETTINGS_LOAD_IO_ERROR; ResetEvent(barrier_entered);
    HANDLE thread = CreateThread(NULL, 0, reload_call, &result, 0, NULL); CHECK(thread); wait_event(barrier_entered);
    CHECK(!ui_settings_flush() && held == 1); SetEvent(proceed); wait_event(thread); CloseHandle(thread);
    CHECK(result == UI_SETTINGS_LOAD_OK && ui_settings_index(UI_SETTING_TIMEOUT) == 5 && stored(2) == 0);
    CHECK(writes == 2 && !held);
}
static void cache_test(void)
{
    ui_settings_init(); CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); settle(UI_SETTINGS_SAVE_OK);
    LONG read_count = reads, write_count = writes;
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); settle(UI_SETTINGS_SAVE_OK); CHECK(reads == read_count && writes == write_count);
    ++path_revision; CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); settle(UI_SETTINGS_SAVE_OK); CHECK(reads == ++read_count && writes == write_count);
    ++volume_revision; CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); settle(UI_SETTINGS_SAVE_OK); CHECK(reads == ++read_count);
    active_writers = 1; CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); settle(UI_SETTINGS_SAVE_FAILED); CHECK(reads == read_count);
    active_writers = 0; CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); settle(UI_SETTINGS_SAVE_OK); CHECK(reads == ++read_count);
    available = false; CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); settle(UI_SETTINGS_SAVE_FAILED); available = true;
    /* Same cached value must restore an externally replaced invalid record. */
    storage_lock(); disk[0] ^= 1; ++path_revision; storage_unlock();
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 1)); settle(UI_SETTINGS_SAVE_OK); CHECK(writes == write_count + 1);
    change_during_read = true;
    CHECK(ui_settings_reload() == UI_SETTINGS_LOAD_IO_ERROR && ui_settings_index(UI_SETTING_TIMEOUT) == 1);
}
static void gates_test(void)
{
    ui_settings_init();
    reset_blocked = true; CHECK(!ui_settings_set(UI_SETTING_TIMEOUT, 0)); reset_blocked = false;
    handing_off = true; CHECK(!ui_settings_set(UI_SETTING_TIMEOUT, 0)); handing_off = false;
    power_pending = true; CHECK(!ui_settings_set(UI_SETTING_TIMEOUT, 0)); power_pending = false;
    sleeping = true; CHECK(!ui_settings_set(UI_SETTING_TIMEOUT, 0)); CHECK(!ui_settings_set(UI_SETTING_UI_SCALE, 4)); sleeping = false;
    CHECK(ui_settings_index(UI_SETTING_TIMEOUT) == 0 && scale == 2 && !writes && !held);
    CHECK(ui_settings_set(UI_SETTING_UI_SCALE, 4)); settle(UI_SETTINGS_SAVE_OK); CHECK(scale == 4);
    write_allowed = false; CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 0)); settle(UI_SETTINGS_SAVE_FAILED); CHECK(ui_settings_flush());
}
static void shutdown_test(void)
{
    ui_settings_init(); block_next_write(); fail_write = 1;
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 4)); wait_event(entered);
    power_pending = true; power_phase = POWER_SAVE; power_save_started = now = 0;
    for (unsigned i = 0; i < 100; ++i) {
        board_save_step(); ++now;
        CHECK(power_phase == POWER_SAVE && held == 1 && !stop_modules && !app_service_flush());
    }
    /* A hung write reaches the board's existing timeout without proceeding to
     * shutdown or releasing the writer's sleep guard. */
    now = POWER_SAVE_TIMEOUT_MS; board_save_step();
    CHECK(power_phase == POWER_IDLE && power_error == BOARD_POWER_ERROR_SAVE && held == 1 && !stop_modules);
    SetEvent(proceed); settle(UI_SETTINGS_SAVE_FAILED); ui_settings_process();
    CHECK(notifications == 1 && ui_settings_save_status() == UI_SETTINGS_SAVE_FAILED);
    LONG attempts = writes;
    epd_service_t service = {0}; service.definition.flush = other_flush; services = &service;
    /* Settings' terminal failure cannot bypass another owner's active work. */
    other_pending = true; power_save_started = now; power_phase = POWER_SAVE;
    board_save_step(); CHECK(power_phase == POWER_SAVE && !stop_modules);
    other_pending = false; board_save_step();
    CHECK(power_phase == POWER_STOP && stop_modules == 1 && !held && writes == attempts);
    /* Subsequent shutdown attempts also finish the barrier immediately. */
    power_phase = POWER_SAVE; board_save_step();
    CHECK(power_phase == POWER_STOP && stop_modules == 2 && writes == attempts);
    CHECK(ui_settings_save_status() == UI_SETTINGS_SAVE_FAILED);
}
static void put32(unsigned char *p, uint32_t value) { for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i)); }
static void options_test(void)
{
    const unsigned char legacy[13] = {1, 4, 3, 1, 1, 0, 1, 0, 2, 0, 1, 1, 0};
    put32(disk, 0x45505332u); put32(disk + 4, 1); put32(disk + 8, 13); put32(disk + 12, storage_crc32(legacy, 13));
    memcpy(disk + 16, legacy, 13); disk_size = 29;
    ui_settings_init();
    CHECK(ui_settings_index(UI_SETTING_TIMEOUT) == 2 && !writes && worker_count == 1);
    CHECK(!ui_settings_set(UI_SETTING_FONT_SIZE, 8));
    CHECK(!ui_settings_set(UI_SETTING_FULL_REFRESH, 9));
    CHECK(ui_settings_index(UI_SETTING_FONT_SIZE) == 0);
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(ui_settings_set(UI_SETTING_FONT_SIZE, i)); settle(UI_SETTINGS_SAVE_OK);
        CHECK(stored(9) == 36 + i * 4);
    }
    const unsigned counts[] = {1, 5, 10, 15, 20, 25, 30, 35, 40};
    for (unsigned i = 0; i < 9; ++i) {
        CHECK(ui_settings_set(UI_SETTING_FULL_REFRESH, i)); settle(UI_SETTINGS_SAVE_OK);
        CHECK(stored(3) == counts[i]);
    }
    CHECK(ui_settings_reload() == UI_SETTINGS_LOAD_OK);
    CHECK(ui_settings_index(UI_SETTING_FONT_SIZE) == 7 && ui_settings_index(UI_SETTING_FULL_REFRESH) == 8);
}
static void startup_test(void)
{
    fail_start = true; ui_settings_init(); CHECK(!ui_settings_set(UI_SETTING_TIMEOUT, 0));
    CHECK(ui_settings_index(UI_SETTING_FULL_REFRESH) == 4 && epd_wave_get_part_times() == 20);
    CHECK(ui_settings_lock_timeout_ms() == 60000u && ui_settings_sleep_timeout_ms() == 60000u);
    CHECK(!held && !worker_count && !writes);
    fail_start = false; CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 0)); settle(UI_SETTINGS_SAVE_OK); CHECK(worker_count == 1);
}
static void schema_test(void)
{
    ui_settings_init(); CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 4)); settle(UI_SETTINGS_SAVE_OK);
    CHECK(disk_size == 16 + 15 * 4 && disk[4] == 4);
    for (size_t i = 16; i < disk_size; i += 4) CHECK(disk[i] != 18);
    put32(disk + 4, 3); ++path_revision;
    CHECK(ui_settings_reload() == UI_SETTINGS_LOAD_UNKNOWN_VERSION);
    CHECK(ui_settings_index(UI_SETTING_TIMEOUT) == 0);
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 4)); settle(UI_SETTINGS_SAVE_OK);
    /* A removed field is invalid even if the header claims the new version. */
    disk[16] = 18;
    put32(disk + 12, storage_crc32(disk + 16, disk_size - 16)); ++path_revision;
    CHECK(ui_settings_reload() == UI_SETTINGS_LOAD_INVALID);
    CHECK(ui_settings_index(UI_SETTING_TIMEOUT) == 0);
}
static void navigation_test(void)
{
    ui_settings_init();
    CHECK(ui_settings_index(UI_SETTING_NAVIGATION) == 1);
    CHECK(ui_settings_set(UI_SETTING_TIMEOUT, 4));
    CHECK(ui_settings_set(UI_SETTING_UI_SCALE, 3));
    CHECK(ui_settings_set(UI_SETTING_NAVIGATION, 0));
    settle(UI_SETTINGS_SAVE_OK);
    CHECK(stored(17) == 0 && stored(2) == 15 && stored(14) == 110);
    ++path_revision;
    CHECK(ui_settings_reload() == UI_SETTINGS_LOAD_OK && ui_settings_index(UI_SETTING_NAVIGATION) == 0);
    /* Incomplete records are rejected as a whole. */
    storage_lock();
    size_t offset=16;
    while(offset+4<=disk_size && disk[offset]!=17)offset+=4;
    CHECK(offset+4<=disk_size);
    memmove(disk+offset,disk+offset+4,disk_size-offset-4);
    disk_size -= 4;
    put32(disk + 8, (uint32_t)disk_size - 16);
    put32(disk + 12, storage_crc32(disk + 16, disk_size - 16));
    ++path_revision;
    storage_unlock();
    CHECK(ui_settings_reload() == UI_SETTINGS_LOAD_INVALID);
    CHECK(ui_settings_index(UI_SETTING_NAVIGATION) == 1 && ui_settings_index(UI_SETTING_TIMEOUT) == 0 &&
          ui_settings_index(UI_SETTING_UI_SCALE) == 2);
    CHECK(!ui_settings_set(UI_SETTING_NAVIGATION, 2));
    CHECK(ui_settings_set(UI_SETTING_NAVIGATION, 0)); settle(UI_SETTINGS_SAVE_OK);
    CHECK(stored(17) == 0 && stored(14) == 100);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); InitializeCriticalSection(&disk_lock); InitializeCriticalSection(&runtime_lock); ui_thread = GetCurrentThreadId();
    entered = CreateEvent(NULL, TRUE, FALSE, NULL); proceed = CreateEvent(NULL, TRUE, FALSE, NULL);
    barrier_entered = CreateEvent(NULL, TRUE, FALSE, NULL); completed = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!strcmp(argv[1], "merge")) merge_test();
    else if (!strcmp(argv[1], "failure")) failure_test();
    else if (!strcmp(argv[1], "reset")) reset_test();
    else if (!strcmp(argv[1], "reload")) reload_test();
    else if (!strcmp(argv[1], "cache")) cache_test();
    else if (!strcmp(argv[1], "gates")) gates_test();
    else if (!strcmp(argv[1], "options")) options_test();
    else if (!strcmp(argv[1], "startup")) startup_test();
    else if (!strcmp(argv[1], "shutdown")) shutdown_test();
    else if (!strcmp(argv[1], "navigation")) navigation_test();
    else if (!strcmp(argv[1], "schema")) schema_test();
    else CHECK(false);
    printf("PASS settings_async %s\n", argv[1]);
    return 0;
}
