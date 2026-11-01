#ifndef EPD_APP_FONT_H
#define EPD_APP_FONT_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* UI thread. Call begin once when opening a reading session, before creating
 * fonts. It restores an available preferred font after storage recovery; do
 * not call it when resuming or reflowing an existing session. */
bool epd_app_reading_font_begin(void);
/* Current actual catalog ID (0 is built-in MiSans) and content revision.
 * Both are read-only snapshots. Compare both before reusing reading layout. */
uint32_t epd_app_reading_font_id(void);
uint32_t epd_app_reading_font_revision(void);

/* UI thread, memory-only capacity check for an immediately following batch.
 * Only the UI creates shells; the font worker can only release their slots.
 * False means defer the batch and retry on a later UI service pass. No shell
 * is allocated or reserved; a true result does not guarantee heap/source success. */
bool epd_app_reading_font_can_create(unsigned count);

/* UI thread. family is a stable reading-font catalog ID, not a list index.
 * identity and fallback must be non-NULL output pointers. Returns
 * an owned asynchronous font shell, or NULL on allocation/identity failure.
 * fallback is set to NULL: only confirmed missing glyphs use remote MiSans.
 * Wait for status==1 before using metrics, and prepare pixels before binding.
 * After removing object references, destroy with epd_app_font_destroy(). */
lv_font_t *epd_app_reading_font_create(unsigned family, unsigned size, unsigned weight,
                               uint32_t *identity, lv_font_t **fallback);
/* Synchronous native MiSans for system UI. family must be 0; ready immediately.
 * Same ownership contract: destroy the returned font; fallback is NULL. */
lv_font_t *epd_app_font_create(unsigned family, unsigned size, unsigned weight,
                               uint32_t *identity, lv_font_t **fallback);
void epd_app_font_destroy(lv_font_t *font);
/* Memory-only. -1: terminal source/glyph error or resource exhaustion;
 * 0: queued/in progress; 1: ready. Only status is UI-thread-only: it publishes
 * metrics exactly once. Worker completion never mutates a published lv_font_t.
 * ready/prepare may run on any thread holding a font reference; ready==1 does
 * not publish LVGL metadata. UI must still call status before using LVGL. */
int epd_app_font_status(const lv_font_t *font);
int epd_app_font_glyph_ready(const lv_font_t *font, uint32_t letter, uint32_t next, bool pixels);
int epd_app_font_prepare_text(const lv_font_t *font, const char *utf8, size_t length, bool pixels);
/* Descriptor readiness (-1/0/1), retaining the caller's current purpose.
 * Records <=1024 UTF-8 bytes may also render new, not-yet-running glyphs.
 * Pixels are optional: OOM falls back to metrics, and repeated preview calls
 * do not retry pixels for measured glyphs. Longer records only request metrics.
 * Use prepare_text(..., true) when pixels must be ready for drawing. */
int epd_app_font_prepare_text_preview(const lv_font_t *font, const char *utf8, size_t length);
typedef struct { uint32_t line_height; int32_t base_line; } epd_app_font_metrics_t;
typedef struct {
    uint32_t adv_w, box_w, box_h, stride;
    int32_t ofs_x, ofs_y;
    const uint8_t *pixels;
    void *token;
} epd_app_font_glyph_t;
/* Thread-safe memory snapshots with the same -1/0/1 result convention.
 * metrics never modifies lv_font_t. acquire queues missing data and returns
 * one owned token on success, including descriptor-only and empty glyphs.
 * Dimensions/advance/offsets are pixels; A8 rows are stride bytes. pixels is
 * NULL for descriptor-only or empty glyphs. Release each success exactly once;
 * do not copy/reuse a live output as another acquisition destination. Tokens
 * preserve returned memory through cancellation, page release and destruction. */
int epd_app_font_metrics(const lv_font_t *font, epd_app_font_metrics_t *out);
int epd_app_font_glyph_acquire(const lv_font_t *font, uint32_t letter, uint32_t next,
                              bool pixels, epd_app_font_glyph_t *out);
void epd_app_font_glyph_release(epd_app_font_glyph_t *glyph);
/* UI thread. A derived reading face owns a reference to its source generation. */
lv_font_t *epd_app_font_oblique(const lv_font_t *font);
/* Reference, destroy, cancel and purpose calls are thread-safe. The caller must
 * own a font reference until its concurrent calls have returned. */
bool epd_app_font_ref(const lv_font_t *font);
void epd_app_font_cancel(const lv_font_t *font);
/* Supported epd_compute_purpose_t values: 1 foreground, 2 background.
 * Changes queued and running batch priority without reopening the font. */
int epd_app_font_set_purpose(const lv_font_t *font, unsigned purpose);
typedef struct epd_app_font_page *epd_app_font_page_t;
/* Thread-safe after the first UI font creation. Independent tokens may overlap
 * while old/new pages coexist. Each unique
 * (font,letter,next) is pinned once per token, including pending pixels. Pin
 * before checking readiness; release the old token after binding the new page.
 * Token references retain fonts; glyph draw references survive token release. */
epd_app_font_page_t epd_app_font_page_create(void);
int epd_app_font_page_pin(epd_app_font_page_t page, const lv_font_t *font,
                         uint32_t letter, uint32_t next, bool pixels);
void epd_app_font_page_release(epd_app_font_page_t page);
/* Thread-safe, memory-only remote glyph pixel reclamation. min_available is
 * the desired total free app PSRAM, including the caller's allocation, reserve
 * and allocator headroom. Stops at that target or after one LRU pass; skips
 * pinned, borrowed/drawing, queued and executing glyphs. Descriptors survive.
 * Returns released cache bytes, excluding heap headers; zero may mean the
 * target was already met or no pixels were reclaimable. Does not initialize,
 * allocate, wait for preparation or queue replacement glyphs. Call outside
 * draw callbacks. Pin/borrow all glyphs needed by a pending page before calling:
 * prepared but unpinned glyphs have no page ownership and may be evicted.
 * Caller must recheck free space and allocation success: the
 * target may remain unmet and other heap users can allocate after return. */
size_t epd_app_font_reclaim_pixels(size_t min_available);
/* UI-thread idle preparation; false means the speculative cache budget is full. */
bool epd_app_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent);
typedef struct
{
    uint64_t glyph_hits, glyph_misses;
    uint64_t prepare_failures;
    uint64_t source_bitmap_calls, source_bitmap_ticks;
    uint32_t ticks_per_second;
} epd_app_font_cache_stats_t;

/* Thread-safe, read-only cumulative counters, never reset on font/cache changes.
 * Subtract snapshots around page binding and completed drawing. Hits/misses
 * count native, derived and remote LVGL pixel lookups outside preparation, not descriptors
 * or store-time duplicate checks. Preparation success does not imply residency.
 * source_bitmap_* counts completed native source bitmap callbacks outside
 * preparation, including failures. It excludes lock waits/cache copies; the
 * source may have its own cache, so these are not rasterization counts. Ticks
 * use ticks_per_second and may be zero for a sub-tick callback. NULL is ignored.
 * No draw wait, allocation, cache mutation or font initialization is performed. */
void epd_app_font_cache_stats(epd_app_font_cache_stats_t *out);
/* Remote diagnostics are separate from the legacy stats ABI. All counters are
 * cumulative since boot. ready_* counts readiness API lookups (including page
 * pins/acquisitions); descriptor_* counts LVGL description callbacks, including
 * layout; draw_* counts only LVGL bitmap callbacks. Worker preparation adds no
 * draw/source_bitmap calls. Subtract snapshots to measure a display interval.
 * Current/peak bytes are charged cache allocations, excluding worker/pipe and
 * allocator headers. Evictions exclude destruction. OOM fields distinguish
 * cache quota, platform reserve, allocator failure, and post-allocation reserve.
 * Pins counts token/glyph pairs. Snapshot does not initialize or allocate. */
typedef struct {
    uint64_t calls, bytes, ticks;
    uint32_t max_ticks, max_bytes;
} epd_app_font_io_timing_t;
typedef struct {
    uint64_t supply_calls, requested_bytes, errors;
    /* lock_wait: before lock to acquisition; lock_hold: acquisition to before
     * unlock (open/stat/seek/read/close). read: syscall only. ftz_copy: memcpy
     * of packed MiSans bytes only. bytes counts actual transferred bytes;
     * lock_wait has no bytes. Ticks use the snapshot's ticks_per_second. */
    epd_app_font_io_timing_t lock_wait, lock_hold, read, ftz_copy;
} epd_app_font_io_stats_t;
typedef struct {
    uint64_t descriptor_hits, descriptor_misses, draw_hits, draw_misses;
    uint64_t ready_hits, ready_misses, prepare_failures;
    uint64_t pixel_evictions, descriptor_evictions;
    uint64_t oom_budget, oom_reserve, oom_alloc, oom_postcheck, worker_rollbacks;
    uint32_t pixel_bytes, pixel_peak, descriptor_bytes, descriptor_peak;
    uint32_t fonts, sources, pins, ticks_per_second;
    epd_app_font_io_stats_t io;
} epd_app_font_remote_stats_t;
void epd_app_font_remote_stats(epd_app_font_remote_stats_t *out);
typedef struct {
    uint64_t count, glyphs, completed, exec_ticks, source_calls, source_bytes;
    uint64_t failures, retries, fallbacks;
    uint32_t max_batch_ticks;
} epd_app_font_batch_profile_t;
typedef struct {
    /* kind: 0 idle, 1 measure, 2 render. Current batch, not cumulative. */
    uint32_t kind, font_id, generation, size_px, weight, oblique;
    uint32_t glyphs, completed, exec_ticks;
} epd_app_font_active_profile_t;
typedef struct {
    epd_app_font_batch_profile_t measure, render;
    epd_app_font_io_stats_t io;
    epd_app_font_io_timing_t open, fstat, seek, close;
    uint64_t cache_hits, cache_misses, cache_bypasses, metadata_hits, metadata_misses;
    epd_app_font_active_profile_t active;
    uint32_t ticks_per_second;
} epd_app_font_remote_profile_t;
/* Quiet, memory-only snapshot of all remote fonts since boot. Does not initialize,
 * allocate, print, wait for drawing, acquire storage, or submit compute work.
 * measure/render count finished batch attempts, including empty metrics queries;
 * glyphs counts submitted records, completed counts returned records. exec_ticks
 * includes source service and transport waits, not just ACPU execution. IO fields
 * account completed transactions; read/seek are nested within lock_hold and exec.
 * Subtract cumulative fields around an interval. Include active.exec_ticks in
 * each endpoint's total exec time to cover a partially published batch; active
 * itself and max_* fields are gauges, not counters to subtract. Unfinished IO
 * is not included. All ticks use ticks_per_second. NULL is ignored. */
void epd_app_font_remote_profile(epd_app_font_remote_profile_t *out);
enum {
    EPD_FONT_DRIVE_READ, EPD_FONT_DRIVE_OUTPUT, EPD_FONT_DRIVE_IDLE,
    EPD_FONT_DRIVE_COMMAND, EPD_FONT_DRIVE_OTHER, EPD_FONT_DRIVE_COUNT
};
typedef struct { uint64_t calls, pipe_ticks; } epd_app_font_drive_op_t;
typedef struct {
    epd_app_font_drive_op_t op[EPD_FONT_DRIVE_COUNT];
    uint64_t step_ticks, supply_calls, supply_ticks, consume_calls, consume_ticks;
    uint32_t ticks_per_second;
} epd_app_font_drive_profile_t;
/* HCPU tick totals for completed pipe_step calls, classified by the returned
 * operation; failed steps use OTHER. pipe_ticks excludes the exact supply_read
 * and consume_output calls. Thus sum(pipe_ticks)+supply_ticks+consume_ticks
 * equals step_ticks. This includes dispatch, ACPU execution and cache work,
 * not just IPC latency. In-flight steps and work outside pipe_step are omitted.
 * Snapshot is allocation/IO-free. Subtract cumulative fields across a window. */
void epd_app_font_drive_profile(epd_app_font_drive_profile_t *out);

typedef struct {
    uint64_t batches, reads, idles, outputs, run_ticks;
    uint64_t read_wait_ticks, idle_wait_ticks, output_wait_ticks;
    uint32_t ticks_per_second;
} epd_app_font_step_profile_t;
/* Quiet cumulative snapshot of accepted FINAL packets, summed over font open,
 * measure and render. Cancelled batches without FINAL contribute no snapshot.
 * Run intervals exclude exchanges but retain interrupts/preemption. Waits use
 * ACPU ticks, which may stop in park: they are not HCPU wall-clock durations.
 * Final OUTPUT acknowledgement and initial COMMAND delivery are excluded.
 * Subtract snapshots; the existing remote profile supplies HCPU wall time.
 * Zero ticks_per_second means no timed packet has arrived. No allocation/IO. */
void epd_app_font_step_profile(epd_app_font_step_profile_t *out);
/* Optional derived A8 pixels in the reader's shared font LRU. The key is an
 * immutable resolved source or wrapper instance, its glyph ID and transformation
 * identity.
 * Call drop before changing/freeing/reusing that instance. UI preparation must
 * wait for drawing before generating pixels; draw callbacks must not wait.
 * Acquire/store return one owned reference in *entry, or NULL in both outputs.
 * Store copies pixels synchronously and never takes ownership of bitmap. Keep
 * source glyph references until generation/copy finishes. On failure retain
 * the immediate bitmap path. Release each acquired entry after drawing; track
 * cache references separately from immediate bitmap allocations. */
const lv_draw_buf_t *epd_app_font_derived_acquire(const lv_font_t *font, uint32_t glyph,
                                                uint32_t style, lv_cache_entry_t **entry);
const lv_draw_buf_t *epd_app_font_derived_store(const lv_font_t *font, uint32_t glyph,
                                              uint32_t style, const lv_draw_buf_t *bitmap,
                                              lv_cache_entry_t **entry);
void epd_app_font_derived_release(lv_cache_entry_t *entry);
/* UI thread only. Outstanding references remain valid until released. */
void epd_app_font_derived_drop(const lv_font_t *font);
/* Optional immutable font bytes, valid for the firmware lifetime. File-backed
 * fonts, including the block-compressed built-in font, return NULL and size 0. */
const void *epd_app_builtin_font_data(size_t *size);

#endif
