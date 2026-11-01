/* SPDX-License-Identifier: Apache-2.0 */
#include "ui_reader_font_remote.h"
#include "reading_font.h"
#include "storage_file.h"
#include "storage_internal.h"
#include "ui_reader_font_source_fd.h"
#include "sleep_guard.h"
#include "platform/epd_power.h"
#include "epd_memory.h"
#include "epd_compute_pipe.h"
#include "../../components/compute_reader_font/include/font_wire.h"
#include <rthw.h>
#include <rtm.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>
#ifdef SF32LB57X
#include <board.h>
#endif

#define PIXEL_BUDGET (2u * 1024u * 1024u)
#define DESC_BUDGET (512u * 1024u)
#define SHARED_RESERVE (1024u * 1024u)
/* Heap block header plus alignment; the postcheck also covers other users. */
#define ALLOCATION_HEADROOM 64u
#define RETRY_TICKS ((RT_TICK_PER_SECOND + 9u) / 10u)
#define WORKER_STACK_BYTES 8192u
/* Page preparation waits on this worker; keep it ahead of document I/O. */
#define WORKER_PRIORITY 20u
/* Two aligned common-pipe buffers, with heap header/alignment headroom. */
#define SOURCE_PIPE_BYTES (EPD_COMPUTE_PIPE_BYTES + \
    ((sizeof(epd_compute_pipe_message_t) + 31u) & ~(size_t)31u) + 128u)
/* Three live layouts plus one replacement; retired shells also consume slots.
 * UI admission defers another full layout until the worker releases room. */
#define FONT_LIMIT (4u * 3u * 2u)
#define TEXT_LIMIT (64u * 1024u)
#define HASH_SIZE 257u
#define SOURCE_SESSION_LIMIT 2u
#define SOURCE_CACHE_BLOCK_BYTES 4096u
#define SOURCE_CACHE_LIMIT (256u * 1024u)
#define SOURCE_CACHE_TOTAL_LIMIT (SOURCE_SESSION_LIMIT * SOURCE_CACHE_LIMIT)
#define BACKGROUND_BATCH_MAX 16u
#ifndef UI_READER_FONT_EAGER
#define UI_READER_FONT_EAGER 1
#endif
#define EAGER_TEXT_MAX 1024u
enum { EAGER_NONE, EAGER_RENDER, EAGER_MEASURE };
/* Requested private allocation bytes, including the remote's stack/control
 * reserve. The target heap's block headers/alignment need separate headroom. */
#define SOURCE_MEMORY_BYTES (704u * 1024u)

extern const unsigned char ui_builtin_font_pack[];
extern const int ui_builtin_font_pack_size;
extern const uint32_t ui_builtin_font_size, ui_builtin_font_signature;

typedef struct remote_font remote_font_t;
typedef struct glyph glyph_t;
typedef epd_app_font_batch_profile_t font_batch_stats_t;
typedef struct source_block {
    struct source_block *next;
    uint32_t offset, length;
    bool busy, discard, metadata;
    uint8_t data[SOURCE_CACHE_BLOCK_BYTES];
} source_block_t;
/* Include node storage and allocator headroom in both source-cache limits. */
#define SOURCE_BLOCK_CHARGE (sizeof(source_block_t) + ALLOCATION_HEADROOM)
#define SOURCE_METADATA_MAX (SOURCE_CACHE_LIMIT / SOURCE_BLOCK_CHARGE - 8u)
typedef struct {
    uint64_t hits, misses, bypasses;
    uint64_t metadata_hits, metadata_misses;
    size_t bytes, peak;
} source_cache_stats_t;
enum { SOURCE_IO_OPEN, SOURCE_IO_FSTAT, SOURCE_IO_SEEK, SOURCE_IO_CLOSE, SOURCE_IO_COUNT };
typedef struct face {
    struct face *next;
    reading_font_lease_t key;
    crf_font_identity identity;
    epd_compute_pipe_host_t pipe;
    epd_compute_pipe_host_t closing;
    remote_font_t *owner;
    uint64_t last_use;
    uint32_t handle, references;
    unsigned purpose;
    bool opened;
    epd_app_font_io_stats_t io;
    epd_app_font_io_timing_t file_io[SOURCE_IO_COUNT];
    source_block_t *blocks;
    source_cache_stats_t cache;
    source_fd fd_state;
    uint32_t checked_path_volume;
    bool path_current;
} face_t;
struct glyph {
    glyph_t *hash_next, *ticket_next, *older, *newer;
    remote_font_t *font;
    uint32_t letter, next, ticket, pins, draws;
    uint8_t purpose, bypasses, eager;
    /* pixels records completed rendering, including a result with no bitmap. */
    bool descriptor, pixels, wanted, busy, render, fallback, error;
    lv_font_glyph_dsc_t dsc;
    lv_draw_buf_t bitmap;
    void *allocation;
    size_t charge;
};
struct remote_font {
    lv_font_t font;
    remote_font_t *next, *work_next, *parent;
    reading_font_lease_t lease;
    face_t *primary, *fallback;
    crf_style style;
    crf_metrics metrics;
    uint32_t id, generation, references, draws, request;
    rt_tick_t pixel_wait_since, retry_since;
    unsigned purpose, bypasses;
    font_batch_stats_t batches[2];
    bool leased, error, metrics_ready, published, queued, working, retired;
    bool pixel_waiting, pixel_wait_reported, deferred, sleep_parked;
};
typedef struct page_pin { struct page_pin *next; glyph_t *glyph; } page_pin_t;
struct epd_app_font_page { page_pin_t *pins; };

static struct rt_mutex cache_lock;
static struct rt_semaphore work_event;
static bool initialized, notified;
static remote_font_t *fonts, *work_head, *work_tail;
static glyph_t *buckets[HASH_SIZE], *tickets[HASH_SIZE], *oldest, *newest;
static size_t descriptor_bytes, pixel_bytes;
static unsigned font_count;
static uint32_t generation, glyph_ticket;
static face_t *sources;
static unsigned resident_sources;
static uint32_t source_generation;
static uint64_t source_clock;
static epd_app_font_remote_stats_t diagnostics;
static font_batch_stats_t batch_totals[2];
static source_cache_stats_t source_cache_totals;
static epd_app_font_io_timing_t file_io_totals[SOURCE_IO_COUNT];
static epd_app_font_active_profile_t active_profile;
static epd_app_font_step_profile_t step_profile_totals;
static epd_app_font_drive_profile_t drive_profile_totals;
static rt_tick_t active_started;
static void fd_state_lock(void *), fd_state_unlock(void *);
static void fd_storage_lock(void *), fd_storage_unlock(void *);
static bool fd_valid(void *, const source_fd_key *);
static int fd_open(void *, const char *);
static bool fd_stat(void *, int, uint32_t), fd_seek(void *, int, uint32_t);
static int fd_read(void *, int, void *, uint32_t), fd_close(void *, int);
static void fd_wake(void *);
static const source_fd_ops source_fd_callbacks = {
    fd_state_lock, fd_state_unlock, fd_storage_lock, fd_storage_unlock,
    fd_valid, fd_open, fd_stat, fd_seek, fd_read, fd_close, fd_wake
};
static source_fd_pool fd_pool = {&source_fd_callbacks, NULL, 0, SOURCE_SESSION_LIMIT};
static bool fd_managed(const face_t *face)
{ return face->identity.source_id && face->key.volume == STORAGE_SD; }
static bool fd_released(face_t *face)
{ return !fd_managed(face) || source_fd_released(&face->fd_state); }


static bool descriptor_cb(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
static const void *bitmap_cb(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
static void release_cb(const lv_font_t *, lv_font_glyph_dsc_t *);
static void worker_entry(void *unused);

static void lock(void) { rt_mutex_take(&cache_lock, RT_WAITING_FOREVER); }
static void unlock(void) { rt_mutex_release(&cache_lock); }

/* cache_lock owns the lists. Only the font worker fills blocks; a busy block
 * remains pinned across storage I/O, even when another thread revokes it. */
static void source_block_drop(face_t *face, source_block_t *block)
{
    source_block_t **p = &face->blocks;
    while (*p != block) p = &(*p)->next;
    *p = block->next;
    face->cache.bytes -= SOURCE_BLOCK_CHARGE;
    source_cache_totals.bytes -= SOURCE_BLOCK_CHARGE;
    epd_app_free(block);
}
static void source_cache_clear(face_t *face)
{
    if (!face) return;
    for (source_block_t *b = face->blocks, *next; b; b = next) {
        next = b->next;
        if (b->busy) b->discard = true;
        else source_block_drop(face, b);
    }
}
static size_t source_cache_evict(void)
{
    for (face_t *face = sources; face; face = face->next) {
        source_block_t *victim = NULL;
        for (source_block_t *b = face->blocks; b; b = b->next)
            if (!b->busy) victim = b;
        if (victim) { source_block_drop(face, victim); return SOURCE_BLOCK_CHARGE; }
    }
    return 0;
}
static void source_block_touch(face_t *face, source_block_t *block)
{
    source_block_t **p = &face->blocks;
    while (*p != block) p = &(*p)->next;
    *p = block->next; block->next = face->blocks; face->blocks = block;
}
bool ui_reader_font_remote_owns(const lv_font_t *font)
{ return font && font->get_glyph_dsc == descriptor_cb; }
static remote_font_t *remote(const lv_font_t *font)
{ return ui_reader_font_remote_owns(font) ? (remote_font_t *)font : NULL; }

static void schedule(remote_font_t *f)
{
    if (f->working) return;
    if (f->sleep_parked && !f->error) return;
    if (f->deferred && !f->error) {
        if ((rt_tick_t)(rt_tick_get() - f->retry_since) < RETRY_TICKS) return;
        f->deferred = false;
    }
    if (!f->queued && !f->working) {
        f->queued = true; f->work_next = NULL;
        if (work_tail) work_tail->work_next = f; else work_head = f;
        work_tail = f;
    }
    if (!notified) { notified = true; rt_sem_release(&work_event); }
}

/* Resource pressure leaves the font usable. A later readiness query retries;
 * the worker must not immediately resubmit a batch that cannot fit. */
static void defer_font(remote_font_t *f)
{ f->deferred = true; f->retry_since = rt_tick_get(); }

static bool transient_status(int status)
{ return status == EPD_COMPUTE_BUSY || status == EPD_COMPUTE_NO_MEMORY; }

static unsigned glyph_priority(const glyph_t *g)
{ return g->bypasses >= EPD_COMPUTE_MAX_BYPASSES ? 0 : g->purpose; }
static unsigned work_priority(remote_font_t *f)
{
    if (f->error || f->bypasses >= EPD_COMPUTE_MAX_BYPASSES) return 0;
    unsigned priority = f->metrics_ready ? EPD_COMPUTE_BACKGROUND : f->purpose;
    for (glyph_t *g = oldest; g; g = g->newer)
        if (g->font == f && g->wanted && !g->error && !g->busy && glyph_priority(g) < priority)
            priority = glyph_priority(g);
    return priority;
}

static remote_font_t *take_work(void)
{
    remote_font_t *best = NULL, *before = NULL, *best_before = NULL;
    unsigned priority = UINT_MAX;
    for (remote_font_t *f = work_head; f; f = f->work_next) {
        unsigned candidate = work_priority(f);
        if (candidate < priority) { best = f; best_before = before; priority = candidate; }
        before = f;
    }
    if (!best) return NULL;
    if (best_before) best_before->work_next = best->work_next; else work_head = best->work_next;
    if (work_tail == best) work_tail = best_before;
    best->work_next = NULL; best->queued = false; best->working = true; best->bypasses = 0;
    for (remote_font_t *f = work_head; f; f = f->work_next)
        if (f->bypasses < EPD_COMPUTE_MAX_BYPASSES) ++f->bypasses;
    return best;
}

static void fail_font(remote_font_t *f)
{
    f->error = true;
    /* A shell only cancels its own in-flight command. Other sizes retain the
     * shared context; a cancelled context is retired by the worker first. */
    if (f->primary && f->primary->owner == f && f->primary->pipe) epd_compute_pipe_cancel(f->primary->pipe);
    if (f->fallback && f->fallback->owner == f && f->fallback->pipe) epd_compute_pipe_cancel(f->fallback->pipe);
    schedule(f);
}

static bool valid(remote_font_t *f)
{
    if (!f || f->error || !f->references) return false;
    if ((f->leased && !reading_font_lease_valid(&f->lease)) ||
        (f->parent && !valid(f->parent))) {
        source_cache_clear(f->primary);
        fail_font(f); return false;
    }
    return true;
}

static unsigned hash(remote_font_t *f, uint32_t letter)
{ return (unsigned)(((uintptr_t)f >> 4) ^ letter) % HASH_SIZE; }
static glyph_t *lookup(remote_font_t *f, uint32_t letter)
{
    for (glyph_t *g = buckets[hash(f, letter)]; g; g = g->hash_next)
        if (g->font == f && g->letter == letter) return g;
    return NULL;
}
static void unlink_lru(glyph_t *g)
{
    if (g->older) g->older->newer = g->newer; else oldest = g->newer;
    if (g->newer) g->newer->older = g->older; else newest = g->older;
}
static void touch(glyph_t *g)
{
    if (g == newest) return;
    unlink_lru(g); g->older = newest; g->newer = NULL;
    if (newest) newest->newer = g; else oldest = g;
    newest = g;
}
static void drop_pixels(glyph_t *g)
{
    epd_app_free(g->allocation); g->allocation = NULL;
    pixel_bytes -= g->charge; g->charge = 0; g->pixels = false;
    memset(&g->bitmap, 0, sizeof(g->bitmap));
}
static void drop_glyph(glyph_t *g)
{
    glyph_t **link = &buckets[hash(g->font, g->letter)];
    while (*link != g) link = &(*link)->hash_next;
    *link = g->hash_next;
    link = &tickets[g->ticket % HASH_SIZE];
    while (*link != g) link = &(*link)->ticket_next;
    *link = g->ticket_next;
    unlink_lru(g); drop_pixels(g); descriptor_bytes -= sizeof(*g); epd_app_free(g);
}
static bool evictable(const glyph_t *g)
{ return !g->pins && !g->draws && !g->busy && !g->wanted; }
size_t ui_reader_font_remote_reclaim_pixels(size_t min_available)
{
    if (!initialized || !min_available) return 0;
    size_t reclaimed = 0;
    lock();
    while (epd_app_psram_available() < min_available) {
        size_t bytes = source_cache_evict();
        if (!bytes) break;
        reclaimed += bytes;
    }
    /* One LRU pass: preserve measurements and never enqueue replacement work. */
    for (glyph_t *g = oldest; g && epd_app_psram_available() < min_available; g = g->newer) {
        if (!evictable(g) || !g->allocation) continue;
        reclaimed += g->charge;
        ++diagnostics.pixel_evictions;
        drop_pixels(g);
    }
    unlock(); return reclaimed;
}
static bool platform_room(size_t bytes)
{
    if (bytes > SIZE_MAX - SHARED_RESERVE) return false;
    while (epd_app_psram_available() < bytes + SHARED_RESERVE) {
        if (source_cache_evict()) continue;
        /* Reclaim pixels first so prepared measurements survive pressure. */
        glyph_t *victim = oldest;
        while (victim && (!evictable(victim) || !victim->allocation)) victim = victim->newer;
        if (victim) { ++diagnostics.pixel_evictions; drop_pixels(victim); continue; }
        victim = oldest;
        while (victim && !evictable(victim)) victim = victim->newer;
        if (!victim) { ++diagnostics.oom_reserve; return false; }
        ++diagnostics.descriptor_evictions; drop_glyph(victim);
    }
    return true;
}
static bool room(size_t bytes, bool pixels)
{
    size_t budget = pixels ? PIXEL_BUDGET : DESC_BUDGET;
    if (bytes > budget) { ++diagnostics.oom_budget; return false; }
    while (bytes > budget - (pixels ? pixel_bytes : descriptor_bytes)) {
        glyph_t *victim = oldest;
        while (victim && (!evictable(victim) ||
                          (pixels && !victim->allocation))) victim = victim->newer;
        if (!victim) { ++diagnostics.oom_budget; return false; }
        if (pixels) { ++diagnostics.pixel_evictions; drop_pixels(victim); }
        else { ++diagnostics.descriptor_evictions; drop_glyph(victim); }
    }
    return bytes <= SIZE_MAX - ALLOCATION_HEADROOM && platform_room(bytes + ALLOCATION_HEADROOM);
}
static void *cache_allocate(size_t bytes, bool pixels)
{
    if (!room(bytes, pixels)) return NULL;
    void *out = epd_app_alloc(bytes, EPD_APP_PSRAM);
    /* Account for allocator overhead and allocations made by other workers
     * between the free-space snapshot and the actual allocation. */
    if (!out) { ++diagnostics.oom_alloc; return NULL; }
    if (epd_app_psram_available() < SHARED_RESERVE) {
        ++diagnostics.oom_postcheck; epd_app_free(out); return NULL;
    }
    uint32_t used = (uint32_t)(bytes + (pixels ? pixel_bytes : descriptor_bytes));
    uint32_t *peak = pixels ? &diagnostics.pixel_peak : &diagnostics.descriptor_peak;
    if (used > *peak) *peak = used;
    return out;
}
static glyph_t *get_glyph(remote_font_t *f, uint32_t letter)
{
    glyph_t *g = lookup(f, letter);
    if (g) { touch(g); return g; }
    g = cache_allocate(sizeof(*g), false);
    if (!g) return NULL;
    memset(g, 0, sizeof(*g)); g->font = f; g->letter = letter;
    /* Descriptors held by LVGL must not resolve to a later allocation. */
    if (glyph_ticket == UINT32_MAX) { epd_app_free(g); return NULL; }
    g->ticket = ++glyph_ticket;
    unsigned b = hash(f, letter); g->hash_next = buckets[b]; buckets[b] = g;
    b = g->ticket % HASH_SIZE; g->ticket_next = tickets[b]; tickets[b] = g;
    g->older = newest; if (newest) newest->newer = g; else oldest = g; newest = g;
    descriptor_bytes += sizeof(*g);
    return g;
}

static face_t *acquire_source(uint32_t id, const reading_font_lease_t *lease)
{
    for (face_t *s = sources; s; s = s->next)
        if (s->identity.source_id == id && (!id || !memcmp(&s->key, lease, sizeof(*lease)))) {
            ++s->references;
            return s;
        }
    if (source_generation == UINT32_MAX) return NULL;
    face_t *s = cache_allocate(sizeof(*s), false);
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    if (id) s->key = *lease;
    s->identity = (crf_font_identity){id, id ? lease->signature : ui_builtin_font_signature,
        ++source_generation, id ? lease->size : ui_builtin_font_size, 0};
    s->handle = s->identity.generation; s->references = 1;
    if (fd_managed(s)) source_fd_init(&s->fd_state, &fd_pool, &s->key, true);
    s->next = sources; sources = s; descriptor_bytes += sizeof(*s);
    return s;
}
static int request_glyph(remote_font_t *f, glyph_t *g, bool pixels)
{
    /* The caller checked the lease while holding cache_lock. */
    if (!g) return glyph_ticket == UINT32_MAX ? -1 : 0;
    if (g->error) return -1;
    if (g->descriptor && (!pixels || g->pixels)) { ++diagnostics.ready_hits; return f->metrics_ready ? 1 : 0; }
    ++diagnostics.ready_misses;
    /* A new request starts a new priority lifetime. Pending/in-flight requests
     * can only be promoted, including a render requested during measurement. */
    if (!g->wanted && !g->busy) { g->purpose = (uint8_t)f->purpose; g->bypasses = 0; }
    else if (f->purpose < g->purpose) g->purpose = (uint8_t)f->purpose;
    g->render |= pixels; g->wanted = true; schedule(f); return 0;
}

/* Initialization is UI-owned. Neither the worker nor draw callbacks initialize
 * LVGL or register a filesystem driver. */
static bool initialize(void)
{
    if (initialized) return true;
    if (epd_app_psram_available() < SHARED_RESERVE + WORKER_STACK_BYTES) return false;
    if (rt_mutex_init(&cache_lock, "rfcache", RT_IPC_FLAG_PRIO) != RT_EOK) return false;
    if (rt_sem_init(&work_event, "rfwork", 0, RT_IPC_FLAG_FIFO) != RT_EOK) {
        rt_mutex_detach(&cache_lock); return false;
    }
    rt_thread_t thread = epd_thread_create_psram("reader_font", worker_entry, NULL,
        WORKER_STACK_BYTES, WORKER_PRIORITY, RT_THREAD_TICK_DEFAULT);
    if (!thread) { rt_sem_detach(&work_event); rt_mutex_detach(&cache_lock); return false; }
    if (epd_app_psram_available() < SHARED_RESERVE) {
        rt_base_t level = rt_hw_interrupt_disable();
        ++diagnostics.oom_postcheck; ++diagnostics.worker_rollbacks;
        rt_hw_interrupt_enable(level);
        rt_thread_delete(thread); rt_sem_detach(&work_event); rt_mutex_detach(&cache_lock); return false;
    }
    if (rt_thread_startup(thread) != RT_EOK) {
        rt_base_t level = rt_hw_interrupt_disable();
        ++diagnostics.worker_rollbacks;
        rt_hw_interrupt_enable(level);
        rt_thread_delete(thread); rt_sem_detach(&work_event); rt_mutex_detach(&cache_lock); return false;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    initialized = true; rt_hw_interrupt_enable(level); return true;
}

bool ui_reader_font_remote_can_create(unsigned count)
{
    if (count > FONT_LIMIT) return false;
    if (!initialized) return true;
    lock(); bool room_available = font_count <= FONT_LIMIT - count; unlock();
    return room_available;
}

lv_font_t *ui_reader_font_remote_create(uint32_t id, unsigned size, unsigned weight, uint32_t *identity)
{
    if (identity) *identity = 0;
    if (!identity || !size || size > CRF_MAX_SIZE_PX || !initialize()) return NULL;
    lock();
    if (font_count >= FONT_LIMIT || generation == UINT32_MAX) {
        unlock(); return NULL;
    }
    remote_font_t *f = cache_allocate(sizeof(*f), false);
    if (!f) { unlock(); return NULL; }
    memset(f, 0, sizeof(*f));
    if (id && !reading_font_acquire(id, &f->lease)) { epd_app_free(f); unlock(); return NULL; }
    /* Charge the shell before allocating its shared source record. */
    descriptor_bytes += sizeof(*f);
    f->primary = acquire_source(id, &f->lease);
    if (!f->primary) {
        if (id) reading_font_release(&f->lease);
        descriptor_bytes -= sizeof(*f); epd_app_free(f); unlock(); return NULL;
    }
    f->leased = id != 0; f->id = id; f->references = 1; f->generation = ++generation;
    f->purpose = EPD_COMPUTE_FOREGROUND;
    f->style.size_px = (uint16_t)size; f->style.weight = weight <= 2 ? (uint8_t)weight : 0;
    f->font.get_glyph_dsc = descriptor_cb; f->font.get_glyph_bitmap = bitmap_cb;
    f->font.release_glyph = release_cb; f->font.kerning = LV_FONT_KERNING_NONE;
    f->next = fonts; fonts = f; ++font_count;
    uint32_t key[] = {id, f->primary->identity.signature, ui_builtin_font_signature, size, weight,
        f->lease.revision, f->lease.failure_revision};
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof(key) / sizeof(*key); ++i) h = (h ^ key[i]) * 16777619u;
    *identity = h; schedule(f); unlock(); return &f->font;
}

bool ui_reader_font_remote_ref(const lv_font_t *font)
{
    remote_font_t *f = remote(font); if (!f) return false;
    lock(); bool ok = valid(f) && f->references < UINT32_MAX;
    if (ok) ++f->references;
    unlock(); return ok;
}
static void unref(remote_font_t *f)
{
    if (f->references && !--f->references) fail_font(f);
}
void ui_reader_font_remote_destroy(lv_font_t *font)
{
    remote_font_t *f = remote(font); if (!f) return;
    lock(); unref(f); unlock();
}
void ui_reader_font_remote_cancel(const lv_font_t *font)
{
    remote_font_t *f = remote(font); if (!f) return;
    lock(); fail_font(f); unlock();
}
void ui_reader_font_remote_revoke(uint32_t id, bool storage)
{
    if (!initialized) return;
    lock();
    for (remote_font_t *f = fonts; f; f = f->next)
        if (f->id && (storage ? f->lease.volume == STORAGE_SD : f->id == id)) fail_font(f);
    unlock();
}
/* UI only. Stop future reads before checking actual file ownership. Retired
 * ACPU memory can outlive the TF handoff; it cannot reopen an invalid lease. */
bool ui_reader_font_remote_release_storage(uint32_t release_id)
{
    if (!initialized) return true;
    ui_reader_font_remote_revoke(0, true);
    bool done = true;
    lock();
    for (face_t *face = sources; face; face = face->next) {
        if (!fd_managed(face)) continue;
        if (!source_fd_retire(&face->fd_state, release_id))
            done = false;
    }
    unlock();
    return done;
}

void ui_reader_font_remote_resume_cleanup(void)
{
    if (!initialized) return;
    bool cleanup = false;
    lock();
    for (remote_font_t *f = fonts; f; f = f->next) {
        if (!f->sleep_parked) continue;
        f->sleep_parked = false;
        schedule(f);
    }
    for (face_t *face = sources; face; face = face->next)
        if (face->closing || (fd_managed(face) && face->fd_state.stopped && !fd_released(face)))
            cleanup = true;
    unlock();
    if (cleanup) fd_wake(NULL);
}

lv_font_t *ui_reader_font_remote_oblique(const lv_font_t *font)
{
    remote_font_t *parent = remote(font);
    if (!parent || !ui_reader_font_remote_ref(font)) return NULL;
    if (parent->style.oblique) return (lv_font_t *)font;
    uint32_t identity;
    /* Both creates run on the UI thread. Cancel/revocation is rechecked under
     * the cache lock before publishing the derived generation. */
    lock();
    lv_font_t *out = ui_reader_font_remote_create(parent->id, parent->style.size_px, parent->style.weight, &identity);
    if (!out) { unref(parent); unlock(); return NULL; }
    remote_font_t *f = remote(out);
    f->parent = parent; f->style.oblique = 1; f->purpose = parent->purpose;
    if (!valid(parent) || (f->leased && memcmp(&f->lease, &parent->lease, sizeof(f->lease)))) fail_font(f);
    unlock(); return out;
}

int ui_reader_font_remote_status(const lv_font_t *font)
{
    remote_font_t *f = remote(font); if (!f) return -1;
    lock(); int status = -1;
    if (valid(f)) {
        status = 0;
        if (!f->metrics_ready) schedule(f);
        if (f->metrics_ready) {
            if (!f->published) {
                f->font.line_height = f->metrics.line_height; f->font.base_line = f->metrics.base_line;
                f->font.underline_position = (int8_t)(-f->style.size_px / 8);
                f->font.underline_thickness = (int8_t)(f->style.size_px / 16 + 1);
                f->published = true;
            }
            status = 1;
        }
    }
    unlock(); return status;
}
int ui_reader_font_remote_ready(const lv_font_t *font, uint32_t letter, uint32_t next, bool pixels)
{
    (void)next;
    remote_font_t *f = remote(font);
    if (!f || letter > 0x10ffffu || (letter >= 0xd800u && letter <= 0xdfffu)) return -1;
    lock(); int result = valid(f) ? request_glyph(f, get_glyph(f, letter), pixels) : -1;
    unlock(); return result;
}

int ui_reader_font_remote_metrics(const lv_font_t *font, epd_app_font_metrics_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    remote_font_t *f = remote(font); if (!f) return -1;
    lock(); int result = valid(f) ? (f->metrics_ready ? 1 : 0) : -1;
    if (!result) schedule(f);
    if (result == 1) {
        out->line_height = (uint32_t)f->metrics.line_height;
        out->base_line = f->metrics.base_line;
    }
    unlock(); return result;
}
int ui_reader_font_remote_acquire(const lv_font_t *font, uint32_t letter, uint32_t next,
                                 bool pixels, epd_app_font_glyph_t *out)
{
    (void)next;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    remote_font_t *f = remote(font);
    if (!f || letter > 0x10ffffu || (letter >= 0xd800u && letter <= 0xdfffu)) return -1;
    lock();
    if (!valid(f)) { unlock(); return -1; }
    glyph_t *g = get_glyph(f, letter);
    int result = request_glyph(f, g, pixels);
    if (result == 1) {
        if (g->draws == UINT32_MAX || f->draws == UINT32_MAX) result = -1;
        else {
            ++g->draws; ++f->draws;
            out->adv_w = g->dsc.adv_w; out->box_w = g->dsc.box_w; out->box_h = g->dsc.box_h;
            out->ofs_x = g->dsc.ofs_x; out->ofs_y = g->dsc.ofs_y; out->stride = g->dsc.stride;
            out->pixels = pixels ? g->bitmap.data : NULL; out->token = g;
        }
    }
    unlock(); return result;
}
void ui_reader_font_remote_release(epd_app_font_glyph_t *glyph)
{
    if (!glyph || !glyph->token) return;
    lock(); glyph_t *g = glyph->token;
    --g->draws; --g->font->draws;
    if (g->font->error) schedule(g->font);
    memset(glyph, 0, sizeof(*glyph)); unlock();
}

static bool utf8_next(const char *text, size_t length, size_t *at, uint32_t *letter)
{
    unsigned c = (uint8_t)text[(*at)++], n; uint32_t value, minimum;
    if (c < 128) { *letter = c; return true; }
    if (c >= 0xc2 && c <= 0xdf) { n = 1; value = c & 31; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; value = c & 15; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; value = c & 7; minimum = 0x10000; }
    else return false;
    if (n > length - *at) return false;
    while (n--) { c = (uint8_t)text[(*at)++]; if ((c & 0xc0) != 0x80) return false; value = (value << 6) | (c & 63); }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    *letter = value; return true;
}
static int prepare_text(const lv_font_t *font, const char *text, size_t length, bool pixels, bool preview)
{
#if !UI_READER_FONT_EAGER
    (void)preview;
#endif
    remote_font_t *f = remote(font);
    if (!f) return -1;
    if ((!text && length) || length > TEXT_LIMIT) {
        lock(); ++diagnostics.prepare_failures; unlock(); return -1;
    }
    /* Validate the record before mutating its queue. */
    size_t at = 0; uint32_t letter;
    while (at < length) if (!utf8_next(text, length, &at, &letter)) {
        lock(); ++diagnostics.prepare_failures; unlock(); return -1;
    }
    lock();
    if (!valid(f)) { ++diagnostics.prepare_failures; unlock(); return -1; }
    int result = f->metrics_ready ? 1 : 0; at = 0;
    glyph_t *pending = NULL;
    bool report = false;
    struct {
        uint32_t letter, generation;
        rt_tick_t elapsed;
        unsigned descriptor, pixels, wanted, busy, metrics, queued, working;
    } wait = {0};
    while (at < length) {
        utf8_next(text, length, &at, &letter);
        glyph_t *g = get_glyph(f, letter);
        int ready = request_glyph(f, g, pixels);
        if (ready < 0) { result = -1; break; }
        /* A short foreground or preview record can obtain metrics and pixels in one
         * command. Readiness probes alone never create speculative pixels. */
#if UI_READER_FONT_EAGER
        if (!pixels && length <= EAGER_TEXT_MAX &&
            (preview || f->purpose == EPD_COMPUTE_FOREGROUND) && g && !g->descriptor && !g->busy &&
            g->eager == EAGER_NONE) g->eager = EAGER_RENDER;
#endif
        if (!ready) { result = 0; if (!pending) pending = g; }
    }
    if (pixels) {
        if (result == 0) {
            rt_tick_t now = rt_tick_get();
            if (!f->pixel_waiting) {
                f->pixel_waiting = true; f->pixel_wait_since = now;
            }
            wait.elapsed = now - f->pixel_wait_since;
            if (pending && !f->pixel_wait_reported && wait.elapsed >= RT_TICK_PER_SECOND) {
                f->pixel_wait_reported = true; report = true;
                wait.letter = pending->letter; wait.generation = f->generation;
                wait.descriptor = pending->descriptor; wait.pixels = pending->pixels;
                wait.wanted = pending->wanted; wait.busy = pending->busy;
                wait.metrics = f->metrics_ready; wait.queued = f->queued; wait.working = f->working;
            }
        }
        else f->pixel_waiting = f->pixel_wait_reported = false;
    }
    if (result < 0) ++diagnostics.prepare_failures;
    unlock();
    if (report)
        rt_kprintf("reader font_prepare_wait font=%p generation=%lu cp=U+%04lX ticks=%lu hz=%u "
            "descriptor=%u pixels=%u wanted=%u busy=%u metrics=%u queued=%u working=%u\n",
            (const void *)font, (unsigned long)wait.generation, (unsigned long)wait.letter,
            (unsigned long)wait.elapsed, RT_TICK_PER_SECOND, wait.descriptor, wait.pixels,
            wait.wanted, wait.busy, wait.metrics, wait.queued, wait.working);
    return result;
}
int ui_reader_font_remote_prepare(const lv_font_t *font, const char *text, size_t length, bool pixels)
{ return prepare_text(font, text, length, pixels, false); }
int ui_reader_font_remote_prepare_preview(const lv_font_t *font, const char *text, size_t length)
{ return prepare_text(font, text, length, false, true); }
/* The caller holds cache_lock. A background caller must not demote an
 * in-flight foreground batch on the shared source. */
static int face_purpose(face_t *face, remote_font_t *f, unsigned purpose)
{
    if (!face || (face->owner && face->owner != f)) return EPD_COMPUTE_OK;
    if (!face->owner || purpose < face->purpose) face->purpose = purpose;
    return face->pipe ? epd_compute_pipe_purpose(face->pipe, (epd_compute_purpose_t)face->purpose) : EPD_COMPUTE_OK;
}
int ui_reader_font_remote_purpose(const lv_font_t *font, unsigned purpose)
{
    remote_font_t *f = remote(font);
    if (!f || (purpose != EPD_COMPUTE_FOREGROUND && purpose != EPD_COMPUTE_BACKGROUND)) return -1;
    lock(); if (!valid(f)) { unlock(); return -1; }
    f->purpose = purpose;
    int result = face_purpose(f->primary, f, purpose);
    if (result == EPD_COMPUTE_OK) result = face_purpose(f->fallback, f, purpose);
    unlock(); return result == EPD_COMPUTE_OK ? 1 : -1;
}

epd_app_font_page_t ui_reader_font_remote_page_create(void)
{
    if (!initialized) return NULL;
    lock(); epd_app_font_page_t page = cache_allocate(sizeof(*page), false);
    if (page) { page->pins = NULL; descriptor_bytes += sizeof(*page); }
    unlock(); return page;
}
int ui_reader_font_remote_page_pin(epd_app_font_page_t page, const lv_font_t *font,
                                   uint32_t letter, uint32_t next, bool pixels)
{
    (void)next;
    remote_font_t *f = remote(font);
    if (!page || !f || letter > 0x10ffffu || (letter >= 0xd800 && letter <= 0xdfff)) return -1;
    lock(); if (!valid(f)) { unlock(); return -1; }
    glyph_t *g = get_glyph(f, letter);
    if (!g) { unlock(); return -1; }
    for (page_pin_t *p = page->pins; p; p = p->next)
        if (p->glyph == g) { int result = request_glyph(f, g, pixels); unlock(); return result; }
    ++g->pins; /* Protect this descriptor while making room for the pin. */
    page_pin_t *pin = cache_allocate(sizeof(*pin), false);
    if (!pin || f->references == UINT32_MAX) {
        epd_app_free(pin); --g->pins; unlock(); return -1;
    }
    pin->glyph = g; pin->next = page->pins; page->pins = pin;
    ++f->references; descriptor_bytes += sizeof(*pin); ++diagnostics.pins;
    int result = request_glyph(f, g, pixels); unlock(); return result;
}
void ui_reader_font_remote_page_release(epd_app_font_page_t page)
{
    if (!page) return;
    lock();
    while (page->pins) {
        page_pin_t *p = page->pins; page->pins = p->next;
        --p->glyph->pins; --diagnostics.pins; unref(p->glyph->font);
        descriptor_bytes -= sizeof(*p); epd_app_free(p);
    }
    descriptor_bytes -= sizeof(*page); epd_app_free(page); unlock();
}

static bool descriptor_cb(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t letter, uint32_t next)
{
    (void)next; remote_font_t *f = remote(font);
    lock(); glyph_t *g = valid(f) && f->published ? lookup(f, letter) : NULL;
    bool ok = g && g->descriptor && !g->error;
    if (ok) ++diagnostics.descriptor_hits; else ++diagnostics.descriptor_misses;
    memset(out, 0, sizeof(*out));
    if (ok) { *out = g->dsc; out->resolved_font = font; touch(g); }
    /* A pending/revoked face must not trigger LVGL's placeholder/fallback
     * resolution. Readiness APIs gate page publication; failed draws are empty. */
    unlock(); return true;
}
static const void *bitmap_cb(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *scratch)
{
    (void)scratch; remote_font_t *f = remote(dsc->resolved_font); if (!f) return NULL;
    lock(); glyph_t *g = NULL;
    if (valid(f))
        for (g = tickets[dsc->gid.index % HASH_SIZE]; g; g = g->ticket_next)
            if (g->font == f && g->ticket == dsc->gid.index) break;
    if (!g || !g->pixels || g->error) { ++diagnostics.draw_misses; unlock(); return NULL; }
    ++diagnostics.draw_hits; touch(g);
    /* Empty results are ready but have no draw buffer or draw reference. */
    if (!g->bitmap.data) { unlock(); return NULL; }
    if (!dsc->entry) { ++g->draws; ++f->draws; dsc->entry = (lv_cache_entry_t *)g; }
    const void *out = dsc->req_raw_bitmap ? (const void *)g->bitmap.data : &g->bitmap;
    unlock(); return out;
}
static void release_cb(const lv_font_t *font, lv_font_glyph_dsc_t *dsc)
{
    (void)font;
    lock(); glyph_t *g = (glyph_t *)dsc->entry;
    if (g) { --g->draws; --g->font->draws; if (g->font->error) schedule(g->font); dsc->entry = NULL; }
    unlock();
}

typedef struct {
    remote_font_t *font;
    face_t *face;
    glyph_t *glyphs[EPD_FONT_BATCH_MAX];
    uint32_t count, completed, request, fallbacks;
    unsigned purpose;
    uint64_t source_calls, source_bytes;
    uint32_t drive_supply_ticks, drive_consume_ticks, drive_supply_calls, drive_consume_calls;
    bool render, eager, final, opening, failed, retry;
} batch_t;

static bool source_valid(remote_font_t *f)
{ lock(); bool ok = valid(f); unlock(); return ok; }

static void timing(epd_app_font_io_timing_t *v, uint32_t bytes, uint32_t ticks)
{
    ++v->calls; v->bytes += bytes; v->ticks += ticks;
    if (ticks > v->max_ticks) v->max_ticks = ticks;
    if (bytes > v->max_bytes) v->max_bytes = bytes;
}
static void timing_add(epd_app_font_io_timing_t *v, const epd_app_font_io_timing_t *a)
{
    v->calls += a->calls; v->bytes += a->bytes; v->ticks += a->ticks;
    if (a->max_ticks > v->max_ticks) v->max_ticks = a->max_ticks;
    if (a->max_bytes > v->max_bytes) v->max_bytes = a->max_bytes;
}
static void io_add(epd_app_font_io_stats_t *v, const epd_app_font_io_stats_t *a)
{
    v->supply_calls += a->supply_calls; v->requested_bytes += a->requested_bytes; v->errors += a->errors;
    timing_add(&v->lock_wait, &a->lock_wait); timing_add(&v->lock_hold, &a->lock_hold);
    timing_add(&v->read, &a->read); timing_add(&v->ftz_copy, &a->ftz_copy);
}
/* Console values saturate at UINT32_MAX; the snapshot retains 64-bit totals. */
static unsigned long log32(uint64_t n) { return (unsigned long)(n > UINT32_MAX ? UINT32_MAX : n); }
static void batch_report(uint32_t id, uint32_t font_generation, const font_batch_stats_t stats[2])
{
    for (unsigned i = 0; i < 2; ++i) {
        const font_batch_stats_t *s = &stats[i];
        if (!s->count) continue;
        rt_kprintf("font batch font=%lu generation=%lu kind=%s count=%lu glyphs=%lu completed=%lu exec_ticks=%lu max_batch_ticks=%lu source_calls=%lu source_bytes=%lu failures=%lu retries=%lu fallbacks=%lu hz=%u u32sat=1\n",
            (unsigned long)id, (unsigned long)font_generation, i ? "render" : "measure", log32(s->count),
            log32(s->glyphs), log32(s->completed), log32(s->exec_ticks), (unsigned long)s->max_batch_ticks,
            log32(s->source_calls), log32(s->source_bytes), log32(s->failures), log32(s->retries), log32(s->fallbacks), RT_TICK_PER_SECOND);
    }
}
/* Elapsed batch time includes source service and transport/scheduler waits;
 * it is not ACPU CPU time. Counts include failed attempts and empty metrics. */
static void batch_account(font_batch_stats_t *s, const batch_t *batch, uint32_t ticks, bool ok)
{
    ++s->count; s->glyphs += batch->count; s->completed += batch->completed;
    s->exec_ticks += ticks; s->source_calls += batch->source_calls; s->source_bytes += batch->source_bytes;
    s->fallbacks += batch->fallbacks;
    if (ticks > s->max_batch_ticks) s->max_batch_ticks = ticks;
    if (!ok) ++s->failures;
    if (batch->retry) ++s->retries;
}
static void source_cache_report(uint32_t id, uint32_t source_version,
                                const source_cache_stats_t *cache, const epd_app_font_io_stats_t *io)
{
    rt_kprintf("font H source=%lu generation=%lu cache_hits=%lu cache_misses=%lu cache_bypasses=%lu cache_bytes=%lu cache_peak=%lu read_calls=%lu read_bytes=%lu metadata_hits=%lu metadata_misses=%lu block_bytes=%u hz=%u u32sat=1\n",
        (unsigned long)id, (unsigned long)source_version, log32(cache->hits), log32(cache->misses),
        log32(cache->bypasses), log32(cache->bytes), log32(cache->peak),
        log32(io->read.calls), log32(io->read.bytes), log32(cache->metadata_hits),
        log32(cache->metadata_misses), SOURCE_CACHE_BLOCK_BYTES, RT_TICK_PER_SECOND);
}
static void io_report(const face_t *face)
{
    epd_app_font_io_stats_t snapshot;
    source_cache_stats_t cache;
    epd_app_font_io_timing_t file_io[SOURCE_IO_COUNT];
    lock(); snapshot = face->io; cache = face->cache;
    memcpy(file_io, face->file_io, sizeof(file_io)); unlock();
    const epd_app_font_io_stats_t *v = &snapshot;
    rt_kprintf("font H source=%lu generation=%lu calls=%lu requested=%lu errors=%lu hz=%u u32sat=1\n",
        (unsigned long)face->identity.source_id, (unsigned long)face->identity.generation,
        log32(v->supply_calls), log32(v->requested_bytes), log32(v->errors), RT_TICK_PER_SECOND);
    const epd_app_font_io_timing_t *parts[] = {&v->lock_wait, &v->lock_hold, &v->read, &v->ftz_copy,
        &file_io[SOURCE_IO_OPEN], &file_io[SOURCE_IO_FSTAT],
        &file_io[SOURCE_IO_SEEK], &file_io[SOURCE_IO_CLOSE]};
    const char *names[] = {"lock_wait", "lock_hold", "read", "ftz_copy", "open", "fstat", "seek", "close"};
    for (unsigned i = 0; i < sizeof(parts) / sizeof(*parts); ++i) {
        const epd_app_font_io_timing_t *t = parts[i];
        rt_kprintf("font H source=%lu generation=%lu %s calls=%lu bytes=%lu ticks=%lu max_ticks=%lu max_bytes=%lu\n",
            (unsigned long)face->identity.source_id, (unsigned long)face->identity.generation, names[i],
            log32(t->calls), log32(t->bytes), log32(t->ticks), (unsigned long)t->max_ticks, (unsigned long)t->max_bytes);
    }
    source_cache_report(face->identity.source_id, face->identity.generation, &cache, v);
}

static bool source_current(face_t *face)
{
    const reading_font_lease_t *key = &face->key;
    if (!reading_font_lease_valid(key) || !storage_session_valid(key->volume, key->session)) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t revision = storage_volume_revision(key->volume);
    /* The leased path is pinned. Mutations advance the volume token before
     * changing content; retry negative checks even without another event. */
    if (!face->path_current || face->checked_path_volume != revision) {
        face->path_current = !storage_file_writers(key->path) &&
            storage_path_revision(key->path) == key->revision;
        face->checked_path_volume = revision;
    }
    bool ok = face->path_current;
    rt_hw_interrupt_enable(level);
    return ok;
}

/* Allocation is optional and never evicts glyphs to make room for raw bytes.
 * All sizes share face->key, including its signature/session/path revisions. */
static unsigned source_cache_classes(face_t *face, source_block_t **metadata, source_block_t **ordinary)
{
    unsigned count = 0;
    *metadata = *ordinary = NULL;
    for (source_block_t *b = face->blocks; b; b = b->next) {
        if (b->metadata) ++count;
        if (b->busy) continue;
        if (b->metadata) *metadata = b; else *ordinary = b;
    }
    return count;
}
static void source_cache_promote(face_t *face, source_block_t *block)
{
    if (block->metadata) return;
    source_block_t *metadata, *ordinary;
    if (source_cache_classes(face, &metadata, &ordinary) >= SOURCE_METADATA_MAX) {
        if (!metadata) return;
        metadata->metadata = false;
    }
    block->metadata = true;
}
static source_block_t *source_cache_reserve(face_t *face, uint32_t offset, uint32_t length, bool metadata)
{
    if (!length || length > SOURCE_CACHE_BLOCK_BYTES - offset % SOURCE_CACHE_BLOCK_BYTES) return NULL;
    source_block_t *block = NULL;
    source_block_t *old_metadata, *old_ordinary;
    unsigned protected_count = source_cache_classes(face, &old_metadata, &old_ordinary);
    /* Ordinary blocks borrow unoccupied protected capacity. Once full, glyph
     * streams recycle ordinary blocks without displacing the hot SFNT tables.
     * Metadata remains replaceable and ordinary reads retain at least 8 slots. */
    if (metadata && protected_count >= SOURCE_METADATA_MAX) block = old_metadata;
    else if (face->cache.bytes <= SOURCE_CACHE_LIMIT - SOURCE_BLOCK_CHARGE &&
        source_cache_totals.bytes <= SOURCE_CACHE_TOTAL_LIMIT - SOURCE_BLOCK_CHARGE &&
        epd_app_psram_available() >= SHARED_RESERVE + SOURCE_BLOCK_CHARGE) {
        block = epd_app_alloc(sizeof(*block), EPD_APP_PSRAM);
        if (block && epd_app_psram_available() < SHARED_RESERVE) { epd_app_free(block); block = NULL; }
        if (block) {
            block->next = face->blocks; face->blocks = block;
            face->cache.bytes += SOURCE_BLOCK_CHARGE;
            source_cache_totals.bytes += SOURCE_BLOCK_CHARGE;
            if (face->cache.bytes > face->cache.peak) face->cache.peak = face->cache.bytes;
            if (source_cache_totals.bytes > source_cache_totals.peak)
                source_cache_totals.peak = source_cache_totals.bytes;
        }
    }
    if (!block) {
        if (metadata && protected_count >= SOURCE_METADATA_MAX) block = old_metadata;
        else block = old_ordinary ? old_ordinary : old_metadata;
    }
    if (block) {
        source_block_touch(face, block);
        block->offset = offset - offset % SOURCE_CACHE_BLOCK_BYTES;
        block->length = 0; block->busy = true; block->discard = false;
        block->metadata = metadata;
    }
    return block;
}

/* The sole font worker owns these accounting buffers across each transaction. */
static epd_app_font_io_stats_t *fd_io;
static epd_app_font_io_timing_t fd_file_io[SOURCE_IO_COUNT];
static uint32_t fd_held_at, fd_bytes;
static void fd_state_lock(void *user) { (void)user; lock(); }
static void fd_state_unlock(void *user) { (void)user; unlock(); }
static void fd_storage_lock(void *user)
{
    (void)user; uint32_t start = (uint32_t)rt_tick_get();
    storage_lock(); fd_held_at = (uint32_t)rt_tick_get();
    timing(&fd_io->lock_wait, 0, fd_held_at - start); fd_bytes = 0;
}
static void fd_storage_unlock(void *user)
{
    (void)user;
    timing(&fd_io->lock_hold, fd_bytes, (uint32_t)rt_tick_get() - fd_held_at);
    storage_unlock();
}
static bool fd_valid(void *user, const source_fd_key *key)
{
    (void)user;
    /* source_fd supplies its embedded key. Both keys are immutable copies of
     * the same lease for the lifetime of the owning face. */
    face_t *face = (face_t *)((char *)key - offsetof(source_fd, key) - offsetof(face_t, fd_state));
    return !storage_changing() && !storage_release_requested() && source_current(face);
}
static int fd_open(void *user, const char *path)
{
    (void)user; uint32_t start = (uint32_t)rt_tick_get(); int fd = open(path, O_RDONLY);
    timing(&fd_file_io[SOURCE_IO_OPEN], 0, (uint32_t)rt_tick_get() - start); return fd;
}
static bool fd_stat(void *user, int fd, uint32_t size)
{
    (void)user; struct stat st; uint32_t start = (uint32_t)rt_tick_get();
    int result = fstat(fd, &st);
    timing(&fd_file_io[SOURCE_IO_FSTAT], 0, (uint32_t)rt_tick_get() - start);
    return !result && S_ISREG(st.st_mode) && (uint64_t)st.st_size == size;
}
static bool fd_seek(void *user, int fd, uint32_t offset)
{
    (void)user; uint32_t start = (uint32_t)rt_tick_get();
    off_t position = lseek(fd, (off_t)offset, SEEK_SET);
    timing(&fd_file_io[SOURCE_IO_SEEK], 0, (uint32_t)rt_tick_get() - start);
    return position == (off_t)offset;
}
static int fd_read(void *user, int fd, void *data, uint32_t length)
{
    (void)user; uint32_t start = (uint32_t)rt_tick_get();
    int got = (int)read(fd, data, length); fd_bytes = got > 0 ? (uint32_t)got : 0;
    timing(&fd_io->read, fd_bytes, (uint32_t)rt_tick_get() - start); return got;
}
static int fd_close(void *user, int fd)
{
    (void)user; uint32_t start = (uint32_t)rt_tick_get(); int result = close(fd);
    timing(&fd_file_io[SOURCE_IO_CLOSE], 0, (uint32_t)rt_tick_get() - start); return result;
}
static void fd_wake(void *user)
{
    /* Retirement and its one deferred close retry also run outside the glyph
     * queue. Do not coalesce them with a wake the worker is already consuming. */
    (void)user; rt_sem_release(&work_event);
}
static void fd_account(face_t *face)
{
    lock();
    for (unsigned i = 0; i < SOURCE_IO_COUNT; ++i) {
        timing_add(&face->file_io[i], &fd_file_io[i]);
        timing_add(&file_io_totals[i], &fd_file_io[i]);
    }
    unlock();
}
static bool fd_drain(face_t *face)
{
    if (!fd_managed(face)) return true;
    bool held = !fd_released(face);
    source_fd_retire(&face->fd_state, 0);
    epd_app_font_io_stats_t io = {0}; fd_io = &io; memset(fd_file_io, 0, sizeof(fd_file_io));
    source_fd_close_result result = source_fd_drain(&face->fd_state);
    fd_account(face); lock(); io_add(&diagnostics.io, &io); io_add(&face->io, &io); unlock();
    if (held && result == SFD_CLOSED) epd_power_notify();
    return result == SFD_CLOSED;
}


static crf_status source_read(face_t *face, uint32_t offset, uint32_t length,
                              uint8_t *data, epd_app_font_io_stats_t *io, uint32_t *actual)
{
    if (fd_managed(face)) {
        fd_io = io; memset(fd_file_io, 0, sizeof(fd_file_io));
        source_fd_result result = source_fd_read(&face->fd_state, offset, length, data, actual);
        fd_account(face);
        if (result != SFD_OK && source_fd_released(&face->fd_state)) epd_power_notify();
        switch (result) {
        case SFD_OK: return CRF_OK;
        case SFD_STALE: return CRF_STALE_SOURCE;
        case SFD_SHORT: return CRF_SHORT_READ;
        default: return CRF_IO_ERROR;
        }
    }
    /* A descriptor exists only inside this one storage transaction. Never
     * hold cache_lock while acquiring storage_lock or waiting for the ACPU. */
    epd_app_font_io_timing_t file_io[SOURCE_IO_COUNT] = {{0}};
    uint32_t start = (uint32_t)rt_tick_get();
    storage_lock();
    uint32_t held = (uint32_t)rt_tick_get();
    timing(&io->lock_wait, 0, held - start);
    crf_status status = CRF_OK;
    bool ok = source_current(face);
    int fd = -1;
    struct stat st;
    *actual = 0;
    /* Count only attempted calls, including failures. Elapsed ticks include
     * filesystem work and scheduling while each syscall is in progress. */
    if (!ok) status = CRF_STALE_SOURCE;
    else {
        start = (uint32_t)rt_tick_get();
        fd = open(face->key.path, O_RDONLY);
        timing(&file_io[SOURCE_IO_OPEN], 0, (uint32_t)rt_tick_get() - start);
        if (fd < 0) status = CRF_IO_ERROR;
    }
    if (status == CRF_OK) {
        start = (uint32_t)rt_tick_get();
        int result = fstat(fd, &st);
        timing(&file_io[SOURCE_IO_FSTAT], 0, (uint32_t)rt_tick_get() - start);
        if (result || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != face->identity.file_size)
            status = CRF_IO_ERROR;
    }
    if (status == CRF_OK) {
        start = (uint32_t)rt_tick_get();
        off_t position = lseek(fd, (off_t)offset, SEEK_SET);
        timing(&file_io[SOURCE_IO_SEEK], 0, (uint32_t)rt_tick_get() - start);
        if (position != (off_t)offset) status = CRF_IO_ERROR;
    }
    if (status == CRF_OK) {
        start = (uint32_t)rt_tick_get();
        int n = (int)read(fd, data, length);
        timing(&io->read, n > 0 ? (uint32_t)n : 0, (uint32_t)rt_tick_get() - start);
        if (n < 0) status = CRF_IO_ERROR; else *actual = (uint32_t)n;
    }
    if (fd >= 0) {
        start = (uint32_t)rt_tick_get();
        int result = close(fd);
        timing(&file_io[SOURCE_IO_CLOSE], 0, (uint32_t)rt_tick_get() - start);
        if (result) status = CRF_IO_ERROR;
    }
    timing(&io->lock_hold, *actual, (uint32_t)rt_tick_get() - held);
    storage_unlock();
    lock();
    for (unsigned i = 0; i < SOURCE_IO_COUNT; ++i) {
        timing_add(&face->file_io[i], &file_io[i]);
        timing_add(&file_io_totals[i], &file_io[i]);
    }
    unlock();
    if (status == CRF_OK && *actual != length) status = CRF_SHORT_READ;
    return status;
}

static int supply_read(batch_t *batch, epd_compute_pipe_message_t *m, uint8_t *data)
{
    remote_font_t *f = batch->font; face_t *face = batch->face;
    const crf_font_identity *id = &face->identity;
    uint32_t size = id->source_id ? id->file_size : (uint32_t)ui_builtin_font_pack_size;
    if (m->values[0] != EPD_FONT_VERSION || m->handle != face->handle ||
        memcmp(&m->values[1], id, sizeof(*id)) || m->size != size ||
        m->length > EPD_COMPUTE_PIPE_BYTES || m->offset > size ||
        m->length > size - m->offset || (m->values[7] & ~EPD_FONT_READ_METADATA) ||
        (!id->source_id && m->values[7])) return EPD_COMPUTE_INVALID;
    crf_status status = CRF_OK; uint32_t actual = 0;
    epd_app_font_io_stats_t io = {0};
    io.supply_calls = 1; io.requested_bytes = m->length;
    ++batch->source_calls; batch->source_bytes += m->length;
    if (!source_valid(f)) status = CRF_STALE_SOURCE;
    else if (!id->source_id) {
        uint32_t start = (uint32_t)rt_tick_get();
        memcpy(data, ui_builtin_font_pack + (size_t)m->offset, m->length);
        timing(&io.ftz_copy, m->length, (uint32_t)rt_tick_get() - start);
        actual = m->length;
    }
    else {
        bool hit = false;
        bool metadata = (m->values[7] & EPD_FONT_READ_METADATA) != 0;
        source_block_t *block = NULL;
        lock();
        if (!source_current(face)) status = CRF_STALE_SOURCE;
        else {
            for (source_block_t *b = face->blocks; b; b = b->next)
                if (!b->busy && m->offset >= b->offset && m->offset - b->offset <= b->length &&
                    m->length <= b->length - (uint32_t)(m->offset - b->offset)) {
                    memcpy(data, b->data + (size_t)(m->offset - b->offset), m->length);
                    if (metadata) source_cache_promote(face, b);
                    source_block_touch(face, b); hit = true; actual = m->length;
                    ++face->cache.hits; ++source_cache_totals.hits;
                    if (metadata) { ++face->cache.metadata_hits; ++source_cache_totals.metadata_hits; }
                    break;
                }
            if (!hit) {
                ++face->cache.misses; ++source_cache_totals.misses;
                if (metadata) { ++face->cache.metadata_misses; ++source_cache_totals.metadata_misses; }
                block = source_cache_reserve(face, (uint32_t)m->offset, m->length, metadata);
                if (!block) { ++face->cache.bypasses; ++source_cache_totals.bypasses; }
            }
        }
        unlock();
        if (status == CRF_OK && !hit) {
            uint32_t offset = block ? block->offset : (uint32_t)m->offset;
            uint32_t length = block ? size - offset : m->length;
            if (block && length > SOURCE_CACHE_BLOCK_BYTES) length = SOURCE_CACHE_BLOCK_BYTES;
            status = source_read(face, offset, length, block ? block->data : data, &io, &actual);
            lock();
            if (!source_current(face) || !valid(f)) status = CRF_STALE_SOURCE;
            if (block) {
                if (status == CRF_OK) {
                    memcpy(data, block->data + (size_t)(m->offset - offset), m->length);
                    block->length = length; actual = m->length;
                }
                block->busy = false;
                if (status != CRF_OK || block->discard) source_block_drop(face, block);
            }
            unlock();
        }
    }
    if (status == CRF_OK && actual != m->length) status = CRF_SHORT_READ;
    if (!source_valid(f) || (id->source_id && !source_current(face))) status = CRF_STALE_SOURCE;
    if (status != CRF_OK && id->source_id) { lock(); source_cache_clear(face); unlock(); }
    if (status == CRF_IO_ERROR || status == CRF_SHORT_READ) {
        if (id->source_id) reading_font_report_failure(id->source_id);
    }
    io.errors = status != CRF_OK;
    lock(); io_add(&diagnostics.io, &io); io_add(&face->io, &io); unlock();
    m->values[6] = status; m->length = status == CRF_OK ? actual : 0;
    return status == CRF_OK ? EPD_COMPUTE_OK : EPD_COMPUTE_COMPONENT_ERROR;
}

static bool store_glyph(glyph_t *g, const crf_glyph *record, const uint8_t *pixels, bool render, bool eager)
{
    if (record->codepoint != g->letter || record->advance_px > UINT16_MAX || record->box_w > CRF_MAX_BOX ||
        record->box_h > CRF_MAX_BOX || record->stride > UINT16_MAX || record->ofs_x < INT16_MIN ||
        record->ofs_x > INT16_MAX || record->ofs_y < INT16_MIN || record->ofs_y > INT16_MAX) return false;
    if (record->flags & CRF_GLYPH_MISSING) {
        if (!g->font->id || g->fallback) { g->error = true; return true; }
        g->fallback = true; g->wanted = true; return true;
    }
    if (record->flags & ~(CRF_GLYPH_CONTROL | CRF_GLYPH_MISSING)) return false;
    lv_font_glyph_dsc_t d = {0};
    d.adv_w = (uint16_t)record->advance_px; d.box_w = (uint16_t)record->box_w; d.box_h = (uint16_t)record->box_h;
    d.ofs_x = (int16_t)record->ofs_x; d.ofs_y = (int16_t)record->ofs_y; d.stride = (uint16_t)record->stride;
    d.format = LV_FONT_GLYPH_FORMAT_A8; d.gid.index = g->ticket;
    if (g->descriptor && (g->dsc.adv_w != d.adv_w || g->dsc.box_w != d.box_w || g->dsc.box_h != d.box_h ||
        g->dsc.ofs_x != d.ofs_x || g->dsc.ofs_y != d.ofs_y)) return false;
    if (render && record->box_w && record->box_h) {
        uint32_t bytes = record->bitmap_size;
        if (record->stride < record->box_w || (record->stride & 3u) ||
            bytes != record->stride * record->box_h || !pixels) return false;
        size_t charge = (size_t)bytes + 31u;
        /* Optional pixels must not evict cached pages merely to retain the
         * unused tail of a record. Explicit pixel demand keeps normal reclaim. */
        bool room_now = charge <= PIXEL_BUDGET - pixel_bytes &&
            epd_app_psram_available() >= charge + ALLOCATION_HEADROOM + SHARED_RESERVE;
        void *allocation = (!eager || g->render || room_now) ? cache_allocate(charge, true) : NULL;
        if (!allocation) {
            /* The measured descriptor remains valid even when pixels cannot
             * be retained. In particular, background pagination can use it. */
            g->dsc = d; g->descriptor = true; g->eager = EAGER_NONE; g->wanted = g->render;
            if (g->render) defer_font(g->font);
            return true;
        }
        uint8_t *aligned = (uint8_t *)(((uintptr_t)allocation + 31u) & ~(uintptr_t)31u);
        memcpy(aligned, pixels, bytes);
        drop_pixels(g); g->allocation = allocation; g->charge = charge; pixel_bytes += charge;
        g->bitmap.header.magic = LV_IMAGE_HEADER_MAGIC; g->bitmap.header.cf = LV_COLOR_FORMAT_A8;
        g->bitmap.header.w = record->box_w; g->bitmap.header.h = record->box_h;
        g->bitmap.header.stride = record->stride; g->bitmap.data = aligned; g->bitmap.data_size = bytes;
        g->bitmap.handlers = lv_draw_buf_get_font_handlers();
        /* The existing handler performs the required PSRAM writeback. */
        lv_draw_buf_flush_cache(&g->bitmap, NULL);
    }
    g->dsc = d; g->descriptor = true; g->eager = EAGER_NONE;
    /* A measured zero-size box is not evidence that rendering completed. */
    if (render) g->pixels = true;
    g->wanted = g->render && !g->pixels;
    return true;
}

static int consume_output(batch_t *batch, epd_compute_pipe_message_t *m, const uint8_t *data)
{
    epd_font_output_t header;
    if (m->length < sizeof(header)) return EPD_COMPUTE_INVALID;
    memcpy(&header, data, sizeof(header));
    size_t records = (size_t)header.completed * sizeof(crf_glyph);
    if (header.version != EPD_FONT_VERSION || header.bytes != m->length || header.request_id != batch->request ||
        header.completed > batch->count || header.first != batch->completed || header.total != batch->count ||
        header.completed > batch->count - batch->completed ||
        memcmp(&header.font, &batch->face->identity, sizeof(header.font)) ||
        header.bitmap_bytes > m->length - sizeof(header) || records > m->length - sizeof(header) - header.bitmap_bytes ||
        sizeof(header) + records + header.bitmap_bytes != m->length ||
        (header.flags & ~(EPD_FONT_OUTPUT_FINAL | EPD_FONT_OUTPUT_OPEN)) ||
        ((header.flags & EPD_FONT_OUTPUT_OPEN) != 0) != batch->opening ||
        (!batch->opening && memcmp(&header.style, &batch->font->style, sizeof(header.style)))) return EPD_COMPUTE_INVALID;
    epd_font_step_wire_t steps = {0};
    bool timed = m->values[1] == EPD_FONT_STEP_PROFILE_MAGIC;
    if (timed) {
        memcpy(&steps, m->path, sizeof(steps));
        if (!(header.flags & EPD_FONT_OUTPUT_FINAL) || !steps.tick_hz || !steps.outputs)
            return EPD_COMPUTE_INVALID;
    }
    const uint8_t *bitmaps = data + sizeof(header) + records;
    lock();
    if (!valid(batch->font)) { unlock(); return EPD_COMPUTE_CANCELLED; }
    if (!batch->opening && !batch->count && header.status == CRF_OK) {
        if (header.metrics.line_height <= 0 || header.metrics.line_height > 2048 ||
            header.metrics.base_line < 0 || header.metrics.base_line > header.metrics.line_height) {
            unlock(); return EPD_COMPUTE_INVALID;
        }
        batch->font->metrics = header.metrics; batch->font->metrics_ready = true;
    }
    for (uint32_t i = 0; i < header.completed; ++i) {
        crf_glyph record; memcpy(&record, data + sizeof(header) + i * sizeof(record), sizeof(record));
        const uint8_t *pixel = NULL;
        if (record.bitmap_size) {
            if (record.bitmap_offset > header.bitmap_bytes || record.bitmap_size > header.bitmap_bytes - record.bitmap_offset) {
                unlock(); return EPD_COMPUTE_INVALID;
            }
            pixel = bitmaps + record.bitmap_offset;
        }
        glyph_t *g = batch->glyphs[batch->completed + i];
        if (!store_glyph(g, &record, pixel, batch->render, batch->eager)) {
            unlock(); return EPD_COMPUTE_INVALID;
        }
        if (batch->eager && g->descriptor && !g->pixels) ++batch->fallbacks;
    }
    if (timed && (!step_profile_totals.ticks_per_second ||
                  step_profile_totals.ticks_per_second == steps.tick_hz)) {
        epd_app_font_step_profile_t *t = &step_profile_totals;
        step_profile_totals.ticks_per_second = steps.tick_hz;
        ++t->batches; t->reads += steps.reads; t->idles += steps.idles; t->outputs += steps.outputs;
        t->run_ticks += steps.run_ticks; t->read_wait_ticks += steps.read_wait_ticks;
        t->idle_wait_ticks += steps.idle_wait_ticks; t->output_wait_ticks += steps.output_wait_ticks;
    }
    batch->completed += header.completed;
    if (!batch->opening) active_profile.completed = batch->completed;
    batch->final = (header.flags & EPD_FONT_OUTPUT_FINAL) != 0;
    if (header.status != CRF_OK) {
        if (batch->eager && !batch->opening &&
            (header.status == CRF_NO_MEMORY || header.status == CRF_OUTPUT_FULL)) {
            /* The failed glyph has no returned descriptor. Measure the
             * unfinished suffix once, even if real pixel demand arrived meanwhile. */
            for (uint32_t i = batch->completed; i < batch->count; ++i) {
                batch->glyphs[i]->eager = EAGER_MEASURE;
                batch->glyphs[i]->wanted = true; ++batch->fallbacks;
            }
        }
        else if (header.status == CRF_NO_MEMORY) {
            batch->retry = true;
            for (uint32_t i = batch->completed; i < batch->count; ++i) batch->glyphs[i]->wanted = true;
        }
        else {
            /* OUTPUT_FULL has already been split down to a single glyph by
             * the remote. Repeating that oversized bitmap cannot help. */
            if (batch->opening || !batch->count || header.status != CRF_OUTPUT_FULL) batch->failed = true;
            for (uint32_t i = batch->completed; i < batch->count; ++i) batch->glyphs[i]->error = true;
        }
    }
    else if (batch->final && batch->completed != batch->count) batch->failed = true;
    unlock(); return EPD_COMPUTE_OK;
}
static int service(void *user, epd_compute_pipe_message_t *m, uint8_t *data)
{
    batch_t *batch = user;
    if (m->values[0] != EPD_FONT_VERSION) return EPD_COMPUTE_INVALID;
    if (m->operation == EPD_PIPE_READ) {
        uint32_t started = (uint32_t)rt_tick_get();
        int result = supply_read(batch, m, data);
        batch->drive_supply_ticks += (uint32_t)rt_tick_get() - started;
        ++batch->drive_supply_calls;
        return result;
    }
    if (m->operation == EPD_PIPE_OUTPUT) {
        uint32_t started = (uint32_t)rt_tick_get();
        int result = consume_output(batch, m, data);
        batch->drive_consume_ticks += (uint32_t)rt_tick_get() - started;
        ++batch->drive_consume_calls;
        if (result != EPD_COMPUTE_OK) batch->failed = true;
        return result;
    }
    batch->failed = true; return EPD_COMPUTE_UNSUPPORTED;
}
static unsigned drive_kind(uint32_t op)
{
    switch (op) {
    case EPD_PIPE_READ: return EPD_FONT_DRIVE_READ;
    case EPD_PIPE_OUTPUT: return EPD_FONT_DRIVE_OUTPUT;
    case EPD_PIPE_IDLE: return EPD_FONT_DRIVE_IDLE;
    case EPD_PIPE_COMMAND: return EPD_FONT_DRIVE_COMMAND;
    default: return EPD_FONT_DRIVE_OTHER;
    }
}
static void drive_account(batch_t *batch, uint32_t elapsed, int result,
                          const epd_compute_pipe_message_t *message)
{
    unsigned kind = result == EPD_COMPUTE_MORE && message ?
        drive_kind(message->operation) : EPD_FONT_DRIVE_OTHER;
    rt_base_t key = rt_hw_interrupt_disable();
    epd_app_font_drive_profile_t *p = &drive_profile_totals;
    ++p->op[kind].calls;
    p->op[kind].pipe_ticks += elapsed - batch->drive_supply_ticks - batch->drive_consume_ticks;
    p->step_ticks += elapsed;
    p->supply_calls += batch->drive_supply_calls; p->supply_ticks += batch->drive_supply_ticks;
    p->consume_calls += batch->drive_consume_calls; p->consume_ticks += batch->drive_consume_ticks;
    rt_hw_interrupt_enable(key);
}

static bool drive(batch_t *batch)
{
    for (;;) {
        if (!source_valid(batch->font)) return false;
        batch->drive_supply_ticks = batch->drive_consume_ticks = 0;
        batch->drive_supply_calls = batch->drive_consume_calls = 0;
        uint32_t started = (uint32_t)rt_tick_get();
        int result = epd_compute_pipe_step(batch->face->pipe, service, batch);
        uint32_t elapsed = (uint32_t)rt_tick_get() - started;
        drive_account(batch, elapsed, result, epd_compute_pipe_result(batch->face->pipe));
        if (transient_status(result)) batch->retry = true;
        if (result != EPD_COMPUTE_MORE || batch->failed) return false;
        const epd_compute_pipe_message_t *m = epd_compute_pipe_result(batch->face->pipe);
        if (m && m->operation == EPD_PIPE_COMMAND) return batch->final;
    }
}

/* Only the font worker opens/closes a source pipe. Publish closing ownership
 * before waiting, and keep it visible until shutdown actually releases it. */
static bool close_source(face_t *face)
{
    bool had_fd = !fd_released(face);
    bool fd_done = fd_drain(face);
    lock(); epd_compute_pipe_host_t pipe = face->pipe ? face->pipe : face->closing;
    source_cache_clear(face);
    face->closing = pipe;
    face->pipe = NULL; face->opened = false; face->owner = NULL;
    unlock();
    if (pipe) {
        sleep_guard_owner_t awake = { .id = "reader_font.close" };
        if (!sleep_guard_acquire_drain(&awake)) return false;
        int result = epd_compute_pipe_shutdown(&pipe);
        if (result == EPD_COMPUTE_OK && fd_done) io_report(face);
        lock(); face->closing = pipe;
        if (result == EPD_COMPUTE_OK) --resident_sources;
        unlock();
        sleep_guard_release(&awake);
        if (result == EPD_COMPUTE_OK) epd_power_notify();
        return result == EPD_COMPUTE_OK && fd_done;
    }
    if (had_fd && fd_done) io_report(face);
    return fd_done;
}

/* Failed shutdowns retain their handle and resident charge. Retry once on a
 * subsequent work event, never in a polling loop or while holding the UI lock. */
static void reap_sources(void)
{
    lock(); face_t *first = sources; unlock();
    /* Drain every retired TF descriptor before any synchronous ACPU CLOSE. */
    for (face_t *s = first; s; s = s->next) {
        lock(); bool pending = fd_managed(s) && s->fd_state.stopped && !fd_released(s); unlock();
        if (pending) fd_drain(s);
    }
    for (face_t *s = first, *next; s; s = next) {
        lock(); next = s->next;
        bool closing = s->closing != NULL ||
            (fd_managed(s) && s->fd_state.stopped && (s->pipe || !fd_released(s)));
        unlock();
        if (closing) close_source(s);
        lock();
        if (!s->references && !s->pipe && !s->closing && fd_released(s)) {
            face_t **p = &sources; while (*p != s) p = &(*p)->next; *p = s->next;
            descriptor_bytes -= sizeof(*s); epd_app_free(s);
        }
        unlock();
    }
}

static void release_source(face_t *face)
{
    if (!face) return;
    lock(); bool last = --face->references == 0; unlock();
    if (!last) return;
    close_source(face);
    lock();
    /* Creation can acquire an existing source while its old pipe is closing. */
    if (!face->references && !face->closing && fd_released(face)) {
        face_t **p = &sources; while (*p != face) p = &(*p)->next; *p = face->next;
        descriptor_bytes -= sizeof(*face); epd_app_free(face);
    }
    unlock();
}

static bool open_face(batch_t *request)
{
    remote_font_t *f = request->font; face_t *face = request->face;
    lock();
    bool closing = face->closing || (fd_managed(face) && face->fd_state.stopped && !fd_released(face));
    unlock();
    if (closing &&
        !close_source(face)) { request->retry = true; return false; }
    lock(); bool stopped = fd_managed(face) && face->fd_state.stopped; unlock();
    if (stopped && !source_fd_restart(&face->fd_state)) return false;
    lock();
    /* The UI may promote selected glyphs before this batch owns the pipe.
     * Refresh under the same lock as ownership so its snapshot cannot demote them. */
    unsigned purpose = request->count ? request->purpose : work_priority(f);
    for (uint32_t i = 0; i < request->count; ++i)
        if (request->glyphs[i]->purpose < purpose) purpose = request->glyphs[i]->purpose;
    if (purpose < EPD_COMPUTE_FOREGROUND) purpose = EPD_COMPUTE_FOREGROUND;
    if (purpose < request->purpose) request->purpose = purpose;
    face->owner = f; face->purpose = request->purpose; face->last_use = ++source_clock;
    if (face->opened) {
        bool ok = valid(f) && epd_compute_pipe_purpose(face->pipe, (epd_compute_purpose_t)face->purpose) == EPD_COMPUTE_OK;
        unlock(); return ok;
    }
    face_t *victim = NULL;
    if (resident_sources >= SOURCE_SESSION_LIMIT)
        for (face_t *s = sources; s; s = s->next)
            if (s != face && s->pipe && !s->owner && (!victim || s->last_use < victim->last_use)) victim = s;
    bool room_available = resident_sources < SOURCE_SESSION_LIMIT || victim;
    unlock();
    if (!room_available) { request->retry = true; return false; }
    if (victim && !close_source(victim)) { request->retry = true; return false; }
    lock();
    bool fd_room = !fd_managed(face) || face->fd_state.held || fd_pool.held < fd_pool.limit;
    unlock();
    if (!fd_room) { request->retry = true; return false; }
    epd_compute_capability_t capability;
    if (epd_compute_query(EPD_FONT_COMPONENT, EPD_FONT_VERSION, &capability) != EPD_COMPUTE_OK ||
        !capability.ready || !(capability.registration.flags & EPD_COMPUTE_STEP_SHARED)) return false;
    epd_font_open_t config = {0}; config.version = EPD_FONT_VERSION; config.bytes = sizeof(config);
    config.font = face->identity; config.handle = face->handle; config.memory_bytes = SOURCE_MEMORY_BYTES;
    if (face->identity.source_id) config.cache_bytes = 256u * 1024u;
    if (!face->identity.source_id) {
        if (ui_builtin_font_pack_size <= 0) return false;
        config.flags = EPD_FONT_SOURCE_FTZ1; config.source_bytes = (uint32_t)ui_builtin_font_pack_size;
#if defined(SF32LB57X) && defined(BUILTIN_RESOURCE_START_ADDR) && defined(BUILTIN_RESOURCE_SIZE)
        uintptr_t address = (uintptr_t)ui_builtin_font_pack;
        if (address >= BUILTIN_RESOURCE_START_ADDR &&
            address - BUILTIN_RESOURCE_START_ADDR <= BUILTIN_RESOURCE_SIZE &&
            config.source_bytes <= BUILTIN_RESOURCE_SIZE - (address - BUILTIN_RESOURCE_START_ADDR))
            config.source_address = (uint32_t)address;
#endif
    }
    epd_compute_pipe_message_t message = {0}; message.operation = EPD_PIPE_OPEN; message.values[0] = EPD_FONT_VERSION;
    if (face->identity.source_id) message.values[7] = EPD_FONT_READ_HINTS_MAGIC;
    message.values[6] = EPD_FONT_STEP_PROFILE_MAGIC;
    epd_compute_pipe_host_t pipe = NULL;
    lock(); purpose = face->purpose; bool enough = platform_room(SOURCE_PIPE_BYTES); unlock();
    if (!enough) { request->retry = true; return false; }
    int opened = epd_compute_pipe_open(EPD_FONT_COMPONENT, EPD_FONT_VERSION, (epd_compute_purpose_t)purpose,
        &message, &config, sizeof(config), &pipe);
    if (transient_status(opened)) request->retry = true;
    if (!pipe) return false;
    /* Failed open may still own a remote session and shared buffers. Publish
     * ownership before returning so normal close/retry retains the handle. */
    lock(); face->pipe = pipe; memset(&face->io, 0, sizeof(face->io));
    memset(face->file_io, 0, sizeof(face->file_io));
    memset(&face->cache, 0, sizeof(face->cache)); ++resident_sources;
    enough = epd_app_psram_available() >= SHARED_RESERVE;
    if (!enough) { ++diagnostics.oom_postcheck; request->retry = true; }
    if (opened != EPD_COMPUTE_OK || !enough || !valid(f)) epd_compute_pipe_cancel(pipe);
    else epd_compute_pipe_purpose(pipe, (epd_compute_purpose_t)face->purpose);
    unlock();
    if (opened != EPD_COMPUTE_OK || !enough) return false;
    batch_t batch = {0}; batch.font = f; batch.face = face; batch.opening = true;
    bool driven = drive(&batch);
    request->source_calls += batch.source_calls; request->source_bytes += batch.source_bytes;
    if (batch.retry) request->retry = true;
    if (batch.failed) request->failed = true;
    if (!driven || batch.retry) return false;
    lock(); face->opened = true; unlock(); return true;
}
static bool run_batch(batch_t *batch)
{
    remote_font_t *f = batch->font;
    if (!open_face(batch)) return false;
    struct { epd_font_command_t header; crf_codepoint letters[EPD_FONT_BATCH_MAX]; } command;
    memset(&command, 0, sizeof(command));
    command.header.version = EPD_FONT_VERSION;
    command.header.bytes = sizeof(command.header) + batch->count * sizeof(crf_codepoint);
    if (!++f->request) ++f->request;
    command.header.request_id = batch->request = f->request;
    command.header.operation = batch->render ? EPD_FONT_RENDER : EPD_FONT_MEASURE;
    command.header.font = batch->face->identity; command.header.style = f->style; command.header.count = batch->count;
    /* Nearby codepoints usually share font-table and outline blocks. Reorder
     * the matching glyph pointers too, so output still maps to its request. */
    for (uint32_t i = 1; i < batch->count; ++i) {
        glyph_t *g = batch->glyphs[i]; uint32_t j = i;
        while (j && batch->glyphs[j - 1]->letter > g->letter) {
            batch->glyphs[j] = batch->glyphs[j - 1]; --j;
        }
        batch->glyphs[j] = g;
    }
    for (uint32_t i = 0; i < batch->count; ++i) command.letters[i].codepoint = batch->glyphs[i]->letter;
    epd_compute_pipe_message_t message = {0}; message.operation = EPD_PIPE_COMMAND; message.values[0] = EPD_FONT_VERSION;
    int status = epd_compute_pipe_command(batch->face->pipe, &message, &command, command.header.bytes);
    if (transient_status(status)) batch->retry = true;
    if (status != EPD_COMPUTE_OK) return false;
    return drive(batch);
}

static void retire(remote_font_t *f)
{
    batch_report(f->id, f->generation, f->batches);
    lock(); face_t *a = f->primary, *b = f->fallback;
    f->primary = f->fallback = NULL; unlock();
    release_source(a); release_source(b);
    lock();
    if (f->leased) { reading_font_release(&f->lease); f->leased = false; }
    f->retired = true;
    for (glyph_t *g = oldest, *next; g; g = next) {
        next = g->newer;
        if (g->font == f) { g->wanted = false; g->busy = false; if (!g->pins && !g->draws) drop_glyph(g); }
    }
    unlock();
}
static bool glyph_render(const glyph_t *g)
{ return g->eager != EAGER_MEASURE && (g->render || g->eager == EAGER_RENDER); }

static void process_font(remote_font_t *f)
{
    lock(); bool alive = valid(f); bool metrics = f->metrics_ready; unlock();
    if (!alive) { if (!f->retired) retire(f); return; }
    sleep_guard_owner_t awake = { .id = "reader_font.batch" };
    /* The cache lock joins failed admission to the UI's resume notification,
     * so a resume cannot miss a font that has just parked itself. */
    lock();
    f->sleep_parked = !sleep_guard_acquire_drain(&awake);
    bool parked = f->sleep_parked;
    unlock();
    if (parked) return;
    batch_t batch = {0}; batch.font = f; batch.face = f->primary;
    lock(); batch.purpose = work_priority(f);
    if (batch.purpose < EPD_COMPUTE_FOREGROUND) batch.purpose = EPD_COMPUTE_FOREGROUND;
    unlock();
    if (metrics) {
        lock();
        glyph_t *first = NULL;
        for (glyph_t *g = oldest; g; g = g->newer)
            if (g->font == f && g->wanted && !g->error && !g->busy &&
                (!first || glyph_priority(g) < glyph_priority(first))) first = g;
        unsigned priority = first ? glyph_priority(first) : UINT_MAX;
        unsigned limit = first && first->purpose == EPD_COMPUTE_BACKGROUND ? BACKGROUND_BATCH_MAX : EPD_FONT_BATCH_MAX;
        for (glyph_t *g = oldest; g && batch.count < limit; g = g->newer) {
            if (g->font != f || !g->wanted || g->error || g->busy) continue;
            if (glyph_priority(g) != priority || g->purpose != first->purpose) continue;
            if (!batch.count) {
                if (g->fallback && !f->fallback) f->fallback = acquire_source(0, NULL);
                batch.face = g->fallback ? f->fallback : f->primary; batch.render = glyph_render(g);
                batch.eager = g->eager == EAGER_RENDER;
                batch.purpose = g->purpose;
                if (!batch.face) { defer_font(f); break; }
            }
            if ((g->fallback ? f->fallback : f->primary) != batch.face || glyph_render(g) != batch.render ||
                (g->eager == EAGER_RENDER) != batch.eager) continue;
            g->busy = true; g->wanted = false; batch.glyphs[batch.count++] = g;
        }
        if (batch.count) {
            for (glyph_t *g = oldest; g; g = g->newer)
                if (g->font == f && g->wanted && !g->error && !g->busy && g->bypasses < EPD_COMPUTE_MAX_BYPASSES)
                    ++g->bypasses;
            for (uint32_t i = 0; i < batch.count; ++i) batch.glyphs[i]->bypasses = 0;
        }
        unlock();
        if (!batch.count) { sleep_guard_release(&awake); return; }
    }
    lock();
    rt_tick_t started = rt_tick_get();
    active_started = started;
    active_profile = (epd_app_font_active_profile_t){
        batch.render ? 2u : 1u, f->id, f->generation,
        f->style.size_px, f->style.weight, f->style.oblique, batch.count, 0, 0
    };
    unlock();
    bool ok = run_batch(&batch);
    lock();
    uint32_t elapsed = (uint32_t)(rt_tick_get() - started);
    batch_account(&f->batches[batch.render], &batch, elapsed, ok);
    batch_account(&batch_totals[batch.render], &batch, elapsed, ok);
    memset(&active_profile, 0, sizeof(active_profile));
    bool discard = !ok || f->error;
    batch.face->owner = NULL;
    for (uint32_t i = 0; i < batch.count; ++i) {
        glyph_t *g = batch.glyphs[i]; g->busy = false;
        if (batch.retry && !g->error) g->wanted = !g->descriptor || (g->render && !g->pixels);
    }
    if (batch.retry && !batch.failed) defer_font(f);
    else if (!ok) fail_font(f);
    unlock();
    if (discard) close_source(batch.face);
    sleep_guard_release(&awake);
}
static void worker_entry(void *unused)
{
    (void)unused;
    for (;;) {
        rt_sem_take(&work_event, RT_WAITING_FOREVER);
        reap_sources();
        {
            lock(); notified = false;
            remote_font_t *f = take_work();
            if (!f) { unlock(); continue; }
            if (work_head && !notified) { notified = true; rt_sem_release(&work_event); }
            unlock(); process_font(f); lock(); f->working = false;
            bool pending = (f->error && !f->retired) || (!f->error && !f->metrics_ready);
            for (glyph_t *g = oldest; g; g = g->newer)
                if (g->font == f && g->wanted && !g->error) { pending = true; break; }
            if (f->error && f->retired && !f->references && !f->draws) {
                for (glyph_t *g = oldest, *next; g; g = next) { next = g->newer; if (g->font == f) drop_glyph(g); }
                remote_font_t **p = &fonts; while (*p != f) p = &(*p)->next; *p = f->next;
                if (f->parent) unref(f->parent);
                descriptor_bytes -= sizeof(*f); --font_count; epd_app_free(f);
            }
            else if (pending && (!f->sleep_parked || f->error) && (!f->deferred || f->error)) schedule(f);
            unlock();
        }
    }
}

void epd_app_font_remote_profile(epd_app_font_remote_profile_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ticks_per_second = RT_TICK_PER_SECOND;
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = initialized;
    rt_hw_interrupt_enable(level);
    if (!ready) return;
    lock();
    out->measure = batch_totals[0]; out->render = batch_totals[1];
    out->io = diagnostics.io;
    out->open = file_io_totals[SOURCE_IO_OPEN]; out->fstat = file_io_totals[SOURCE_IO_FSTAT];
    out->seek = file_io_totals[SOURCE_IO_SEEK]; out->close = file_io_totals[SOURCE_IO_CLOSE];
    out->cache_hits = source_cache_totals.hits; out->cache_misses = source_cache_totals.misses;
    out->cache_bypasses = source_cache_totals.bypasses;
    out->metadata_hits = source_cache_totals.metadata_hits;
    out->metadata_misses = source_cache_totals.metadata_misses;
    out->active = active_profile;
    if (out->active.kind) out->active.exec_ticks = (uint32_t)(rt_tick_get() - active_started);
    unlock();
}
RTM_EXPORT(epd_app_font_remote_profile);

void epd_app_font_drive_profile(epd_app_font_drive_profile_t *out)
{
    if (!out) return;
    rt_base_t key = rt_hw_interrupt_disable();
    *out = drive_profile_totals;
    rt_hw_interrupt_enable(key);
    out->ticks_per_second = RT_TICK_PER_SECOND;
}
RTM_EXPORT(epd_app_font_drive_profile);

void epd_app_font_step_profile(epd_app_font_step_profile_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = initialized;
    rt_hw_interrupt_enable(level);
    if (!ready) return;
    lock(); *out = step_profile_totals; unlock();
}
RTM_EXPORT(epd_app_font_step_profile);

void ui_reader_font_remote_stats(epd_app_font_remote_stats_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!initialized) {
        *out = diagnostics; out->ticks_per_second = RT_TICK_PER_SECOND;
        rt_hw_interrupt_enable(level); return;
    }
    rt_hw_interrupt_enable(level);
    lock(); *out = diagnostics;
    out->pixel_bytes = (uint32_t)pixel_bytes; out->descriptor_bytes = (uint32_t)descriptor_bytes;
    out->fonts = font_count; out->sources = resident_sources; out->ticks_per_second = RT_TICK_PER_SECOND;
    font_batch_stats_t snapshot[2]; memcpy(snapshot, batch_totals, sizeof(snapshot));
    source_cache_stats_t cache = source_cache_totals;
    unlock();
    batch_report(0, 0, snapshot);
    rt_kprintf("font H cache_total hits=%lu misses=%lu bypasses=%lu bytes=%lu peak=%lu read_calls=%lu read_bytes=%lu block_bytes=%u hz=%u u32sat=1\n",
        log32(cache.hits), log32(cache.misses), log32(cache.bypasses), log32(cache.bytes), log32(cache.peak),
        log32(out->io.read.calls), log32(out->io.read.bytes), SOURCE_CACHE_BLOCK_BYTES, RT_TICK_PER_SECOND);
}
void ui_reader_font_remote_draw_stats(epd_app_font_cache_stats_t *out)
{
    if (!out || !initialized) return;
    lock(); out->glyph_hits += diagnostics.draw_hits; out->glyph_misses += diagnostics.draw_misses;
    out->prepare_failures += diagnostics.prepare_failures; unlock();
}

bool ui_reader_font_remote_glyph_fallback(const lv_font_t *font, uint32_t letter)
{
    remote_font_t *f = remote(font); if (!f) return false;
    lock(); glyph_t *g = valid(f) ? lookup(f, letter) : NULL;
    bool found = g && g->descriptor && !g->error && g->fallback;
    unlock(); return found;
}
