#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <crtdbg.h>
#include <signal.h>
#include <stdarg.h>
/* Historical fixture threshold; production no longer reserves this amount. */
#define SHARED_RESERVE (1024u * 1024u)
#include "epd_compute_pipe.h"
#ifdef REAL_FONT_PIPE
static int observed_pipe_step(epd_compute_pipe_host_t, epd_compute_pipe_service_t, void *);
#define epd_compute_pipe_step observed_pipe_step
#endif
#ifndef READER_FONT_TEST_SOURCE
#define READER_FONT_TEST_SOURCE "../../src/ui/ui_reader_font_remote.c"
#endif
#include READER_FONT_TEST_SOURCE
#include "../../src/services/sleep_guard.c"
#ifdef REAL_FONT_PIPE
#undef epd_compute_pipe_step
#endif
#include "src/draw/lv_draw_buf_private.h"

static DWORD ui_thread;
static CONDITION_VARIABLE transition;
static CRITICAL_SECTION interrupt_gate;
static HANDLE io_entered, io_continue;
static volatile LONG hold_io, fixture_generation = 1, leases, allocations, pipe_count;
static volatile LONG raw_reads, pack_reads, remote_opens, commands, fail_alloc, bad_packet;
static volatile LONG fail_shutdown, shutdown_calls, fail_open_retained, lease_checks, storage_locks;
static volatile LONG fake_ticks, live_mutex, live_sem, live_thread, started_threads, fail_start, io_logs;
static volatile LONG batch_logs;
static volatile LONG source_cache_logs;
static char last_batch_log[768];
static volatile LONG allocation_race, open_transient, output_status, command_transient, step_transient;
static unsigned output_status_skip;
static volatile LONG64 available_bytes = 64u * 1024u * 1024u;
static __declspec(thread) unsigned cache_depth;
static unsigned glyph_side = 8;
static bool allow_shared = true;
static uint32_t open_order[32];
static unsigned open_purposes[32];
static uint32_t command_order[EPD_FONT_BATCH_MAX], command_count;
static void (*cache_unlock_probe)(void);
static __declspec(thread) bool unlock_probe_active;
#ifndef REAL_FONT_PIPE
static const unsigned char fixture_font_pack[32] = {1};
#else
extern const unsigned char fixture_font_pack[];
extern const int fixture_font_pack_size;
#endif
#ifndef REAL_FONT_BYTES
#define REAL_FONT_BYTES 4096
#endif
static uint32_t fixture_size = REAL_FONT_BYTES, fixture_session = 1, fixture_writers;
static uint32_t fixture_signature, fixture_failure_revision;
static bool fixture_mounted = true, pattern_reads;
static unsigned source_fault;
static bool fixture_handoff;
static volatile LONG hold_close;
static volatile LONG hold_shutdown, power_notifications;
static bool operation_timing_test;
static LONG source_opens, source_closes, source_stats, source_seeks;
static uint64_t physical_bytes;
static __declspec(thread) unsigned io_depth;
static unsigned open_fds;
static off_t file_offset;
/* Memory policy and allocator ownership are exercised by unified_resource;
 * this fixture controls physical exhaustion through available_bytes/fail_alloc. */
#ifndef READER_FONT_REAL_CACHE
struct rt_memheap { unsigned identity; } app_image_psram_memheap;
static __declspec(thread) epd_memory_owner_t fixture_owner = EPD_MEMORY_OWNER_SYSTEM;
epd_memory_owner_t epd_memory_owner_current(void) { return fixture_owner; }
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner)
{
    if (owner != fixture_owner) return EPD_MEMORY_OWNER_INVALID;
    return fixture_owner;
}
void epd_memory_owner_leave(epd_memory_owner_t owner) { fixture_owner = owner; }
epd_memory_owner_t epd_memory_owner_enter_trusted(epd_memory_owner_t owner)
{ epd_memory_owner_t old = fixture_owner; fixture_owner = owner; return old; }
void epd_memory_owner_leave_trusted(epd_memory_owner_t owner) { fixture_owner = owner; }
#endif
static __declspec(thread) size_t admission_busy;
bool epd_memory_app_backend(epd_memory_region_t region, epd_memory_backend_t *backend)
{ if (region != EPD_MEMORY_REGION_HCPU_PSRAM) return false; *backend = 1; return true; }
size_t epd_app_allocation_peak(size_t size)
{ return size > SIZE_MAX - 64 ? SIZE_MAX : size + 64; }
static epd_memory_recovery_result_t fixture_recovery(epd_memory_recovery_t *state, bool failed)
{
    bool enough = epd_app_psram_available() >= state->physical && admission_busy != state->physical;
    state->result = enough && !failed ? EPD_MEMORY_OK : EPD_MEMORY_NO_CAPACITY;
    return state->state = enough && !failed ? EPD_MEMORY_RECOVERY_READY : EPD_MEMORY_RECOVERY_PENDING;
}
epd_memory_recovery_result_t epd_memory_recovery_begin(epd_memory_recovery_t *state,
    epd_memory_backend_t backend, size_t bytes, bool failed)
{
    assert(!state->used && backend == 1 && bytes);
    memset(state, 0, sizeof(*state));
    state->used = true; state->backend = backend; state->physical = bytes;
    state->owner = epd_memory_owner_current(); state->started = rt_tick_get();
    return fixture_recovery(state, failed);
}
epd_memory_recovery_result_t epd_memory_recovery_poll(epd_memory_recovery_t *state, bool cancelled)
{
    assert(state->used);
    if (cancelled || state->state == EPD_MEMORY_RECOVERY_DENIED ||
        (uint32_t)(rt_tick_get() - state->started) >= 5u * RT_TICK_PER_SECOND) {
        state->result = cancelled ? EPD_MEMORY_CANCELLED : EPD_MEMORY_TIMEOUT;
        return state->state = EPD_MEMORY_RECOVERY_DENIED;
    }
    return fixture_recovery(state, false);
}
epd_memory_recovery_result_t epd_memory_recovery_failed(epd_memory_recovery_t *state)
{
    assert(state->used && state->state == EPD_MEMORY_RECOVERY_READY);
    if (epd_memory_recovery_poll(state, false) == EPD_MEMORY_RECOVERY_DENIED) return state->state;
    return fixture_recovery(state, true);
}
#ifndef READER_FONT_REAL_CACHE
int epd_resource_cache_open(epd_resource_cache_t *cache, epd_resource_lane_t lane,
    int (*shrink)(void *, uint32_t, size_t, size_t *), void *context)
{ (void)lane; (void)context; assert(shrink); cache->provider = 1; cache->owner = EPD_MEMORY_OWNER_SYSTEM; return EPD_RESOURCE_OK; }
int epd_resource_cache_adopt(epd_resource_cache_t *cache, void *allocation)
{ assert(cache->provider && allocation);
  return fixture_owner == cache->owner ? EPD_RESOURCE_OK : EPD_RESOURCE_INVALID; }
int epd_resource_cache_update(epd_resource_cache_t *cache, const void *allocator,
    size_t total, size_t reclaimable, uint32_t cost, uint32_t value)
{ (void)cost; (void)value; assert(cache->provider && allocator && reclaimable <= total); return EPD_RESOURCE_OK; }
#endif
const system_font_header_t *ui_font_resource(void)
{
    static system_font_header_t header;
    header.font_bytes = REAL_FONT_BYTES;
    header.signature = 77;
#ifdef REAL_FONT_PIPE
    header.packed_bytes = fixture_font_pack_size;
#else
    header.packed_bytes = sizeof(fixture_font_pack);
#endif
    return &header;
}
const uint8_t *ui_font_resource_data(void) { return fixture_font_pack; }
#ifdef REAL_FONT_PIPE
#include "../../components/compute_reader_font/font_diagnostics.h"
extern void real_close_fault(bool), real_open_fault(bool), real_no_buffers(void);
extern void real_allocation_fault(bool);
static uint8_t *real_font_bytes;
#endif

rt_tick_t rt_tick_get(void)
{
    return (rt_tick_t)fake_ticks;
}
void rt_kprintf(const char *format, ...)
{
    assert(!cache_depth);
    if (!strncmp(format, "font batch ", 11)) {
        va_list args; va_start(args, format);
        vsnprintf(last_batch_log, sizeof(last_batch_log), format, args); va_end(args);
        InterlockedIncrement(&batch_logs);
    } else if (strstr(format, "cache_total") || strstr(format, "cache_hits"))
        InterlockedIncrement(&source_cache_logs);
    else InterlockedIncrement(&io_logs);
}
static void worker_only(void) { assert(GetCurrentThreadId() != ui_thread); assert(!cache_depth); }
void epd_power_notify(void) { InterlockedIncrement(&power_notifications); }
rt_base_t rt_hw_interrupt_disable(void) { EnterCriticalSection(&interrupt_gate); return 0; }
void rt_hw_interrupt_enable(rt_base_t level) { (void)level; LeaveCriticalSection(&interrupt_gate); }
int rt_mutex_init(struct rt_mutex *m, const char *name, int flag)
{ (void)name; (void)flag; InitializeCriticalSection(&m->cs); ++live_mutex; return 0; }
int rt_mutex_take(struct rt_mutex *m, int timeout)
{ assert(timeout == RT_WAITING_FOREVER); EnterCriticalSection(&m->cs); ++cache_depth; return 0; }
int rt_mutex_release(struct rt_mutex *m)
{
    assert(cache_depth); --cache_depth; WakeAllConditionVariable(&transition); LeaveCriticalSection(&m->cs);
    /* Pause only after the outer cache lock is released, so the UI can perform
     * the competing operation. Probes may inspect state under the same lock. */
    if (m == &cache_lock && !cache_depth && !unlock_probe_active &&
        GetCurrentThreadId() != ui_thread && cache_unlock_probe) {
        unlock_probe_active = true;
        cache_unlock_probe();
        unlock_probe_active = false;
    }
    return 0;
}
int rt_mutex_detach(struct rt_mutex *m) { DeleteCriticalSection(&m->cs); --live_mutex; return 0; }
int rt_sem_init(struct rt_semaphore *s, const char *name, unsigned value, int flag)
{ (void)name; (void)flag; s->event = CreateSemaphoreA(NULL, (LONG)value, 65535, NULL); assert(s->event); ++live_sem; return 0; }
int rt_sem_take(struct rt_semaphore *s, int timeout)
{ assert(timeout == RT_WAITING_FOREVER); assert(WaitForSingleObject(s->event, INFINITE) == WAIT_OBJECT_0); return 0; }
int rt_sem_release(struct rt_semaphore *s) { assert(ReleaseSemaphore(s->event, 1, NULL)); return 0; }
int rt_sem_detach(struct rt_semaphore *s) { CloseHandle(s->event); --live_sem; return 0; }
struct test_thread { void (*fn)(void *); void *arg; HANDLE thread; size_t charge; };
static DWORD WINAPI start_worker(LPVOID value)
{ struct test_thread *t = value; t->fn(t->arg); return 0; }
rt_thread_t epd_thread_create_psram(const char *name, void (*fn)(void *), void *arg,
                                   rt_uint32_t stack, rt_uint8_t priority, rt_uint32_t tick)
{
    (void)name; (void)stack; (void)priority; (void)tick;
    struct test_thread *t = calloc(1, sizeof(*t)); assert(t); t->fn = fn; t->arg = arg; t->charge = stack + 32u;
    InterlockedExchangeAdd64(&available_bytes, -(LONG64)t->charge); ++live_thread; return t;
}
int rt_thread_startup(rt_thread_t t)
{ if (fail_start) return -1;
    ++started_threads; t->thread = CreateThread(NULL, 0, start_worker, t, 0, NULL); assert(t->thread); return 0; }
int rt_thread_delete(rt_thread_t t) { assert(!t->thread); InterlockedExchangeAdd64(&available_bytes, (LONG64)t->charge); --live_thread; free(t); return 0; }
typedef union { long double alignment; size_t bytes; } allocation_header;
size_t epd_app_psram_available(void) { return (size_t)InterlockedCompareExchange64(&available_bytes, 0, 0); }
void *epd_app_alloc(size_t bytes, epd_app_memory_t region)
{
    (void)region; if (InterlockedExchange(&fail_alloc, 0)) return NULL;
    if (bytes + 32u > epd_app_psram_available()) return NULL;
    allocation_header *out = malloc(sizeof(*out) + bytes); assert(out); out->bytes = bytes + 32u;
    InterlockedExchangeAdd64(&available_bytes, -(LONG64)out->bytes);
    InterlockedExchangeAdd64(&available_bytes, -(LONG64)InterlockedExchange(&allocation_race, 0));
    InterlockedIncrement(&allocations); return out + 1;
}
void epd_app_free(void *out)
{
    if (out) {
        allocation_header *h = (allocation_header *)out - 1;
        InterlockedExchangeAdd64(&available_bytes, (LONG64)h->bytes);
        InterlockedDecrement(&allocations); free(h);
    }
}

bool reading_font_acquire(uint32_t id, reading_font_lease_t *lease)
{
    if (!id || id > 3) return false;
    memset(lease, 0, sizeof(*lease)); lease->id = id; lease->signature = 87 + id;
    lease->signature += fixture_signature;
    lease->size = fixture_size; lease->session = fixture_session; lease->revision = (uint32_t)fixture_generation;
    lease->failure_revision = fixture_failure_revision;
    lease->volume = STORAGE_SD; strcpy(lease->path, "/sdcard/font.ttf"); InterlockedIncrement(&leases); return true;
}
void reading_font_release(reading_font_lease_t *lease)
{ assert(lease->id); lease->id = 0; InterlockedDecrement(&leases); }
bool reading_font_lease_valid(const reading_font_lease_t *lease)
{ InterlockedIncrement(&lease_checks); return lease->id && fixture_mounted && !fixture_writers &&
    lease->signature == 87 + lease->id + fixture_signature && lease->size == fixture_size &&
    lease->session == fixture_session && lease->failure_revision == fixture_failure_revision &&
    lease->revision == (uint32_t)fixture_generation; }
void reading_font_report_failure(uint32_t id) { (void)id; InterlockedIncrement(&fixture_generation); }
bool storage_session_valid(storage_volume_t volume, uint32_t session) { (void)volume; return fixture_mounted && session == fixture_session; }
uint32_t storage_file_writers(const char *path) { (void)path; return fixture_writers; }
uint32_t storage_path_revision(const char *path) { (void)path; return (uint32_t)fixture_generation; }
uint32_t storage_volume_revision(storage_volume_t volume) { (void)volume; return (uint32_t)fixture_generation; }
bool storage_changing(void) { return fixture_handoff; }
bool storage_release_requested(void) { return fixture_handoff; }
void storage_lock(void) { worker_only(); assert(!io_depth); ++io_depth;
    InterlockedIncrement(&storage_locks); InterlockedExchangeAdd(&fake_ticks, 3); }
void storage_unlock(void) { worker_only(); assert(io_depth == 1); --io_depth; }
int open(const char *path, int flags) { (void)path; (void)flags; worker_only(); assert(io_depth && open_fds < 2);
    ++source_opens; if(operation_timing_test) InterlockedExchangeAdd(&fake_ticks,7); if (source_fault == 1) return -1; ++open_fds; return 1; }
int fstat(int fd, struct stat *s) { assert(fd == 1 && io_depth && open_fds); ++source_stats; if(operation_timing_test) InterlockedExchangeAdd(&fake_ticks,11);
    s->st_size = fixture_size; s->st_mode = 1; return source_fault == 2 ? -1 : 0; }
off_t lseek(int fd, off_t offset, int from) { assert(fd == 1 && from == SEEK_SET && io_depth && open_fds);
    ++source_seeks; if(operation_timing_test) InterlockedExchangeAdd(&fake_ticks,13); file_offset = offset; return source_fault == 3 ? -1 : offset; }
int read(int fd, void *out, unsigned size)
{
    worker_only(); assert(fd == 1 && io_depth && open_fds); InterlockedIncrement(&raw_reads); InterlockedExchangeAdd(&fake_ticks, 5);
    if (hold_io) { SetEvent(io_entered); assert(WaitForSingleObject(io_continue, 5000) == WAIT_OBJECT_0); }
    if (source_fault == 4) return -1;
    if (source_fault == 5 && size) --size;
    physical_bytes += size;
    #ifdef REAL_FONT_PIPE
    assert(file_offset >= 0 && (uint64_t)file_offset + size <= fixture_size);
    memcpy(out, real_font_bytes + file_offset, size); file_offset += size;
#else
    if (pattern_reads) {
        assert(file_offset >= 0 && (uint64_t)file_offset + size <= fixture_size);
        for (unsigned i = 0; i < size; ++i) {
            uint32_t at = (uint32_t)file_offset + i;
            ((uint8_t *)out)[i] = (uint8_t)(at ^ (at >> 9) ^ (at >> 17));
        }
    } else memset(out, 0x22, size);
#endif
    return (int)size;
}
int close(int fd) { worker_only(); assert(fd == 1 && io_depth && open_fds);
    if (InterlockedExchange(&hold_close, 0)) {
        SetEvent(io_entered); assert(WaitForSingleObject(io_continue, 5000) == WAIT_OBJECT_0);
    }
    if (source_fault != 6) --open_fds; ++source_closes;
    InterlockedExchangeAdd(&fake_ticks, operation_timing_test ? 17 : 2); return source_fault == 6 ? -1 : 0; }
static lv_draw_buf_handlers_t handlers;
lv_draw_buf_handlers_t *lv_draw_buf_get_font_handlers(void) { return &handlers; }
void lv_draw_buf_flush_cache(const lv_draw_buf_t *buf, const lv_area_t *area)
{ (void)area; assert(buf->handlers == &handlers); assert(buf->data && !((uintptr_t)buf->data & 31)); }

#ifndef REAL_FONT_PIPE
int epd_compute_query(const char *name, uint32_t version, epd_compute_capability_t *cap)
{
    (void)name; (void)version; memset(cap, 0, sizeof(*cap)); cap->ready = true;
    cap->registration.flags = allow_shared ? EPD_COMPUTE_STEP_SHARED : 0; return 0;
}
struct epd_compute_pipe_host {
    void *storage;
    epd_font_open_t open;
    epd_font_command_t command;
    crf_codepoint input[EPD_FONT_BATCH_MAX];
    epd_compute_pipe_message_t message;
    unsigned phase, first, purpose;
    volatile LONG cancelled;
};
int epd_compute_pipe_open(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *m, const void *data, size_t length, epd_compute_pipe_host_t *out)
{
    worker_only(); assert(!strcmp(name, EPD_FONT_COMPONENT) && version == EPD_FONT_VERSION);
    assert(m->operation == EPD_PIPE_OPEN && length == sizeof(epd_font_open_t));
    int transient = (int)InterlockedExchange(&open_transient, 0);
    if (transient) { *out = NULL; return transient; }
    epd_compute_pipe_host_t p = calloc(1, sizeof(*p)); assert(p);
    p->storage = epd_app_alloc(SOURCE_PIPE_BYTES - 64u, EPD_APP_PSRAM); assert(p->storage);
    memcpy(&p->open, data, length); p->purpose = purpose; *out = p;
    assert(p->open.memory_bytes == EPD_FONT_MEMORY_DEFAULT);
    assert(p->open.memory_bytes == 704u * 1024u);
    assert(p->open.cache_bytes == (p->open.font.source_id ? 256u * 1024u : 0));
    assert(p->open.flags == (p->open.font.source_id ? 0 : EPD_FONT_SOURCE_FTZ1));
    InterlockedIncrement(&pipe_count);
    assert(pipe_count <= 2);
    LONG index = InterlockedIncrement(&remote_opens) - 1;
    if (index < 32) { open_order[index] = p->open.font.generation; open_purposes[index] = purpose; }
    return fail_open_retained ? EPD_COMPUTE_COMPONENT_ERROR : 0;
}
void epd_compute_pipe_cancel(epd_compute_pipe_host_t p) { InterlockedExchange(&p->cancelled, 1); }
int epd_compute_pipe_shutdown(epd_compute_pipe_host_t *p)
{
    worker_only(); if (!*p) return EPD_COMPUTE_OK;
    sleep_guard_snapshot_t snapshot; sleep_guard_snapshot(&snapshot);
    assert(snapshot.held_count && snapshot.state != SLEEP_GUARD_SLEEPING);
    if (InterlockedExchange(&hold_shutdown, 0)) {
        SetEvent(io_entered); assert(WaitForSingleObject(io_continue, 5000) == WAIT_OBJECT_0);
    }
    InterlockedIncrement(&shutdown_calls); epd_compute_pipe_cancel(*p);
    if (fail_shutdown) return EPD_COMPUTE_COMPONENT_ERROR;
    InterlockedDecrement(&pipe_count); epd_app_free((*p)->storage); free(*p); *p = NULL; return EPD_COMPUTE_OK;
}
int epd_compute_pipe_purpose(epd_compute_pipe_host_t p, epd_compute_purpose_t purpose) { p->purpose = purpose; return 0; }
const epd_compute_pipe_message_t *epd_compute_pipe_result(epd_compute_pipe_host_t p) { return &p->message; }
static void (*command_probe)(epd_compute_pipe_host_t);
static void (*output_probe)(void);
static bool batch_source_reads;
int epd_compute_pipe_command(epd_compute_pipe_host_t p, const epd_compute_pipe_message_t *m, const void *data, size_t n)
{
    worker_only(); assert(p->phase == 2 && m->operation == EPD_PIPE_COMMAND);
    int transient = (int)InterlockedExchange(&command_transient, 0);
    if (transient) return transient;
    memcpy(&p->command, data, sizeof(p->command));
    assert(n == sizeof(p->command) + p->command.count * sizeof(crf_codepoint));
    memcpy(p->input, (const uint8_t *)data + sizeof(p->command), p->command.count * sizeof(crf_codepoint));
    command_count = p->command.count;
    for (unsigned i = 0; i < command_count; ++i) command_order[i] = p->input[i].codepoint;
    p->phase = 3; p->first = 0; InterlockedIncrement(&commands);
    if (command_probe) command_probe(p);
    return 0;
}
int epd_compute_pipe_step(epd_compute_pipe_host_t p, epd_compute_pipe_service_t callback, void *user)
{
    worker_only();
    if (p->cancelled) return EPD_COMPUTE_CANCELLED;
    int transient = (int)InterlockedExchange(&step_transient, 0);
    if (transient) return transient;
    epd_compute_pipe_message_t *m = &p->message;
    memset(m, 0, sizeof(*m)); m->values[0] = EPD_FONT_VERSION;
    if (p->phase == 0 || (p->phase == 3 && batch_source_reads)) {
        bool opening = p->phase == 0;
        uint8_t bytes[16]; m->operation = EPD_PIPE_READ; m->handle = p->open.handle;
        m->length = sizeof(bytes); m->size = p->open.source_bytes ? p->open.source_bytes : p->open.font.file_size;
        if (p->open.flags) { assert(p->open.flags == EPD_FONT_SOURCE_FTZ1); InterlockedIncrement(&pack_reads); }
        memcpy(&m->values[1], &p->open.font, sizeof(p->open.font));
        int result = callback(user, m, bytes); p->phase = opening ? 1 : 4;
        return result == EPD_COMPUTE_OK ? EPD_COMPUTE_MORE : EPD_COMPUTE_COMPONENT_ERROR;
    }
    if (p->phase == 2) { m->operation = EPD_PIPE_COMMAND; return EPD_COMPUTE_MORE; }
    uint8_t payload[EPD_FONT_PACKET_BYTES]; memset(payload, 0, sizeof(payload));
    epd_font_output_t h = {0}; crf_glyph g = {0};
    h.version = EPD_FONT_VERSION; h.font = p->open.font; h.flags = EPD_FONT_OUTPUT_FINAL;
    h.metrics.line_height = p->command.style.size_px + 2; h.metrics.base_line = 4;
    if (p->phase == 1) { h.flags |= EPD_FONT_OUTPUT_OPEN; p->phase = 2; }
    else {
        h.request_id = p->command.request_id; h.style = p->command.style;
        h.total = p->command.count; h.first = p->first;
        if (h.total) {
            h.completed = 1; g.codepoint = p->input[p->first].codepoint;
            bool missing = g.codepoint == 0x10ffff || (p->open.font.source_id && g.codepoint == 'X');
            if (missing) { g.flags = CRF_GLYPH_MISSING; g.bitmap_offset = CRF_NO_BITMAP; }
            else if (g.codepoint < 32 || g.codepoint == ' ') { g.flags = CRF_GLYPH_CONTROL; g.advance_px = 4; }
            else if (g.codepoint == 0xa0) { g.glyph_index = 98; g.advance_px = 6; }
            else {
                g.glyph_index = g.codepoint; g.box_h = glyph_side;
                g.box_w = glyph_side + (h.style.oblique ? (glyph_side - 1) / 4 : 0);
                g.advance_px = g.box_w + 1; g.stride = (g.box_w + 3) & ~3u;
                if (p->command.operation == EPD_FONT_RENDER) g.bitmap_size = g.stride * g.box_h;
            }
            h.bitmap_bytes = g.bitmap_size; ++p->first;
            if (p->first < h.total) h.flags = 0; else p->phase = 2;
        }
        else p->phase = 2;
    }
    int status = 0;
    if (output_status_skip) --output_status_skip;
    else status = (int)InterlockedExchange(&output_status, 0);
    if (status) {
        h.status = status; h.completed = h.bitmap_bytes = 0;
        h.flags = (h.flags & EPD_FONT_OUTPUT_OPEN) | EPD_FONT_OUTPUT_FINAL; p->phase = 2;
    }
    h.bytes = sizeof(h) + h.completed * sizeof(g) + h.bitmap_bytes;
    assert(h.bytes <= sizeof(payload));
    if (InterlockedExchange(&bad_packet, 0)) ++h.font.generation;
    memcpy(payload, &h, sizeof(h));
    if (h.completed) memcpy(payload + sizeof(h), &g, sizeof(g));
    if (h.bitmap_bytes) memset(payload + sizeof(h) + sizeof(g), 0xab, h.bitmap_bytes);
    m->operation = EPD_PIPE_OUTPUT; m->length = h.bytes;
    int consumed = callback(user, m, payload);
    if (output_probe) output_probe();
    return consumed == EPD_COMPUTE_OK ? EPD_COMPUTE_MORE : EPD_COMPUTE_INVALID;
}
#endif

static void idle(void)
{
    ULONGLONG deadline = GetTickCount64() + 10000;
    EnterCriticalSection(&cache_lock.cs);
    for (;;) {
        bool busy = work_head != NULL;
        for (remote_font_t *f = fonts; f; f = f->next) busy |= f->working;
        if (!busy) break;
        ULONGLONG now = GetTickCount64();
        if (now >= deadline || !SleepConditionVariableCS(&transition, &cache_lock.cs, (DWORD)(deadline - now))) {
            fputs("FAIL worker idle timeout (10 seconds)\n", stderr);
            fflush(NULL); ExitProcess(124);
        }
    }
    LeaveCriticalSection(&cache_lock.cs);
}
static lv_font_t *create(unsigned id)
{ uint32_t identity; lv_font_t *f = ui_reader_font_remote_create(id, 24, 0, &identity); assert(f && identity); return f; }
static void ready_font(lv_font_t *f) { idle(); assert(ui_reader_font_remote_status(f) == 1); }
static void prepare(lv_font_t *f, const char *s, bool pixels)
{ assert(ui_reader_font_remote_prepare(f, s, strlen(s), pixels) >= 0); idle(); }
/* Exercise descriptor-only states through the background measurement path. */
static void measure_only(lv_font_t *f, const char *s)
{
    unsigned purpose = remote(f)->purpose;
    assert(ui_reader_font_remote_purpose(f, EPD_COMPUTE_BACKGROUND) == 1);
    prepare(f, s, false);
    assert(ui_reader_font_remote_purpose(f, purpose) == 1);
}
static void destroy(lv_font_t *f) { ui_reader_font_remote_destroy(f); idle(); }
static lv_font_glyph_dsc_t descriptor(lv_font_t *f, uint32_t c)
{ lv_font_glyph_dsc_t d; memset(&d, 0, sizeof(d)); assert(f->get_glyph_dsc(f, &d, c, 0)); return d; }

static void async_metrics(void)
{
    hold_io = 1; lv_font_t *f = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    assert(ui_reader_font_remote_status(f) == 0 && f->line_height == 0);
    lv_font_glyph_dsc_t d = descriptor(f, 'A'); assert(!d.box_w && !f->fallback);
    assert(ui_reader_font_remote_purpose(f, EPD_COMPUTE_BACKGROUND) == 1);
    assert(ui_reader_font_remote_prepare(f, "ABCD", 4, false) == 0);
    assert(ui_reader_font_remote_purpose(f, EPD_COMPUTE_FOREGROUND) == 1);
    SetEvent(io_continue); idle();
    assert(f->line_height == 0 && f->base_line == 0);
    assert(ui_reader_font_remote_status(f) == 1 && f->line_height == 26 && f->base_line == 4);
    assert(ui_reader_font_remote_ready(f, 'A', 0, false) == 1);
    LONG reads = raw_reads + pack_reads, calls = commands;
    d = descriptor(f, 'A'); assert(d.box_w == glyph_side);
    assert(!f->get_glyph_bitmap(&d, NULL));
    assert(reads == raw_reads + pack_reads && calls == commands);
    destroy(f);
}
static void batch_cache(void)
{
    lv_font_t *f = create(0); ready_font(f); LONG before = commands;
    prepare(f, "ABBA", true); assert(commands == before + 1);
    assert(ui_reader_font_remote_prepare(f, "ABBA", 4, true) == 1);
    lv_font_glyph_dsc_t d = descriptor(f, 'A');
    const lv_draw_buf_t *buf = f->get_glyph_bitmap(&d, NULL);
    assert(buf && d.entry && buf->data[0] == 0xab);
    LONG reads = raw_reads + pack_reads;
    for (unsigned i = 0; i < 50; ++i) { lv_font_glyph_dsc_t copy = descriptor(f, 'A'); assert(copy.box_w); }
    assert(reads == raw_reads + pack_reads); f->release_glyph(f, &d); destroy(f);
}
static void empty_measure_requires_render(void)
{
    lv_font_t *f = create(0); ready_font(f);
    const char text[] = " \xc2\xa0\n";
    measure_only(f, text);
    assert(ui_reader_font_remote_prepare(f, text, sizeof(text) - 1, false) == 1);
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    epd_app_font_glyph_t borrowed;
    LONG before = commands;
    /* Keep the worker out until every public entry point has seen the same
     * measured-but-not-rendered zero-size descriptor. */
    lock();
    assert(ui_reader_font_remote_ready(f, 0xa0, 0, true) == 0);
    assert(ui_reader_font_remote_page_pin(page, f, 0xa0, 0, true) == 0);
    assert(ui_reader_font_remote_acquire(f, 0xa0, 0, true, &borrowed) == 0 && !borrowed.token);
    assert(ui_reader_font_remote_prepare(f, text, sizeof(text) - 1, true) == 0);
    unlock(); idle();
    assert(commands == before + 1);
    assert(ui_reader_font_remote_prepare(f, text, sizeof(text) - 1, true) == 1);
    assert(ui_reader_font_remote_page_pin(page, f, 0xa0, 0, true) == 1);
    assert(ui_reader_font_remote_acquire(f, 0xa0, 0, true, &borrowed) == 1);
    assert(borrowed.token && !borrowed.pixels && borrowed.adv_w == 6 && !borrowed.box_w && !borrowed.box_h);
    ui_reader_font_remote_page_release(page); destroy(f);
    assert(fonts && !pipe_count); /* An empty acquire still owns a reference. */
    ui_reader_font_remote_release(&borrowed); idle(); assert(!fonts);
}
static void empty_bitmap_callbacks(void)
{
    lv_font_t *f = create(0); ready_font(f); prepare(f, "\xc2\xa0", true);
    assert(ui_reader_font_remote_ready(f, 0xa0, 0, true) == 1);
    LONG before = commands, allocated = allocations;
    epd_app_font_remote_stats_t stats; ui_reader_font_remote_stats(&stats);
    for (unsigned raw = 0; raw < 2; ++raw) {
        lv_font_glyph_dsc_t d = descriptor(f, 0xa0);
        assert(d.adv_w == 6 && !d.box_w && !d.box_h);
        d.req_raw_bitmap = raw;
        assert(!f->get_glyph_bitmap(&d, NULL) && !d.entry);
        f->release_glyph(f, &d);
    }
    idle();
    assert(commands == before && allocations == allocated && !pixel_bytes);
    epd_app_font_remote_stats_t after; ui_reader_font_remote_stats(&after);
    assert(after.draw_hits == stats.draw_hits + 2 && after.draw_misses == stats.draw_misses);
    destroy(f);
}
static void pending_normal_glyph(void)
{
    lv_font_t *f = create(0); ready_font(f);
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    epd_app_font_glyph_t borrowed;
    lock();
    assert(ui_reader_font_remote_ready(f, 'A', 0, false) == 0);
    lv_font_glyph_dsc_t pending = descriptor(f, 'A');
    assert(!pending.box_w && !pending.box_h && !pending.resolved_font);
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == 0);
    assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &borrowed) == 0 && !borrowed.token);
    assert(ui_reader_font_remote_prepare(f, "A", 1, true) == 0);
    unlock(); idle();
    assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &borrowed) == 1);
    assert(borrowed.pixels && borrowed.box_w == glyph_side && borrowed.box_h == glyph_side);
    ui_reader_font_remote_release(&borrowed);
    /* Evicting ordinary pixels must reset readiness even with a cached descriptor. */
    ui_reader_font_remote_page_release(page);
    lock(); drop_pixels(lookup(remote(f), 'A'));
    assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 0); unlock(); idle();
    assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &borrowed) == 1 && borrowed.pixels);
    ui_reader_font_remote_release(&borrowed); destroy(f);
}
static void pin_admission_retry(void)
{
    lv_font_t *f = create(0); ready_font(f);
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    lock();
    uint32_t refs = remote(f)->references;
    admission_busy = epd_app_allocation_peak(sizeof(glyph_t));
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == EPD_APP_FONT_PIN_PENDING);
    assert(!page->pins && !lookup(remote(f), 'A') && remote(f)->references == refs);
    admission_busy = epd_app_allocation_peak(sizeof(page_pin_t));
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == EPD_APP_FONT_PIN_PENDING);
    glyph_t *g = lookup(remote(f), 'A');
    assert(g && !g->pins && !page->pins && remote(f)->references == refs);
    admission_busy = 0;
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == 0);
    assert(g->pins == 1 && page->pins && remote(f)->references == refs + 1);
    unlock(); idle();
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == 1 && g->pins == 1);
    ui_reader_font_remote_page_release(page);
    page = ui_reader_font_remote_page_create(); assert(page);
    lock();
    admission_busy = epd_app_allocation_peak(sizeof(page_pin_t));
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == EPD_APP_FONT_PIN_PENDING);
    fake_ticks += 5u * RT_TICK_PER_SECOND;
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == EPD_APP_FONT_NO_MEMORY);
    assert(!g->pins && !page->pins && remote(f)->references == refs);
    admission_busy = 0;
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(42);
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == 1);
    epd_memory_owner_leave_trusted(old);
    unlock();
    ui_reader_font_remote_page_release(page); destroy(f);
}
static void long_text_empty_glyph(void)
{
    char text[8192]; size_t length = 0;
    for (unsigned repeat = 0; repeat < 12; ++repeat)
        for (unsigned c = 33; c < 127; ++c) {
            text[length++] = (char)c; text[length++] = ' ';
            text[length++] = '\xc2'; text[length++] = '\xa0';
        }
    lv_font_t *f = create(1); ready_font(f);
    assert(ui_reader_font_remote_prepare(f, text, length, true) == 0); idle();
    LONG before = commands, reads = raw_reads + pack_reads, allocated = allocations;
    for (unsigned repeat = 0; repeat < 100; ++repeat)
        assert(ui_reader_font_remote_prepare(f, text, length, true) == 1);
    epd_app_font_page_t a = ui_reader_font_remote_page_create(), b = ui_reader_font_remote_page_create();
    assert(a && b);
    assert(ui_reader_font_remote_page_pin(a, f, 0xa0, 0, true) == 1);
    assert(ui_reader_font_remote_page_pin(a, f, 0xa0, 0, true) == 1);
    assert(ui_reader_font_remote_page_pin(b, f, 0xa0, 0, true) == 1);
    lock(); assert(lookup(remote(f), 0xa0)->pins == 2); unlock();
    assert(ui_reader_font_remote_glyph_fallback(f, 'X'));
    assert(!ui_reader_font_remote_glyph_fallback(f, 0xa0));
    ui_reader_font_remote_page_release(a); ui_reader_font_remote_page_release(b); idle();
    assert(commands == before && raw_reads + pack_reads == reads && allocations == allocated);
    destroy(f);
}
static void missing_only_fallback(void)
{
    lv_font_t *f = create(1); ready_font(f); assert(remote_opens == 1);
    prepare(f, "A", true); assert(remote_opens == 1);
    prepare(f, "X", true); assert(remote_opens == 2 && raw_reads == 1 && pack_reads == 1);
    assert(ui_reader_font_remote_ready(f, 'X', 0, true) == 1 && !f->fallback);
    assert(ui_reader_font_remote_ready(f, 0x10ffff, 0, true) == 0); idle();
    assert(ui_reader_font_remote_ready(f, 0x10ffff, 0, true) == 1);
    lv_font_glyph_dsc_t square = descriptor(f, 0x10ffff);
    assert(square.is_placeholder && square.box_w && square.box_h && square.adv_w);
    assert(f->get_glyph_bitmap(&square, NULL)); f->release_glyph(f, &square);
    destroy(f);
}
static void revoke_generation(void)
{
    lv_font_t *f = create(1); ready_font(f); prepare(f, "AX", true);
    lv_font_glyph_dsc_t d = descriptor(f, 'A'); assert(f->get_glyph_bitmap(&d, NULL));
    InterlockedIncrement(&fixture_generation);
    assert(ui_reader_font_remote_ready(f, 'A', 0, true) == -1);
    assert(ui_reader_font_remote_status(f) == -1); idle(); assert(!leases && !pipe_count);
    assert(!f->get_glyph_bitmap(&d, NULL));
    lv_font_glyph_dsc_t empty = descriptor(f, 'X'); assert(!empty.box_w);
    f->release_glyph(f, &d); destroy(f);
    lv_font_t *new_font = create(1); ready_font(new_font); destroy(new_font);
}
static void cancel_during_io(void)
{
    hold_io = 1; lv_font_t *f = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    ui_reader_font_remote_cancel(f);
    assert(ui_reader_font_remote_status(f) == -1);
    ui_reader_font_remote_destroy(f);
    assert(leases == 1 && allocations > 0);
    SetEvent(io_continue); idle(); assert(!leases && !fonts && !allocations);
}
static void page_tokens_and_draws(void)
{
    lv_font_t *f = create(0); ready_font(f);
    epd_app_font_page_t old = ui_reader_font_remote_page_create(), next = ui_reader_font_remote_page_create();
    assert(ui_reader_font_remote_page_pin(old, f, 'A', 0, true) == 0); idle();
    assert(ui_reader_font_remote_page_pin(next, f, 'A', 0, true) == 1);
    assert(ui_reader_font_remote_page_pin(next, f, 'A', 0, true) == 1);
    lock(); assert(lookup(remote(f), 'A')->pins == 2); unlock();
    lv_font_glyph_dsc_t d = descriptor(f, 'A'); assert(f->get_glyph_bitmap(&d, NULL));
    ui_reader_font_remote_destroy(f); ui_reader_font_remote_page_release(old);
    assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 1);
    ui_reader_font_remote_page_release(next); idle();
    assert(fonts && allocations && !pipe_count);
    f->release_glyph(f, &d); idle(); assert(!fonts && !allocations);
}
static void oblique_lifetime_priority(void)
{
    lv_font_t *base = create(1), *italic = ui_reader_font_remote_oblique(base); assert(italic);
    ready_font(italic); assert(ui_reader_font_remote_status(base) == 1);
    assert(ui_reader_font_remote_purpose(italic, EPD_COMPUTE_BACKGROUND) == 1);
#ifndef REAL_FONT_PIPE
    assert(remote(italic)->primary->pipe->purpose == EPD_COMPUTE_BACKGROUND);
#endif
    destroy(base); prepare(italic, "A", true);
    lv_font_glyph_dsc_t d = descriptor(italic, 'A'); assert(d.box_w == glyph_side + (glyph_side - 1) / 4);
    assert(ui_reader_font_remote_ref(italic)); destroy(italic);
    assert(ui_reader_font_remote_status(italic) == 1); destroy(italic);
}
static void pixel_lru(void)
{
    glyph_side = 64; lv_font_t *f = create(0); ready_font(f);
    epd_app_font_page_t page = ui_reader_font_remote_page_create();
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == 0); idle();
    for (uint32_t c = 1000; c < 1600; ++c) {
        assert(ui_reader_font_remote_ready(f, c, 0, true) == 0); idle();
        assert(ui_reader_font_remote_ready(f, c, 0, true) == 1);
    }
    lock(); assert(pixel_bytes <= PIXEL_BUDGET && descriptor_bytes <= DESC_BUDGET);
    assert(lookup(remote(f), 'A')->pixels && !lookup(remote(f), 1000)->pixels); unlock();
    assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 1);
    ui_reader_font_remote_page_release(page); destroy(f);
}
static void paged_pin_pressure(void)
{
    enum { PAGES = 3, PER_PAGE = 96, SHIFT = 72 };
    glyph_side = 64;
    lv_font_t *base = create(0), *italic = ui_reader_font_remote_oblique(base); assert(italic);
    ready_font(base); ready_font(italic);
    epd_app_font_page_t pages[PAGES];
    for (unsigned p = 0; p < PAGES; ++p) {
        pages[p] = ui_reader_font_remote_page_create(); assert(pages[p]);
        for (uint32_t c = 1000 + p * SHIFT; c < 1000 + p * SHIFT + PER_PAGE; ++c)
            assert(ui_reader_font_remote_page_pin(pages[p], c & 1 ? italic : base, c, 0, true) >= 0);
        idle();
        for (uint32_t c = 1000 + p * SHIFT; c < 1000 + p * SHIFT + PER_PAGE; ++c)
            assert(ui_reader_font_remote_ready(c & 1 ? italic : base, c, 0, true) == 1);
    }
    lock(); assert(lookup(remote(base), 1072)->pins == 2); unlock();
    epd_app_font_glyph_t reading;
    assert(ui_reader_font_remote_acquire(base, 1000, 0, true, &reading) == 1);
    lv_font_glyph_dsc_t drawing = descriptor(italic, 1001);
    assert(italic->get_glyph_bitmap(&drawing, NULL));
    /* A new page has bound; retire only the previous page's token. */
    ui_reader_font_remote_page_release(pages[0]);
    for (uint32_t c = 2000; c < 2700; ++c) {
        assert(ui_reader_font_remote_ready(base, c, 0, true) == 0); idle();
        assert(ui_reader_font_remote_ready(base, c, 0, true) == 1);
    }
    lock();
    assert(pixel_bytes <= PIXEL_BUDGET && descriptor_bytes <= DESC_BUDGET);
    assert(!lookup(remote(base), 1002)->pixels && lookup(remote(base), 1000)->pixels);
    assert(lookup(remote(italic), 1001)->pixels && lookup(remote(base), 1072)->pins == 1);
    unlock();
    for (unsigned p = 1; p < PAGES; ++p)
        for (uint32_t c = 1000 + p * SHIFT; c < 1000 + p * SHIFT + PER_PAGE; ++c)
            assert(ui_reader_font_remote_ready(c & 1 ? italic : base, c, 0, true) == 1);
    ui_reader_font_remote_page_release(pages[2]);
    assert(ui_reader_font_remote_ready(base, 1072, 0, true) == 1);
    ui_reader_font_remote_page_release(pages[1]);
    destroy(base); destroy(italic);
    assert(reading.pixels[0] == 0xab && fonts);
    ui_reader_font_remote_release(&reading); italic->release_glyph(italic, &drawing); idle();
    assert(!fonts);
}
static void descriptor_lru_stale(void)
{
    lv_font_t *f = create(0); ready_font(f); prepare(f, "A", true);
    lv_font_glyph_dsc_t old = descriptor(f, 'A');
    for (uint32_t c = 1000; c < 5000; ++c) { assert(ui_reader_font_remote_ready(f, c, 0, false) == 0); idle(); }
    lock(); assert(descriptor_bytes <= DESC_BUDGET); assert(!lookup(remote(f), 'A')); unlock();
    prepare(f, "A", true); assert(!f->get_glyph_bitmap(&old, NULL)); destroy(f);
}
static void invalid_and_failure(void)
{
    lv_font_t *f = create(0); ready_font(f);
    LONG before = commands;
    assert(ui_reader_font_remote_prepare(f, "\xc0\xaf", 2, true) == -1);
    assert(ui_reader_font_remote_ready(f, 0xd800, 0, false) == -1);
    assert(ui_reader_font_remote_purpose(f, 8) == -1);
    assert(commands == before);
    fail_alloc = 1; assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 0);
    bad_packet = 1; prepare(f, "B", true);
    assert(ui_reader_font_remote_status(f) == -1); destroy(f);
}
static void pinned_budget(void)
{
    lv_font_t *f = create(0); ready_font(f);
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    unsigned accepted = 0;
    for (uint32_t c = 1000; c < 11000; ++c) {
        int status = ui_reader_font_remote_page_pin(page, f, c, 0, false);
        if (status < 0) break;
        ++accepted; idle();
    }
    assert(accepted > 100 && accepted < 10000);
    assert(ui_reader_font_remote_ready(f, 1000, 0, false) == 1);
    lock(); assert(descriptor_bytes <= DESC_BUDGET); unlock();
    ui_reader_font_remote_page_release(page); destroy(f);
}
static void worker_priority(void)
{
    hold_io = 1; lv_font_t *blocked = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    lv_font_t *background = create(2), *foreground = create(3);
    assert(ui_reader_font_remote_purpose(background, EPD_COMPUTE_BACKGROUND) == 1);
    assert(ui_reader_font_remote_purpose(foreground, EPD_COMPUTE_FOREGROUND) == 1);
    SetEvent(io_continue); idle();
    assert(open_order[1] == remote(foreground)->primary->identity.generation);
    assert(open_order[2] == remote(background)->primary->identity.generation);
    destroy(blocked); destroy(background); destroy(foreground);
}

static void source_sharing(void)
{
    lv_font_t *a = create(1); ready_font(a);
    uint32_t identity;
    lv_font_t *b = ui_reader_font_remote_create(1, 36, 2, &identity);
    lv_font_t *c = ui_reader_font_remote_oblique(a); assert(b && c);
    ready_font(b); ready_font(c);
    assert(remote(a)->primary == remote(b)->primary && remote(a)->primary == remote(c)->primary);
    assert(remote_opens == 1 && raw_reads == 1 && pipe_count == 1);
    assert(a->line_height == 26 && b->line_height == 38 && c->line_height == 26);
    prepare(b, "A", true); prepare(c, "A", true);
    assert(descriptor(b, 'A').box_w == glyph_side);
    assert(descriptor(c, 'A').box_w > glyph_side);
    ui_reader_font_remote_cancel(b); idle();
    assert(ui_reader_font_remote_status(b) == -1 && ui_reader_font_remote_status(a) == 1);
    prepare(a, "B", true); assert(remote_opens == 1);
    destroy(b); destroy(a); assert(ui_reader_font_remote_status(c) == 1); destroy(c);
}
static void builtin_source_sharing(void)
{
    lv_font_t *faces[6]; uint32_t identity;
    for (unsigned i = 0; i < 6; ++i) {
        faces[i] = ui_reader_font_remote_create(0, 24 + i * 2, i & 1, &identity);
        assert(faces[i]); ready_font(faces[i]);
        assert(remote(faces[i])->primary == remote(faces[0])->primary);
    }
    assert(remote_opens == 1 && resident_sources == 1);
    lv_font_t *raw = create(1); ready_font(raw);
    assert(remote_opens == 2 && resident_sources == 2);
    destroy(faces[0]); faces[0] = create(0); ready_font(faces[0]);
    assert(remote(faces[0])->primary == remote(faces[1])->primary && remote_opens == 2);
    for (unsigned i = 0; i < 6; ++i) destroy(faces[i]);
    destroy(raw);
}
static void source_capacity_fallback(void)
{
    lv_font_t *a = create(1); ready_font(a); prepare(a, "A", true);
    lv_font_t *b = create(2); ready_font(b);
    lv_font_t *c = create(3); ready_font(c);
    assert(pipe_count == 2 && remote_opens == 3 && !remote(a)->primary->pipe);
    assert(ui_reader_font_remote_ready(a, 'A', 0, true) == 1 && remote_opens == 3);
    prepare(a, "B", true); assert(pipe_count == 2 && remote_opens == 4);
    prepare(a, "X", true);
    lv_font_t *builtin = create(0); ready_font(builtin);
    assert(remote(a)->fallback == remote(builtin)->primary);
    LONG before = remote_opens;
    prepare(builtin, "A", true); assert(remote_opens == before);
    prepare(b, "X", true);
    assert(remote(b)->fallback == remote(a)->fallback && pipe_count == 2);
    destroy(a); destroy(b); destroy(c); destroy(builtin);
}
static void cancel_shared_active(void)
{
    hold_io = 1; lv_font_t *a = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    lv_font_t *b = create(1);
    assert(remote(a)->primary == remote(b)->primary);
    ui_reader_font_remote_cancel(a);
    assert(ui_reader_font_remote_status(a) == -1 && ui_reader_font_remote_status(b) == 0);
    SetEvent(io_continue); ready_font(b);
    assert(remote_opens == 2 && pipe_count == 1);
    prepare(b, "A", true); destroy(a); destroy(b);
}
static void shared_required(void)
{
    allow_shared = false;
    lv_font_t *f = create(0); idle();
    assert(ui_reader_font_remote_status(f) == -1 && !remote_opens && !pipe_count);
    destroy(f);
}
static void source_generation_isolation(void)
{
    lv_font_t *old = create(1); ready_font(old); prepare(old, "A", true);
    epd_app_font_glyph_t borrowed;
    assert(ui_reader_font_remote_acquire(old, 'A', 0, true, &borrowed) == 1);
    face_t *previous = remote(old)->primary;
    uint32_t previous_generation = previous->identity.generation;
    InterlockedIncrement(&fixture_generation);
    lv_font_t *fresh = create(1); ready_font(fresh);
    assert(remote(fresh)->primary != previous);
    assert(remote(fresh)->primary->identity.generation != previous_generation);
    assert(ui_reader_font_remote_status(old) == -1); idle();
    prepare(fresh, "A", true);
    assert(ui_reader_font_remote_ready(fresh, 'A', 0, true) == 1);
    assert(borrowed.pixels[0] == 0xab);
    destroy(old); ui_reader_font_remote_release(&borrowed); destroy(fresh);
}
static void cancel_shared_inactive(void)
{
    hold_io = 1; lv_font_t *active = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    lv_font_t *cancelled = create(1);
    ui_reader_font_remote_cancel(cancelled); ui_reader_font_remote_destroy(cancelled);
    SetEvent(io_continue); ready_font(active);
    assert(remote_opens == 1 && pipe_count == 1);
    prepare(active, "A", true); destroy(active);
}
static DWORD WINAPI borrow_from_worker(LPVOID arg)
{
    lv_font_t *f = arg;
    epd_app_font_metrics_t m;
    assert(ui_reader_font_remote_metrics(f, &m) == 1 && m.line_height == 26 && m.base_line == 4);
    assert(!f->line_height && !f->base_line);
    assert(ui_reader_font_remote_prepare(f, "A ", 2, true) == 0); idle();
    assert(ui_reader_font_remote_prepare(f, "A ", 2, true) == 1);
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    assert(ui_reader_font_remote_page_pin(page, f, 'A', 0, true) == 1);
    epd_app_font_glyph_t glyph, space, desc;
    assert(ui_reader_font_remote_acquire(f, 'A', 0, false, &desc) == 1 && !desc.pixels && desc.token);
    assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &glyph) == 1);
    assert(glyph.pixels && glyph.box_w == glyph_side && glyph.stride >= glyph.box_w);
    assert(ui_reader_font_remote_acquire(f, ' ', 0, true, &space) == 1 && !space.pixels && space.token);
    ui_reader_font_remote_page_release(page);
    ui_reader_font_remote_cancel(f); ui_reader_font_remote_destroy(f); idle();
    assert(glyph.pixels[0] == 0xab && fonts && !pipe_count);
    ui_reader_font_remote_release(&desc); ui_reader_font_remote_release(&space);
    ui_reader_font_remote_release(&glyph); assert(!glyph.token && !glyph.pixels);
    ui_reader_font_remote_release(&glyph); idle();
    return 0;
}
static void worker_snapshots(void)
{
    lv_font_t *f = create(0); idle();
    HANDLE t = CreateThread(NULL, 0, borrow_from_worker, f, 0, NULL); assert(t);
    assert(WaitForSingleObject(t, 10000) == WAIT_OBJECT_0); CloseHandle(t);
}

typedef struct { const char *name; void (*run)(void); } test_t;
static void shutdown_retained(void)
{
    lv_font_t *a = create(0); ready_font(a);
    fail_shutdown = 1; destroy(a);
    assert(!fonts && sources && sources->closing && !sources->pipe);
    assert(resident_sources == 1 && pipe_count == 1 && descriptor_bytes == sizeof(face_t));
    LONG calls = shutdown_calls;
    idle(); assert(shutdown_calls == calls); /* No periodic cleanup polling. */
    fail_shutdown = 0;
    lv_font_t *b = create(1); ready_font(b);
    assert(resident_sources == 1 && pipe_count == 1 && !sources->closing);
    destroy(b);
}
static void shutdown_capacity(void)
{
    lv_font_t *a = create(0), *b = create(1); ready_font(a); ready_font(b);
    fail_shutdown = 1;
    lv_font_t *c = create(2); idle();
    assert(ui_reader_font_remote_status(c) == 0 && remote_opens == 2 && resident_sources == 2);
    destroy(c); fail_shutdown = 0;
    lv_font_t *d = create(3); ready_font(d);
    assert(resident_sources <= 2 && pipe_count <= 2);
    destroy(a); destroy(b); destroy(d);
}
static void platform_creation_reserve(void)
{
    available_bytes = 512u * 1024u;
    lv_font_t *f = create(0); ready_font(f);
    uint32_t id = 99; available_bytes = 1;
    assert(!ui_reader_font_remote_create(1, 24, 0, &id) && !id && !leases);
    assert(font_count == 1 && !diagnostics.oom_postcheck);
    available_bytes = 64u * 1024u * 1024u; destroy(f);
}
static void platform_source_reserve(void)
{
    lv_font_t *warm = create(0); ready_font(warm); destroy(warm);
    lock(); lv_font_t *f = create(1); available_bytes = 1; unlock(); idle();
    assert(ui_reader_font_remote_status(f) == 0 && !resident_sources);
    uint32_t started = remote(f)->memory_recovery.started;
    for (unsigned i = 0; i < 3; ++i) {
        fake_ticks += RETRY_TICKS; ui_reader_font_remote_status(f); idle();
        assert(remote(f)->memory_recovery.started == started);
    }
    available_bytes = 512u * 1024u; fake_ticks += RETRY_TICKS;
    assert(ui_reader_font_remote_status(f) == 0); /* Enqueue the deferred request. */
    ready_font(f); destroy(f);
}
static void platform_pixel_reclaim(void)
{
    glyph_side = 64;
    lv_font_t *f = create(0); ready_font(f); prepare(f, "AB", true);
    epd_app_font_glyph_t held; assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &held) == 1);
    available_bytes = 512u * 1024u; prepare(f, "C", true);
    assert(lookup(remote(f), 'B')->pixels && held.pixels[0] == 0xab);
    size_t released = 0; remote_resource_shrink(NULL, 1, 1, &released);
    assert(released && held.pixels[0] == 0xab);
    ui_reader_font_remote_release(&held); destroy(f);
}
static void platform_pinned_failure(void)
{
    lv_font_t *f = create(0); ready_font(f); prepare(f, "A", true); measure_only(f, "B");
    epd_app_font_glyph_t held; assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &held) == 1);
    available_bytes = 1; prepare(f, "B", true);
    assert(remote(f)->memory_recovery.used && held.pixels[0] == 0xab);
    fake_ticks += 5u * RT_TICK_PER_SECOND;
    ui_reader_font_remote_prepare(f, "B", 1, true); idle();
    assert(ui_reader_font_remote_status(f) == 1 && held.pixels[0] == 0xab);
    assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 1);
    assert(ui_reader_font_remote_ready(f, 'B', 0, false) == 1);
    assert(ui_reader_font_remote_prepare(f, "B", 1, true) == EPD_APP_FONT_NO_MEMORY);
    epd_app_font_glyph_t glyph;
    assert(ui_reader_font_remote_acquire(f, 'B', 0, true, &glyph) == EPD_APP_FONT_NO_MEMORY && !glyph.token);
    epd_app_font_page_t page = ui_reader_font_remote_page_create();
    assert(!page);
    available_bytes = 64u * 1024u * 1024u;
    uint32_t requests = remote(f)->request;
    for (unsigned i = 0; i < 3; ++i) {
        fake_ticks += RT_TICK_PER_SECOND;
        assert(ui_reader_font_remote_prepare(f, "B", 1, true) == EPD_APP_FONT_NO_MEMORY);
        idle(); assert(remote(f)->request == requests);
    }
    ui_reader_font_remote_retry_memory(f); idle();
    assert(ui_reader_font_remote_ready(f, 'B', 0, true) == 1 && held.pixels[0] == 0xab);
    ui_reader_font_remote_release(&held); destroy(f);
}
static void platform_source_denied_retry(void)
{
    lv_font_t *warm = create(0); ready_font(warm); destroy(warm);
    lock(); lv_font_t *f = create(1); available_bytes = 1; unlock(); idle();
    fake_ticks += 5u * RT_TICK_PER_SECOND;
    ui_reader_font_remote_status(f); idle();
    assert(ui_reader_font_remote_status(f) == EPD_APP_FONT_NO_MEMORY);
    epd_app_font_metrics_t metrics;
    assert(ui_reader_font_remote_metrics(f, &metrics) == EPD_APP_FONT_NO_MEMORY);
    assert(!remote(f)->error && !resident_sources);
    available_bytes = 64u * 1024u * 1024u;
    fake_ticks += RT_TICK_PER_SECOND;
    assert(ui_reader_font_remote_status(f) == EPD_APP_FONT_NO_MEMORY);
    ui_reader_font_remote_retry_memory(f); ready_font(f);
    ui_reader_font_remote_cancel(f); idle();
    ui_reader_font_remote_retry_memory(f); idle();
    assert(ui_reader_font_remote_status(f) == -1);
    destroy(f);
}
static void worker_reserve_rollback(void)
{
    uint32_t id = 1; size_t before = 512u * 1024u;
    available_bytes = before; fail_start = 1;
    assert(!ui_reader_font_remote_create(0, 24, 0, &id));
    assert(!initialized && !live_mutex && !live_sem && !live_thread && !started_threads);
    assert(epd_app_psram_available() == before && diagnostics.worker_rollbacks == 1);
    assert(!diagnostics.oom_postcheck);
    fail_start = 0; lv_font_t *font = create(0); ready_font(font); destroy(font);
    assert(live_mutex == 1 && live_sem == 1 && live_thread == 1 && started_threads == 1);
}
static void remote_diagnostics(void)
{
    epd_app_font_remote_stats_t s; ui_reader_font_remote_stats(&s);
    assert(!initialized && !s.pixel_bytes && !s.draw_hits && !s.io.supply_calls);
    lv_font_t *font = create(1); ready_font(font); prepare(font, "AB", true);
    ui_reader_font_remote_stats(&s);
    assert(s.ready_misses && !s.draw_hits && !s.draw_misses);
    assert(s.io.supply_calls == 1 && s.io.requested_bytes == 16 && !s.io.errors);
    assert(s.io.read.calls == 1 && s.io.read.bytes == SOURCE_CACHE_BLOCK_BYTES &&
           s.io.read.max_bytes == SOURCE_CACHE_BLOCK_BYTES);
    assert(s.io.read.ticks == 5 && s.io.read.max_ticks == 5);
    assert(s.io.lock_wait.ticks == 3 && s.io.lock_wait.max_ticks == 3);
    assert(s.io.lock_hold.ticks == 5 && s.io.lock_hold.max_ticks == 5);
    assert(!s.io.ftz_copy.calls && s.pixel_peak >= s.pixel_bytes && s.pixel_bytes);
    lv_font_glyph_dsc_t d = descriptor(font, 'A');
    assert(font->get_glyph_bitmap(&d, NULL)); font->release_glyph(font, &d);
    d = descriptor(font, 'Z'); d.resolved_font = font;
    assert(!font->get_glyph_bitmap(&d, NULL));
    ui_reader_font_remote_stats(&s);
    assert(s.draw_hits == 1 && s.draw_misses == 1 && s.descriptor_hits && s.descriptor_misses);
    epd_app_font_cache_stats_t legacy = {0}; legacy.source_bitmap_calls = 11; legacy.source_bitmap_ticks = 19;
    ui_reader_font_remote_draw_stats(&legacy);
    assert(legacy.glyph_hits == 1 && legacy.glyph_misses == 1 && legacy.source_bitmap_calls == 11 && legacy.source_bitmap_ticks == 19);
    assert(!io_logs); destroy(font); assert(!io_logs);
    ui_reader_font_remote_stats(&s);
    assert(s.io.lock_hold.ticks == 7 && s.io.lock_hold.max_ticks == 5);
    font = create(0); ready_font(font); ui_reader_font_remote_stats(&s);
    assert(s.io.ftz_copy.calls == 1 && s.io.ftz_copy.bytes == 16 && s.io.ftz_copy.max_bytes == 16);
    destroy(font); ui_reader_font_remote_stats(&s);
    assert(!s.fonts && !s.sources && !s.pins && !s.pixel_bytes && !s.descriptor_bytes);
    assert(s.pixel_peak && s.descriptor_peak && !io_logs);
}
#ifndef REAL_FONT_PIPE
static void quiet_profile(epd_app_font_remote_profile_t *out)
{
    LONG logs = io_logs, batches = batch_logs, caches = source_cache_logs;
    LONG reads = raw_reads, opens = source_opens, closes = source_closes, allocs = allocations;
    epd_app_font_remote_profile(out);
    assert(logs == io_logs && batches == batch_logs && caches == source_cache_logs);
    assert(reads == raw_reads && opens == source_opens && closes == source_closes && allocs == allocations);
}
static void remote_profile(void)
{
    epd_app_font_remote_profile_t begin, end, again;
    quiet_profile(&begin); quiet_profile(NULL);
    assert(!initialized && begin.ticks_per_second == RT_TICK_PER_SECOND);
    assert(!begin.measure.count && !begin.render.count && !begin.active.kind && !begin.io.supply_calls);
    operation_timing_test = true;
    lv_font_t *font = create(1); ready_font(font); prepare(font, "AB", true);
    quiet_profile(&end);
    assert(end.measure.count == 1 && !end.measure.glyphs && !end.measure.completed);
    assert(end.render.count == 1 && end.render.glyphs == 2 && end.render.completed == 2);
    assert(!end.active.kind && !end.measure.failures && !end.render.failures);
    assert(end.measure.exec_ticks == 39 && !end.render.exec_ticks);
    assert(end.io.supply_calls == 1 && end.io.requested_bytes == 16 && end.cache_misses == 1);
    assert(end.io.read.calls == 1 && end.io.read.bytes == 4096 && end.io.read.ticks == 5);
    assert(end.io.lock_wait.ticks == 3 && end.io.lock_hold.ticks == 36);
    assert(end.open.calls == 1 && end.open.ticks == 7 && end.fstat.calls == 1 && end.fstat.ticks == 11);
    assert(end.seek.calls == 1 && end.seek.ticks == 13 && !end.close.calls);
    prepare(font, "AB", true); quiet_profile(&again);
    assert(!memcmp(&end, &again, sizeof(end)));
    destroy(font); quiet_profile(&again);
    assert(again.close.calls == 1 && again.close.ticks == 17);
    assert(again.io.lock_hold.ticks == end.io.lock_hold.ticks + 17);
    assert(again.measure.count == end.measure.count && again.render.count == end.render.count);
    assert(again.seek.calls == end.seek.calls && !again.active.kind);
}
static void profile_partial_output(void)
{
    epd_app_font_remote_profile_t p;
    epd_app_font_remote_profile(&p);
    if (p.active.kind != 2 || p.active.completed != 1) return;
    output_probe = NULL;
    assert(p.active.glyphs == 2);
    SetEvent(io_entered);
    assert(WaitForSingleObject(io_continue, 5000) == WAIT_OBJECT_0);
}
static void remote_profile_inflight(void)
{
    epd_app_font_remote_profile_t p, done;
    operation_timing_test = true; hold_io = 1;
    InterlockedExchange(&fake_ticks, (LONG)(UINT32_MAX - 20u));
    lv_font_t *font = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    quiet_profile(&p);
    assert(p.active.kind == 1 && p.active.font_id == 1 && p.active.size_px == 24);
    assert(!p.measure.count && !p.active.glyphs && p.active.exec_ticks == 39);
    assert(!p.io.supply_calls && !p.io.read.calls);
    hold_io = 0; SetEvent(io_continue); ready_font(font);
    quiet_profile(&done);
    assert(!done.active.kind && done.measure.count == 1 && done.measure.exec_ticks >= p.active.exec_ticks);
    ResetEvent(io_entered); ResetEvent(io_continue); output_probe = profile_partial_output;
    assert(ui_reader_font_remote_prepare(font, "AB", 2, true) == 0);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    InterlockedExchangeAdd(&fake_ticks, 17);
    quiet_profile(&p);
    assert(p.active.kind == 2 && p.active.glyphs == 2 && p.active.completed == 1);
    assert(!p.render.count && p.active.exec_ticks == 17);
    assert(ui_reader_font_remote_ready(font, 'A', 0, true) == 1);
    SetEvent(io_continue); idle(); quiet_profile(&done);
    assert(!done.active.kind && done.render.count == 1 && done.render.completed == 2);
    assert(done.render.exec_ticks == p.active.exec_ticks);
    destroy(font);
}
static void quiet_retire_diagnostic(void)
{
    lv_font_t *font = create(1); ready_font(font); prepare(font, "AB", true);
    epd_app_font_remote_profile_t before, after;
    quiet_profile(&before);
    assert(before.measure.count && before.render.count && before.io.supply_calls);
    assert(!io_logs && !batch_logs && !source_cache_logs);
    destroy(font);
    assert(!io_logs && !batch_logs && !source_cache_logs);
    quiet_profile(&after);
    assert(after.measure.count == before.measure.count && after.render.count == before.render.count);
    assert(after.io.supply_calls == before.io.supply_calls && after.io.read.bytes == before.io.read.bytes);
    assert(after.close.calls == 1);
    epd_app_font_remote_stats_t stats;
    ui_reader_font_remote_stats(&stats);
    assert(batch_logs == 2 && source_cache_logs == 1 && !io_logs);
    assert(!stats.fonts && !stats.sources && !stats.pixel_bytes && !stats.io.errors);
}
static void retire_batch_error_report(void)
{
    lv_font_t *font = create(0); ready_font(font);
    bad_packet = 1;
    prepare(font, "A", true);
    epd_app_font_remote_profile_t profile;
    quiet_profile(&profile);
    assert(profile.measure.failures + profile.render.failures > 0);
    destroy(font);
    assert(batch_logs && strstr(last_batch_log, "failures=1"));
}
#endif
static void diagnostics_pressure(void)
{
    lv_font_t *f = create(0); ready_font(f); prepare(f, "AB", true);
    lock(); assert(!room(PIXEL_BUDGET + 1, true)); unlock();
    assert(diagnostics.oom_budget && !diagnostics.oom_postcheck);
    size_t released = 0; remote_resource_shrink(NULL, 1, 1, &released);
    assert(released && diagnostics.pixel_evictions); destroy(f);
}
static void open_failure_retains_handle(void)
{
    fail_open_retained = fail_shutdown = 1;
    lv_font_t *f = create(0); idle();
    assert(ui_reader_font_remote_status(f) == -1 && sources && sources->closing && resident_sources == 1);
    destroy(f); assert(!fonts && sources && sources->closing && pipe_count == 1);
    fail_open_retained = fail_shutdown = 0;
    f = create(0); ready_font(f); destroy(f);
    assert(!sources && !resident_sources && !pipe_count);
}
static void cached_prepare_one_lease_check(void)
{
    lv_font_t *f = create(1); ready_font(f); prepare(f, "ABC", true);
    LONG before = lease_checks, reads_before = raw_reads, locks_before = storage_locks, calls = commands;
    assert(ui_reader_font_remote_prepare(f, "ABCABCABCABC", 12, true) == 1);
    assert(lease_checks == before + 1 && raw_reads == reads_before && storage_locks == locks_before && calls == commands);
    destroy(f);
}
static void record_batches_are_bounded(void)
{
    lv_font_t *f = create(0); ready_font(f);
    char text[128 * 3];
    for (unsigned i = 0; i < 128; ++i) {
        uint32_t c = 0x4e00 + i;
        text[i * 3] = (char)(0xe0 | (c >> 12));
        text[i * 3 + 1] = (char)(0x80 | ((c >> 6) & 63));
        text[i * 3 + 2] = (char)(0x80 | (c & 63));
    }
    LONG before = commands;
    assert(ui_reader_font_remote_prepare(f, text, sizeof(text), false) == 0); idle();
    assert(commands == before + 2);
    assert(ui_reader_font_remote_prepare(f, text, sizeof(text), false) == 1);
    assert(commands == before + 2); destroy(f);
}
static void batch_codepoint_order(void)
{
    lv_font_t *f = create(0); ready_font(f);
    const char text[] = "zaybxc";
    prepare(f, text, false);
    assert(command_count == 6);
    const char sorted[] = "abcxyz";
    for (unsigned i = 0; i < 6; ++i) assert(command_order[i] == (uint32_t)sorted[i]);
    prepare(f, text, true);
    for (unsigned i = 0; i < 6; ++i) {
        epd_app_font_glyph_t g;
        assert(ui_reader_font_remote_acquire(f, text[i], 0, true, &g) == 1);
        assert(g.pixels && g.pixels[0] == 0xab && g.adv_w == glyph_side + 1);
        ui_reader_font_remote_release(&g);
    }
    assert(!strcmp(text, "zaybxc")); destroy(f);
}
static void postcheck_overhead_reclaim(void)
{
    lv_font_t *f = create(0); ready_font(f); measure_only(f, "B");
    available_bytes = 512u * 1024u; allocation_race = 128;
    prepare(f, "B", true);
    assert(ui_reader_font_remote_ready(f, 'B', 0, true) == 1 && !diagnostics.oom_postcheck);
    available_bytes = 64u * 1024u * 1024u; destroy(f);
}
static void postcheck_race_retry(void)
{
    lv_font_t *f = create(0); ready_font(f); measure_only(f, "B");
    available_bytes = 512u * 1024u; allocation_race = 128;
    prepare(f, "B", true);
    assert(ui_reader_font_remote_ready(f, 'B', 0, true) == 1 && !diagnostics.oom_postcheck);
    available_bytes = 64u * 1024u * 1024u; destroy(f);
}
static void source_transient_retry(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        open_transient = i ? EPD_COMPUTE_NO_MEMORY : EPD_COMPUTE_BUSY;
        lv_font_t *f = create(0); idle();
        assert(ui_reader_font_remote_status(f) == 0);
        InterlockedExchangeAdd(&fake_ticks, RT_TICK_PER_SECOND);
        assert(ui_reader_font_remote_status(f) == 0); ready_font(f);
        prepare(f, "A", true); assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 1);
        destroy(f);
    }
}
static void output_memory_retry(void)
{
    lv_font_t *f = create(0); ready_font(f); measure_only(f, "A");
    output_status = CRF_NO_MEMORY; prepare(f, "A", true);
    assert(ui_reader_font_remote_ready(f, 'A', 0, false) == 1);
    assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 0);
    InterlockedExchangeAdd(&fake_ticks, RT_TICK_PER_SECOND);
    prepare(f, "A", true); assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 1);
    output_status = CRF_OUTPUT_FULL; prepare(f, "B", true);
    assert(ui_reader_font_remote_ready(f, 'B', 0, true) == -1);
    assert(ui_reader_font_remote_status(f) == 1); destroy(f);
}
#ifndef REAL_FONT_PIPE
static void persistent_memory_command(epd_compute_pipe_host_t pipe)
{ (void)pipe; output_status = CRF_NO_MEMORY; }
static void persistent_b_memory(epd_compute_pipe_host_t pipe)
{
    for (unsigned i = 0; i < pipe->command.count; ++i)
        if (pipe->input[i].codepoint == 'B') output_status = CRF_NO_MEMORY;
}
static void remote_memory_pixel_failure(bool wrap)
{
    lv_font_t *f = create(0); ready_font(f); prepare(f, "A", true); measure_only(f, "B");
    epd_app_font_glyph_t held; assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &held) == 1);
    if (wrap) InterlockedExchange(&fake_ticks, (LONG)(UINT32_MAX - 2u * RT_TICK_PER_SECOND));
    command_probe = persistent_b_memory;
    uint32_t began = rt_tick_get();
    prepare(f, "B", true);
    assert(remote(f)->remote_memory_waiting && remote(f)->remote_memory_since == began);
    for (unsigned i = 1; i < 5; ++i) {
        fake_ticks += RT_TICK_PER_SECOND;
        if (i == 2) {
            /* An unrelated successful batch must not renew B's deadline. */
            lock(); lookup(remote(f), 'B')->purpose = EPD_COMPUTE_BACKGROUND; unlock();
            prepare(f, "C", true); assert(ui_reader_font_remote_ready(f, 'C', 0, true) == 1);
        }
        prepare(f, "B", true);
        assert(remote(f)->remote_memory_since == began);
        assert(!remote(f)->memory_recovery.used);
    }
    fake_ticks += RT_TICK_PER_SECOND;
    LONG before = commands, opens = remote_opens;
    for (unsigned i = 0; i < 20; ++i) {
        assert(ui_reader_font_remote_prepare(f, "B", 1, true) == EPD_APP_FONT_NO_MEMORY);
        idle(); fake_ticks += RETRY_TICKS;
    }
    assert(commands == before && remote_opens == opens && !remote(f)->error);
    assert(ui_reader_font_remote_status(f) == 1 && held.pixels[0] == 0xab);
    assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 1);
    assert(ui_reader_font_remote_ready(f, 'B', 0, false) == 1);
    epd_app_font_glyph_t absent;
    assert(ui_reader_font_remote_acquire(f, 'B', 0, true, &absent) == EPD_APP_FONT_NO_MEMORY && !absent.token);
    command_probe = NULL; ui_reader_font_remote_retry_memory(f); idle();
    assert(ui_reader_font_remote_ready(f, 'B', 0, true) == 1 && held.pixels[0] == 0xab);
    assert(!remote(f)->remote_memory_waiting && !remote(f)->remote_memory_denied);
    ui_reader_font_remote_release(&held); destroy(f);
}
static void persistent_output_memory(void) { remote_memory_pixel_failure(false); }
static void persistent_output_memory_wrap(void) { remote_memory_pixel_failure(true); }
static void remote_memory_initial_failure(bool opening)
{
    lv_font_t *warm = create(0); ready_font(warm); destroy(warm);
    if (opening) output_status = CRF_NO_MEMORY; else command_probe = persistent_memory_command;
    lv_font_t *f = create(1); idle(); uint32_t began = remote(f)->remote_memory_since;
    assert(remote(f)->remote_memory_waiting && !remote(f)->metrics_ready);
    for (unsigned i = 1; i < 5; ++i) {
        fake_ticks += RT_TICK_PER_SECOND;
        if (opening) output_status = CRF_NO_MEMORY;
        assert(ui_reader_font_remote_status(f) == 0); idle();
        assert(remote(f)->remote_memory_since == began);
        assert(!remote(f)->memory_recovery.used);
    }
    fake_ticks += RT_TICK_PER_SECOND;
    assert(ui_reader_font_remote_status(f) == EPD_APP_FONT_NO_MEMORY);
    epd_app_font_metrics_t metrics;
    assert(ui_reader_font_remote_metrics(f, &metrics) == EPD_APP_FONT_NO_MEMORY);
    LONG before = commands, opens = remote_opens;
    for (unsigned i = 0; i < 20; ++i) {
        fake_ticks += RETRY_TICKS;
        assert(ui_reader_font_remote_status(f) == EPD_APP_FONT_NO_MEMORY); idle();
    }
    assert(commands == before && remote_opens == opens && !remote(f)->error);
    command_probe = NULL; output_status = 0;
    ui_reader_font_remote_retry_memory(f); ready_font(f); destroy(f);
}
static void persistent_metrics_memory(void) { remote_memory_initial_failure(false); }
static void persistent_open_memory(void) { remote_memory_initial_failure(true); }
static void remote_memory_success_resets(void)
{
    lv_font_t *f = create(0); ready_font(f); measure_only(f, "AB");
    output_status = CRF_NO_MEMORY; prepare(f, "A", true);
    fake_ticks += RT_TICK_PER_SECOND; prepare(f, "A", true);
    assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 1 && !remote(f)->remote_memory_waiting);
    fake_ticks += 6u * RT_TICK_PER_SECOND;
    output_status = CRF_NO_MEMORY; prepare(f, "B", true);
    assert(remote(f)->remote_memory_since == rt_tick_get());
    assert(ui_reader_font_remote_ready(f, 'B', 0, true) == 0);
    fake_ticks += RT_TICK_PER_SECOND; prepare(f, "B", true);
    assert(ui_reader_font_remote_ready(f, 'B', 0, true) == 1 && !remote(f)->remote_memory_waiting);
    destroy(f);
}
static void remote_memory_cancelled_retry(void)
{
    for (unsigned expired = 0; expired < 2; ++expired) {
        lv_font_t *f = create(0); ready_font(f); prepare(f, "A", true); measure_only(f, "B");
        epd_app_font_glyph_t held; assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &held) == 1);
        command_probe = persistent_memory_command; prepare(f, "B", true);
        if (expired) {
            fake_ticks += 5u * RT_TICK_PER_SECOND;
            assert(ui_reader_font_remote_ready(f, 'B', 0, true) == EPD_APP_FONT_NO_MEMORY);
        }
        ui_reader_font_remote_cancel(f); idle(); LONG before = commands, opens = remote_opens;
        command_probe = NULL; ui_reader_font_remote_retry_memory(f);
        fake_ticks += 6u * RT_TICK_PER_SECOND; idle();
        assert(commands == before && remote_opens == opens && ui_reader_font_remote_status(f) == -1);
        assert(held.pixels[0] == 0xab); destroy(f);
        ui_reader_font_remote_release(&held); idle();
    }
}
#endif
static void command_transient_retry(void)
{
    lv_font_t *f = create(0); ready_font(f);
    for (unsigned i = 0; i < 2; ++i) {
        if (i) step_transient = EPD_COMPUTE_NO_MEMORY;
        else command_transient = EPD_COMPUTE_BUSY;
        const char *text = i ? "B" : "A";
        prepare(f, text, true);
        assert(ui_reader_font_remote_prepare(f, text, 1, true) == 0);
        assert(ui_reader_font_remote_status(f) == 1);
        InterlockedExchangeAdd(&fake_ticks, RT_TICK_PER_SECOND);
        prepare(f, text, true);
        assert(ui_reader_font_remote_prepare(f, text, 1, true) == 1);
    }
    destroy(f);
}
static void deferred_neighbor_lifetime(void)
{
    lv_font_t *f = create(0); ready_font(f); prepare(f, "A", true); prepare(f, "B", false);
    assert(ui_reader_font_remote_ref(f)); /* Neighbor shares this font shell. */
    epd_app_font_glyph_t held;
    assert(ui_reader_font_remote_acquire(f, 'A', 0, true, &held) == 1);
    available_bytes = 1;
    prepare(f, "B", true); destroy(f); /* Release only the neighbor's reference. */
    assert(held.pixels[0] == 0xab && ui_reader_font_remote_status(f) == 1);
    assert(ui_reader_font_remote_prepare(f, "B", 1, false) == 1);
    available_bytes = 64u * 1024u * 1024u;
    InterlockedExchangeAdd(&fake_ticks, RT_TICK_PER_SECOND);
    prepare(f, "B", true); assert(ui_reader_font_remote_ready(f, 'B', 0, true) == 1);
    prepare(f, "C", false); available_bytes = 1;
    prepare(f, "C", true); ui_reader_font_remote_cancel(f); idle();
    assert(ui_reader_font_remote_prepare(f, "C", 1, false) == -1);
    assert(held.pixels[0] == 0xab);
    available_bytes = 64u * 1024u * 1024u;
    ui_reader_font_remote_release(&held); destroy(f);
}
#include "priority_cases.h"
#include "eager_cases.h"
#include "preview_cases.h"
/* Exercise the registered production shrink implementation in bounded steps. */
static size_t provider_reclaim_target(size_t target)
{
    size_t total = 0;
    while (epd_app_psram_available() < target) {
        size_t released = 0;
        remote_resource_shrink(NULL, 1, target - epd_app_psram_available(), &released);
        total += released;
        if (!released) break;
    }
    return total;
}
#include "reclaim_cases.h"
#include "source_cache_cases.h"


#ifndef REAL_FONT_PIPE
static void operation_timing_worker(void)
{
    operation_timing_test=true;
    for(unsigned fault=0;fault<9;++fault) {
        /* Retained SD descriptors exercise failed close in the FD cases. */
        if (fault == 6) continue;
        remote_font_t font; source_fixture(&font,1); font.primary->key.volume=STORAGE_FLASH;
        face_t *face=font.primary; face->key.volume=STORAGE_FLASH;
        uint8_t data[4096]; uint32_t actual;
        epd_app_font_io_stats_t io={0};
        source_fault=fault<7?fault:0;
        fixture_mounted=fault!=7;
        if(fault==8) InterlockedExchange(&fake_ticks,(LONG)(UINT32_MAX-20u));
        LONG logs=io_logs;
        crf_status rc=source_read(face,0,sizeof(data),data,&io,&actual);
        assert(io_logs==logs && !io_depth && !open_fds);
        assert(rc==(fault==7?CRF_STALE_SOURCE:fault==5?CRF_SHORT_READ:
            (fault>0 && fault<7)?CRF_IO_ERROR:CRF_OK));
        unsigned calls[4]={fault!=7,fault!=7 && fault!=1,
            fault!=7 && fault!=1 && fault!=2,fault!=7 && fault!=1};
        unsigned ticks[4]={7,11,13,17};
        unsigned total=0;
        for(unsigned i=0;i<SOURCE_IO_COUNT;++i) {
            assert(face->file_io[i].calls==calls[i]);
            assert(face->file_io[i].ticks==calls[i]*ticks[i]);
            assert(face->file_io[i].max_ticks==calls[i]*ticks[i]);
            total+=calls[i]*ticks[i];
        }
        assert(io.lock_hold.ticks==total+io.read.ticks);
        assert(io.lock_wait.ticks==3);
        fixture_mounted=true; source_fault=0; source_fixture_release(&font);
    }
    remote_font_t font; source_fixture(&font,1); font.primary->key.volume=STORAGE_FLASH;
    LONG logs=io_logs;
    assert(source_request(&font,0,1024)==CRF_OK);
    assert(source_request(&font,1024,1024)==CRF_OK);
    assert(io_logs==logs);
    for(unsigned i=0;i<SOURCE_IO_COUNT;++i) assert(font.primary->file_io[i].calls==1);
    io_report(font.primary);
    assert(io_logs==logs+9);
    source_fixture_release(&font);
}
static void operation_timing(void) { source_case_run(operation_timing_worker); }
#endif

#include "fd_cases.h"
#ifndef REAL_FONT_PIPE
static void retire_io_error_report(void)
{
    source_fault = 5;
    lv_font_t *font = create(1); idle();
    assert(ui_reader_font_remote_status(font) < 0);
    epd_app_font_remote_profile_t profile;
    quiet_profile(&profile);
    assert(profile.io.errors);
    source_fault = 0;
    destroy(font);
    assert(io_logs >= 9 && source_cache_logs >= 1);
}
#endif
#ifndef READER_FONT_REAL_CACHE
static void shared_caller(void)
{
    for (unsigned caller = 0x100; caller <= 0x200; caller += 0x100) {
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(caller);
        for (unsigned id = 0; id < 2; ++id) {
            lv_font_t *font = create(id); ready_font(font); prepare(font, "AB", true);
            assert(epd_memory_owner_current() == caller);
            assert(remote_resource.owner == EPD_MEMORY_OWNER_SYSTEM);
            void *probe = epd_app_alloc(8, EPD_APP_PSRAM); assert(probe);
            assert(epd_resource_cache_adopt(&remote_resource, probe) == EPD_RESOURCE_INVALID);
            epd_app_free(probe);
            epd_app_font_glyph_t held = {0};
            assert(ui_reader_font_remote_acquire(font, 'A', 0, true, &held) == 1);
            assert(held.pixels);
            ui_reader_font_remote_release(&held); destroy(font); idle();
            assert(epd_memory_owner_current() == caller && !allocations && !leases);
        }
        uint32_t identity = 1;
        InterlockedExchange(&fail_alloc, 1);
        assert(!ui_reader_font_remote_create(0, 24, 0, &identity) && !identity);
        assert(epd_memory_owner_current() == caller && !allocations);
        epd_memory_owner_leave_trusted(previous);
    }
}
#endif
static const test_t tests[] = {
#ifndef READER_FONT_REAL_CACHE
    {"shared_caller", shared_caller},
#endif
    {"batch_codepoint_order", batch_codepoint_order},
#ifndef REAL_FONT_PIPE
    {"remote_profile", remote_profile},
    {"remote_profile_inflight", remote_profile_inflight},
    {"quiet_retire_diagnostic", quiet_retire_diagnostic},
    {"retire_batch_error_report", retire_batch_error_report},
    {"retire_io_error_report", retire_io_error_report},
    {"persistent_fd_counts", persistent_fd_counts},
    {"persistent_fd_close_failure", persistent_fd_close_failure},
    {"persistent_fd_release_inflight", persistent_fd_release_inflight},
    {"persistent_fd_release_retry", persistent_fd_release_retry},
    {"persistent_fd_sleep_handoff", persistent_fd_sleep_handoff},
    {"persistent_fd_closing_visible", persistent_fd_closing_visible},
    {"persistent_fd_sleep_batch", persistent_fd_sleep_batch},
    {"persistent_fd_batch_commit", persistent_fd_batch_commit},
    {"persistent_fd_sleep_failed_close", persistent_fd_sleep_failed_close},
    {"operation_timing", operation_timing},
    {"source_cache_reads", source_cache_reads},
    {"source_cache_bounds", source_cache_bounds},
    {"source_cache_metadata", source_cache_metadata},
    {"source_cache_metadata_sources", source_cache_metadata_sources},
    {"source_cache_pressure", source_cache_pressure},
    {"source_cache_invalidations", source_cache_invalidations},
    {"source_cache_failures", source_cache_failures},
    {"source_cache_inflight", source_cache_inflight},
    {"prepare_queues_pending", prepare_queues_pending},
    {"preview_isolation", preview_isolation},
    {"preview_bounds", preview_bounds},
#if UI_READER_FONT_EAGER
    {"preview_priority", preview_priority},
    {"preview_fairness", preview_fairness},
    {"preview_inflight_measure", preview_inflight_measure},
    {"preview_host_oom", preview_host_oom},
    {"preview_remote_oom", preview_remote_oom},
    {"preview_errors_cancel", preview_errors_cancel},
    {"eager_foreground", eager_foreground},
    {"eager_background_fairness", eager_background_fairness},
    {"eager_bounds", eager_bounds},
    {"eager_host_oom", eager_host_oom},
    {"eager_remote_oom", eager_remote_oom},
    {"eager_partial_oom", eager_partial_oom},
    {"eager_demand_during_oom", eager_demand_during_oom},
    {"eager_errors_cancel", eager_errors_cancel},
#endif
    {"glyph_foreground_priority", glyph_foreground_priority},
    {"glyph_background_fairness", glyph_background_fairness},
    {"glyph_inflight_promotion", glyph_inflight_promotion},
    {"glyph_precommand_promotion", glyph_precommand_promotion},
    {"glyph_reopen_promotion", glyph_reopen_promotion},
    {"glyph_initial_priority", glyph_initial_priority},
    {"batch_summaries", batch_summaries},
#endif
    {"reclaim_roundtrip", reclaim_roundtrip},
#ifndef REAL_FONT_PIPE
    {"reclaim_inert", reclaim_inert},
    {"reclaim_frame_pressure", reclaim_frame_pressure},
    {"reclaim_protected_cancel", reclaim_protected_cancel},
#endif
    {"output_memory_retry", output_memory_retry},
#ifndef REAL_FONT_PIPE
    {"persistent_output_memory", persistent_output_memory},
    {"persistent_output_memory_wrap", persistent_output_memory_wrap},
    {"persistent_metrics_memory", persistent_metrics_memory},
    {"persistent_open_memory", persistent_open_memory},
    {"remote_memory_success_resets", remote_memory_success_resets},
    {"remote_memory_cancelled_retry", remote_memory_cancelled_retry},
#endif
    {"command_transient_retry", command_transient_retry},
    {"deferred_neighbor_lifetime", deferred_neighbor_lifetime},
    {"postcheck_overhead_reclaim", postcheck_overhead_reclaim},
    {"postcheck_race_retry", postcheck_race_retry},
    {"source_transient_retry", source_transient_retry},
    {"empty_measure_requires_render", empty_measure_requires_render},
    {"empty_bitmap_callbacks", empty_bitmap_callbacks},
    {"pending_normal_glyph", pending_normal_glyph},
    {"pin_admission_retry", pin_admission_retry},
    {"long_text_empty_glyph", long_text_empty_glyph},
    {"record_batches_are_bounded", record_batches_are_bounded},
    {"open_failure_retains_handle", open_failure_retains_handle},
    {"cached_prepare_one_lease_check", cached_prepare_one_lease_check},
    {"worker_reserve_rollback", worker_reserve_rollback},
    {"remote_diagnostics", remote_diagnostics}, {"diagnostics_pressure", diagnostics_pressure},
    {"async_metrics", async_metrics}, {"batch_cache", batch_cache},
    {"missing_only_fallback", missing_only_fallback}, {"revoke_generation", revoke_generation},
    {"cancel_during_io", cancel_during_io}, {"page_tokens_and_draws", page_tokens_and_draws},
    {"oblique_lifetime_priority", oblique_lifetime_priority}, {"pixel_lru", pixel_lru},
    {"descriptor_lru_stale", descriptor_lru_stale}, {"invalid_and_failure", invalid_and_failure},
    {"pinned_budget", pinned_budget}, {"worker_priority", worker_priority},
    {"source_sharing", source_sharing}, {"builtin_source_sharing", builtin_source_sharing},
    {"source_capacity_fallback", source_capacity_fallback},
    {"cancel_shared_active", cancel_shared_active}, {"shared_required", shared_required},
    {"worker_snapshots", worker_snapshots}, {"source_generation_isolation", source_generation_isolation},
    {"cancel_shared_inactive", cancel_shared_inactive}, {"paged_pin_pressure", paged_pin_pressure},
    {"shutdown_retained", shutdown_retained}, {"shutdown_capacity", shutdown_capacity},
    {"platform_creation_reserve", platform_creation_reserve}, {"platform_source_reserve", platform_source_reserve},
    {"platform_pixel_reclaim", platform_pixel_reclaim}, {"platform_pinned_failure", platform_pinned_failure},
    {"platform_source_denied_retry", platform_source_denied_retry}
};

#ifdef REAL_FONT_PIPE
static bool body_trace;
static unsigned hold_eager_operation;
static unsigned engine_errors, measured_records, rendered_records;
typedef struct { epd_compute_pipe_service_t callback; void *user; } observation_t;
static int observed_service(void *user, epd_compute_pipe_message_t *m, uint8_t *data)
{
    observation_t *o = user;
    batch_t *batch = o->user;
#ifdef READER_FONT_SERVICE_PROBE
    READER_FONT_SERVICE_PROBE(o->user, m, data);
#endif
    if (batch->eager && hold_eager_operation == m->operation) {
        hold_eager_operation = 0; SetEvent(io_entered);
        assert(WaitForSingleObject(io_continue, 5000) == WAIT_OBJECT_0);
    }
    if (body_trace && m->operation == EPD_PIPE_OUTPUT && m->length >= sizeof(epd_font_output_t)) {
        epd_font_output_t h; memcpy(&h, data, sizeof(h));
        if (batch->render) rendered_records += h.completed; else measured_records += h.completed;
        if (h.status != CRF_OK) {
            unsigned index = h.first + h.completed;
            uint32_t cp = index < batch->count ? batch->glyphs[index]->letter : 0;
            ++engine_errors;
            printf("FAIL engine stage=%s codepoint=U+%04X status=%u(%s) request=%u first=%u completed=%u total=%u required_bytes=%u\n",
                   batch->render ? "render" : "measure", cp, h.status,
                   crf_status_string((crf_status)h.status), h.request_id, h.first, h.completed, h.total, h.required_bytes);
        }
        if (h.completed <= EPD_FONT_BATCH_MAX && sizeof(h) + h.completed * sizeof(crf_glyph) <= m->length) {
            for (unsigned i = 0; i < h.completed; ++i) {
                crf_glyph g; memcpy(&g, data + sizeof(h) + i * sizeof(g), sizeof(g));
                if (g.flags & CRF_GLYPH_MISSING) {
                    ++engine_errors;
                    printf("FAIL engine codepoint=U+%04X status=%u(%s) glyph_flags=%u(MISSING)\n",
                           g.codepoint, h.status, crf_status_string((crf_status)h.status), g.flags);
                }
            }
        }
    }
    int status = o->callback(o->user, m, data);
    if (body_trace && status != EPD_COMPUTE_OK) {
        ++engine_errors;
        printf("FAIL service operation=%u status=%d\n", m->operation, status);
    }
    return status;
}
static int observed_pipe_step(epd_compute_pipe_host_t pipe, epd_compute_pipe_service_t callback, void *user)
{
    observation_t o = {callback, user};
    int status = epd_compute_pipe_step(pipe, observed_service, &o);
    if (body_trace && status != EPD_COMPUTE_OK && status != EPD_COMPUTE_MORE) {
        ++engine_errors;
        printf("FAIL pipe step status=%d\n", status);
    }
    return status;
}
static void body_stats(const char *stage)
{
    epd_app_font_remote_stats_t s; ui_reader_font_remote_stats(&s);
    printf("STATS stage=%s prepare_failures=%llu font_io_errors=%llu oom_budget=%llu oom_reserve=%llu oom_alloc=%llu oom_postcheck=%llu ftz_reads=%llu ftz_bytes=%llu pixels=%u descriptors=%u\n",
           stage, (unsigned long long)s.prepare_failures, (unsigned long long)s.io.errors,
           (unsigned long long)s.oom_budget, (unsigned long long)s.oom_reserve,
           (unsigned long long)s.oom_alloc, (unsigned long long)s.oom_postcheck,
           (unsigned long long)s.io.ftz_copy.calls, (unsigned long long)s.io.ftz_copy.bytes,
           s.pixel_bytes, s.descriptor_bytes);
}
static void body_failure(lv_font_t *font, const char *stage, size_t offset, uint32_t cp,
                         int ready, int acquired, const char *reason)
{
    lock(); remote_font_t *f = remote(font); glyph_t *g = lookup(f, cp);
    printf("FAIL glyph stage=%s byte=%zu codepoint=U+%04X glyph_ready=%d acquire=%d reason=%s font_error=%d glyph_error=%d descriptor=%d pixels=%d wanted=%d busy=%d\n",
           stage, offset, cp, ready, acquired, reason, f->error, g ? g->error : -1,
           g ? g->descriptor : -1, g ? g->pixels : -1, g ? g->wanted : -1, g ? g->busy : -1);
    unlock();
}
static int real_body(const char *path, unsigned px, const char *style, const char *csv_path, unsigned render_chars)
{
    FILE *file = fopen(path, "rb"); assert(file);
    char *text = malloc(TEXT_LIMIT + 1u); assert(text);
    size_t length = fread(text, 1, TEXT_LIMIT + 1u, file); assert(!ferror(file)); fclose(file);
    assert(length && length <= TEXT_LIMIT);
    uint32_t *letters = calloc(length + 1u, sizeof(*letters));
    uint32_t *widths = calloc(length, sizeof(*widths));
    size_t *offsets = calloc(length, sizeof(*offsets)); assert(letters && widths && offsets);
    size_t count = 0, at = 0;
    while (at < length) {
        offsets[count] = at;
        if (!utf8_next(text, length, &at, &letters[count])) {
            printf("FAIL fixture invalid UTF-8 byte=%zu\n", offsets[count]); return 2;
        }
        widths[count++] = UINT32_MAX;
    }
    FILE *csv = fopen(csv_path, "w"); assert(csv);
    fputs("stage,index,byte,codepoint,next_codepoint,ready,acquire,advance,box_w,box_h,stride,ofs_x,ofs_y,nonzero_pixels,pixel_hash\n", csv);
    body_trace = true;
    uint32_t identity;
    lv_font_t *base = ui_reader_font_remote_create(0, px, !strcmp(style, "bold") ? CRF_WEIGHT_BOLD : CRF_WEIGHT_NORMAL, &identity);
    if (!base) { printf("FAIL font create px=%u style=%s status=-1\n", px, style); return 1; }
    lv_font_t *font = !strcmp(style, "italic") ? ui_reader_font_remote_oblique(base) : base;
    if (!font) { puts("FAIL oblique create status=-1"); destroy(base); return 1; }
    idle();
    int status = ui_reader_font_remote_status(font);
    printf("BODY px=%u style=%s bytes=%zu codepoints=%zu font_status=%d source=FTZ1\n", px, style, length, count, status);
    unsigned failures = status != 1;
    for (unsigned phase = 0; phase < 2; ++phase) {
        const char *stage = phase ? "render" : "measure";
        size_t stage_count = phase && render_chars && render_chars < count ? render_chars : count;
        size_t stage_length = stage_count < count ? offsets[stage_count] : length;
        ULONGLONG begin = GetTickCount64();
        int first = ui_reader_font_remote_prepare(font, text, stage_length, phase != 0);
        body_stats(phase ? "render_submitted" : "first_measure_submitted");
        idle();
        body_stats(phase ? "render_completed" : "first_measure_completed");
        int final = ui_reader_font_remote_prepare(font, text, stage_length, phase != 0);
        idle();
        unsigned stage_failures = first < 0 || final != 1, acquired_count = 0;
        uint64_t total_width = 0, nonzero_total = 0;
        for (size_t i = 0; i < stage_count; ++i) {
            uint32_t cp = letters[i], next = letters[i + 1];
            int ready = ui_reader_font_remote_ready(font, cp, next, phase != 0);
            epd_app_font_glyph_t g;
            int acquired = ui_reader_font_remote_acquire(font, cp, next, phase != 0, &g);
            const char *reason = NULL;
            uint64_t nonzero = 0;
            uint32_t pixel_hash = 0;
            if (ready != 1 || acquired != 1) reason = "not_ready";
            if (acquired == 1) {
                ++acquired_count; total_width += g.adv_w;
                if (!g.token) reason = "no_token";
                if (!phase) widths[i] = g.adv_w;
                else {
                    if (widths[i] != UINT32_MAX && widths[i] != g.adv_w) reason = "advance_changed";
                    if (g.box_w && g.box_h && (!g.pixels || g.stride < g.box_w)) reason = "invalid_bitmap";
                    if (g.pixels) {
                        pixel_hash = 2166136261u;
                        for (size_t p = 0; p < (size_t)g.stride * g.box_h; ++p) {
                            nonzero += g.pixels[p] != 0;
                            pixel_hash = (pixel_hash ^ g.pixels[p]) * 16777619u;
                        }
                    }
                    /* Exercise the production LVGL draw callbacks as well as borrowing A8 pixels. */
                    lv_font_glyph_dsc_t d = descriptor(font, cp);
                    const lv_draw_buf_t *draw = font->get_glyph_bitmap(&d, NULL);
                    if (g.box_w && g.box_h) {
                        if (!draw || draw->data != g.pixels || draw->header.stride != g.stride) reason = "draw_mismatch";
                    }
                    else if (draw) reason = "empty_draw_buffer";
                    font->release_glyph(font, &d);
                }
            }
            nonzero_total += nonzero;
            fprintf(csv, "%s,%zu,%zu,U+%04X,U+%04X,%d,%d,%u,%u,%u,%u,%d,%d,%llu,%08x\n",
                    stage, i, offsets[i], cp, next, ready, acquired, g.adv_w, g.box_w, g.box_h,
                    g.stride, g.ofs_x, g.ofs_y, (unsigned long long)nonzero, pixel_hash);
            if (reason) { ++stage_failures; body_failure(font, stage, offsets[i], cp, ready, acquired, reason); }
            if (acquired == 1) ui_reader_font_remote_release(&g);
        }
        printf("%s stage=%s px=%u style=%s prepare_initial=%d prepare_final=%d acquired=%u/%zu width_sum=%llu nonzero_pixels=%llu failures=%u elapsed_ms=%llu\n",
               stage_failures ? "FAIL" : "PASS", stage, px, style, first, final, acquired_count, stage_count,
               (unsigned long long)total_width, (unsigned long long)nonzero_total, stage_failures,
               (unsigned long long)(GetTickCount64() - begin));
        failures += stage_failures;
    }
    fclose(csv);
    body_stats("before_destroy");
    if (font != base) destroy(font);
    destroy(base); real_no_buffers();
    assert(!fonts && !allocations && !descriptor_bytes && !pixel_bytes && !sources && !resident_sources && !leases);
    printf("ENGINE measured_records=%u rendered_records=%u errors=%u memory_peak=%zu ftz_inflates=%llu cleanup=clean\n",
           measured_records, rendered_records, engine_errors, epd_reader_font_last_session.engine.memory_peak,
           (unsigned long long)epd_reader_font_last_session.ftz.inflate_calls);
    failures += engine_errors;
    printf("%s real_body px=%u style=%s failures=%u\n", failures ? "FAIL" : "PASS", px, style, failures);
    free(offsets); free(widths); free(letters); free(text);
    return failures ? 1 : 0;
}
#endif
#include "eager_real.h"
#include "preview_real.h"
static void abort_without_dialog(int value) { (void)value; ExitProcess(3); }
int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    signal(SIGABRT, abort_without_dialog);
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    assert(argc >= 2);
    if (!strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(tests) / sizeof(*tests); ++i) puts(tests[i].name);
        return 0;
    }
    ui_thread = GetCurrentThreadId(); InitializeConditionVariable(&transition); InitializeCriticalSection(&interrupt_gate);
    io_entered = CreateEventA(NULL, TRUE, FALSE, NULL); io_continue = CreateEventA(NULL, TRUE, FALSE, NULL);
#ifdef REAL_FONT_PIPE
    assert(argc == 3 || argc == 5 || argc == 7 || argc == 8);
    if (argc >= 7) return real_body(argv[3], (unsigned)strtoul(argv[4], NULL, 10), argv[5], argv[6],
                                  argc == 8 ? (unsigned)strtoul(argv[7], NULL, 10) : 0);
    FILE *file = fopen(argv[2], "rb"); assert(file);
    real_font_bytes = malloc(ui_builtin_font_size); assert(real_font_bytes);
    assert(fread(real_font_bytes, 1, ui_builtin_font_size, file) == ui_builtin_font_size); fclose(file);
    if (!strcmp(argv[1], "real_preview")) {
        assert(argc == 5); real_preview((unsigned)strtoul(argv[3], NULL, 10), argv[4]);
        free(real_font_bytes); return 0;
    }
#if UI_READER_FONT_EAGER
    real_eager_faults();
#endif
    reclaim_roundtrip();
    lv_font_t *font = create(0), *italic = ui_reader_font_remote_oblique(font); assert(italic);
    uint32_t identity;
    lv_font_t *large = ui_reader_font_remote_create(0, 36, 2, &identity); assert(large);
    ready_font(font); ready_font(italic); ready_font(large);
    assert(remote(font)->primary == remote(italic)->primary && resident_sources == 1);
    assert(remote(large)->primary == remote(font)->primary && large->line_height > font->line_height);
    const char text[] = "ASCII \xc2\xa0\xe4\xb8\xad\xe6\x96\x87";
    prepare(font, text, false); prepare(italic, text, false);
    assert(ui_reader_font_remote_prepare(font, text, strlen(text), false) == 1);
    prepare(font, text, true); prepare(italic, text, true); prepare(large, text, true);
    assert(ui_reader_font_remote_ready(font, 0x4e2d, 0, true) == 1);
    assert(ui_reader_font_remote_ready(italic, 0x4e2d, 0, true) == 1);
    lv_font_glyph_dsc_t a = descriptor(font, 0x4e2d), b = descriptor(italic, 0x4e2d);
    assert(a.box_w && a.box_h && b.box_w == a.box_w + (a.box_h - 1) / 4);
    assert(ui_reader_font_remote_ready(large, 0x4e2d, 0, true) == 1);
    assert(descriptor(large, 0x4e2d).box_h > a.box_h);
    const lv_draw_buf_t *normal = font->get_glyph_bitmap(&a, NULL), *slant = italic->get_glyph_bitmap(&b, NULL);
    assert(normal && slant);
    for (unsigned y = 0; y < a.box_h; ++y) {
        unsigned shift = (a.box_h - y - 1) / 4;
        assert(!memcmp(normal->data + y * normal->header.stride,
                       slant->data + y * slant->header.stride + shift, a.box_w));
    }
    font->release_glyph(font, &a); italic->release_glyph(italic, &b);
    destroy(font); destroy(italic); destroy(large);
    assert(!fonts && !allocations && !descriptor_bytes && !pixel_bytes && !sources && !resident_sources);
    /* Compare the actual FTZ provider path with raw external TTF byte for byte. */
    lv_font_t *packed = create(0), *raw = create(1); ready_font(packed); ready_font(raw);
    prepare(packed, text, true); prepare(raw, text, true);
    const uint32_t chars[] = {'A', 'S', 'C', 'I', ' ', 0xa0, 0x4e2d, 0x6587};
    for (unsigned i = 0; i < sizeof(chars) / sizeof(*chars); ++i) {
        epd_app_font_glyph_t x, y;
        assert(ui_reader_font_remote_acquire(packed, chars[i], 0, true, &x) == 1);
        assert(ui_reader_font_remote_acquire(raw, chars[i], 0, true, &y) == 1);
        assert(x.adv_w == y.adv_w && x.box_w == y.box_w && x.box_h == y.box_h &&
               x.stride == y.stride && x.ofs_x == y.ofs_x && x.ofs_y == y.ofs_y);
        assert(!memcmp(x.pixels, y.pixels, x.stride * x.box_h));
        if (chars[i] == 0xa0) {
            assert(x.adv_w > 0);
            for (unsigned p = 0; p < x.stride * x.box_h; ++p) assert(!x.pixels[p]);
            printf("MiSans NBSP: advance=%u box=%ux%u stride=%u, FTZ/TTF pixels identical and blank\n",
                   x.adv_w, x.box_w, x.box_h, x.stride);
        }
        ui_reader_font_remote_release(&x); ui_reader_font_remote_release(&y);
    }
    LONG reads_before = raw_reads, allocations_before = allocations;
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        assert(ui_reader_font_remote_prepare(raw, text, strlen(text), true) == 1);
        lv_font_glyph_dsc_t d = descriptor(raw, 0x4e2d);
        assert(raw->get_glyph_bitmap(&d, NULL)); raw->release_glyph(raw, &d);
    }
    assert(raw_reads == reads_before && allocations == allocations_before);
    epd_app_font_remote_stats_t io_stats; ui_reader_font_remote_stats(&io_stats);
    printf("Raw source supply: calls=%llu read_syscalls=%llu bytes=%llu\n",
        (unsigned long long)(io_stats.io.supply_calls - io_stats.io.ftz_copy.calls),
        (unsigned long long)io_stats.io.read.calls, (unsigned long long)io_stats.io.read.bytes);
    destroy(packed); destroy(raw);
    packed = create(0); ready_font(packed); prepare(packed, text, true);
    uint32_t summary_sequence = epd_reader_font_last_session.sequence;
    real_close_fault(true); destroy(packed);
    assert(epd_reader_font_last_session.sequence == summary_sequence);
    assert(!fonts && sources && sources->closing && resident_sources == 1);
    real_close_fault(false); raw = create(1); ready_font(raw); destroy(raw);
    real_no_buffers();
    assert(epd_reader_font_last_session.sequence == summary_sequence + 4 && epd_reader_font_last_session.font.source_id == 1);
    assert(epd_reader_font_last_session.engine.read_calls && epd_reader_font_last_session.engine.memory_peak);
    assert(!epd_reader_font_last_session.ftz_workspace && !epd_reader_font_last_session.ftz.inflate_calls);
    assert(!fonts && !allocations && !descriptor_bytes && !pixel_bytes && !sources && !resident_sources);
    real_open_fault(true); real_close_fault(true);
    raw = create(1); idle(); assert(ui_reader_font_remote_status(raw) == -1);
    destroy(raw); assert(sources && sources->closing && resident_sources == 1);
    real_open_fault(false); real_close_fault(false);
    raw = create(1); ready_font(raw); destroy(raw); real_no_buffers();
    assert(!allocations && !sources && !resident_sources && !descriptor_bytes);
    free(real_font_bytes); puts("PASS real HCPU cache + host pipe + remote pipe + ACPU engine (MiSans/oblique)");
    return 0;
#else
    for (unsigned i = 0; i < sizeof(tests) / sizeof(*tests); ++i) if (!strcmp(argv[1], tests[i].name)) {
        tests[i].run(); idle();
        assert(!fonts && !font_count && !pixel_bytes && !descriptor_bytes && !allocations && !leases && !pipe_count);
        assert(!sources && !resident_sources);
        printf("PASS %s\n", tests[i].name); return 0;
    }
    return 1;
#endif
}
