/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_memory.h"
#include "epd_input_test.h"
#include "epd_resource_runtime.h"
#include "epd_activity_work.h"
#include "platform/epd_app_font.h"
#include "platform/epd_reader_font.h"
#include "reader_internal.h"
#include "reader_i18n.h"
#include "reader_font.h"
#include "reader_geometry.h"
#include "reader_view_touch_ui.h"
#include "reader_note_ui.h"
#include "reader_compute_client.h"
#include "reader_page_cache.h"
#include "reader_frame_store.h"
#include "epd_compute.h"
#include "../../../components/compute_reader_render/include/epd_reader_render.h"
#include "../../../components/compute_reader_render/include/epd_reader_render_image.h"
#include "../../../components/compute_reading_data/include/epd_reading_data.h"
#include "ui_settings.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define VIEW_TEXT EPD_READER_TEXT
#define VIEW_RUNS EPD_READER_RUNS
#define VIEW_HEIGHT (view.status.bottom - view.status.top)
#define VIEW_BLOCK EPD_READER_BATCH
#define VIEW_METRICS_BATCH 32u
#define VIEW_METRICS_WINDOW 64u
#define VIEW_LAYOUT_AHEAD 4u
#define VIEW_NEAR_PAGES 3u
#define VIEW_BURST_TURN_MS 2000u
#define VIEW_OPPOSITE_PIXELS 1u
#define VIEW_AHEAD_WAIT_MS 2000u
#define VIEW_GLYPH_BURST 8u
#define VIEW_GLYPH_BURST_MS 4u
#define VIEW_INDEX_MAGIC 0x58445642u
#define VIEW_INDEX_VERSION 2u
#define VIEW_INDEX_CHECKPOINT_PAGES 32u
#define VIEW_INDEX_CHECKPOINT_MS 30000u
#define VIEW_LAYOUT_VERSION EPD_READER_LAYOUT_CACHE_VERSION
#define VIEW_CACHE_BUDGET (2u * 1024u * 1024u)
#define VIEW_FRAME_BUDGET (6u * 1024u * 1024u)
#define VIEW_RENDER_WORKSPACE (64u * 1024u)
#define VIEW_NEIGHBOR_PRIORITY 4096u
#define VIEW_FRAME_WAIT_MS 1000u
#define VIEW_PRESSURE_WAIT_MS 5000u
#define VIEW_RENDER_COMMANDS 32u
#define VIEW_RENDER_SLICE_MS 4u
#ifndef VIEW_BACKGROUND_TRACE
#define VIEW_BACKGROUND_TRACE 0
#endif
#define VIEW_INDEX_LIMIT (512u * 1024u)
#define VIEW_INDEX_GLOBAL (3u * VIEW_INDEX_LIMIT)
static size_t layout_bytes, image_bytes, frame_bytes, index_bytes;
static epd_resource_cache_t view_resource;
static bool view_resource_pending;
static size_t view_resource_requested;
static uint32_t view_resource_backend;
static void view_resource_publish(void);
static void view_resource_step(void);
#define VIEW_PREVIEW_ORIGIN 0x40000000u
#define VIEW_BACK_WINDOW_MIN 4096u
#define VIEW_BACK_WINDOW_MAX 16384u
#define VIEW_VISITED_PAGES 128u

static void *view_allocate(size_t bytes, epd_app_memory_t region)
{
    epd_memory_region_t backend = region == EPD_APP_PSRAM ? EPD_MEMORY_REGION_HCPU_PSRAM : EPD_MEMORY_REGION_SRAM;
    if (epd_resource_context_reserve(backend, epd_app_allocation_peak(bytes)) != EPD_RESOURCE_OK) return NULL;
    void *memory = epd_app_alloc(bytes, region);
    if (memory && view_resource.provider &&
        epd_resource_cache_adopt(&view_resource, memory) != EPD_RESOURCE_OK) {
        epd_app_free(memory);
        return NULL;
    }
    return memory;
}
static void *view_resize(void *old, size_t old_bytes, size_t bytes)
{
    void *fresh = view_allocate(bytes, EPD_APP_PSRAM);
    if (!fresh) return NULL;
    if (old) memcpy(fresh, old, old_bytes < bytes ? old_bytes : bytes);
    epd_app_free(old);
    return fresh;
}
static int view_reserve_result;
static bool view_memory_room(size_t bytes)
{
    view_reserve_result = epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM,
        epd_app_allocation_peak(bytes));
    return view_reserve_result == EPD_RESOURCE_OK;
}
static epd_memory_recovery_result_t view_memory_recover(epd_memory_recovery_t *request,
                                                        size_t physical, bool failed)
{
    epd_memory_backend_t backend;
    if (request->used) return epd_memory_recovery_poll(request, false);
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend)) return EPD_MEMORY_RECOVERY_DENIED;
    return epd_memory_recovery_begin(request, backend, physical, failed);
}

typedef epd_reader_run_t view_run_t;
typedef struct {
    EPD_READER_PAGE_FIELDS(document_cursor_t);
    uint32_t loaded, generation, measured, prepared;
    document_record_t record;
    char data[DOCUMENT_TEXT_LIMIT + 1];
    epd_reader_glyph_t decoded[DOCUMENT_TEXT_LIMIT];
    document_image_t image;
    bool has_record;
    bool blocked, blocked_required, read_foreground;
    uint32_t blocked_intent;
    document_record_request_t *read_request;
    rt_tick_t wait_started;
    unsigned wait_kind;
    uint32_t warm_run;
} view_page_t;
typedef char layout_prefix_matches[(offsetof(view_page_t, loaded) == sizeof(epd_reader_page_t)) ? 1 : -1];
typedef char layout_record_limit_matches[(DOCUMENT_TEXT_LIMIT == EPD_READER_RECORD_TEXT) ? 1 : -1];
typedef char layout_style_matches[((unsigned)BOOK_STYLE_PREFORMAT == EPD_READER_PREFORMAT &&
    (unsigned)BOOK_STYLE_CENTER == EPD_READER_CENTER && (unsigned)BOOK_STYLE_RIGHT == EPD_READER_RIGHT) ? 1 : -1];
static uint32_t page_generation;
typedef struct { uint32_t record; unsigned level; } toc_entry_t;
typedef struct view_image {
    struct view_image *next;
    document_t *document;
    document_picture_t picture;
    document_image_request_t *request;
    uint32_t record;
    unsigned references;
    size_t bytes;
    int state;
    bool retried;
    rt_tick_t started;
} view_image_t;
typedef struct view_widget {
    struct view_widget *next;
    lv_obj_t *object;
    uint32_t link;
    bool image, used, note;
    reader_view_touch_t touch;
} view_widget_t;
typedef struct {
    uint32_t magic, version, layout, end, count, toc_count, toc_scanned, complete, checksum;
} view_index_header_t;
typedef char index_header_matches[(sizeof(view_index_header_t) == sizeof(epd_reading_index_header_t)) ? 1 : -1];
typedef struct {
    document_cursor_t start, end;
    uint32_t layout;
    bool eof;
} view_visited_page_t;
typedef struct view_frame view_frame_t;
static view_frame_t *render_frame;
typedef struct view_state {
    uint32_t input_order;
    bool input_enabled;
    struct view_state *next;
    document_t *document;
    document_reader_t *index_reader, *page_reader, *aux_reader;
    document_progress_t progress;
    reader_view_t status;
    char title[DOCUMENT_TITLE_MAX];
    unsigned wanted, count, capacity, toc_count, toc_capacity;
    /* Sparse entries are boundaries at multiples of sparse_stride. The
     * independent total keeps counting after the resident table fills. */
    unsigned sparse_stride, sparse_pages, sparse_next, sparse_goal;
    bool sparse_seek, toc_limited, counting_only;
    bool numeric_target, whole_source;
    bool count_loaded, count_saved;
    document_cursor_t counting_origin[2];
    document_cursor_t numbered_cursor;
    unsigned numbered_page;
    reader_save_fn save;
    document_t *(*reopen)(void *user, char *error, size_t size);
    void *user;
    reader_position_t saved_position;
    uint32_t toc_scanned, layout, display_layout;
    view_visited_page_t visited[VIEW_VISITED_PAGES];
    unsigned visited_count, visited_next;
    bool complete, restoring, display_pending, notified, index_loaded, index_loading, index_saved, from_cache;
    document_record_request_t *back_request;
    uint32_t back_request_offset;
    unsigned index_saved_count;
    rt_tick_t index_saved_at;
    bool index_save_failed;
    int direction;
    reader_error_t failure;
    reader_error_t request_failure;
    uint32_t request_failure_intent;
    document_cursor_t anchor;
    /* Preview publication is independent of the chapter's page-number pass. */
    bool preview;
    bool preview_cache_active;
    unsigned preview_page;
    bool preview_back_scanning, preview_back_ready;
    bool preview_back_seeking;
    bool preview_back_exact, preview_back_eof;
    uint32_t preview_back_scan, preview_back_low;
    unsigned preview_back_headers;
    rt_tick_t preview_back_started, preview_back_seek_ticks;
    bool preview_back_blocked, preview_back_failed;
    unsigned preview_back_base;
    document_cursor_t preview_back_end;
    int turn_pending, turn_remaining;
    bool preview_clipped;
    document_cursor_t preview_end;
    document_cursor_t *pages;
    toc_entry_t *toc;
    char chapter_title[256];
    uint32_t chapter_begin;
    unsigned chapter_toc, chapter_count;
    bool chapter_cached;
    view_page_t *index, *building, *display;
    reader_page_cache_t cache;
    view_page_t *prefetch;
    unsigned prefetch_number, cache_blocked;
    document_t *prefetch_document;
    uint32_t prefetch_layout, prefetch_session;
    unsigned ahead_page;
    uint32_t ahead_layout, ahead_intent;
    rt_tick_t ahead_started;
    bool ahead_attempted;
    uint32_t blocked_epoch;
    uint32_t cache_begin, cache_end;
    bool cache_chapter_known;
    bool allow_pixels, allow_glyphs, glyph_turn, glyph_waited, background_paused;
    unsigned glyph_steps, work_text_steps, work_glyph_calls;
    rt_tick_t glyph_ticks, work_glyph_ticks, work_draw_ticks, work_logged_at;
    rt_tick_t published_at;
    bool trace_published;
    rt_tick_t turn_started, layout_ready_at, images_ready_at, cache_logged_at;
    rt_tick_t trace_read_ticks, trace_metric_ticks, trace_layout_ticks, compute_started;
    rt_tick_t trace_record_wait, trace_glyph_wait;
    unsigned near_record_queued, near_record_ready;
    rt_tick_t near_started, near_service_ticks;
    uint32_t near_intent;
    unsigned near_number, near_steps;
    bool near_tracing;
    rt_tick_t work_started, work_budget;
    bool work_exhausted;
    unsigned trace_metric_calls;
    bool compute_traced;
    bool trace_pending, trace_hit, trace_layout_ready, trace_images_ready, trace_font_wait;
    uint32_t wait_trace_intent;
    unsigned wait_trace_mask;
    unsigned cache_hits, cache_misses, cache_logged_count;
    uint32_t prefetch_record, prefetch_publication;
    lv_font_t *fonts[3], *fallbacks[3], *retired[6];
    lv_font_t *oblique[3], *retired_oblique[3];
    uint32_t font_id, font_revision, font_identity;
    reader_geometry_t font_area;
    /* A rejected request is distinct from the configuration still on screen. */
    bool font_failed, font_metrics_ready;
    bool font_memory_blocked;
    uint32_t font_blocked_intent;
    uint32_t failed_font_id, failed_font_revision;
    reader_geometry_t failed_font_area;
    unsigned appearance;
    unsigned failed_font_settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    lv_obj_t *root;
    view_image_t *images;
    view_widget_t *widgets;
    char link_target[DOCUMENT_TEXT_LIMIT + 1];
    uint32_t link_scan;
    reader_compute_job_t *compute_job, *compute_spare;
    bool compute_foreground;
    view_page_t *compute_page;
    uint32_t session, intent;
    unsigned references;
    view_frame_t *frames, *rendering;
    epd_app_font_page_t pins;
    uint32_t pin_run, pin_byte, pin_generation;
    bool backward_target, selected, displayed, parked;
    bool boundary_target;
    unsigned chapter_percent;
    document_record_request_t *progress_request;
    document_cursor_t boundary_anchor;
    bool initial_target_ready;
    document_cursor_t initial_anchor;
    int deferred_turns, segment_turns;
    bool forward_step;
    document_cursor_t forward_from;
    struct view_state *previous, *following;
    epd_app_font_metrics_t metrics[3], italic_metrics[3];
    bool initialized, init_attempted, init_alloc_retried, target_pending;
    epd_memory_recovery_t init_recovery;
    bool init_blocked;
    uint32_t init_blocked_intent;
    uint32_t init_blocked_epoch;
    uint32_t published_intent, published_generation;
    bool frame_published;
    bool pixels_blocked, pixels_memory_blocked, pixels_retry_ready;
    uint32_t pixels_blocked_intent;
    unsigned pixels_blocked_priority;
    size_t pixels_blocked_bytes, pixels_blocked_available;
    bool image_memory_reclaimed, image_pressure_requested;
    uint32_t image_memory_intent, image_memory_layout, image_pressure_generation;
    rt_tick_t image_memory_started;
    epd_memory_recovery_t image_recovery;
    unsigned neighbor_attempts;
    uint32_t neighbor_publication, neighbor_intent;
    size_t neighbor_epoch;
    size_t window_state_charge, window_workspace_charge;
    unsigned window_attempts;
    uint32_t window_attempt_intent;
    size_t window_attempt_epoch;
    uint32_t window_probe_revision;
    epd_memory_recovery_t window_recovery[2];
    uint32_t navigation_link, navigation_toc;
} view_state_t;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
typedef struct {
    uintptr_t context;
    document_cursor_t cursor, end;
    uint32_t layout, generation;
    unsigned number, chapter;
    bool mapped, known, present, complete, pixelsready, cold, cancelled, hot;
} view_window_row_t;
typedef struct {
    uint32_t session, intent, serial, published_intent, published_generation;
    rt_tick_t tick;
    unsigned hot;
    bool ready, published, paused, suspended;
    view_window_row_t pages[7]; /* -3, -2, -1, current, +1, +2, +3 */
} view_window_snapshot_t;
#endif

static view_state_t *state, *primary_state, *parked_state;
#define view (*state)
static uint32_t input_sequence;
static void view_input_set(view_state_t *owner, bool enabled)
{
    owner->input_enabled = enabled;
    if (!++input_sequence) ++input_sequence;
    owner->input_order = input_sequence;
}

static struct {
    uint32_t session, layout, chapters, next, total;
    bool complete, failed;
    view_state_t *counter;
    uint32_t *ends;
    size_t bytes;
} pagination;
static void pagination_snapshot(view_state_t *context, reader_view_t *info);
static void state_retire(view_state_t *retired);
static view_state_t *state_neighbor(document_t *document, bool backward);
static bool neighbor_step(bool pixels);
static void window_clear(void);
static bool window_pixels_pending(view_state_t *owner);
static bool window_runnable(view_state_t *owner, view_state_t *current);
static bool pagination_requested(void);
static bool window_prepare(view_state_t *current, bool pixels);
static bool window_reclaim(unsigned priority);
static bool window_admit(size_t bytes, unsigned priority);
/* Four off-screen slots extend the one-hop reader without owning a graph. */
static struct { view_state_t *owner; int hop; } window_slots[4];

static unsigned window_contexts(view_state_t *current, view_state_t **out)
{
    unsigned count = 0;
    view_state_t *all[7] = {current, current ? current->previous : NULL,
        current ? current->following : NULL, window_slots[0].owner,
        window_slots[1].owner, window_slots[2].owner, window_slots[3].owner};
    for (unsigned i = 0; i < 7; ++i) if (all[i]) {
        bool duplicate = false;
        for (unsigned j = 0; j < count; ++j) duplicate |= out[j] == all[i];
        if (!duplicate) out[count++] = all[i];
    }
    return count;
}
static view_state_t *window_owner(int hop)
{
    if (!primary_state) return NULL;
    if (!hop) return primary_state;
    if (hop == -1) return primary_state->previous;
    if (hop == 1) return primary_state->following;
    for (unsigned i = 0; i < 4; ++i)
        if (window_slots[i].owner && window_slots[i].hop == hop) return window_slots[i].owner;
    return NULL;
}
static bool window_extra(const view_state_t *owner)
{
    for (unsigned i = 0; i < 4; ++i) if (window_slots[i].owner == owner) return true;
    return false;
}
static bool window_charge(size_t bytes)
{
    if (bytes > VIEW_CACHE_BUDGET) return false;
    rt_enter_critical();
    bool fits = layout_bytes <= VIEW_CACHE_BUDGET - bytes;
    if (fits) layout_bytes += bytes;
    rt_exit_critical();
    return fits;
}
static void window_uncharge_bytes(size_t bytes)
{ rt_enter_critical(); layout_bytes -= bytes; rt_exit_critical(); }
static unsigned window_number(const view_state_t *owner)
{ return owner->preview_cache_active ? owner->preview_page : owner->wanted; }
static bool window_start(const view_state_t *owner, document_cursor_t cursor)
{
    if (!cursor.record && !cursor.byte) return true;
    document_anchor_t anchor;
    return document_source_position(owner->document, cursor.record, cursor.byte, &anchor) &&
        anchor.offset <= owner->progress.source_begin && !anchor.skip_utf8;
}
static bool window_end(const view_state_t *owner, document_cursor_t cursor, bool eof)
{
    return eof || (owner->progress.complete && !owner->progress.result &&
        cursor.record == owner->progress.end && !cursor.byte);
}
static bool window_page(view_state_t *owner, unsigned number, document_cursor_t *start, bool *eof)
{
    if (owner->status.ready && owner->display && number == window_number(owner)) {
        *start = owner->display->start;
        *eof = window_end(owner, owner->display->cursor, owner->display->eof); return true;
    }
    reader_cached_page_t *p = reader_page_cache_find(&owner->cache, number, owner->layout);
    if (!p) return false;
    *start = (document_cursor_t){p->start.record, p->start.byte};
    *eof = window_end(owner, (document_cursor_t){p->end.record, p->end.byte}, p->eof);
    return true;
}
static bool window_entry(view_state_t *owner, int direction, unsigned *number)
{
    *number = window_number(owner);
    if (!owner->status.ready) return true;
    /* TXT slices contain context before their requested entry. Match the
     * same document target used by target_resolve, not the context boundary. */
    if (!owner->progress.chapters) {
        document_cursor_t target;
        document_anchor_t source;
        if (document_target_cursor(owner->document, &target.record, &target.byte) > 0 &&
            document_source_position(owner->document, target.record, target.byte, &source) &&
            source.unit == 1u) {
            if (owner->display && owner->display_layout == owner->layout) {
                document_cursor_t edge = direction > 0 ? owner->display->start : owner->display->cursor;
                if (edge.record == target.record && edge.byte == target.byte) return true;
            }
            for (reader_cached_page_t *p = owner->cache.first; p; p = p->next) {
                if (p->layout != owner->layout) continue;
                uint32_t record = direction > 0 ? p->start.record : p->end.record;
                uint32_t byte = direction > 0 ? p->start.byte : p->end.byte;
                if (record == target.record && byte == target.byte) {
                    *number = p->number; return true;
                }
            }
            return false;
        }
    }
    document_cursor_t start; bool eof;
    if (window_page(owner, *number, &start, &eof) &&
        (direction > 0 ? window_start(owner, start) : eof)) return true;
    for (reader_cached_page_t *p = owner->cache.first; p; p = p->next)
        if (p->layout == owner->layout && (direction > 0 ?
            window_start(owner, (document_cursor_t){p->start.record,p->start.byte}) :
            window_end(owner, (document_cursor_t){p->end.record,p->end.byte}, p->eof))) {
            *number = p->number; return true;
        }
    return false;
}
typedef struct { view_state_t *owner; unsigned number, distance; } window_page_ref_t;
typedef struct {
    window_page_ref_t pages[3];
    unsigned count;
    int missing_hop;
    view_state_t *predecessor;
} window_route_t;
static window_route_t window_route_limit(int direction, unsigned limit)
{
    window_route_t route = {0};
    view_state_t *owner = primary_state;
    if (!owner || !owner->status.ready) return route;
    unsigned number = window_number(owner);
    int hop = 0;
    for (unsigned distance = 1; distance <= limit; ++distance) {
        document_cursor_t start; bool eof;
        if (!window_page(owner, number, &start, &eof)) break;
        bool boundary = direction > 0 ? eof : window_start(owner, start);
        if (boundary) {
            hop += direction;
            if (hop < -3 || hop > 3) break;
            view_state_t *next = window_owner(hop);
            if (!next) {
                if (document_adjacent_available(owner->document, direction)) {
                    route.missing_hop = hop;
                    route.predecessor = owner;
                }
                break;
            }
            owner = next;
            if (!window_entry(owner, direction, &number)) break;
        } else {
            if (direction > 0 ? number == UINT_MAX : number == 0) break;
            number += direction;
        }
        route.pages[route.count++] = (window_page_ref_t){owner, number, distance};
    }
    return route;
}
static unsigned leading_pixel_pages(void);
static window_route_t window_route(int direction)
{
    int leading = primary_state && primary_state->direction < 0 ? -1 : 1;
    return window_route_limit(direction, direction == leading ? leading_pixel_pages() : VIEW_OPPOSITE_PIXELS);
}
static bool window_neighbor_work(view_state_t *current)
{
    for (int direction = -1; direction <= 1; direction += 2) {
        window_route_t route = window_route(direction);
        if (route.missing_hop) return true;
        for (unsigned i = 0; i < route.count; ++i) {
            view_state_t *owner = route.pages[i].owner;
            if (owner != current && window_runnable(owner, current)) return true;
        }
    }
    return false;
}
static unsigned window_priority(const view_state_t *owner, unsigned number)
{
    if (!primary_state) return UINT_MAX;
    int first = primary_state->direction < 0 ? -1 : 1;
    for (unsigned side = 0; side < 2; ++side) {
        window_route_t route = window_route_limit(side ? -first : first, VIEW_NEAR_PAGES);
        for (unsigned i = 0; i < route.count; ++i)
            if (route.pages[i].owner == owner && route.pages[i].number == number)
                return route.pages[i].distance + (side ? 3u : 0u);
    }
    return UINT_MAX;
}

static bool window_prewarm_rank(unsigned rank)
{
    return rank <= leading_pixel_pages() ||
           (rank > VIEW_NEAR_PAGES && rank <= VIEW_NEAR_PAGES + VIEW_OPPOSITE_PIXELS);
}


static void core_seek(unsigned page);
static void core_seek_cursor(document_cursor_t cursor);
static bool core_waiting(void);
static void near_trace_finish(const char *reason);
static void core_set_error_code(reader_error_t error);
static unsigned elapsed_ms(rt_tick_t start);
static void target_finish(void);
static bool cursor_at_segment_start(document_cursor_t cursor);
static void font_priority(bool foreground);
static bool frame_reclaim_layout(size_t bytes, unsigned request_priority);
static bool frame_reclaim_pages(size_t bytes);
static bool near_frames_pending(void);
static bool near_direction_pending(void);
static bool layout_ahead_ready(void);
static bool layout_ahead_expired(void);
static unsigned indexed_pages(const view_state_t *s)
{ return s->sparse_stride ? s->sparse_pages : s->count; }

static void compute_cancel(void)
{
    reader_compute_cancel(view.compute_job);
    view.compute_job = NULL;
    view.compute_page = NULL;
    view.compute_traced = false;
}
static reader_compute_job_t *compute_acquire(int *result)
{
    view_state_t *contexts[9]; unsigned count = window_contexts(primary_state, contexts);
    if (parked_state) contexts[count++] = parked_state;
    if (pagination.counter) contexts[count++] = pagination.counter;
    for (unsigned i = 0; i < count; ++i) if (contexts[i] != state && contexts[i]->compute_spare) {
        reader_compute_job_t *job = contexts[i]->compute_spare;
        contexts[i]->compute_spare = NULL;
        *result = EPD_COMPUTE_OK;
        return job;
    }
    return reader_compute_create(result);
}

static void navigation_clear(void)
{
    document_navigation_release(view.document);
    view.navigation_link = view.navigation_toc = 0;
}

static void link_clear(void)
{
    navigation_clear();
    document_target_require(view.document, false);
    view.link_target[0] = 0;
    view.chapter_percent = 0;
    if (view.progress_request) {
        document_record_cancel(view.progress_request);
        view.progress_request = NULL;
    }
}

static bool view_interrupted(void);
static void fail(reader_error_t error)
{
    if (view_interrupted()) return;
    link_clear();
    if (view.failure != error)
        rt_kprintf("reader failure session=%u intent=%u error=%u end=%u document_result=%d selected=%u\n",
            (unsigned)view.session, (unsigned)view.intent, (unsigned)error,
            (unsigned)view.progress.end, view.progress.result, (unsigned)view.selected);
    view.failure = error;
}
static bool cursor_before(document_cursor_t a, document_cursor_t b)
{ return a.record < b.record || (a.record == b.record && a.byte < b.byte); }
static bool cursor_equal(document_cursor_t a, document_cursor_t b)
{ return a.record == b.record && a.byte == b.byte; }
static void visited_clear(view_state_t *owner)
{
    owner->visited_count = owner->visited_next = 0;
}
static void visited_record(view_state_t *owner, const epd_reader_page_t *page, uint32_t layout)
{
    document_cursor_t start = {page->start.record, page->start.byte};
    document_cursor_t end = {page->cursor.record, page->cursor.byte};
    if (layout != owner->layout || !page->done || !page->count ||
        !cursor_before(start, end) || cursor_before((document_cursor_t){owner->progress.end, 0}, end)) return;
    unsigned slot = owner->visited_next;
    for (unsigned i = 0; i < owner->visited_count; ++i)
        if (owner->visited[i].layout == layout && cursor_equal(owner->visited[i].end, end)) {
            owner->visited[i] = (view_visited_page_t){start, end, layout, page->eof != 0};
            return;
        }
    owner->visited[slot] = (view_visited_page_t){start, end, layout, page->eof != 0};
    owner->visited_next = (slot + 1u) % VIEW_VISITED_PAGES;
    if (owner->visited_count < VIEW_VISITED_PAGES) ++owner->visited_count;
}
static bool visited_find(document_cursor_t end, view_visited_page_t *page)
{
    if (cursor_before((document_cursor_t){view.progress.end, 0}, end)) return false;
    for (unsigned i = 0; i < view.visited_count; ++i) {
        const view_visited_page_t *p = &view.visited[i];
        if (p->layout == view.layout && cursor_equal(p->end, end) && cursor_before(p->start, end)) {
            *page = *p;
            return true;
        }
    }
    return false;
}
static void visited_forget(document_cursor_t end)
{
    for (unsigned i = 0; i < view.visited_count; ++i)
        if (view.visited[i].layout == view.layout && cursor_equal(view.visited[i].end, end))
            view.visited[i].start = end;
}
static bool preview_visible(void)
{
    return view.preview && view.status.ready && view.display_layout == view.layout &&
        cursor_equal(view.display->start, view.anchor);
}
static unsigned font_slot(unsigned style)
{ return style & BOOK_STYLE_HEADING ? 2 : style & BOOK_STYLE_BOLD ? 1 : 0; }
static const lv_font_t *font_for(unsigned style)
{
    unsigned slot = font_slot(style);
    return (style & BOOK_STYLE_ITALIC) && view.oblique[slot] ? view.oblique[slot] : view.fonts[slot];
}
static unsigned width_available(void) { return view.status.content_width; }
static void page_reset(view_page_t *p, document_cursor_t start)
{
    if (view.compute_page == p) compute_cancel();
    document_record_cancel(p->read_request); p->read_request = NULL;
    memset(p, 0, offsetof(view_page_t, runs));
    p->done = p->eof = 0;
    p->has_record = false; p->loaded = UINT32_MAX; p->measured = p->prepared = 0;
    p->warm_run = 0;
    memset(&p->record, 0, sizeof(p->record));
    memset(&p->image, 0, sizeof(p->image));
    p->start = p->cursor = start;
    p->collect = true;
    p->generation = ++page_generation;
    p->wait_kind = 0;
    p->blocked = p->blocked_required = p->read_foreground = false;
    if (p == view.prefetch) {
        view.prefetch_document = view.document;
        view.prefetch_layout = view.layout; view.prefetch_session = view.session;
    }
}
static void preview_start(document_cursor_t cursor)
{
    near_trace_finish("reset");
    view.sparse_seek = false;
    view.numbered_page = 0;
    document_record_cancel(view.back_request); view.back_request = NULL;
    reader_page_cache_clear(&view.cache);
    view.cache_blocked = view.prefetch_number = UINT_MAX;
    view.preview_cache_active = view.preview = true;
    view.preview_page = VIEW_PREVIEW_ORIGIN;
    view.cache.wanted = view.preview_page; view.cache.window_dirty = true;
    view.preview_back_scanning = view.preview_back_ready = false;
    view.preview_back_seeking = false;
    view.preview_back_blocked = view.preview_back_failed = false;
    view.preview_clipped = false;
    view.anchor = cursor; view.restoring = true;
    view.turn_pending = view.turn_remaining = 0;
    view.forward_step = false;
    page_reset(view.building, cursor);
    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
}
static bool reserve_pages(unsigned n)
{
    size_t toc_bytes = view.toc_capacity * sizeof(*view.toc);
    size_t maximum = (VIEW_INDEX_LIMIT - sizeof(view_index_header_t) - toc_bytes) / sizeof(*view.pages);
    if (n > maximum || n > READER_PAGE_MAX + 1) return false;
    if (n <= view.capacity) return true;
    unsigned capacity = view.capacity ? view.capacity * 2 : 256;
    if (capacity < n) capacity = n;
    if (capacity > maximum) capacity = (unsigned)maximum;
    size_t delta = (capacity - view.capacity) * sizeof(*view.pages);
    if (index_bytes + delta > VIEW_INDEX_GLOBAL) return false;
    size_t charge = view.window_state_charge ? delta : 0;
    if (charge && !window_charge(charge)) return false;
    void *data = view_resize(view.pages, view.capacity * sizeof(*view.pages), capacity * sizeof(*view.pages));
    if (!data) { window_uncharge_bytes(charge); return false; }
    view.window_workspace_charge += charge;
    index_bytes += delta; view.pages = data; view.capacity = capacity;
    return true;
}
static bool add_toc(uint32_t record, unsigned level)
{
    if (view.counting_only || view.toc_limited) return true;
    if (view.toc_count == view.toc_capacity)
    {
        if (view.toc_count >= SIZE_MAX / sizeof(*view.toc))
        { view.toc_limited = true; return true; }
        unsigned capacity = view.toc_capacity ? view.toc_capacity * 2 : 32;
        size_t maximum = (VIEW_INDEX_LIMIT - sizeof(view_index_header_t) -
            view.capacity * sizeof(*view.pages)) / sizeof(*view.toc);
        if (capacity > maximum) capacity = (unsigned)maximum;
        size_t delta = (capacity - view.toc_capacity) * sizeof(*view.toc);
        if (capacity <= view.toc_count || index_bytes + delta > VIEW_INDEX_GLOBAL)
        { view.toc_limited = true; return true; }
        void *p = view_resize(view.toc, view.toc_capacity * sizeof(*view.toc), capacity * sizeof(*view.toc));
        if (!p) { view.toc_limited = true; return true; }
        index_bytes += delta; view.toc = p; view.toc_capacity = capacity;
    }
    view.toc[view.toc_count++] = (toc_entry_t){record, level};
    return true;
}
static unsigned utf8_letter(const char *p, unsigned n, uint32_t *letter)
{
    unsigned char c = (unsigned char)*p;
    unsigned length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (length > n || c < 0xc2 || c > 0xf4)
    { *letter = c < 0x80 ? c : '?'; return 1; }
    uint32_t value = c & ((1u << (7 - length)) - 1);
    for (unsigned i = 1; i < length; ++i)
    {
        if (((unsigned char)p[i] & 0xc0) != 0x80) { *letter = '?'; return 1; }
        value = (value << 6) | ((unsigned char)p[i] & 0x3f);
    }
    *letter = value;
    return length;
}
static bool page_required(const view_page_t *p)
{
    if (!view.selected) return false;
    if (p == view.building) return true;
    if (p == view.prefetch) return view.preview_back_scanning && view.turn_pending < 0;
    return p == view.index && !preview_visible() &&
        (view.restoring || view.link_target[0] || view.wanted >= view.count);
}
static bool request_memory_failed(const view_state_t *owner)
{
    return owner->request_failure && owner->request_failure_intent == owner->intent;
}
static bool page_work_allowed(const view_page_t *p)
{
    return !p->blocked || p->blocked_intent != view.intent ||
        (!p->blocked_required && page_required(p));
}
static int page_memory_block(view_page_t *p, const char *stage, reader_error_t error)
{
    if (view_interrupted()) return 0;
    if (!page_work_allowed(p)) return 0;
    p->blocked = true; p->blocked_intent = view.intent;
    p->blocked_required = page_required(p);
    if (p->blocked_required) {
        view.request_failure = error; view.request_failure_intent = view.intent;
    }
    rt_kprintf("reader resource_block session=%u intent=%u stage=%s generation=%u record=%u required=%u error=%u\n",
        (unsigned)view.session, (unsigned)view.intent, stage, (unsigned)p->generation,
        (unsigned)p->cursor.record, (unsigned)p->blocked_required, (unsigned)error);
    return 0;
}
static int page_error(view_page_t *p, const char *stage, int result)
{
    if (view_interrupted()) return -1;
    if (!page_work_allowed(p)) return 0;
    rt_kprintf("reader page_error session=%u intent=%u stage=%s result=%d generation=%u record=%u byte=%u kind=%u length=%u\n",
        (unsigned)view.session, (unsigned)view.intent, stage, result, (unsigned)p->generation,
        (unsigned)p->cursor.record, (unsigned)p->cursor.byte, (unsigned)p->record.kind, (unsigned)p->record.length);
    p->blocked = true; p->blocked_intent = view.intent;
    p->blocked_required = page_required(p);
    return p->blocked_required ? -1 : 0;
}
static int decode_at(view_page_t *p, unsigned at, bool trace)
{
    if (p->decoded[at].bytes) return 1;
    uint32_t letter;
    unsigned bytes = utf8_letter(p->data + at, p->record.length - at, &letter);
    epd_app_font_glyph_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    if (letter != '\r' && letter != '\n')
    {
        int ready = epd_app_font_glyph_ready(font_for(p->record.style), letter == '\t' ? ' ' : letter, 0, false);
        if (ready == EPD_APP_FONT_NO_MEMORY) return page_memory_block(p, "glyph_ready", READER_ERROR_FONT_MEMORY);
        if (ready < 0) return page_error(p, "glyph_ready", (int)letter);
        if (!ready) return 0;
        ready = epd_app_font_glyph_acquire(font_for(p->record.style), letter == '\t' ? ' ' : letter, 0, false, &glyph);
        if (ready == EPD_APP_FONT_NO_MEMORY) return page_memory_block(p, "glyph_acquire", READER_ERROR_FONT_MEMORY);
        if (ready < 0) return page_error(p, "glyph_acquire", (int)letter);
        if (!ready) return 0;
        uint32_t advance = glyph.adv_w;
        epd_app_font_glyph_release(&glyph); glyph.adv_w = advance;
        if (trace) ++view.trace_metric_calls;
    }
    p->decoded[at].letter = letter;
    p->decoded[at].bytes = bytes;
    p->decoded[at].width = glyph.adv_w;
    return 1;
}
static bool page_target(const view_page_t *p)
{
    /* An admitted target or near layout must keep its own source moving while
     * optional I/O is paused. Far pagination remains speculative. */
    unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
    unsigned distance = view.prefetch_number > wanted ?
        view.prefetch_number - wanted : wanted - view.prefetch_number;
    bool near = view.status.ready && !view.background_paused &&
        ((p == view.prefetch && !view.preview_back_scanning && distance && distance <= 3) ||
         (p == view.index && !view.preview && !view.complete && view.count <= view.wanted + 3));
    return view.compute_foreground && (p == view.building ||
        (p == view.prefetch && view.preview_back_scanning && view.turn_pending < 0) ||
        (p == view.index && (view.restoring || view.link_target[0] || view.wanted >= view.count)) || near);
}
static int page_step(view_page_t *p, bool indexing, document_reader_t *reader,
                     const document_progress_t *status)
{
    if (!page_work_allowed(p)) return 0;
    bool backfill = p == view.prefetch && view.preview_back_scanning;
    bool clip_page = (p == view.building && view.preview_clipped) || backfill;
    document_cursor_t clip_end = backfill ? view.preview_back_end : view.preview_end;
    bool target = page_target(p);
    bool trace = target && view.trace_pending && !view.trace_layout_ready;
    if (trace && p->wait_kind) {
        rt_tick_t waited = rt_tick_get() - p->wait_started;
        if (p->wait_kind == 1) view.trace_record_wait += waited;
        else view.trace_glyph_wait += waited;
    }
    p->wait_kind = 0;
    if (view.compute_job)
    {
        if (view.compute_page != p) return 0;
        int result;
        if (!reader_compute_poll(view.compute_job, &result)) return 0;
        if (view.compute_traced) view.trace_layout_ticks += rt_tick_get() - view.compute_started;
        view.compute_traced = false;
        epd_reader_batch_t *batch = reader_compute_data(view.compute_job);
        if (result == EPD_COMPUTE_OK && batch->generation == p->generation)
        {
            if (!epd_reader_page_copy((epd_reader_page_t *)p, &batch->page))
                result = EPD_COMPUTE_COMPONENT_ERROR;
            if (clip_page && batch->kind == EPD_READER_EOF)
                p->eof = false;
            if (p->cursor.record != p->loaded) p->has_record = false;
            if (p->done && p->read_request) {
                document_record_cancel(p->read_request);
                p->read_request = NULL;
            }
        }
        else if (result == EPD_COMPUTE_OK) result = EPD_COMPUTE_COMPONENT_ERROR;
        if (result == EPD_COMPUTE_OK) view.compute_spare = view.compute_job;
        else reader_compute_release(view.compute_job);
        view.compute_job = NULL; view.compute_page = NULL;
        return result == EPD_COMPUTE_OK ? 1 : page_error(p, "layout_result", result);
    }
    if (p->done) return 1;
    bool clipped = clip_page && cursor_equal(p->cursor, clip_end);
    bool eof = p->cursor.record == status->end || clipped;
    if (eof && !status->complete && !clipped) {
        /* Required input must advance even while speculative prefetch is paused. */
        if (target && !p->read_request) {
            p->read_request = document_record_request(view.document, p->cursor.record, true);
            p->read_foreground = true;
            if (!p->read_request) return page_memory_block(p, "record_request", READER_ERROR_LAYOUT_MEMORY);
            if (trace) rt_kprintf("reader input_wait session=%u intent=%u record=%u\n",
                (unsigned)view.session, (unsigned)view.intent, (unsigned)p->cursor.record);
        }
        return 0;
    }
    if (eof && p->read_request) {
        document_record_cancel(p->read_request); p->read_request = NULL;
    }
    if (!eof && (!p->has_record || p->loaded != p->cursor.record))
    {
        rt_tick_t started = trace ? rt_tick_get() : 0;
        (void)reader;
        if (!p->read_request) {
            p->read_request = document_record_request(view.document, p->cursor.record, target);
            p->read_foreground = target;
        }
        if (!p->read_request) return page_memory_block(p, "record_request", READER_ERROR_LAYOUT_MEMORY);
        document_record_data_t data;
        int loaded = document_record_take(p->read_request, &data);
        if (!loaded) {
            if (target && !p->read_foreground) {
                document_record_cancel(p->read_request);
                p->read_request = document_record_request(view.document, p->cursor.record, true);
                p->read_foreground = true;
                if (!p->read_request) return page_memory_block(p, "record_promote", READER_ERROR_LAYOUT_MEMORY);
            }
            if (trace) { p->wait_started = rt_tick_get(); p->wait_kind = 1; }
            return 0;
        }
        int reason = loaded < 0 ? document_record_result(p->read_request) : BOOK_OK;
        uint32_t limit = clip_page && clip_end.record < status->end ? clip_end.record : status->end;
        int next = DOCUMENT_NEXT_NONE;
        if (loaded > 0 && target && view.selected && !indexing && !backfill && !view.background_paused)
            next = document_record_advance(p->read_request, limit);
        if (view.near_tracing && p == view.prefetch) {
            view.near_record_queued += next == DOCUMENT_NEXT_QUEUED;
            view.near_record_ready += next == DOCUMENT_NEXT_READY;
        }
        if (next == DOCUMENT_NEXT_NONE) {
            document_record_cancel(p->read_request);
            p->read_request = NULL;
        }
        if (reason == BOOK_NO_MEMORY) return page_memory_block(p, "record_take", READER_ERROR_LAYOUT_MEMORY);
        if (loaded < 0) return page_error(p, "record_take", loaded);
        p->record = data.record; p->image = data.image;
        p->loaded = p->cursor.record; p->has_record = true;
        if (p->record.kind < DOCUMENT_TEXT || p->record.kind > DOCUMENT_METADATA ||
            (p->cursor.byte && p->record.kind != DOCUMENT_TEXT) || p->cursor.byte > p->record.length) return page_error(p, "record_header", -1);
        if (p->record.kind != DOCUMENT_IMAGE)
        {
            if (p->record.length > DOCUMENT_TEXT_LIMIT) return page_error(p, "record_length", -1);
            memcpy(p->data, data.text, p->record.length + 1);
            if (p->cursor.byte < p->record.length && p->cursor.byte &&
                ((unsigned char)p->data[p->cursor.byte] & 0xc0) == 0x80) return page_error(p, "record_cursor", -1);
            memset(p->decoded, 0, sizeof(p->decoded));
        }
        if (p->record.kind == DOCUMENT_METADATA) {
            p->cursor.record += sizeof(document_record_t) + p->record.length;
            p->cursor.byte = 0; p->has_record = false;
            return 1;
        }
        p->measured = p->prepared = p->cursor.byte;
        if (trace) view.trace_read_ticks += rt_tick_get() - started;
    }
    unsigned text_limit = VIEW_BLOCK;
    if (!eof && p->record.kind == DOCUMENT_TEXT)
    {
        unsigned end = p->cursor.byte, events = 0;
        unsigned window = view.counting_only ? VIEW_BLOCK : VIEW_METRICS_WINDOW;
        while (end < p->record.length && events < window) {
            uint32_t letter;
            end += utf8_letter(p->data + end, p->record.length - end, &letter);
            ++events;
        }
        text_limit = events ? events : 1;
        /* Word wrapping can inspect the rest of an ASCII word beyond this slice. */
        if (end && end < p->record.length && (unsigned char)p->data[end - 1] < 128 &&
            !isspace((unsigned char)p->data[end - 1]))
            while (end < p->record.length && (unsigned char)p->data[end] < 128 &&
                   !isspace((unsigned char)p->data[end])) ++end;
        if (p->prepared < end) {
            epd_app_font_set_purpose(font_for(p->record.style),
                target && view.selected ? EPD_COMPUTE_FOREGROUND : EPD_COMPUTE_BACKGROUND);
            unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
            unsigned distance = view.prefetch_number > wanted ?
                view.prefetch_number - wanted : wanted - view.prefetch_number;
            bool near = view.selected && p == view.prefetch &&
                !view.preview_back_scanning && (distance <= 3 ||
                (view.direction >= 0 && view.prefetch_number > wanted && distance == VIEW_LAYOUT_AHEAD));
            int ready = near ?
                epd_app_font_prepare_text_preview(font_for(p->record.style), p->data + p->prepared,
                    end - p->prepared) :
                epd_app_font_prepare_text(font_for(p->record.style), p->data + p->prepared,
                    end - p->prepared, false);
            if (ready == EPD_APP_FONT_NO_MEMORY) return page_memory_block(p, "font_prepare", READER_ERROR_FONT_MEMORY);
            if (ready < 0) return page_error(p, "font_prepare", ready);
            p->prepared = end;
        }
        rt_tick_t started = trace ? rt_tick_get() : 0;
        for (unsigned i = 0; i < VIEW_METRICS_BATCH && p->measured < end; ++i)
        {
            int decoded = decode_at(p, p->measured, trace);
            if (decoded <= 0) {
                if (!decoded && trace) { p->wait_started = rt_tick_get(); p->wait_kind = 2; }
                return decoded;
            }
            p->measured += p->decoded[p->measured].bytes;
            if (view.work_budget && rt_tick_get() - view.work_started >= view.work_budget) break;
        }
        if (trace) view.trace_metric_ticks += rt_tick_get() - started;
        if (p->measured < end) return 1;
    }
    if (!eof && p->record.kind == DOCUMENT_ANCHOR)
    {
        if (indexing && p->cursor.record >= view.toc_scanned)
        {
            size_t id = strlen(p->data);
            if (id + 1 < p->record.length && p->data[id + 1] && !add_toc(p->cursor.record, p->record.style)) return -1;
            view.toc_scanned = p->cursor.record + sizeof(document_record_t) + p->record.length;
        }
    }
    int result;
    reader_compute_job_t *job = view.compute_spare;
    view.compute_spare = NULL;
    if (!job) job = compute_acquire(&result);
    if (!job) return result == EPD_COMPUTE_BUSY ? 0 : page_error(p, "layout_create", result);
    epd_reader_batch_t *batch = reader_compute_data(job);
    if (!epd_reader_page_copy(&batch->page, (const epd_reader_page_t *)p))
    { reader_compute_release(job); return page_error(p, "layout_input", -1); }
    batch->generation = p->generation;
    batch->kind = eof ? EPD_READER_EOF : p->record.kind;
    batch->length = clipped ? p->cursor.byte : eof ? 0 : p->record.length;
    batch->style = p->record.style; batch->link = p->record.link;
    batch->next_record = p->cursor.record + sizeof(document_record_t) + p->record.length;
    batch->width = width_available(); batch->height = VIEW_HEIGHT;
    batch->line_space = view.status.line_space;
    batch->base_height = view.metrics[0].line_height;
    static const unsigned paragraph_font_sizes[] = {UI_READING_FONT_SIZES};
    batch->font_size = paragraph_font_sizes[ui_settings_index(UI_SETTING_FONT_SIZE)];
    batch->paragraph = p->record.paragraph;
    unsigned slot = font_slot(p->record.style);
    batch->font_height = (p->record.style & BOOK_STYLE_ITALIC) ? view.italic_metrics[slot].line_height : view.metrics[slot].line_height;
    batch->image_width = batch->image_height = 0;
    if (!eof && p->record.kind == DOCUMENT_IMAGE) {
        unsigned width, height;
        book_picture_size(&p->image.layout, batch->width, batch->height, &width, &height);
        batch->image_width = width; batch->image_height = height;
    }
    batch->limit = text_limit;
    if (!eof && clip_page && p->cursor.record == clip_end.record && p->record.kind == DOCUMENT_TEXT) {
        unsigned events = 0;
        for (unsigned at = p->cursor.byte; at < clip_end.byte && events < batch->limit;
             at += p->decoded[at].bytes) {
            if (!p->decoded[at].bytes) { reader_compute_release(job); return page_error(p, "clip_glyph", -1); }
            ++events;
        }
        if (events < batch->limit) batch->limit = events;
    }
    if (!eof && p->record.kind == DOCUMENT_TEXT) {
        memcpy(batch->data, p->data, p->record.length + 1);
        memcpy(batch->glyphs, p->decoded, p->record.length * sizeof(*p->decoded));
    }
    view.compute_job = job; view.compute_page = p;
    view.compute_traced = trace;
    if (trace) view.compute_started = rt_tick_get();
    reader_compute_submit_target(job, target && view.selected);
    return 0;
}

static void near_trace_finish(const char *reason)
{
    if (!view.near_tracing) return;
    view_page_t *p = view.prefetch;
    if (p->wait_kind) {
        rt_tick_t waited = rt_tick_get() - p->wait_started;
        if (p->wait_kind == 1) view.trace_record_wait += waited;
        else view.trace_glyph_wait += waited;
        p->wait_kind = 0;
    }
    if (view.compute_page == p && view.compute_traced) {
        view.trace_layout_ticks += rt_tick_get() - view.compute_started;
        view.compute_traced = false;
    }
    rt_kprintf("reader near_detail session=%u intent=%u page=%u chapter=%u end=%s start=%u:%u cursor=%u:%u total_ms=%u read_ms=%u metric_ms=%u layout_ms=%u record_wait_ms=%u glyph_wait_ms=%u service_ms=%u calls=%u done=%u next_queued=%u next_ready=%u\n",
        (unsigned)view.session, (unsigned)view.near_intent, view.near_number, (unsigned)view.progress.chapter,
        reason, p->start.record, p->start.byte, p->cursor.record, p->cursor.byte, elapsed_ms(view.near_started),
        (unsigned)((uint64_t)view.trace_read_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.trace_metric_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.trace_layout_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.trace_record_wait * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.trace_glyph_wait * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.near_service_ticks * 1000 / RT_TICK_PER_SECOND),
        view.near_steps, (unsigned)p->done, view.near_record_queued, view.near_record_ready);
    view.near_tracing = false;
}
static int near_page_step(void)
{
    unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
    unsigned distance = view.prefetch_number > wanted ? view.prefetch_number - wanted : wanted - view.prefetch_number;
    bool trace = view.selected && !view.trace_pending && !view.preview_back_scanning &&
        distance && distance <= 3 && page_target(view.prefetch);
    if (trace && !view.near_tracing) {
        view.near_tracing = true; view.near_started = rt_tick_get();
        view.near_intent = view.intent; view.near_number = view.prefetch_number;
        view.near_service_ticks = 0; view.near_steps = 0;
        view.near_record_queued = view.near_record_ready = 0;
        view.trace_read_ticks = view.trace_metric_ticks = view.trace_layout_ticks = 0;
        view.trace_record_wait = view.trace_glyph_wait = 0; view.trace_metric_calls = 0;
        view.prefetch->wait_kind = 0;
    }
    /* Borrow the foreground counters only inside page_step. Background layout
     * does not publish a target or change the foreground trace lifecycle. */
    bool pending = view.trace_pending, ready = view.trace_layout_ready;
    if (trace) { view.trace_pending = true; view.trace_layout_ready = false; }
    rt_tick_t started = rt_tick_get();
    int result = page_step(view.prefetch, false, view.page_reader, &view.progress);
    if (trace) { view.near_service_ticks += rt_tick_get() - started; ++view.near_steps; }
    view.trace_pending = pending; view.trace_layout_ready = ready;
    if (view.near_tracing && (result < 0 || view.prefetch->done)) near_trace_finish(result < 0 ? "error" : "complete");
    return result;
}

static void layout_for_area(view_state_t *context, reader_geometry_t area)
{
    reader_view_t *status = &context->status;
    context->font_area = area;
    status->margin = ui_reading_margin_px(context->settings[UI_SETTING_MARGIN - UI_SETTING_FONT_SIZE]);
    status->content_width = area.width - 2 * (int)status->margin;
    status->top = area.top;
    status->bottom = area.bottom;
    status->line_space = ui_reading_line_space(context->fonts[0]->line_height,
        context->settings[UI_SETTING_LINE_SPACING - UI_SETTING_FONT_SIZE]);
    uint32_t key = context->font_identity ^ VIEW_LAYOUT_VERSION;
    key = (key ^ context->font_id) * 16777619u;
    key = (key ^ context->font_revision) * 16777619u;
    key = (key ^ (unsigned)status->content_width) * 16777619u;
    key = (key ^ (unsigned)(area.bottom - area.top)) * 16777619u;
    key = (key ^ (unsigned)status->line_space) * 16777619u;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        key = (key ^ context->settings[i - UI_SETTING_FONT_SIZE]) * 16777619u;
    /* Appearance changes pixels, not text positions or chapter pagination. */
    context->layout = key * 16777619u;
    context->font_metrics_ready = false;
}

static bool prepare_fonts(view_state_t *state)
{
    static const unsigned sizes[] = {UI_READING_FONT_SIZES};
    lv_font_t *fonts[3] = {0}, *fallbacks[3] = {0};
    lv_font_t *oblique[3] = {0};
    uint32_t identity = 0, font_identity = 2166136261u;
    uint32_t font_id = epd_app_reading_font_id();
    uint32_t font_revision = epd_app_reading_font_revision();
    reader_geometry_t area = reader_geometry();
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
    unsigned size = sizes[settings[0]];
    unsigned attempt = 0;
retry:
    ++attempt;
    font_identity = 2166136261u;
    for (unsigned i = 0; i < 3; ++i)
    {
        fonts[i] = epd_app_reading_font_create(font_id, size + (i == 2 ? 6 : 0),
                     i ? 1 : settings[UI_SETTING_FONT_WEIGHT - UI_SETTING_FONT_SIZE], &identity, &fallbacks[i]);
        if (fonts[i]) oblique[i] = document_font_oblique(fonts[i]);
        if (!fonts[i] || !oblique[i])
        {
            rt_kprintf("reader font apply id=%lu revision=%lu slot=%u stage=%s attempt=%u result=failed\n",
                (unsigned long)font_id, (unsigned long)font_revision, i,
                fonts[i] ? "oblique" : "create", attempt);
            for (unsigned j = 0; j <= i; ++j)
            {
                document_font_free(oblique[j]); epd_app_font_destroy(fonts[j]); epd_app_font_destroy(fallbacks[j]);
                oblique[j] = fonts[j] = fallbacks[j] = NULL;
            }
            if (attempt == 1)
            {
                /* Cached pages own copies; the displayed page and its fonts stay live. */
                reader_page_cache_clear(&view.cache);
                view.cache_blocked = view.prefetch_number = UINT_MAX;
                goto retry;
            }
            view.font_failed = true;
            view.failed_font_id = font_id;
            view.failed_font_revision = font_revision;
            view.failed_font_area = area;
            memcpy(view.failed_font_settings, settings, sizeof(settings));
            return false;
        }
        fonts[i]->kerning = LV_FONT_KERNING_NONE;
        if (fallbacks[i]) fallbacks[i]->kerning = LV_FONT_KERNING_NONE;
        font_identity = (font_identity ^ identity) * 16777619u;
    }
    for (unsigned i = 0; i < 3; ++i)
    {
        if (view.retired[i])
        { document_font_free(view.oblique[i]); epd_app_font_destroy(view.fonts[i]); epd_app_font_destroy(view.fallbacks[i]); }
        else { view.retired[i] = view.fonts[i]; view.retired[i + 3] = view.fallbacks[i]; view.retired_oblique[i] = view.oblique[i]; }
        view.fonts[i] = fonts[i]; view.fallbacks[i] = fallbacks[i];
        view.oblique[i] = oblique[i];
    }
    view.status.font = view.fonts[0];
    view.font_id = font_id;
    view.font_revision = font_revision;
    view.font_identity = font_identity;
    view.font_failed = false;
    memcpy(view.settings, settings, sizeof(settings));
    layout_for_area(state, area);
    return true;
}

static bool fonts_ready(void)
{
    if (view.font_metrics_ready) return true;
    if (view.font_memory_blocked && view.font_blocked_intent == view.intent) return false;
    for (unsigned i = 0; i < 3; ++i) {
        int normal = epd_app_font_metrics(view.fonts[i], &view.metrics[i]);
        int oblique = epd_app_font_metrics(view.oblique[i], &view.italic_metrics[i]);
        if ((normal < 0 && normal != EPD_APP_FONT_NO_MEMORY) ||
            (oblique < 0 && oblique != EPD_APP_FONT_NO_MEMORY)) { fail(READER_ERROR_FONT_MEMORY); return false; }
        if (normal == EPD_APP_FONT_NO_MEMORY || oblique == EPD_APP_FONT_NO_MEMORY) {
            view.font_memory_blocked = true; view.font_blocked_intent = view.intent;
            if (view.selected) {
                view.request_failure = READER_ERROR_FONT_MEMORY; view.request_failure_intent = view.intent;
            }
            return false;
        }
        if (!normal || !oblique) return false;
    }
    view.status.line_space = ui_reading_line_space(view.metrics[0].line_height,
        view.settings[UI_SETTING_LINE_SPACING - UI_SETTING_FONT_SIZE]);
    for (unsigned i = 0; i < 3; ++i) {
        view.layout = (view.layout ^ view.metrics[i].line_height) * 16777619u;
        view.layout = (view.layout ^ view.metrics[i].base_line) * 16777619u;
    }
    view.layout = (view.layout ^ view.status.line_space) * 16777619u;
    view.font_metrics_ready = true;
    return true;
}

static epd_reading_data_t data_client, data_active;
static bool data_retiring, data_cancelled;
static epd_compute_wait_t render_wait;
static epd_reader_render_t render_active;
static bool render_required;
static bool render_sessions_pending(void);
/* Cancellation is nonblocking. This critical section also excludes client
 * detachment, so close cannot race a cancellation caller. */
static void index_cancel(void)
{
    rt_enter_critical();
    if (data_active) { data_cancelled = true; epd_reading_data_cancel(data_active); }
    rt_exit_critical();
}
static void render_cancel(void);
static void render_cancel_optional(void);
static void view_cancel(void) { index_cancel(); render_cancel(); }
static bool index_client_cleanup(bool closing)
{
    if (!data_client) return false;
    if (!closing && !data_retiring && epd_reading_data_status(data_client) == EPD_COMPUTE_OK) return false;
    epd_reading_data_cancel(data_client);
    epd_reading_data_close(&data_client);
    data_retiring = data_client != NULL;
    return data_retiring;
}
static epd_reading_data_t index_client_begin(void)
{
    data_cancelled = false;
    if (view_interrupted() || render_sessions_pending() || index_client_cleanup(false)) return NULL;
    if (!data_client && epd_reading_data_create(EPD_COMPUTE_BACKGROUND, &data_client) != EPD_COMPUTE_OK) return NULL;
    rt_enter_critical();
    bool interrupted = view_interrupted();
    if (!interrupted) data_active = data_client;
    rt_exit_critical();
    return interrupted ? NULL : data_client;
}
static bool index_client_end(int result)
{
    rt_enter_critical(); data_active = NULL; rt_exit_critical();
    bool accepted = result == EPD_COMPUTE_OK && !view_interrupted();
    data_retiring = true;
    index_client_cleanup(false);
    return accepted;
}
static bool index_checksum(const void *data, size_t size, uint32_t *hash)
{
    epd_reading_data_t client = index_client_begin();
    if (!client) return false;
    *hash = EPD_READING_DATA_FNV_SEED;
    return index_client_end(epd_reading_data_hash_skip(client, data, size,
        offsetof(view_index_header_t, checksum), sizeof(uint32_t), hash));
}
static bool index_validate(const void *blob, size_t size)
{
    epd_reading_data_t client = index_client_begin();
    if (!client) return false;
    epd_reading_index_spec_t spec = {VIEW_INDEX_MAGIC, VIEW_INDEX_VERSION, view.layout,
        view.progress.end, READER_PAGE_MAX, DOCUMENT_TEXT_LIMIT};
    return index_client_end(epd_reading_data_index_check(client, blob, size, &spec));
}
static bool index_size(uint32_t count, uint32_t toc_count, size_t *size)
{
    if (!count || count > READER_PAGE_MAX ||
        (uint64_t)(count + 1u) * sizeof(document_cursor_t) +
        (uint64_t)toc_count * sizeof(toc_entry_t) > SIZE_MAX - sizeof(view_index_header_t)) return false;
    *size = sizeof(view_index_header_t) + (size_t)(count + 1u) * sizeof(document_cursor_t) +
            (size_t)toc_count * sizeof(toc_entry_t);
    return *size <= VIEW_INDEX_LIMIT;
}
typedef struct { uint32_t magic, version, layout, end, pages, checksum; } view_count_header_t;
#define VIEW_COUNT_MAGIC 0x544e4352u
static uint32_t count_checksum(const view_count_header_t *header)
{
    const uint8_t *bytes = (const uint8_t *)header;
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < offsetof(view_count_header_t, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
static void index_load(void)
{
    if (!view.counting_only && core_waiting()) return;
    if (view.index_loaded || !view.progress.complete) return;
    if (view.count || view.index->cursor.record || view.index->cursor.byte) { view.index_loaded = true; return; }
    rt_tick_t started = rt_tick_get();
    void *blob = NULL;
    size_t size = 0, expected;
    int loaded = document_index_load_async(view.document, view.layout, &blob, &size);
    view.index_loading = loaded == 0;
    if (!loaded) return;
    view.index_loaded = true;
    if (loaded < 0) goto done;
    if (!blob || size < sizeof(view_index_header_t)) goto done;
    view_index_header_t header;
    memcpy(&header, blob, sizeof(header));
    if (header.magic != VIEW_INDEX_MAGIC || header.version != VIEW_INDEX_VERSION || header.layout != view.layout ||
        header.end != view.progress.end || header.complete > 1 || header.toc_scanned > header.end ||
        !index_size(header.count, header.toc_count, &expected) || expected != size ||
        !index_validate(blob, size)) goto done;
    document_cursor_t *pages = (document_cursor_t *)((char *)blob + sizeof(header));
    toc_entry_t *toc = (toc_entry_t *)(pages + header.count + 1u);
    if (view.counting_only) {
        view.count = header.count; view.complete = header.complete != 0;
        view.count_saved = view.complete;
        view.from_cache = true;
        page_reset(view.index, pages[header.count]); view.index->collect = false;
        goto done;
    }
    unsigned old_toc_capacity = view.toc_capacity;
    view.toc_capacity = header.toc_count;
    bool reserved = reserve_pages(header.count + 1u);
    view.toc_capacity = old_toc_capacity;
    if (!reserved) goto done;
    size_t old_toc_bytes = old_toc_capacity * sizeof(*view.toc);
    size_t new_toc_bytes = (size_t)header.toc_count * sizeof(*view.toc);
    if (index_bytes - old_toc_bytes + new_toc_bytes > VIEW_INDEX_GLOBAL) goto done;
    toc_entry_t *copy = new_toc_bytes ? view_allocate(new_toc_bytes, EPD_APP_PSRAM) : NULL;
    if (new_toc_bytes && !copy) goto done;
    if (new_toc_bytes) memcpy(copy, toc, new_toc_bytes);
    memcpy(view.pages, pages, (size_t)(header.count + 1u) * sizeof(*pages));
    epd_app_free(view.toc); view.toc = copy;
    index_bytes = index_bytes - old_toc_bytes + new_toc_bytes;
    view.count = header.count; view.toc_count = view.toc_capacity = header.toc_count;
    view.toc_scanned = header.toc_scanned;
    view.complete = view.index_saved = header.complete != 0;
    view.index_saved_count = header.count; view.index_saved_at = rt_tick_get();
    view.from_cache = true;
    page_reset(view.index, view.pages[view.count]);
done:
    epd_app_free(blob);
    rt_kprintf("reader index_load hit=%u pages=%u complete=%u bytes=%u ms=%u\n",
        view.from_cache, view.count, view.complete, (unsigned)size, elapsed_ms(started));
}
static void index_save(bool force)
{
    /* Existing dense prefix snapshots remain valid. Sparse runtime boundaries
     * must never be serialized as consecutive pages in the v2 format. */
    if (view.sparse_stride || view.counting_only) return;
    if (!view.document || !view.count || !view.status.ready || view.failure || view.index_save_failed) return;
    if (view_interrupted() || render_sessions_pending()) return;
    if (!force && core_waiting()) return;
    if (view.index_saved_count == view.count && view.index_saved == view.complete) return;
    if (!force && !view.complete && view.index_saved_count &&
        (view.count - view.index_saved_count < VIEW_INDEX_CHECKPOINT_PAGES ||
         elapsed_ms(view.index_saved_at) < VIEW_INDEX_CHECKPOINT_MS)) return;
    document_progress(view.document, &view.progress);
    if (!view.progress.complete || view.progress.result) return;
    size_t size;
    if (!index_size(view.count, view.toc_count, &size)) return;
    view_index_header_t *blob = view_allocate(size, EPD_APP_PSRAM);
    if (!blob) { view.index_save_failed = true; return; }
    *blob = (view_index_header_t){VIEW_INDEX_MAGIC, VIEW_INDEX_VERSION, view.layout,
                                view.progress.end, view.count, view.toc_count,
                                view.complete ? view.progress.end : view.toc_scanned, view.complete, 0};
    document_cursor_t *pages = (document_cursor_t *)(blob + 1);
    memcpy(pages, view.pages, (size_t)(view.count + 1u) * sizeof(*pages));
    if (view.toc_count) memcpy(pages + view.count + 1u, view.toc, (size_t)view.toc_count * sizeof(*view.toc));
    if (!index_checksum(blob, size, &blob->checksum)) {
        if (!data_cancelled && !view_interrupted()) view.index_save_failed = true;
        epd_app_free(blob); return;
    }
    bool accepted = document_index_save(view.document, view.layout, blob, size);
    if (accepted)
    {
        view.index_saved_count = view.count; view.index_saved = view.complete;
        view.index_saved_at = rt_tick_get();
    }
    else view.index_save_failed = true;
    rt_kprintf("reader index_save pages=%u complete=%u queued=%u bytes=%u\n",
        view.count, view.complete, accepted, (unsigned)size);
    epd_app_free(blob);
}
/* Fixed-size derived metadata, protected independently of the large cursor
 * payload. Its file is already bound to source identity by document_index. */
static bool count_cache_load(void)
{
    if (view.count_loaded || view.count || !view.progress.complete) return false;
    void *blob = NULL; size_t size = 0;
    int loaded = document_count_load_async(view.document, view.layout, &blob, &size);
    if (!loaded) return true;
    view.count_loaded = true;
    if (loaded > 0 && blob && size == sizeof(view_count_header_t)) {
        view_count_header_t summary; memcpy(&summary, blob, sizeof(summary));
        if (summary.magic == VIEW_COUNT_MAGIC && summary.version == 1 && summary.layout == view.layout &&
            summary.end == view.progress.end && summary.pages && summary.pages <= READER_PAGE_MAX &&
            summary.checksum == count_checksum(&summary)) {
            view.count = summary.pages; view.complete = view.from_cache = view.count_saved = true;
        }
    }
    epd_app_free(blob);
    return false;
}
static void count_cache_save(void)
{
    if (!view.counting_only && !view.sparse_stride) return;
    if (view.count_saved || !view.complete || !view.progress.complete || view.progress.result || view.failure) return;
    view_count_header_t header = {VIEW_COUNT_MAGIC, 1, view.layout, view.progress.end, indexed_pages(state), 0};
    if (!header.pages) return;
    header.checksum = count_checksum(&header);
    view.count_saved = document_count_save(view.document, view.layout, &header, sizeof(header));
}
typedef union { size_t bytes; uint64_t alignment; } cache_allocation_t;
static void *cache_allocate(size_t size)
{
    if (size > VIEW_CACHE_BUDGET || layout_bytes > VIEW_CACHE_BUDGET - size) return NULL;
    if (!window_charge(size)) return NULL;
    cache_allocation_t *allocation = view_allocate(sizeof(*allocation) + size, EPD_APP_PSRAM);
    if (!allocation) { window_uncharge_bytes(size); return NULL; }
    allocation->bytes = size;
    return allocation + 1;
}
static void cache_free(void *memory)
{
    if (!memory) return;
    cache_allocation_t *allocation = (cache_allocation_t *)memory - 1;
    window_uncharge_bytes(allocation->bytes); epd_app_free(allocation);
}

static void core_initialize_release(void)
{
    reader_compute_release(view.compute_spare); view.compute_spare = NULL;
    index_bytes -= view.capacity * sizeof(*view.pages);
    view.capacity = 0;
    if (!view.counting_only) epd_app_free(view.pages);
    view.pages = NULL;
    epd_app_free(view.index); epd_app_free(view.building);
    epd_app_free(view.display); epd_app_free(view.prefetch);
    view.index = view.building = view.display = view.prefetch = NULL;
    window_uncharge_bytes(view.window_workspace_charge); view.window_workspace_charge = 0;
}
static epd_memory_recovery_result_t core_initialize_recover(size_t peak, bool failed)
{
    /* UI input and suspend/resume replace the activity credit. Revalidate
     * admission under that credit without restarting the deadline or pressure. */
    if (view.init_recovery.used) {
        view.init_recovery.owner = epd_memory_owner_current();
        if (failed) return epd_memory_recovery_failed(&view.init_recovery);
    }
    return view_memory_recover(&view.init_recovery, peak, failed);
}
static bool core_initialize_failed(size_t peak, bool allocation_failed)
{
    core_initialize_release();
    if (view.selected && allocation_failed && !view.init_alloc_retried) {
        view.init_alloc_retried = true;
        epd_memory_recovery_result_t result = core_initialize_recover(peak, true);
        if (result != EPD_MEMORY_RECOVERY_DENIED) return false;
    }
    view.init_blocked = true; view.init_blocked_intent = view.intent;
    view.init_blocked_epoch = epd_memory_capacity_epoch();
    if (view.selected) { view.init_attempted = true; fail(READER_ERROR_LAYOUT_MEMORY); }
    return false;
}
static bool core_initialize(void)
{
    uint32_t epoch = epd_memory_capacity_epoch();
    if (view.init_blocked && !view.selected && view.init_blocked_intent == view.intent &&
        epoch == view.init_blocked_epoch) return false;
    size_t required = (view.counting_only ? 1u : 4u) * sizeof(view_page_t) +
        sizeof(epd_reader_batch_t) + (view.counting_only ? 0u : 256u * sizeof(*view.pages)) + 512u;
    /* Selection is updated before primary_state during a chapter handoff. */
    size_t peak = (view.counting_only ? 1u : 4u) * epd_app_allocation_peak(sizeof(view_page_t)) +
        epd_app_allocation_peak(sizeof(epd_reader_batch_t)) +
        (view.counting_only ? 0u : epd_app_allocation_peak(256u * sizeof(*view.pages)));
    if (view.selected && view.init_recovery.used) {
        epd_memory_recovery_result_t result = core_initialize_recover(peak, false);
        if (result == EPD_MEMORY_RECOVERY_PENDING) return false;
        if (result == EPD_MEMORY_RECOVERY_DENIED) return core_initialize_failed(peak, false);
    }
    bool admitted = frame_reclaim_layout(required, view.selected ? 0u : VIEW_NEIGHBOR_PRIORITY) &&
        epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM, peak) == EPD_RESOURCE_OK;
    if (!admitted && view.selected) {
        epd_memory_recovery_result_t result = core_initialize_recover(peak, false);
        if (result == EPD_MEMORY_RECOVERY_PENDING) return false;
        admitted = result == EPD_MEMORY_RECOVERY_READY;
    }
    if (!admitted) return core_initialize_failed(peak, false);
    if (view.window_state_charge && !view.window_workspace_charge) {
        size_t workspace = required - (view.counting_only ? 0u : 256u * sizeof(*view.pages));
        if (!window_charge(workspace)) return core_initialize_failed(peak, true);
        view.window_workspace_charge = workspace;
    }
    view.init_blocked = false;
    rt_tick_t started = rt_tick_get();
    view.status.title = view.title; view.status.text = "";
    document_prefetch_enable(view.document, false);
    view.cache = (reader_page_cache_t){.budget = VIEW_CACHE_BUDGET / 3u,
        .allocate = cache_allocate, .release = cache_free};
    view.cache_blocked = view.prefetch_number = UINT_MAX;
    view.prefetch_record = UINT32_MAX;
    int compute_status;
    view.compute_spare = compute_acquire(&compute_status);
    view.index = view_allocate(sizeof(*view.index), EPD_APP_PSRAM);
    if (view.index) memset(view.index, 0, sizeof(*view.index));
    if (view.counting_only) {
        if ((!view.compute_spare && compute_status != EPD_COMPUTE_BUSY) || !view.index)
        { return core_initialize_failed(peak, true); }
        view.pages = view.counting_origin;
        page_reset(view.index, (document_cursor_t){0, 0}); view.index->collect = false;
        view.initialized = view.init_attempted = true;
        return true;
    }
    view.building = view_allocate(sizeof(*view.building), EPD_APP_PSRAM);
    view.display = view_allocate(sizeof(*view.display), EPD_APP_PSRAM);
    view.prefetch = view_allocate(sizeof(*view.prefetch), EPD_APP_PSRAM);
    if (view.building) memset(view.building, 0, sizeof(*view.building));
    if (view.display) memset(view.display, 0, sizeof(*view.display));
    if (view.prefetch) memset(view.prefetch, 0, sizeof(*view.prefetch));
    if ((!view.compute_spare && compute_status != EPD_COMPUTE_BUSY) ||
        !view.index || !view.building || !view.display || !view.prefetch || !reserve_pages(2))
    { return core_initialize_failed(peak, true); }
    view.pages[0] = (document_cursor_t){0, 0};
    page_reset(view.index, view.pages[0]); page_reset(view.building, view.pages[0]); page_reset(view.display, view.pages[0]);
    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
    view.restoring = true; view.target_pending = true; view.status.indexing = true;
    if (!view.direction) view.direction = view.backward_target ? -1 : 1;
    view.trace_pending = true; view.turn_started = started;
    view.initialized = view.init_attempted = true;
    return true;
}

static void image_release(view_image_t *image)
{
    if (!image || --image->references) return;
    if (image->request) document_image_cancel(image->request);
    image_bytes -= document_picture_charge(image->bytes);
    document_picture_release(&image->picture);
    epd_app_free(image);
}
static void images_free(view_image_t *images)
{
    while (images)
    {
        view_image_t *next = images->next;
        image_release(images);
        images = next;
    }
}
static void core_release(void)
{
    near_trace_finish("release");
    link_clear();
    compute_cancel();
    reader_compute_release(view.compute_spare); view.compute_spare = NULL;
    document_record_cancel(view.back_request); view.back_request = NULL;
    if (view.index) document_record_cancel(view.index->read_request);
    if (view.building) document_record_cancel(view.building->read_request);
    if (view.display) document_record_cancel(view.display->read_request);
    if (view.prefetch) document_record_cancel(view.prefetch->read_request);
    images_free(view.images); view.images = NULL;
    index_bytes -= view.capacity * sizeof(*view.pages) + view.toc_capacity * sizeof(*view.toc);
    view.capacity = view.toc_capacity = 0;
    if (!view.counting_only) epd_app_free(view.pages);
    epd_app_free(view.toc);
    epd_app_free(view.index); epd_app_free(view.building); epd_app_free(view.display);
    epd_app_free(view.prefetch);
    view.index = view.building = view.display = view.prefetch = NULL;
    reader_page_cache_close(&view.cache);
    window_uncharge_bytes(view.window_workspace_charge); view.window_workspace_charge = 0;
}

static bool image_matches(const view_image_t *image, const view_run_t *run)
{
    return image->document == view.document && image->record == run->image - 1;
}
static bool page_uses_image(const view_page_t *page, const view_image_t *image)
{
    if (!page || !page->done || !page->collect) return false;
    for (unsigned i = 0; i < page->count; ++i)
        if (page->runs[i].image && image_matches(image, &page->runs[i])) return true;
    return false;
}
static bool target_needed(void)
{
    return !view.preview && !view.restoring && !view.link_target[0] && view.wanted < view.count &&
        (!view.status.ready || view.display_layout != view.layout || view.status.page != view.wanted + 1 ||
         !cursor_equal(view.display->start, view.pages[view.wanted]));
}
static bool image_page_nearby(const reader_cached_page_t *page)
{
    unsigned wanted = view.preview_cache_active && view.preview ? view.preview_page : view.wanted;
    unsigned distance = page->number > wanted ? page->number - wanted : wanted - page->number;
    return page->layout == view.layout && distance <= 3;
}
static void images_prune_keep(bool optional, const view_image_t *pending)
{
    view_image_t **at = &view.images;
    while (*at)
    {
        view_image_t *image = *at;
        bool keep = image == pending || image->references > 1 ||
            (view.display_pending && page_uses_image(view.building, image));
        if (!optional)
        {
            for (reader_cached_page_t *p = view.cache.first; p; p = p->next)
            {
                if (!image_page_nearby(p)) continue;
                for (unsigned i = 0; i < p->count; ++i)
                    if (p->runs[i].image && image_matches(image, &p->runs[i])) keep = true;
            }
        }
        if (keep) at = &image->next;
        else
        {
            *at = image->next; image->next = NULL; images_free(image);
        }
    }
}
static void images_prune(bool optional)
{ images_prune_keep(optional, NULL); }
static view_image_t *image_find(const view_run_t *run)
{
    for (view_image_t *image = view.images; image; image = image->next)
        if (image_matches(image, run)) return image;
    return NULL;
}
static void images_cancel_pending(void)
{
    view.image_memory_reclaimed = view.image_pressure_requested = false;
    memset(&view.image_recovery, 0, sizeof(view.image_recovery));
    view_image_t **at = &view.images;
    while (*at)
    {
        view_image_t *image = *at;
        if (image->request)
        { *at = image->next; image->next = NULL; images_free(image); }
        else at = &image->next;
    }
}
/* A target gets one reclamation pass and one pressure request, even if it has
 * several images. A wait with no submitted decoder must have its own deadline. */
static int image_memory_room(size_t bytes, bool required)
{
    size_t extra = bytes + sizeof(view_image_t) + 64u;
    if (image_bytes <= DOCUMENT_IMAGE_CACHE_BYTES - bytes && view_memory_room(extra)) return 1;
    if (!required) return -3;
    if (!view.image_memory_reclaimed || view.image_memory_intent != view.intent ||
        view.image_memory_layout != view.layout) {
        view.image_memory_reclaimed = true;
        view.image_pressure_requested = false;
        memset(&view.image_recovery, 0, sizeof(view.image_recovery));
        view.image_memory_intent = view.intent; view.image_memory_layout = view.layout;
        view.image_memory_started = rt_tick_get();
        images_prune(true);
        view_state_t *others[7]; unsigned contexts = window_contexts(primary_state, others);
        for (unsigned n = 0; n < contexts; ++n) if (others[n] != state) {
            view_image_t **at = &others[n]->images;
            while (*at) {
                view_image_t *image = *at;
                if (image->references > 1) { at = &image->next; continue; }
                *at = image->next; image->next = NULL; images_free(image);
            }
        }
        if (image_bytes <= DOCUMENT_IMAGE_CACHE_BYTES - bytes) {
            if (!frame_reclaim_layout(extra, 0u)) frame_reclaim_pages(extra);
        }
    }
    if (image_bytes > DOCUMENT_IMAGE_CACHE_BYTES - bytes) goto exhausted;
    epd_memory_recovery_result_t admission = view_memory_recover(&view.image_recovery,
        epd_app_allocation_peak(extra), false);
    /* Recovery may describe an earlier, smaller image in this target. Only
     * admission of the current peak permits its allocation. */
    if (admission == EPD_MEMORY_RECOVERY_READY && view_memory_room(extra)) return 1;
    if (admission == EPD_MEMORY_RECOVERY_PENDING) return 0;
exhausted:
    fail(READER_ERROR_IMAGE_MEMORY);
    return -3;
}
/* Pending pixels are polled once per service call, never in the compute loop. */
static int images_prepare_runs(const view_run_t *runs, unsigned count, bool required)
{
    if (view.failure) return -1;
    int ready = 1;
    for (unsigned i = 0; i < count; ++i)
    {
        const view_run_t *run = &runs[i];
        if (!run->image) continue;
        view_image_t *image = image_find(run);
        if (!image)
        {
            size_t image_length = run->image_bytes & ~EPD_READER_IMAGE_FULL;
            if (image_length <= sizeof(document_image_t) ||
                image_length > sizeof(document_image_t) + ((DOCUMENT_IMAGE_WIDTH + 1) / 2) * DOCUMENT_IMAGE_HEIGHT) {
                if (required) fail(READER_ERROR_IMAGE_READ);
                return -1;
            }
            size_t bytes = image_length - sizeof(document_image_t);
            size_t charge = document_picture_charge(bytes);
            int room = image_memory_room(charge, required);
            if (room <= 0) return room;
            image = view_allocate(sizeof(*image), EPD_APP_PSRAM);
            if (!image) { if (required) fail(READER_ERROR_IMAGE_MEMORY); return -3; }
            memset(image, 0, sizeof(*image));
            image->references = 1;
            image->bytes = bytes; image_bytes += charge;
            image->document = view.document; image->record = run->image - 1;
            image->next = view.images; view.images = image;
            image->started = rt_tick_get();
            image->request = document_image_request(view.document, image->record, bytes);
            if (!image->request) image->state = BOOK_NO_MEMORY;
            rt_kprintf("reader image record=%u size=%ux%u bytes=%u target=%u queued=%u\n",
                image->record, run->width, run->height, (unsigned)bytes, required, image->request != NULL);
        }
        if (!image->state && image->request)
        {
            image->state = document_image_take(image->request, &image->picture);
            if (image->state)
            {
                rt_kprintf("reader image record=%u result=%d ms=%u\n", image->record,
                    image->state, elapsed_ms(image->started));
                document_image_cancel(image->request); image->request = NULL;
            }
        }
        if (image->state < 0 && required && !image->retried)
        {
            images_prune_keep(true, image);
            image->retried = true;
            image->started = rt_tick_get();
            image->request = document_image_request(view.document, image->record, image->bytes);
            image->state = image->request ? 0 : BOOK_NO_MEMORY;
        }
        if (image->state < 0)
        {
            if (required) fail(image->state == BOOK_NO_MEMORY ? READER_ERROR_IMAGE_MEMORY : READER_ERROR_IMAGE_READ);
            return -1;
        }
        if (!image->state) ready = 0;
    }
    return ready;
}
static int images_prepare(view_page_t *page, bool required)
{ return images_prepare_runs(page->runs, page->count, required); }
static bool images_ready(const view_page_t *page)
{
    for (unsigned i = 0; i < page->count; ++i)
    {
        if (!page->runs[i].image) continue;
        view_image_t *image = image_find(&page->runs[i]);
        if (!image || image->state != 1) return false;
    }
    return true;
}

static bool page_fonts_ready(view_page_t *page)
{
    if (!page_work_allowed(page)) return false;
    unsigned pending = page->count;
    for (unsigned i = page->warm_run; i < page->count; ++i) {
        const view_run_t *run = &page->runs[i];
        if (run->image || !run->bytes) continue;
        if ((uint32_t)run->text + run->bytes > page->bytes) { fail(READER_ERROR_LAYOUT); return false; }
        int ready = epd_app_font_prepare_text(font_for(run->style), page->text + run->text, run->bytes, true);
        if (ready == EPD_APP_FONT_NO_MEMORY) { page_memory_block(page, "page_fonts", READER_ERROR_FONT_MEMORY); return false; }
        if (ready < 0) { fail(READER_ERROR_FONT_MEMORY); return false; }
        if (!ready && pending == page->count) pending = i;
        if (!ready && view.trace_images_ready && !view.trace_font_wait && elapsed_ms(view.images_ready_at) >= 1000) {
            view.trace_font_wait = true;
            rt_kprintf("reader font_wait session=%u intent=%u run=%u/%u bytes=%u elapsed_ms=%u\n",
                (unsigned)view.session, (unsigned)view.intent, i, page->count,
                (unsigned)run->bytes, elapsed_ms(view.images_ready_at));
        }
    }
    page->warm_run = pending;
    return pending == page->count;
}
static unsigned duration_ms(rt_tick_t ticks)
{ return (unsigned)((uint64_t)ticks * 1000 / RT_TICK_PER_SECOND); }
static unsigned elapsed_ms(rt_tick_t start) { return duration_ms(rt_tick_get() - start); }

static void trace_start(void)
{
    near_trace_finish("target");
    document_prefetch_enable(view.document, false);
    view.trace_pending = true; view.trace_hit = false;
    view.trace_layout_ready = view.trace_images_ready = view.trace_font_wait = false;
    view.trace_read_ticks = view.trace_metric_ticks = view.trace_layout_ticks = 0;
    view.trace_record_wait = view.trace_glyph_wait = 0;
    view.trace_metric_calls = 0;
    view.glyph_turn = true;
    view.glyph_steps = 0; view.glyph_ticks = 0;
    view.trace_published = false;
    view.turn_started = rt_tick_get();
}

static void trace_ready(void)
{
    if (!view.trace_pending || !view.display_pending) return;
    if (!view.trace_layout_ready) {
        view.trace_layout_ready = true; view.layout_ready_at = rt_tick_get();
        if (view.trace_hit) ++view.cache_hits; else ++view.cache_misses;
        rt_kprintf("reader layout page=%u hit=%u ms=%u hits=%u misses=%u\n",
            view.wanted + 1, view.trace_hit, elapsed_ms(view.turn_started), view.cache_hits, view.cache_misses);
        unsigned warmed = 0;
        reader_cached_page_t *p = reader_page_cache_find(&view.cache, view.wanted, view.layout);
        if (p) {
            for (unsigned i = 0; i < p->warm_run; ++i)
                if (!p->runs[i].image) warmed += p->runs[i].bytes;
            warmed += p->warm_byte;
        }
        rt_kprintf("reader target_detail page=%u read_ms=%u layout_ms=%u metric_ms=%u metric_calls=%u warm_bytes=%u record_wait_ms=%u glyph_wait_ms=%u\n",
            view.wanted + 1, (unsigned)((uint64_t)view.trace_read_ticks * 1000 / RT_TICK_PER_SECOND),
            (unsigned)((uint64_t)view.trace_layout_ticks * 1000 / RT_TICK_PER_SECOND),
            (unsigned)((uint64_t)view.trace_metric_ticks * 1000 / RT_TICK_PER_SECOND),
            view.trace_metric_calls, warmed, duration_ms(view.trace_record_wait), duration_ms(view.trace_glyph_wait));
    }
    if (!view.trace_images_ready && images_ready(view.building)) {
        view.trace_images_ready = true; view.images_ready_at = rt_tick_get();
        rt_kprintf("reader resources page=%u images_wait_ms=%u\n",
            view.wanted + 1, elapsed_ms(view.layout_ready_at));
    }
}
static bool cache_contains(unsigned number)
{
    return reader_page_cache_find(&view.cache, number, view.layout) != NULL;
}
static bool cache_in_window(unsigned number)
{
    if (number >= view.count) return false;
    unsigned distance = number > view.wanted ? number - view.wanted : view.wanted - number;
    return distance <= 3 || (view.pages[number + 1].record >= view.cache_begin &&
                            view.pages[number].record < view.cache_end);
}
static void cache_sync(void)
{
    if (view.background_paused) return;
    if (!view.preview && view.preview_cache_active) {
        reader_page_cache_clear(&view.cache);
        view.preview_cache_active = false;
        view.preview_back_scanning = view.preview_back_ready = false;
        view.cache_blocked = view.prefetch_number = UINT_MAX;
        page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
    }
    uint32_t record = view.preview || view.restoring ? view.anchor.record :
        view.wanted < view.count ? view.pages[view.wanted].record : view.index->cursor.record;
    document_chapter_range_t range;
    view.cache_chapter_known = false;
    view.cache_begin = 0;
    view.cache_end = view.progress.end;
    if (document_chapter_range(view.document, record, &range)) {
        view.cache_chapter_known = range.indexed;
        view.cache_begin = range.previous_begin;
        view.cache_end = range.next_end;
        if (view.prefetch_record != record || view.prefetch_publication != range.publication) {
            document_chapter_prefetch(view.document, record);
            view.prefetch_record = record; view.prefetch_publication = range.publication;
        }
    }
    unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
    if (view.cache.wanted != wanted) {
        view.cache_blocked = UINT_MAX;
        /* The completed predecessor window belongs to the previous anchor. */
        view.preview_back_ready = false;
    }
    reader_page_cache_window(&view.cache, wanted, view.cache_begin, view.cache_end);
    if (view.cache_blocked != UINT_MAX && epd_memory_capacity_epoch() != view.blocked_epoch)
        view.cache_blocked = UINT_MAX;
}
static reader_cached_page_t *cache_store(unsigned number, view_page_t *page)
{
    if (view.preview_cache_active && view.preview) {
        if (page == view.index) return NULL;
        number = page == view.prefetch ? view.prefetch_number : view.preview_page;
        /* A fallback can reuse a local id with a different clipped boundary. */
        reader_cached_page_t **at = &view.cache.first;
        while (*at && (*at)->number < number) at = &(*at)->next;
        if (*at && (*at)->number == number &&
            (!cursor_equal((document_cursor_t){(*at)->start.record, (*at)->start.byte}, page->start) ||
             !cursor_equal((document_cursor_t){(*at)->end.record, (*at)->end.byte}, page->cursor))) {
            reader_cached_page_t *old = *at;
            reader_page_cache_remove(&view.cache, old);
        }
        return reader_page_cache_put(&view.cache, number, view.layout, (const epd_reader_page_t *)page);
    }
    if (view.preview && page != view.index && cursor_equal(page->start, view.anchor)) return NULL;
    if (!cache_in_window(number)) return NULL;
    reader_cached_page_t *p = reader_page_cache_put(&view.cache, number, view.layout,
                                                  (const epd_reader_page_t *)page);
    if (!p) {
        if (view.cache_blocked == UINT_MAX)
            rt_kprintf("reader cache full page=%u pages=%u bytes=%u free=%u\n",
                number + 1, view.cache.count, (unsigned)view.cache.bytes, (unsigned)epd_app_psram_available());
        view.cache_blocked = number;
        view.blocked_epoch = epd_memory_capacity_epoch();
    }
    return p;
}
static bool cache_candidate_in_range(unsigned number)
{
    if (number >= view.count || number == view.wanted || !cache_in_window(number)) return false;
    if (view.cache_blocked == UINT_MAX) return true;
    unsigned d = number > view.wanted ? number - view.wanted : view.wanted - number;
    unsigned blocked = view.cache_blocked > view.wanted ? view.cache_blocked - view.wanted : view.wanted - view.cache_blocked;
    return d < blocked;
}
static bool cache_candidate_allowed(unsigned number)
{
    return cache_candidate_in_range(number) && !cache_contains(number);
}
static unsigned cache_candidate(void)
{
    for (unsigned d = 1; d <= 3; ++d) {
        unsigned forward = view.wanted + d, back = view.wanted >= d ? view.wanted - d : UINT_MAX;
        if (view.direction < 0) { unsigned swap = forward; forward = back; back = swap; }
        if (cache_candidate_allowed(forward)) return forward;
        if (cache_candidate_allowed(back)) return back;
    }
    reader_cached_page_t *p = view.cache.first;
    for (unsigned n = view.wanted + 4; n < view.count; ++n) {
        if (view.pages[n].record >= view.cache_end) break;
        while (p && p->number < n) p = p->next;
        if ((!p || p->number != n) && cache_candidate_in_range(n)) return n;
    }
    unsigned end = view.wanted > 3 ? view.wanted - 3 : 0;
    if (end > view.count) end = view.count;
    unsigned begin = 0, hi = end;
    while (begin < hi) {
        unsigned mid = begin + (hi - begin) / 2;
        if (view.pages[mid + 1].record < view.cache_begin) begin = mid + 1;
        else hi = mid;
    }
    unsigned candidate = UINT_MAX;
    p = view.cache.first;
    /* Keep the nearest backward gap while advancing the sorted list once. */
    for (unsigned n = begin; n < end; ++n) {
        bool resident = false;
        while (p && p->number <= n) {
            if (p->number == n && p->layout == view.layout) resident = true;
            p = p->next;
        }
        if (!resident && cache_candidate_in_range(n)) candidate = n;
    }
    return candidate;
}
static reader_cached_page_t *cache_warm_candidate(void)
{
    if (!view.allow_glyphs || (core_waiting() && !view.display_pending)) return NULL;
    unsigned wanted = view.preview_cache_active && view.preview ? view.preview_page : view.wanted;
    if (view.display_pending) {
        reader_cached_page_t *p = reader_page_cache_find(&view.cache, wanted, view.layout);
        if (p && p->warm_run < p->count) return p;
    }
    /* Completed near frames no longer need glyphs; warm one extra layout. */
    if (layout_ahead_ready()) {
        reader_cached_page_t *p = reader_page_cache_find(&view.cache, wanted + VIEW_LAYOUT_AHEAD, view.layout);
        if (p && !p->warm_failed && p->warm_run < p->count) return p;
    }
    /* The displayed page has already drawn its glyphs. Warm the next turns. */
    for (unsigned d = 1; d <= 3; ++d) {
        unsigned numbers[2] = {wanted + d, wanted >= d ? wanted - d : UINT_MAX};
        if (view.direction < 0) { unsigned swap = numbers[0]; numbers[0] = numbers[1]; numbers[1] = swap; }
        for (unsigned i = 0; i < 2; ++i) {
            reader_cached_page_t *p = reader_page_cache_find(&view.cache, numbers[i], view.layout);
            if (p && view.preview_back_scanning && p->number < view.preview_back_base) continue;
            if (p && p->warm_run < p->count) return p;
        }
    }
    return NULL;
}
static bool cache_warm(void)
{
    reader_cached_page_t *p = cache_warm_candidate();
    if (!p) return false;
    if (view.glyph_steps < VIEW_GLYPH_BURST) ++view.glyph_steps;
    if (view.glyph_steps >= VIEW_GLYPH_BURST) view.glyph_turn = false;
    rt_tick_t started = rt_tick_get();
    if (p->warm_scan <= p->warm_run || p->warm_scan >= p->count) {
        p->warm_scan = p->warm_run;
        p->warm_pending = p->count;
    }
    unsigned pending = p->warm_pending, next = p->warm_scan;
    while (next < p->count) {
        const view_run_t *run = &p->runs[next];
        int ready = 1;
        if (!run->image && run->bytes) {
            if ((uint32_t)run->text + run->bytes > p->bytes) ready = -1;
            else ready = epd_app_font_prepare_text(font_for(run->style),
                reader_page_cache_text(p) + run->text, run->bytes, true);
            if (ready < 0) ++p->warm_failed;
            ++view.work_glyph_calls;
        }
        if (!ready && pending == p->count) pending = next;
        ++next;
        if (elapsed_ms(started) >= VIEW_GLYPH_BURST_MS) break;
    }
    p->warm_run = pending < next ? pending : next;
    p->warm_pending = pending;
    p->warm_scan = next == p->count ? p->warm_run : next;
    p->warm_byte = 0;
    rt_tick_t finished = rt_tick_get();
    view.work_glyph_ticks += finished - started;
    if (view.glyph_turn) {
        view.glyph_ticks += finished - started;
        rt_tick_t limit = rt_tick_from_millisecond(VIEW_GLYPH_BURST_MS);
        if (view.glyph_ticks >= (limit ? limit : 1)) view.glyph_turn = false;
    }
    return p->warm_run == p->count;
}
static bool cache_target_pending(void)
{
    return !view.preview && view.allow_glyphs && view.display_pending && !cache_contains(view.wanted) &&
        view.cache_blocked != view.wanted;
}
static void text_turn(void)
{
    view.glyph_turn = true;
    view.glyph_steps = 0; view.glyph_ticks = 0;
    ++view.work_text_steps;
}
static void trace_work(void)
{
    if ((!view.work_text_steps && !view.work_glyph_calls) || elapsed_ms(view.work_logged_at) < 1000) return;
#if VIEW_BACKGROUND_TRACE
    rt_kprintf("reader work page=%u text_steps=%u glyph_calls=%u glyph_ms=%u draw_wait_ms=%u\n",
        view.wanted + 1, view.work_text_steps, view.work_glyph_calls,
        (unsigned)((uint64_t)view.work_glyph_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.work_draw_ticks * 1000 / RT_TICK_PER_SECOND));
#endif
    view.work_text_steps = view.work_glyph_calls = 0;
    view.work_glyph_ticks = view.work_draw_ticks = 0;
    view.work_logged_at = rt_tick_get();
}
static bool page_runnable(const view_page_t *page)
{
    return page_work_allowed(page) && !page->done && (page->cursor.record < view.progress.end || view.progress.complete ||
        (page->cursor.record == view.progress.end && !page->read_request && page_target(page)));
}
static void preview_back_discard(void)
{
    /* Rolling entries are not predecessors of the anchor until the scan ends. */
    while (view.cache.first && view.cache.first->number < view.preview_back_base) {
        reader_cached_page_t *p = view.cache.first;
        reader_page_cache_remove(&view.cache, p);
    }
}
static void preview_back_stop(bool failed)
{
    document_record_cancel(view.back_request); view.back_request = NULL;
    preview_back_discard();
    view.preview_back_scanning = false; view.preview_back_ready = true;
    view.preview_back_seeking = false;
    view.preview_back_exact = view.preview_back_eof = false;
    if (failed) view.preview_back_failed = true;
    view.prefetch_number = UINT_MAX;
    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
}
static bool preview_back_begin(void)
{
    if (!view.preview_cache_active || view.preview_back_failed || view.preview_back_scanning ||
        (!view.turn_pending && (view.preview_back_ready || view.preview_back_blocked))) return false;
    document_cursor_t end = view.turn_pending < 0 ? view.anchor : view.display->start;
    unsigned base = view.preview_page;
    if (!view.turn_pending)
        for (reader_cached_page_t *p = view.cache.first; p && p->number < base; p = p->next) {
            if (p->layout == view.layout) { end = (document_cursor_t){p->start.record, p->start.byte}; base = p->number; break; }
        }
    if (!end.record && !end.byte) { view.preview_back_ready = true; return false; }
    view_visited_page_t visited;
    bool exact = visited_find(end, &visited);
    uint32_t window = view.display->bytes > VIEW_BACK_WINDOW_MAX / 4 ?
        VIEW_BACK_WINDOW_MAX : view.display->bytes * 4;
    if (window < VIEW_BACK_WINDOW_MIN) window = VIEW_BACK_WINDOW_MIN;
    uint32_t low = end.record > window ? end.record - window : 0;
    uint32_t start = 0;
    document_chapter_range_t range;
    if (document_chapter_range(view.document, end.record, &range) && range.indexed &&
        cursor_before((document_cursor_t){range.previous_begin, 0}, end))
        start = range.previous_begin;
    /* Page boundaries already visited also provide validated record offsets. */
    unsigned lo = 0, hi = view.count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (view.pages[mid].record <= low) lo = mid + 1; else hi = mid;
    }
    document_cursor_t checkpoint = {start, 0};
    if (lo && view.pages[lo - 1].record >= start) checkpoint = view.pages[lo - 1];
    if (exact) checkpoint = visited.start;
    start = checkpoint.record;
    view.preview_back_end = end; view.preview_back_base = base;
    if (view.turn_pending < 0) preview_back_discard();
    view.preview_back_scanning = true;
    view.preview_back_exact = exact;
    view.preview_back_eof = exact && visited.eof;
    view.preview_back_seeking = !exact && !view.sparse_stride && start < low;
    view.preview_back_scan = start; view.preview_back_low = low;
    view.preview_back_headers = 0; view.preview_back_seek_ticks = 0;
    view.preview_back_started = rt_tick_get();
    view.prefetch_number = base - 1;
    page_reset(view.prefetch, checkpoint);
    return true;
}
static bool preview_back_seek_step(void)
{
    if (!page_work_allowed(view.prefetch)) return false;
    uint32_t at = view.preview_back_scan;
    rt_tick_t started = rt_tick_get();
    if (view.back_request && view.back_request_offset != at)
    { document_record_cancel(view.back_request); view.back_request = NULL; }
    if (!view.back_request)
    { view.back_request = document_record_seek_request(view.document, at,
        view.preview_back_low, view.preview_back_end.record, view.turn_pending < 0); view.back_request_offset = at; }
    if (!view.back_request) { page_memory_block(view.prefetch, "seek_request", READER_ERROR_LAYOUT_MEMORY); return false; }
    unsigned headers = 0;
    int ready = document_record_seek_take(view.back_request, &at, &headers);
    if (!ready) return false;
    int reason = ready < 0 ? document_record_result(view.back_request) : BOOK_OK;
    document_record_cancel(view.back_request); view.back_request = NULL;
    if (reason == BOOK_NO_MEMORY) { page_memory_block(view.prefetch, "seek_take", READER_ERROR_LAYOUT_MEMORY); return false; }
    if (ready < 0) { preview_back_stop(true); return false; }
    view.preview_back_seek_ticks += rt_tick_get() - started;
    view.preview_back_headers += headers;
    view.preview_back_seeking = false;
    page_reset(view.prefetch, (document_cursor_t){at, 0});
    return true;
}
static void preview_back_finish(void)
{
    view_page_t *page = view.prefetch;
    document_cursor_t next = page->cursor;
    bool target = view.turn_pending < 0 && cursor_equal(view.preview_back_end, view.anchor);
    if (view.preview_back_exact && (!cursor_equal(next, view.preview_back_end) || !page->count)) {
        /* A stale boundary gets one ordinary window replay, never a retry loop. */
        visited_forget(view.preview_back_end);
        preview_back_stop(false);
        view.preview_back_ready = false;
        return;
    }
    if (view.preview_back_exact) page->eof = view.preview_back_eof;
    if (!cursor_before(page->start, next)) { preview_back_stop(true); return; }
    if (cursor_equal(next, view.preview_back_end) && (target || VIEW_BACKGROUND_TRACE))
        rt_kprintf("reader back_ready headers=%u seek_ms=%u total_ms=%u target=%u\n",
            view.preview_back_headers,
            (unsigned)((uint64_t)view.preview_back_seek_ticks * 1000 / RT_TICK_PER_SECOND),
            elapsed_ms(view.preview_back_started), target ? 1u : 0u);
    if (target && (cursor_equal(next, view.preview_back_end) || page->eof))
        rt_kprintf("reader back_finish session=%u intent=%u start=%u:%u next=%u:%u end=%u:%u count=%u eof=%u\n",
            (unsigned)view.session, (unsigned)view.intent,
            (unsigned)page->start.record, (unsigned)page->start.byte,
            (unsigned)next.record, (unsigned)next.byte,
            (unsigned)view.preview_back_end.record, (unsigned)view.preview_back_end.byte,
            (unsigned)page->count, (unsigned)page->eof);
    if (page->count && !view.preview_back_blocked) {
        reader_cached_page_t **at = &view.cache.first;
        while (*at && (*at)->number < view.preview_back_base) {
            reader_cached_page_t *p = *at;
            if (p->number <= view.preview_back_base - 3) {
                reader_page_cache_remove(&view.cache, p);
            } else { --p->number; at = &p->next; }
        }
        if (!reader_page_cache_put(&view.cache, view.preview_back_base - 1, view.layout, (epd_reader_page_t *)page)) {
            view.preview_back_blocked = true;
            preview_back_discard();
            if (!target) { preview_back_stop(false); return; }
        }
    }
    if (target && cursor_equal(next, view.preview_back_end) && page->count) {
        view.preview_back_scanning = false;
        view.preview_back_exact = view.preview_back_eof = false;
        view.preview_page = view.preview_back_base - 1;
        unsigned ahead, back;
        reader_page_cache_span(&view.cache, view.preview_page, view.layout, &ahead, &back);
        /* Reaching the requested entry does not establish its predecessors.
         * A saved chapter index can start this scan at the last page. */
        view.preview_back_ready = back >= 3 || window_start(state, page->start);
        view.anchor = page->start; view.restoring = true; view.turn_pending = 0;
        view.preview_clipped = true; view.preview_end = next;
        view.prefetch = view.building; view.building = page;
        view.prefetch_number = UINT_MAX;
        page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
        target_finish();
        return;
    }
    if (cursor_equal(next, view.preview_back_end) || page->eof) {
        if (target && !page->count && !page->start.record && !page->start.byte &&
            cursor_equal(next, view.preview_back_end) && view.display->count &&
            view.display_layout == view.layout && cursor_equal(view.display->start, next)) {
            /* A metadata-only prefix has no preceding reading page. */
            preview_back_stop(false);
            view.segment_turns = view.turn_remaining - 1;
            view.turn_remaining = view.turn_pending = 0;
            page_reset(view.building, view.anchor);
            epd_reader_page_copy((epd_reader_page_t *)view.building, (epd_reader_page_t *)view.display);
            view.display_pending = false;
            return;
        }
        if (target) { preview_back_stop(true); return; }
        view.preview_back_scanning = false; view.preview_back_ready = true;
        if (view.preview_back_exact) {
            unsigned ahead, back;
            reader_page_cache_span(&view.cache, view.preview_page, view.layout, &ahead, &back);
            view.preview_back_ready = back >= 3 || (!page->start.record && !page->start.byte);
        }
        view.preview_back_exact = view.preview_back_eof = false;
        view.prefetch_number = UINT_MAX;
        page_reset(page, (document_cursor_t){UINT32_MAX, 0});
    } else page_reset(page, next);
}
static bool preview_cached_target(void)
{
    if (!view.preview_cache_active || !view.preview) return false;
    for (reader_cached_page_t *p = view.cache.first; p; p = p->next) {
        if (view.preview_back_scanning && p->number < view.preview_back_base) continue;
        document_cursor_t cursor = view.turn_pending < 0 ?
            (document_cursor_t){p->end.record, p->end.byte} :
            (document_cursor_t){p->start.record, p->start.byte};
        if (p->layout != view.layout || !cursor_equal(cursor, view.anchor)) continue;
        document_cursor_t end = {p->end.record, p->end.byte};
        if (view.preview_clipped && !cursor_equal(end, view.preview_end)) continue;
        view.preview_page = p->number;
        view.anchor = (document_cursor_t){p->start.record, p->start.byte};
        view.preview_clipped = !p->eof; view.preview_end = end; view.turn_pending = 0;
        page_reset(view.building, view.anchor);
        reader_page_cache_load(p, (epd_reader_page_t *)view.building);
        view.trace_hit = true;
        return true;
    }
    return false;
}
static unsigned preview_candidate(document_cursor_t *start)
{
    if (!view.preview_cache_active || !view.preview || !preview_visible() ||
        view.display_pending || view.turn_pending) return UINT_MAX;
    unsigned next = view.preview_page + 1;
    *start = view.display->cursor;
    if (view.display->eof) return UINT_MAX;
    for (reader_cached_page_t *p = view.cache.first; p; p = p->next) {
        if (p->number < next) continue;
        if (p->number != next || p->layout != view.layout ||
            !cursor_equal(*start, (document_cursor_t){p->start.record, p->start.byte})) break;
        if (p->eof) return UINT_MAX;
        *start = (document_cursor_t){p->end.record, p->end.byte}; ++next;
    }
    /* One extra layout uses already published input and never owns a pixel frame. */
    bool ahead = next > view.preview_page && next - view.preview_page == VIEW_LAYOUT_AHEAD &&
        layout_ahead_ready() && start->record < view.progress.end &&
        (view.prefetch_number != next || view.prefetch->done || page_runnable(view.prefetch));
    if (next > view.preview_page && next - view.preview_page == VIEW_LAYOUT_AHEAD &&
        view.selected && view.direction >= 0 && !ahead) return UINT_MAX;
    if (next == view.cache_blocked || (next == view.prefetch_number && !page_work_allowed(view.prefetch)) ||
        (view.progress.complete && start->record >= view.progress.end) ||
        (next - view.preview_page > 3 && ((!view.cache_chapter_known && !ahead) ||
                                           start->record >= view.cache_end))) return UINT_MAX;
    return next;
}
static bool preview_ahead_pending(void)
{
    if (!view.preview_cache_active || !view.preview || !layout_ahead_ready()) return false;
    reader_cached_page_t *page = reader_page_cache_find(&view.cache,
        view.preview_page + VIEW_LAYOUT_AHEAD, view.layout);
    if (page) return !page->warm_failed && page->warm_run < page->count;
    document_cursor_t start;
    unsigned next = preview_candidate(&start);
    return next != UINT_MAX && next > view.preview_page &&
        next - view.preview_page == VIEW_LAYOUT_AHEAD;
}
static void preview_prefetch_finish(void)
{
    if (view.preview_back_scanning) { preview_back_finish(); return; }
    if (!cache_store(view.prefetch_number, view.prefetch)) {
        view.cache_blocked = view.prefetch_number;
        view.blocked_epoch = epd_memory_capacity_epoch();
    }
    if (view.selected && view.prefetch_number >= view.preview_page &&
        view.prefetch_number - view.preview_page <= 3)
        rt_kprintf("reader near_layout session=%u page=%u current=%u cached=%u tick=%u start=%u:%u end=%u:%u\n",
            (unsigned)view.session, view.prefetch_number, view.preview_page,
            view.cache_blocked != view.prefetch_number, (unsigned)rt_tick_get(),
            view.prefetch->start.record, view.prefetch->start.byte,
            view.prefetch->cursor.record, view.prefetch->cursor.byte);
}
static void preview_back(document_cursor_t end)
{
    unsigned lo = 0, hi = view.count;
    while (lo + 1 < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (cursor_before(view.pages[mid], end)) lo = mid; else hi = mid;
    }
    view.preview_end = end; view.anchor = view.pages[lo]; view.wanted = lo;
    view.preview_clipped = true; view.restoring = false;
    if (view.preview_cache_active && view.preview_page) --view.preview_page;
    page_reset(view.building, view.anchor);
    view.display_pending = view.notified = false;
}
static void restore_target(void)
{
    if (view.sparse_stride) {
        if (view.preview && view.turn_pending < 0 && !preview_cached_target()) {
            if (!view.preview_back_scanning) preview_back_begin();
            if (view.preview_back_failed) fail(READER_ERROR_PAGE_READ);
        }
        return;
    }
    if (view.preview && view.turn_pending < 0) {
        if (preview_cached_target()) return;
        if (view.preview_back_scanning && !cursor_equal(view.preview_back_end, view.anchor))
            preview_back_stop(false);
        view_visited_page_t visited;
        if (!view.preview_back_scanning && visited_find(view.anchor, &visited) && preview_back_begin()) return;
        if (!view.preview_back_scanning && view.count &&
            (view.complete || !cursor_before(view.pages[view.count], view.anchor))) {
            preview_back(view.anchor); view.turn_pending = 0;
            return;
        }
        if (view.preview_back_scanning || preview_back_begin()) return;
        if (view.preview_back_failed) { fail(READER_ERROR_PAGE_READ); return; }
    }
    if (view.preview && !preview_visible() && !view.turn_pending) return;
    if (view.restoring && view.count && (view.complete || cursor_before(view.anchor, view.pages[view.count])))
    {
        unsigned lo = 0, hi = view.count;
        while (lo + 1 < hi)
        {
            unsigned mid = lo + (hi - lo) / 2;
            if (!cursor_before(view.anchor, view.pages[mid])) lo = mid; else hi = mid;
        }
        view.wanted = lo;
        if (view.preview && view.turn_pending < 0) {
            preview_back(view.anchor);
        }
        view.turn_pending = 0;
        view.restoring = false;
    }
    if (view.complete && view.wanted >= view.count) view.wanted = view.count ? view.count - 1 : 0;
    cache_sync();
    if (target_needed() && !view.display_pending) {
        reader_cached_page_t *p = reader_page_cache_find(&view.cache, view.wanted, view.layout);
        if (p && cursor_equal((document_cursor_t){p->start.record, p->start.byte}, view.pages[view.wanted])) {
            page_reset(view.building, (document_cursor_t){p->start.record, p->start.byte});
            reader_page_cache_load(p, (epd_reader_page_t *)view.building);
            view.display_pending = true; view.notified = false;
            view.trace_hit = true;
        }
    }
}
static void sparse_compact(void)
{
    unsigned kept = view.count / 2;
    for (unsigned i = 1; i <= kept; ++i) view.pages[i] = view.pages[i * 2];
    view.count = kept;
    view.sparse_stride *= 2;
}
static void sparse_seek_page(unsigned number)
{
    unsigned slot = number / view.sparse_stride;
    if (slot > view.count) slot = view.count;
    preview_start(view.pages[slot]);
    view.sparse_next = slot * view.sparse_stride;
    view.sparse_goal = number; view.sparse_seek = true;
    view.display_pending = view.notified = false;
}
static void index_finish(void)
{
    if (!view.index->count && view.index->eof && indexed_pages(state)) { view.complete = true; return; }
    if (!view.sparse_stride && !reserve_pages(view.count + 2)) {
        view.sparse_pages = view.count; view.sparse_stride = 1;
        sparse_compact();
        /* Preserve the bound bitmap. Only a not-yet-visible numeric request
         * needs replay from its nearest retained checkpoint. */
        if (!view.preview) {
            if (core_waiting()) sparse_seek_page(view.wanted);
            else { preview_start(view.display->start); view.restoring = false; }
        }
    }
    if (view.sparse_stride) {
        document_cursor_t end = view.index->cursor;
        if (view.preview && !cursor_before(view.anchor, view.index->start) && cursor_before(view.anchor, end)) {
            view.numbered_cursor = view.anchor; view.numbered_page = view.sparse_pages + 1;
        }
        ++view.sparse_pages;
        if (!(view.sparse_pages % view.sparse_stride)) {
            if (view.count + 1 >= view.capacity) sparse_compact();
            if (!(view.sparse_pages % view.sparse_stride)) view.pages[++view.count] = end;
        }
        view.complete = view.index->eof;
        page_reset(view.index, end); view.index->collect = false;
        return;
    }
    unsigned number = view.count++;
    view.pages[view.count] = view.index->cursor;
    view.complete = view.index->eof;
    restore_target();
    if (view.index->collect)
    {
        if (!view.preview && !view.restoring && !view.link_target[0] && number == view.wanted)
        {
            view_page_t *old = view.building;
            view.building = view.index; view.index = old;
            view.display_pending = true; view.notified = false;
        }
        else cache_store(number, view.index);
    }
    page_reset(view.index, view.pages[view.count]);
    view.index->collect = !view.preview && (view.restoring || view.count <= view.wanted + 3 ||
        (view.pages[view.count].record >= view.cache_begin && view.pages[view.count].record < view.cache_end));
}

static bool link_process(void)
{
    if (!view.link_target[0]) return false;
    document_prefetch_enable(view.document, false);
    int complete = document_target_require(view.document, true);
    uint32_t record;
    if (document_anchor_find(view.document, view.link_target, &record))
    {
        view.anchor = (document_cursor_t){record, 0};
        view.restoring = true; link_clear();
        view.display_pending = view.notified = false;
        return true;
    }
    if (complete != 0)
    { link_clear(); fail(READER_ERROR_LINK_MISSING); }
    return false;
}
static bool page_read_failed(void)
{
    if (!view.from_cache) { fail(READER_ERROR_PAGE_READ); return false; }
    view.from_cache = view.complete = view.index_saved = false;
    view.index_saved_count = 0; view.index_save_failed = false;
    view.anchor = view.pages[view.wanted]; view.restoring = true;
    view.count = view.toc_count = view.toc_scanned = 0;
    view.pages[0] = (document_cursor_t){0, 0};
    page_reset(view.index, view.pages[0]); page_reset(view.building, view.pages[0]);
    reader_page_cache_clear(&view.cache); view.cache_blocked = UINT_MAX;
    view.prefetch_number = UINT_MAX;
    images_cancel_pending();
    return true;
}

static void target_finish(void)
{
    if (view.sparse_seek) {
        if (!view.building->count && view.building->eof && view.sparse_next) {
            sparse_seek_page(view.sparse_next - 1);
            return;
        }
        if (view.sparse_next < view.sparse_goal && !view.building->eof) {
            ++view.sparse_next;
            view.anchor = view.building->cursor;
            page_reset(view.building, view.anchor);
            return;
        }
        view.wanted = view.sparse_next; view.sparse_seek = false;
        view.numbered_cursor = view.building->start; view.numbered_page = view.sparse_next + 1;
        view.restoring = false;
    }
    if (view.deferred_turns) { view.turn_remaining = view.deferred_turns; view.deferred_turns = 0; }
    if (view.forward_step) {
        view.forward_step = false;
        if (!view.building->count && view.building->eof) {
            /* Trailing metadata does not form another reading page. */
            view.segment_turns = view.turn_remaining + 1; view.turn_remaining = 0;
            view.anchor = view.forward_from; view.display_pending = false;
            return;
        }
    }
    if (view.preview_cache_active && view.preview) cache_store(view.preview_page, view.building);
    if (view.preview && view.turn_remaining > 0 && !view.building->eof &&
        !(view.progress.complete && view.building->cursor.record == view.progress.end)) {
        --view.turn_remaining;
        view.forward_step = true; view.forward_from = view.building->start;
        view.anchor = view.building->cursor; view.restoring = true;
        if (view.preview_cache_active) ++view.preview_page;
        view.preview_clipped = false;
        page_reset(view.building, view.anchor);
        return;
    }
    if (view.preview && view.turn_remaining < 0 && !cursor_at_segment_start(view.building->start)) {
        ++view.turn_remaining;
        view.anchor = view.building->start; view.turn_pending = -1;
        view.preview_clipped = false;
        if (preview_cached_target()) return;
        page_reset(view.building, view.anchor);
        view.restoring = true;
        return;
    }
    if (view.turn_remaining) {
        view.segment_turns = view.turn_remaining;
        view.turn_remaining = 0;
        view.display_pending = false;
        return;
    }
    view.display_pending = true; view.notified = false;
}
static void preview_ahead_retire(void)
{
    if (!view.preview_cache_active || !view.preview || view.preview_back_scanning ||
        !layout_ahead_expired() || view.prefetch_number <= view.preview_page ||
        view.prefetch_number - view.preview_page != VIEW_LAYOUT_AHEAD) return;
    /* A slow optional provider must release the shared page for reverse work. */
    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
    view.prefetch_number = UINT_MAX;
}
static bool index_work_needed(const view_state_t *owner)
{
    if (!owner->index || owner->complete || owner->failure) return false;
    if (pagination_requested() || owner->numeric_target || owner->link_target[0] ||
        !owner->status.ready || owner->turn_pending < 0 || (!owner->preview && owner->restoring)) return true;
    if (owner->preview) return false;
    unsigned count = indexed_pages(owner);
    return count <= owner->wanted || count - owner->wanted <= VIEW_LAYOUT_AHEAD;
}

static bool compute_one(bool foreground)
{
    if (request_memory_failed(state)) return false;
    if (view.segment_turns) return false;
    preview_ahead_retire();
    view.compute_foreground = foreground || (view.background_paused && core_waiting());
    if (cache_target_pending()) cache_store(view.wanted, view.building);
    if (view.compute_job)
    {
        int status;
        /* The worker owns a separate batch; nearby glyphs can progress while it runs. */
        if (!reader_compute_poll(view.compute_job, &status)) return cache_warm();
        view_page_t *p = view.compute_page;
        int result = p == view.prefetch ? near_page_step() : page_step(p, p == view.index, NULL, &view.progress);
        if (p == view.index)
        {
            if (result < 0) { fail(READER_ERROR_LAYOUT); return false; }
            if (p->done && (!view.background_paused || view.restoring ||
                view.link_target[0] || view.wanted >= view.count)) index_finish();
        }
        else if (p == view.building)
        {
            if (result < 0 || (p->done && !view.preview && !cursor_equal(p->cursor, view.pages[view.wanted + 1])))
                return page_read_failed();
            if (p->done) target_finish();
        }
        else if (p == view.prefetch) {
            if (view.preview && view.preview_cache_active) {
                if (result < 0) {
                    view.cache_blocked = view.prefetch_number;
                    view.blocked_epoch = epd_memory_capacity_epoch();
                    if (view.preview_back_scanning) preview_back_stop(true);
                } else if (p->done && !view.background_paused) preview_prefetch_finish();
            } else if (result < 0 || (p->done && !cursor_equal(p->cursor, view.pages[view.prefetch_number + 1]))) {
                view.cache_blocked = view.prefetch_number;
                view.blocked_epoch = epd_memory_capacity_epoch();
                rt_kprintf("reader prefetch page=%u error=layout\n", view.prefetch_number + 1);
                view.prefetch_number = UINT_MAX;
                page_reset(p, (document_cursor_t){UINT32_MAX, 0});
            } else if (p->done && !view.background_paused) cache_store(view.prefetch_number, p);
        }
        return true;
    }
    if (view.link_target[0] && link_process()) return true;
    if (view.failure) return false;
    if (view.background_paused && !core_waiting()) return false;
    if (view.preview && (!preview_visible() || view.turn_pending) && !view.display_pending &&
        preview_cached_target()) { target_finish(); return true; }
    if (view.preview && !preview_visible() && !view.turn_pending) {
        if (view.display_pending) return false;
        if (view.building->done) { target_finish(); return true; }
        if (!page_runnable(view.building)) return false;
        int result = page_step(view.building, false, view.page_reader, &view.progress);
        if (result < 0) { fail(READER_ERROR_PAGE_READ); return false; }
        if (view.building->done) target_finish();
        return result > 0;
    }
    /* Resolve an indexed predecessor before admitting a reverse scan. */
    if (view.preview && view.turn_pending < 0 && !view.preview_back_scanning &&
        view.count && (view.complete || !cursor_before(view.pages[view.count], view.anchor)))
        restore_target();
    if (view.preview && view.preview_cache_active && (preview_visible() || view.turn_pending < 0) &&
        (!view.background_paused || view.turn_pending < 0)) {
        unsigned ahead, back;
        reader_page_cache_span(&view.cache, view.preview_page, view.layout, &ahead, &back);
        /* Finish forward near frames before lending the shared page to reverse work. */
        unsigned behind = view.direction < 0 ? VIEW_NEAR_PAGES : VIEW_OPPOSITE_PIXELS;
        if (!view.preview_back_scanning && back < behind &&
            (ahead >= 3 || view.direction < 0 || view.turn_pending < 0 || view.display->eof) &&
            (!view.selected || view.direction < 0 || view.turn_pending < 0 || !near_direction_pending()) &&
            (view.direction < 0 || !preview_ahead_pending()))
            preview_back_begin();
        if (view.preview_back_scanning) {
            if (view.preview_back_seeking) return preview_back_seek_step();
            if (view.prefetch->done) { preview_back_finish(); return true; }
            if (page_runnable(view.prefetch)) {
                text_turn();
                int result = page_step(view.prefetch, false, view.page_reader, &view.progress);
                if (result < 0) preview_back_stop(true);
                else if (view.prefetch->done) preview_back_finish();
                return result > 0;
            }
        }
    }
    /* A paused batch can finish an index page before its boundary is appended. */
    if (!view.complete && view.index->done && (!view.background_paused ||
        view.restoring || view.link_target[0] || view.wanted >= view.count)) {
        index_finish();
        return true;
    }
    bool pending = view.display_pending;
    restore_target();
    if (!pending && view.display_pending) return true;
    if (!view.background_paused && view.preview_cache_active && preview_visible() && !view.turn_pending) {
        if (view.glyph_turn && cache_warm()) return true;
        document_cursor_t start;
        unsigned number = preview_candidate(&start);
        if (number != UINT_MAX) {
            text_turn();
            if (view.prefetch_number != number || !cursor_equal(view.prefetch->start, start)) {
                near_trace_finish("replace");
                view.prefetch_number = number; page_reset(view.prefetch, start);
                if (view.selected && number >= view.preview_page && number - view.preview_page <= 3)
                    rt_kprintf("reader near_start session=%u page=%u current=%u tick=%u start=%u:%u\n",
                        (unsigned)view.session, number, view.preview_page, (unsigned)rt_tick_get(),
                        start.record, start.byte);
            }
            if (view.prefetch->done) { preview_prefetch_finish(); return true; }
            if (page_runnable(view.prefetch)) {
                int result = near_page_step();
                if (result < 0) {
                    view.cache_blocked = number; view.blocked_epoch = epd_memory_capacity_epoch();
                } else if (view.prefetch->done) preview_prefetch_finish();
                return result > 0;
            }
        }
    }
    if ((foreground || view.background_paused) && target_needed() && !view.display_pending)
    {
        if (!cursor_equal(view.building->start, view.pages[view.wanted]))
            page_reset(view.building, view.pages[view.wanted]);
        if (view.building->done)
        { view.display_pending = true; view.notified = false; return true; }
        if (page_runnable(view.building))
        {
            int result = page_step(view.building, false, view.page_reader, &view.progress);
            if (view.building->done && !cursor_equal(view.building->cursor, view.pages[view.wanted + 1]))
                result = -1;
            if (result < 0)
                return page_read_failed();
            if (view.building->done) { view.display_pending = true; view.notified = false; }
            return result > 0;
        }
    }
    if (!view.background_paused && !view.preview && !view.restoring && !view.link_target[0] && view.wanted < view.count) {
        /* Credit persists across service calls. Each bounded glyph burst gives
         * runnable text a turn; a pending worker can use otherwise idle time. */
        if (view.glyph_turn && cache_warm()) return true;
        unsigned number = cache_candidate();
        bool grow_forward = !view.complete && view.count <= view.wanted + 3 && page_runnable(view.index);
        if (grow_forward && number < view.wanted) {
            unsigned back = view.wanted - number, forward = view.count - view.wanted;
            if (back < forward || (back == forward && view.direction < 0)) grow_forward = false;
        }
        if (!grow_forward && number != UINT_MAX) {
            text_turn();
            if (view.prefetch_number != number || !cursor_equal(view.prefetch->start, view.pages[number])) {
                near_trace_finish("replace");
                view.prefetch_number = number;
                page_reset(view.prefetch, view.pages[number]);
            }
            if (view.prefetch->done) {
                if (cursor_equal(view.prefetch->cursor, view.pages[number + 1]))
                    cache_store(number, view.prefetch);
                else {
                    view.cache_blocked = number; view.blocked_epoch = epd_memory_capacity_epoch();
                    view.prefetch_number = UINT_MAX;
                    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
                }
                return false;
            }
            int result = near_page_step();
            if (result < 0) {
                view.cache_blocked = number; view.blocked_epoch = epd_memory_capacity_epoch();
                rt_kprintf("reader prefetch page=%u error=read\n", number + 1);
                view.prefetch_number = UINT_MAX;
                page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
            }
            return result > 0;
        }
    }
    if (!view.index_loading && index_work_needed(state) && page_runnable(view.index) &&
        (!view.background_paused || core_waiting()))
    {
        text_turn();
        int result = page_step(view.index, true, view.index_reader, &view.progress);
        if (result < 0) fail(READER_ERROR_LAYOUT);
        if (result > 0 && view.index->done) index_finish();
        return result > 0;
    }
    return cache_warm();
}
static bool target_runnable(void)
{
    int status;
    if (view.compute_job && !reader_compute_poll(view.compute_job, &status)) return false;
    if (view.failure) return false;
    if (view.preview && view.turn_pending < 0 && view.preview_back_scanning)
        return view.preview_back_seeking || view.prefetch->done || page_runnable(view.prefetch);
    if (view.preview && !view.turn_pending) return !preview_visible() && !view.display_pending &&
        (view.building->done || page_runnable(view.building));
    if (view.link_target[0]) return view.link_scan < view.progress.end || view.progress.complete;
    if (view.restoring && !view.sparse_stride)
    {
        if (view.count && (view.complete || cursor_before(view.anchor, view.pages[view.count]))) return true;
        return !view.complete && (view.index->done || page_runnable(view.index));
    }
    if (view.wanted >= view.count) return !view.complete && (view.index->done || page_runnable(view.index));
    if (target_needed() && !view.display_pending)
    {
        if (!cursor_equal(view.building->start, view.pages[view.wanted]) ||
            view.building->done || page_runnable(view.building)) return true;
        if (cache_contains(view.wanted)) return true;
    }
    return false;
}
static bool compute_runnable(bool foreground)
{
    if (request_memory_failed(state)) return false;
    int status;
    if (view.compute_job) return reader_compute_poll(view.compute_job, &status) ||
        (!view.background_paused && (cache_target_pending() || cache_warm_candidate() != NULL));
    if (view.failure) return false;
    if (view.background_paused) return target_runnable();
    if ((foreground || view.restoring || view.link_target[0]) && target_runnable()) return true;
    if (view.preview_cache_active && view.preview) {
        document_cursor_t start;
        if (view.preview_back_scanning) return page_runnable(view.prefetch) || view.prefetch->done;
        if (preview_candidate(&start) != UINT_MAX || cache_warm_candidate()) return true;
    }
    if (!view.preview && !view.restoring && !view.link_target[0] && view.wanted < view.count) {
        if (cache_target_pending()) return true;
        if (cache_candidate() != UINT_MAX) return true;
        if (cache_warm_candidate()) return true;
    }
    return index_work_needed(state) && (view.index->done || page_runnable(view.index));
}
static void publish_state(bool foreground)
{
    if (view.failure)
    {
        if (view.status.error_code != view.failure)
        { core_set_error_code(view.failure); ++view.status.revision; }
        view.status.indexing = false;
        return;
    }
    bool finished = view.complete && view.status.pages != indexed_pages(state);
    view.status.indexing = !view.complete;
    if (view.complete) view.status.pages = indexed_pages(state);
    if (preview_visible() && !view.restoring && view.status.page != view.wanted + 1) {
        view.status.page = view.wanted + 1;
        ++view.status.revision;
    }
    if (foreground && view.display_pending && !view.notified && images_ready(view.building) && page_fonts_ready(view.building))
    {
        view.notified = true; ++view.status.revision;
        if (view.trace_pending) {
            view.published_at = rt_tick_get(); view.trace_published = true;
            rt_kprintf("reader publish page=%u ready_wait_ms=%u\n", view.wanted + 1,
                view.trace_images_ready ? (unsigned)((uint64_t)(view.published_at - view.images_ready_at) *
                    1000 / RT_TICK_PER_SECOND) : 0);
        }
    }
    (void)finished;
}
static void core_background_pause(bool paused)
{
    if (view.background_paused == paused) return;
    view.background_paused = paused;
    if (paused) {
        view.allow_glyphs = false;
        /* Foreground layout also waits for asynchronous records and glyphs.
         * Pausing speculation must not reopen its compute session per batch. */
        if (!core_waiting()) reader_compute_idle();
        if (view.document) view_input_set(state, false);
    }
}

static void document_flow(void)
{
    view_input_set(state, !view.link_target[0] && (view.numeric_target ||
        (!view.background_paused && view.status.ready && !core_waiting() &&
         !near_frames_pending() &&
         (state != primary_state || !view.progress.complete || !window_neighbor_work(state)))));
}

static reader_work_t core_step(uint32_t budget_ms, bool foreground, bool commit)
{
    view.work_exhausted = false;
    if (!view.document) return READER_WORK_IDLE;
    rt_tick_t started = rt_tick_get();
    rt_tick_t budget = rt_tick_from_millisecond(budget_ms > INT_MAX ? INT_MAX : (int)budget_ms);
    if (!budget) budget = 1;
    document_progress(view.document, &view.progress);
    if (view.progress.result)
    {
        document_status_t status;
        document_status(view.document, &status);
        if (view_interrupted()) return READER_WORK_IDLE;
        fail(status.error_code ? status.error_code : READER_ERROR_IO);
    }
    if (view_interrupted()) return READER_WORK_IDLE;
    /* The owner installs settings at display idle. Never publish an older
     * prepared page while a replacement configuration is waiting. */
    document_flow();
    if (!view.failure && !fonts_ready()) return READER_WORK_IDLE;
    if (!view.background_paused || core_waiting()) index_load();
    cache_sync();
    view.allow_pixels = commit;
    /* First publication returns immediately; later calls may fill the bind wait. */
    bool present = foreground && commit && core_waiting() && !view.notified;
    view.allow_glyphs = !view.background_paused;
    view.glyph_waited = false;
    view.work_started = started; view.work_budget = budget;
    if (budget_ms && !view.failure)
        while ((!present || !view.display_pending) && compute_one(foreground)) {
            if (view.display_pending && !view.trace_layout_ready) trace_ready();
            if (view.failure) break;
            if (rt_tick_get() - started >= budget) { view.work_exhausted = true; break; }
        }
    view.work_budget = 0;
    if (view_interrupted()) return READER_WORK_IDLE;
    if (!view.failure)
    {
        trace_ready();
        if (view.allow_pixels) {
            if (!view.background_paused) images_prune(false);
            if (view.display_pending) images_prepare(view.building, view.selected);

        }
        /* Publish the target before starting optional I/O. */
        if (!view.background_paused && view.status.ready && !core_waiting())
        {
            if (view.allow_pixels)
                for (reader_cached_page_t *p = view.cache.first; p && !view.failure; p = p->next) {
                    if (image_page_nearby(p)) images_prepare_runs(p->runs, p->count, false);
                }
            index_save(false);
        }
        trace_ready();
        if (!present) trace_work();
        if (VIEW_BACKGROUND_TRACE && view.cache_logged_count != view.cache.count && !view.background_paused && elapsed_ms(view.cache_logged_at) >= 1000) {
            unsigned ahead = 0, back = 0;
            reader_page_cache_span(&view.cache, view.preview_cache_active && view.preview ? view.preview_page : view.wanted,
                                   view.layout, &ahead, &back);
            rt_kprintf("reader cache page=%u pages=%u ahead=%u back=%u bytes=%u window=%u..%u\n",
                view.wanted + 1, view.cache.count, ahead, back, (unsigned)view.cache.bytes,
                view.cache_begin, view.cache_end);
            view.cache_logged_count = view.cache.count; view.cache_logged_at = rt_tick_get();
        }
    }
    if (view.failure) { near_trace_finish("error"); compute_cancel(); images_cancel_pending(); }
    if (commit)
    {
        document_status_t status;
        document_status(view.document, &status);
        if (status.title[0]) snprintf(view.title, sizeof(view.title), "%s", status.title);
        publish_state(foreground);
    }
    if (foreground && target_runnable()) return READER_WORK_TARGET;
    if (compute_runnable(foreground)) return READER_WORK_BACKGROUND;
    /* A foreground input wait is still part of the current layout operation.
     * Actual reader pause/suspend requests release in worker_quiesce. */
    if (view.background_paused && !core_waiting()) reader_compute_idle();
    return READER_WORK_IDLE;
}

static void core_seek(unsigned page)
{
    if (!page || !view.document || view.failure) return;
    if (view.status.pages && page > view.status.pages) page = view.status.pages;
    if (page > READER_PAGE_MAX) page = READER_PAGE_MAX;
    if (!view.preview && !view.restoring && !view.link_target[0] && view.wanted == page - 1) return;
    compute_cancel();
    trace_start();
    view.numeric_target = true;
    if (view.sparse_stride) {
        sparse_seek_page(page - 1);
        images_prune(false);
        return;
    }
    view.preview = false; view.turn_pending = view.turn_remaining = 0;
    view.preview_clipped = false;
    if (page - 1 != view.wanted) view.direction = page - 1 > view.wanted ? 1 : -1;
    view.restoring = false; view.wanted = page - 1; link_clear();
    view.display_pending = view.notified = false;
    page_reset(view.building, view.wanted < view.count ? view.pages[view.wanted] : (document_cursor_t){0, 0});
    restore_target();
    images_prune(false);
}
static void core_cancel_pending(void)
{
    if (!view.status.ready || view.failure) return;
    compute_cancel();
    view.sparse_seek = false;
    view.turn_pending = view.turn_remaining = 0;
    if (view.display_layout != view.layout || view.restoring || view.preview)
    {
        /* The visible page number belongs to the previous layout. Resolve its
         * original text cursor again, even if a new page is already prepared. */
        view.anchor = view.display->start;
        view.restoring = true;
        view.preview = true;
        view.preview_clipped = false;
        page_reset(view.building, view.anchor);
        link_clear(); view.link_scan = 0;
        view.display_pending = view.notified = false;
        if (view.preview_cache_active && view.display_layout == view.layout)
            preview_cached_target();
        images_cancel_pending();
        images_prune(true);
        return;
    }
    if (!preview_visible()) core_seek(view.status.page);
}
/* The same owner keeps its document alive. A reset stamps the layout and
 * session before any read or compute request can borrow this scratch page. */
static bool prefetch_turn_target(int direction)
{
    return direction == 1 && view.preview_cache_active && !view.preview_back_scanning &&
        view.display_layout == view.layout && !view.display->eof &&
        !(view.progress.complete && view.display->cursor.record == view.progress.end) &&
        view.prefetch_document == view.document && view.prefetch_layout == view.layout &&
        view.prefetch_session == view.session && view.prefetch->generation && view.prefetch->collect &&
        view.preview_page < UINT_MAX && view.prefetch_number == view.preview_page + 1 &&
        cursor_equal(view.prefetch->start, view.display->cursor) &&
        !cursor_before(view.prefetch->cursor, view.prefetch->start);
}
static void prefetch_take_target(void)
{
    view_page_t *old = view.building;
    view.building = view.prefetch; view.prefetch = old;
    view.prefetch_number = UINT_MAX;
    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
    view.building->wait_kind = 0;
    if (view.compute_page == view.building) {
        view.compute_traced = true; view.compute_started = rt_tick_get();
    }
    rt_kprintf("reader layout_handoff session=%u intent=%u start=%u:%u cursor=%u:%u generation=%u compute=%u read=%u\n",
        (unsigned)view.session, (unsigned)view.intent, view.building->start.record, view.building->start.byte,
        view.building->cursor.record, view.building->cursor.byte, (unsigned)view.building->generation,
        view.compute_page == view.building, view.building->read_request != NULL);
}
static void core_turn(int direction)
{
    if (!direction || !view.document || view.failure) return;
    bool take_prefetch = prefetch_turn_target(direction);
    near_trace_finish("turn");
    if (!take_prefetch || view.compute_page != view.prefetch) compute_cancel();
    /* A forward turn must release the shared prefetch page from reverse scanning. */
    if (direction > 0 && view.preview_back_scanning) preview_back_stop(false);
    view.sparse_seek = false;
    view.forward_step = false;
    if (!view.preview_cache_active) preview_start(view.display->start);
    link_clear(); view.link_scan = 0;
    view.preview = true; view.restoring = true; view.preview_clipped = false;
    view.display_pending = view.notified = false;
    view.anchor = view.display->start;
    view.turn_pending = 0; view.turn_remaining = direction;
    view.direction = direction < 0 ? -1 : 1;
    page_reset(view.building, view.anchor);
    epd_reader_page_copy((epd_reader_page_t *)view.building, (epd_reader_page_t *)view.display);
    trace_start();
    target_finish();
    if (take_prefetch && view.forward_step && !view.turn_remaining &&
        cursor_equal(view.anchor, view.prefetch->start)) prefetch_take_target();
}
static bool core_waiting(void)
{
    if (view.target_pending || view.segment_turns) return !view.failure;
    if (!view.failure && preview_visible() && !view.turn_pending && !view.display_pending) return false;
    return !view.failure && (!view.status.ready || view.preview || view.restoring || view.link_target[0] ||
        view.wanted + 1 != view.status.page || view.display_pending ||
        view.display_layout != view.layout || target_needed());
}
static void core_seek_cursor(document_cursor_t cursor)
{
    if (!view.document || view.failure) return;
    trace_start();
    compute_cancel();
    preview_start(cursor);
    link_clear(); view.link_scan = 0;
    view.display_pending = view.notified = false;
    images_cancel_pending();
    restore_target();
    images_prune(true);
}

static void core_set_error_code(reader_error_t error)
{
    view.status.error_code = error;
}

/* The worker owns mutable layout state. The mailbox carries only ownership
 * transfers and small interaction intents; it is never held across a wait. */
struct view_frame {
    view_frame_t *next, *backing;
    view_state_t *owner;
    unsigned references, number;
    uint32_t intent, generation, layout, handoff_intent;
    size_t window_charge;
    bool complete, required, cancelled, drop_reported, discard_pixels;
    epd_compute_buffer_t pixels;
    reader_frame_store_t cold;
    reader_frame_block_t *thaw_next;
    size_t thaw_offset;
    uint32_t cold_intent, thaw_intent;
    bool cold_attempted, cold_reusing;
    epd_reader_render_t renderer;
    view_image_t *render_image;
    lv_image_dsc_t image;
    reader_view_t info;
    reader_position_t position;
    char title[DOCUMENT_TITLE_MAX];
    epd_reader_page_t page;
    epd_app_font_page_t pins;
    rt_tick_t requested_at, ready_at, preparation_started, resources_at, completed_at;
    unsigned pin_run, pin_byte, render_run, render_byte, render_row;
    unsigned pin_ready_pass, pin_ready_peak;
    rt_tick_t pin_progress_at;
    bool pin_recovery_used, pin_creating;
    int32_t render_x;
    unsigned appearance;
    bool render_struck, render_guided, full_picture;
    bool pins_pending, resources_ready, render_initialized, render_finished, memory_waiting, render_decorated;
    rt_tick_t memory_wait_started;
    uint32_t memory_ticket;
    rt_tick_t memory_requested;
    bool memory_reclaimed;
    bool pressure_requested;
    uint32_t pressure_generation;
    rt_tick_t pressure_started;
    epd_memory_recovery_t admission_recovery;
    uint32_t memory_intent;
    const char *admission_stage;
    int admission_result;
    size_t admission_need;
    bool allocation_attempted;
};
typedef enum { INTENT_NONE, INTENT_SEEK, INTENT_TURN, INTENT_CURSOR,
    INTENT_CANCEL, INTENT_TOC, INTENT_CHAPTER, INTENT_LINK, INTENT_REFLOW,
    INTENT_PROGRESS, INTENT_SOURCE, INTENT_REPAINT, INTENT_RESIZE } view_intent_t;
typedef struct {
    view_intent_t kind;
    int value;
    document_cursor_t cursor;
    uint32_t session, serial;
    view_frame_t *origin;
    document_anchor_t source;
    unsigned appearance;
    reader_geometry_t geometry;
} view_command_t;
static struct {
    epd_activity_work_t *activity;
    rt_thread_t thread;
    rt_sem_t wake, stopped;
    bool stop, paused, suspended, replace;
    uint32_t session, intent, result_intent;
    rt_tick_t requested_at;
    view_state_t *incoming, *configuration, *retired;
    view_frame_t *ready, *ack, *cleanup;
    view_command_t command;
    reader_work_t work;
    reader_error_t error;
    unsigned pages, page, percent;
    document_t *document;
    document_cursor_t cursor;
    uint32_t layout;
    bool indexing, rejected, burst, count_requested;
} mailbox;
static unsigned leading_pixel_pages(void)
{
    rt_enter_critical();
    bool burst = mailbox.burst;
    rt_exit_critical();
    return burst ? VIEW_NEAR_PAGES : 1u;
}
typedef struct {
    epd_compute_buffer_t pixels;
    uint32_t session;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    uint32_t intent;
#endif
} frame_spare_t;
static frame_spare_t frame_spare;
static frame_spare_t *frame_spare_extra;
static struct {
    view_state_t *session;
    uint32_t id, intent;
    rt_tick_t requested_at;
    reader_view_t info;
    reader_save_fn save;
    void *user;
    view_frame_t *ready, *bound;
    lv_obj_t *root, *bitmap;
    view_widget_t *widgets;
    bool paused, pending, configuration_failed, appearance_pending;
    unsigned requested_appearance;
    int turn_delta;
    rt_tick_t last_turn;
    bool last_turn_valid;
    uint32_t font_id, font_revision;
    reader_geometry_t area;
    unsigned appearance;
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    char title[DOCUMENT_TITLE_MAX];
} ui;
/* UI-owned requests contain only immutable configuration and async font shells.
 * Source I/O, metrics and glyph rendering belong to the platform font worker. */
static struct {
    epd_activity_work_t *activity;
    view_state_t *context;
    view_frame_t *origin;
    view_command_t command;
    uint32_t session;
    bool opening;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    uint16_t trace_flags;
    epd_resource_activity_t trace_activity;
    rt_tick_t trace_ready_at;
#endif
} font_request;
static char visible_chapter[256];

#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
enum {
    FONT_TRACE_SCREEN_WAIT = 1u, FONT_TRACE_SCREEN_ALLOW = 2u,
    FONT_TRACE_ACTIVITY_WAIT = 4u, FONT_TRACE_READY_SEEN = 8u,
    FONT_TRACE_READY_LOGGED = 16u, FONT_TRACE_READY_WAKE = 32u,
    FONT_TRACE_BEGIN = 64u, FONT_TRACE_END = 128u,
    FONT_TRACE_SLOTS_WAIT = 256u, FONT_TRACE_SERVICE_WAIT = 512u
};
static void font_trace_emit(const char *event, rt_tick_t tick, int result)
{
    rt_kprintf("reader init_trace session=%u intent=%u request=%u event=%s tick=%u result=%d\n",
        (unsigned)ui.id, (unsigned)ui.intent, (unsigned)ui.requested_at,
        event, (unsigned)tick, result);
}
static void font_trace_once(unsigned flag, const char *event, int result)
{
    rt_enter_critical();
    bool report = font_request.trace_activity && !(font_request.trace_flags & flag);
    if (report) font_request.trace_flags |= flag;
    rt_exit_critical();
    if (report) font_trace_emit(event, rt_tick_get(), result);
}
/* Observe the public activity snapshot without entering/reclassifying credit.
 * Only the token crosses threads; a delayed notice never dereferences a work. */
static void font_trace_probe(bool notified)
{
    rt_enter_critical();
    epd_resource_activity_t activity = font_request.trace_activity;
    bool pending = activity && !(font_request.trace_flags & FONT_TRACE_READY_SEEN);
    rt_exit_critical();
    if (!pending) return;
    epd_resource_activity_snapshot_t snapshot;
    if (epd_resource_activity_get(activity, &snapshot) != EPD_RESOURCE_OK ||
        snapshot.state != EPD_RESOURCE_ADMITTED) return;
    rt_tick_t tick = rt_tick_get();
    rt_enter_critical();
    if (font_request.trace_activity == activity && !(font_request.trace_flags & FONT_TRACE_READY_SEEN)) {
        font_request.trace_ready_at = tick;
        font_request.trace_flags |= FONT_TRACE_READY_SEEN | (notified ? FONT_TRACE_READY_WAKE : 0u);
    }
    rt_exit_critical();
}
static void font_trace_ready(void)
{
    rt_enter_critical();
    unsigned flags = font_request.trace_flags;
    bool report = font_request.trace_activity && (flags & FONT_TRACE_READY_SEEN) &&
        !(flags & FONT_TRACE_READY_LOGGED);
    rt_tick_t tick = font_request.trace_ready_at;
    if (report) font_request.trace_flags |= FONT_TRACE_READY_LOGGED;
    rt_exit_critical();
    if (report) font_trace_emit(flags & FONT_TRACE_READY_WAKE ? "activity_ready_wake" : "activity_ready_sample", tick, 1);
}
static void font_trace_arm(epd_activity_work_t *work, rt_tick_t requested)
{
    epd_resource_activity_t activity = epd_activity_work_activity(work);
    rt_enter_critical();
    font_request.trace_flags = 0; font_request.trace_ready_at = 0;
    font_request.trace_activity = activity;
    rt_exit_critical();
    font_trace_emit("activity_request", requested, activity != 0);
    font_trace_probe(false);
}
static void font_trace_stop(void)
{
    rt_enter_critical(); font_request.trace_activity = 0; rt_exit_critical();
}
static void font_trace_gate(bool commit)
{
    font_trace_ready();
    font_trace_once(commit ? FONT_TRACE_SCREEN_ALLOW : FONT_TRACE_SCREEN_WAIT,
                    commit ? "screen_allow" : "screen_wait", commit);
}
#endif

static struct {
    epd_app_font_remote_profile_t begin;
    epd_app_font_step_profile_t steps;
    epd_app_font_drive_profile_t drive;
    uint32_t session;
    rt_tick_t started;
    bool pending;
} first_font;

static unsigned long long font_profile_delta(uint64_t end, uint64_t begin)
{ return end >= begin ? (unsigned long long)(end - begin) : 0; }

static uint64_t font_profile_exec(const epd_app_font_remote_profile_t *profile, unsigned kind)
{
    uint64_t ticks = kind == 1 ? profile->measure.exec_ticks : profile->render.exec_ticks;
    return ticks + (profile->active.kind == kind ? profile->active.exec_ticks : 0);
}

static void first_font_begin(uint32_t session)
{
    epd_app_font_remote_profile_t snapshot;
    epd_app_font_remote_profile(&snapshot);
    epd_app_font_step_profile_t steps;
    epd_app_font_step_profile(&steps);
    epd_app_font_drive_profile_t drive;
    epd_app_font_drive_profile(&drive);
    rt_enter_critical();
    first_font.steps = steps; first_font.drive = drive;
    first_font.begin = snapshot; first_font.session = session;
    first_font.started = rt_tick_get(); first_font.pending = true;
    rt_exit_critical();
}

static void first_font_cancel(void)
{
    rt_enter_critical(); first_font.pending = false; rt_exit_critical();
}

static void first_font_ready(uint32_t session, uint32_t intent, uint32_t font_id)
{
    epd_app_font_remote_profile_t begin, end;
    epd_app_font_step_profile_t step_begin, step_end;
    epd_app_font_drive_profile_t drive_begin, drive_end;
    rt_tick_t started;
    rt_enter_critical();
    if (!first_font.pending || first_font.session != session) { rt_exit_critical(); return; }
    step_begin = first_font.steps; drive_begin = first_font.drive;
    begin = first_font.begin; started = first_font.started; first_font.pending = false;
    rt_exit_critical();
    epd_app_font_remote_profile(&end);
    epd_app_font_step_profile(&step_end);
    epd_app_font_drive_profile(&drive_end);
    rt_kprintf("reader first_font session=%u intent=%u font=%u window_ms=%u hz=%u measure_batches=%llu measure_glyphs=%llu measure_exec_ticks=%llu render_batches=%llu render_glyphs=%llu render_exec_ticks=%llu active_kind=%u active_glyphs=%u active_completed=%u\n",
        (unsigned)session, (unsigned)intent, (unsigned)font_id, elapsed_ms(started), (unsigned)end.ticks_per_second,
        font_profile_delta(end.measure.count, begin.measure.count), font_profile_delta(end.measure.glyphs, begin.measure.glyphs),
        font_profile_delta(font_profile_exec(&end, 1), font_profile_exec(&begin, 1)),
        font_profile_delta(end.render.count, begin.render.count), font_profile_delta(end.render.glyphs, begin.render.glyphs),
        font_profile_delta(font_profile_exec(&end, 2), font_profile_exec(&begin, 2)),
        (unsigned)end.active.kind, (unsigned)end.active.glyphs, (unsigned)end.active.completed);
    rt_kprintf("reader first_font_io session=%u intent=%u source_calls=%llu requested_bytes=%llu errors=%llu read_calls=%llu read_bytes=%llu read_ticks=%llu lock_wait_ticks=%llu lock_hold_ticks=%llu cache_hits=%llu cache_misses=%llu\n",
        (unsigned)session, (unsigned)intent,
        font_profile_delta(end.io.supply_calls, begin.io.supply_calls), font_profile_delta(end.io.requested_bytes, begin.io.requested_bytes),
        font_profile_delta(end.io.errors, begin.io.errors), font_profile_delta(end.io.read.calls, begin.io.read.calls),
        font_profile_delta(end.io.read.bytes, begin.io.read.bytes), font_profile_delta(end.io.read.ticks, begin.io.read.ticks),
        font_profile_delta(end.io.lock_wait.ticks, begin.io.lock_wait.ticks), font_profile_delta(end.io.lock_hold.ticks, begin.io.lock_hold.ticks),
        font_profile_delta(end.cache_hits, begin.cache_hits), font_profile_delta(end.cache_misses, begin.cache_misses));
    rt_kprintf("reader first_font_file session=%u intent=%u open_calls=%llu open_ticks=%llu stat_calls=%llu stat_ticks=%llu seek_calls=%llu seek_ticks=%llu close_calls=%llu close_ticks=%llu\n",
        (unsigned)session, (unsigned)intent,
        font_profile_delta(end.open.calls, begin.open.calls), font_profile_delta(end.open.ticks, begin.open.ticks),
        font_profile_delta(end.fstat.calls, begin.fstat.calls), font_profile_delta(end.fstat.ticks, begin.fstat.ticks),
        font_profile_delta(end.seek.calls, begin.seek.calls), font_profile_delta(end.seek.ticks, begin.seek.ticks),
        font_profile_delta(end.close.calls, begin.close.calls), font_profile_delta(end.close.ticks, begin.close.ticks));
    rt_kprintf("reader first_font_step session=%u intent=%u acpu_hz=%u batches=%llu run=%llu readwait=%llu idlewait=%llu outputwait=%llu reads=%llu idles=%llu outputs=%llu\n",
        (unsigned)session, (unsigned)intent, (unsigned)step_end.ticks_per_second,
        font_profile_delta(step_end.batches, step_begin.batches),
        font_profile_delta(step_end.run_ticks, step_begin.run_ticks),
        font_profile_delta(step_end.read_wait_ticks, step_begin.read_wait_ticks),
        font_profile_delta(step_end.idle_wait_ticks, step_begin.idle_wait_ticks),
        font_profile_delta(step_end.output_wait_ticks, step_begin.output_wait_ticks),
        font_profile_delta(step_end.reads, step_begin.reads),
        font_profile_delta(step_end.idles, step_begin.idles),
        font_profile_delta(step_end.outputs, step_begin.outputs));
    static const char *const names[] = {"read", "output", "idle", "command", "other"};
    for (unsigned i = 0; i < EPD_FONT_DRIVE_COUNT; ++i)
        rt_kprintf("reader first_font_drive session=%u intent=%u op=%s calls=%llu pipe_ticks=%llu hz=%u\n",
            (unsigned)session, (unsigned)intent, names[i],
            font_profile_delta(drive_end.op[i].calls, drive_begin.op[i].calls),
            font_profile_delta(drive_end.op[i].pipe_ticks, drive_begin.op[i].pipe_ticks),
            (unsigned)drive_end.ticks_per_second);
    rt_kprintf("reader first_font_service session=%u intent=%u step_ticks=%llu supply_calls=%llu supply_ticks=%llu consume_calls=%llu consume_ticks=%llu hz=%u\n",
        (unsigned)session, (unsigned)intent,
        font_profile_delta(drive_end.step_ticks, drive_begin.step_ticks),
        font_profile_delta(drive_end.supply_calls, drive_begin.supply_calls),
        font_profile_delta(drive_end.supply_ticks, drive_begin.supply_ticks),
        font_profile_delta(drive_end.consume_calls, drive_begin.consume_calls),
        font_profile_delta(drive_end.consume_ticks, drive_begin.consume_ticks),
        (unsigned)drive_end.ticks_per_second);
}

/* Immediate-target adoption requires exact adjacency. The command pins
 * its immutable origin frame until the worker consumes it. */
static bool frame_turn_matches(const view_frame_t *frame, const view_command_t *command)
{
    const view_frame_t *origin = command->origin;
    if (!frame || !frame->owner || frame->cancelled ||
        (!frame->pins && !frame->renderer && !frame->render_finished) ||
        command->kind != INTENT_TURN || !origin || command->session != frame->owner->session ||
        origin->owner != frame->owner || origin->layout != frame->layout ||
        frame->layout != frame->owner->layout) return false;
    if (command->value == 1)
        return !origin->page.eof && cursor_equal(
            (document_cursor_t){origin->page.cursor.record, origin->page.cursor.byte},
            (document_cursor_t){frame->page.start.record, frame->page.start.byte});
    if (command->value == -1)
        return !frame->page.eof && cursor_equal(
            (document_cursor_t){frame->page.cursor.record, frame->page.cursor.byte},
            (document_cursor_t){origin->page.start.record, origin->page.start.byte});
    return false;
}
/* The UI may only inspect pinned immutable frame data. This is provisional:
 * the worker proves the cached cursor chain before keeping optional work. */
static bool frame_turn_candidate(const view_frame_t *frame, const view_command_t *command)
{
    if (frame_turn_matches(frame, command)) return true;
    const view_frame_t *origin = command->origin;
    if (!frame || !origin || !frame->owner || frame->cancelled ||
        (!frame->pins && !frame->renderer && !frame->render_finished) ||
        command->kind != INTENT_TURN || (command->value != 1 && command->value != -1) ||
        command->session != frame->owner->session || origin->owner != frame->owner ||
        origin->layout != frame->layout || frame->layout != frame->owner->layout ||
        (command->value > 0 ? origin->page.eof || origin->number == UINT_MAX : !origin->number)) return false;
    unsigned target = command->value > 0 ? origin->number + 1 : origin->number - 1;
    unsigned distance = frame->number > target ? frame->number - target : target - frame->number;
    return distance && distance <= 3;
}
static void render_cancel(void)
{
    rt_enter_critical();
    if (render_frame) render_frame->cancelled = true;
    if (render_wait) epd_compute_wait_cancel(render_wait);
    if (render_active) epd_reader_render_cancel(render_active);
    rt_exit_critical();
}
static void render_cancel_optional(void)
{
    rt_enter_critical();
    if (!render_required) render_cancel();
    rt_exit_critical();
}
static void turn_cancel(void)
{
    index_cancel();
    rt_enter_critical();
    if (!frame_turn_candidate(render_frame, &mailbox.command)) render_cancel();
    rt_exit_critical();
}
static void frame_trace_note(const view_frame_t *frame, const char *event, uint32_t intent)
{
    rt_kprintf("reader frame_%s session=%u intent=%u from=%u page=%u chapter=%u start=%u:%u age_ms=%u pins=%u:%u resources=%u renderer=%u cancelled=%u\n",
        event, (unsigned)frame->owner->session, (unsigned)intent, (unsigned)frame->intent,
        frame->number, (unsigned)frame->owner->progress.chapter, frame->page.start.record, frame->page.start.byte,
        elapsed_ms(frame->preparation_started), frame->pin_run, frame->pin_byte,
        (unsigned)frame->resources_ready, frame->renderer != NULL, (unsigned)frame->cancelled);
}
static void frame_drop_note(view_frame_t *frame)
{
    if (frame->complete || frame->drop_reported) return;
    frame->drop_reported = true;
    frame_trace_note(frame, "drop", frame->owner->intent);
}
static void frame_handoff_note(view_frame_t *frame, uint32_t intent)
{
    if (frame->handoff_intent == intent) return;
    frame->handoff_intent = intent;
    rt_kprintf("reader frame_adopt session=%u intent=%u pending=1\n",
        (unsigned)frame->owner->session, (unsigned)intent);
    frame_trace_note(frame, "handoff", intent);
}
static bool view_interrupted(void)
{
    rt_enter_critical();
    bool interrupted = !state || mailbox.stop || mailbox.suspended ||
        mailbox.session != view.session || mailbox.intent != view.intent;
    rt_exit_critical();
    return interrupted;
}
static bool render_sessions_pending(void)
{
    rt_enter_critical(); bool pending = render_frame || mailbox.cleanup; rt_exit_critical();
    return pending;
}
static void wake_view(void)
{
    rt_enter_critical();
    if (mailbox.wake) rt_sem_release(mailbox.wake);
    rt_exit_critical();
}
static void view_activity_wake(void *unused)
{
    (void)unused;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    font_trace_probe(true);
#endif
    wake_view(); epd_reader_ui_wake();
}
static epd_activity_work_t *view_activity_new(void)
{
    epd_activity_work_t *work = epd_activity_work_queue(EPD_RESOURCE_INTERACTIVE, 0, 65536u, 4096u, view_activity_wake, NULL);
    if (!work) work = epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS, 0, 65536u, 4096u, view_activity_wake, NULL);
    return work;
}

void epd_reader_view_advance(void) { wake_view(); }
static void frame_ref(view_frame_t *frame)
{
    if (!frame) return;
    rt_enter_critical(); ++frame->references; rt_exit_critical();
}
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
static void frame_pixels_free(epd_compute_buffer_t *pixels, const char *reason,
                              uint32_t session, uint32_t intent)
{
    uintptr_t base = (uintptr_t)pixels->allocation;
    uintptr_t data = (uintptr_t)pixels->data;
    unsigned capacity = pixels->capacity;
    epd_compute_buffer_free(pixels);
    if (base) rt_kprintf("reader pixel_buffer event=free reason=%s session=%u intent=%u base=0x%08x data=0x%08x capacity=%u\n",
        reason, (unsigned)session, (unsigned)intent, (unsigned)base, (unsigned)data, capacity);
}
#else
#define frame_pixels_free(pixels, reason, session, intent) epd_compute_buffer_free(pixels)
#endif

static void frame_spare_install(uint32_t session)
{
    frame_spare_t *extra = view_allocate(sizeof(*extra), EPD_APP_PSRAM);
    if (!extra) return;
    memset(extra, 0, sizeof(*extra));
    rt_enter_critical();
    if (!frame_spare_extra && mailbox.thread && !mailbox.stop && !mailbox.suspended &&
        !mailbox.replace && mailbox.session == session) {
        frame_spare_extra = extra;
        extra = NULL;
    }
    rt_exit_critical();
    epd_app_free(extra);
}

/* Called with the reader critical section held. */
static frame_spare_t frame_spare_detach(frame_spare_t *slot)
{
    frame_spare_t spare = *slot;
    memset(slot, 0, sizeof(*slot));
    return spare;
}

static bool frame_spare_release(frame_spare_t *spare, const char *reason)
{
    (void)reason;
    size_t capacity = spare->pixels.capacity;
    if (!capacity) return false;
    frame_pixels_free(&spare->pixels, reason, spare->session, spare->intent);
    rt_enter_critical(); frame_bytes -= capacity; rt_exit_critical();
    return true;
}

static void frame_spare_clear(const char *reason, bool discard_extra)
{
    frame_spare_t second = {0};
    rt_enter_critical();
    frame_spare_t first = frame_spare_detach(&frame_spare);
    frame_spare_t *extra = frame_spare_extra;
    if (extra) second = frame_spare_detach(extra);
    if (discard_extra) frame_spare_extra = NULL;
    rt_exit_critical();
    frame_spare_release(&first, reason);
    frame_spare_release(&second, reason);
    if (discard_extra) epd_app_free(extra);
}

static bool frame_spare_matches(const frame_spare_t *slot, const view_frame_t *frame)
{
    size_t bytes = (frame->image.data_size + 31u) & ~(size_t)31u;
    return slot && slot->pixels.data && slot->session == frame->owner->session &&
        slot->pixels.capacity >= bytes;
}

/* A larger compatible buffer stays available for the next request. */
static frame_spare_t *frame_spare_find(const view_frame_t *frame)
{
    if (frame->owner->session != mailbox.session || mailbox.stop ||
        mailbox.replace || mailbox.suspended) return NULL;
    frame_spare_t *slot = frame_spare_matches(&frame_spare, frame) ? &frame_spare : NULL;
    if (frame_spare_matches(frame_spare_extra, frame) &&
        (!slot || frame_spare_extra->pixels.capacity < slot->pixels.capacity))
        slot = frame_spare_extra;
    return slot;
}

static bool frame_spare_available(const view_frame_t *frame)
{
    rt_enter_critical(); bool matches = frame_spare_find(frame) != NULL; rt_exit_critical();
    return matches;
}

static void frame_spare_trim(const view_frame_t *frame)
{
    frame_spare_t spare = {0};
    rt_enter_critical();
    if (frame->owner->session == mailbox.session && !frame_spare_find(frame)) {
        if (!frame_spare_extra) {
            spare = frame_spare_detach(&frame_spare);
        } else if (frame->required) {
            /* Free one obsolete size before trying a foreground allocation. */
            frame_spare_t *slot = frame_spare.pixels.capacity >= frame_spare_extra->pixels.capacity ?
                &frame_spare : frame_spare_extra;
            spare = frame_spare_detach(slot);
        }
    }
    rt_exit_critical();
    frame_spare_release(&spare, "spare_trim");
}

static size_t frame_spare_reclaimable(void)
{
    rt_enter_critical();
    size_t bytes = frame_spare.pixels.capacity;
    if (frame_spare_extra) bytes += frame_spare_extra->pixels.capacity;
    rt_exit_critical();
    return bytes;
}

static bool frame_spare_reclaim(size_t deficit, size_t *physical_extra)
{
    if (!deficit) return false;
    frame_spare_t spare = {0};
    rt_enter_critical();
    frame_spare_t *slot = frame_spare.pixels.data ? &frame_spare : NULL;
    if (frame_spare_extra && frame_spare_extra->pixels.data) {
        size_t second = frame_spare_extra->pixels.capacity;
        if (!slot || (slot->pixels.capacity >= deficit ?
                second >= deficit && second < slot->pixels.capacity :
                second > slot->pixels.capacity))
            slot = frame_spare_extra;
    }
    if (slot) spare = frame_spare_detach(slot);
    rt_exit_critical();
    if (!spare.pixels.allocation) return false;
    epd_memory_block_snapshot_t block;
    if (epd_memory_block_get(spare.pixels.allocation, &block) == EPD_MEMORY_OK &&
        block.physical > spare.pixels.capacity)
        *physical_extra += block.physical - spare.pixels.capacity;
    return frame_spare_release(&spare, "pressure_clear");
}

static bool frame_spare_keep(view_frame_t *frame)
{
    /* Keep raw buffers intact after rendering and external references drain. */
    if (frame->discard_pixels || frame->renderer || frame->backing || !frame->pixels.data ||
        ((uintptr_t)frame->pixels.data & 31u)) return false;
    frame_spare_t replaced = {0};
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    epd_compute_buffer_t retained = {0};
#endif
    rt_enter_critical();
    frame_spare_t *slot = &frame_spare;
    if (slot->pixels.data && frame_spare_extra &&
        frame_spare_extra->pixels.capacity < slot->pixels.capacity)
        slot = frame_spare_extra;
    bool keep = mailbox.thread && !mailbox.stop && !mailbox.suspended && !mailbox.replace &&
        frame->owner->session == mailbox.session &&
        (!slot->pixels.data || frame->pixels.capacity > slot->pixels.capacity);
    if (keep) {
        replaced = *slot;
        slot->pixels = frame->pixels;
        slot->session = frame->owner->session;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
        slot->intent = frame->intent;
        retained = frame->pixels;
#endif
        memset(&frame->pixels, 0, sizeof(frame->pixels));
    }
    rt_exit_critical();
    frame_spare_release(&replaced, "spare_replace");
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    if (keep) rt_kprintf("reader pixel_buffer event=retain session=%u intent=%u page=%u bytes=%u complete=%u base=%p data=%p capacity=%u\n",
        (unsigned)frame->owner->session, (unsigned)frame->intent, frame->number,
        (unsigned)frame->image.data_size, (unsigned)frame->complete,
        retained.allocation, retained.data, (unsigned)retained.capacity);
#else
    if (keep) rt_kprintf("reader pixel_buffer event=retain session=%u page=%u bytes=%u complete=%u\n",
        (unsigned)frame->owner->session, frame->number, (unsigned)frame->image.data_size,
        (unsigned)frame->complete);
#endif
    return keep;
}
static bool frame_spare_take(view_frame_t *frame)
{
    rt_enter_critical();
    frame_spare_t *slot = frame_spare_find(frame);
    bool take = slot != NULL;
    if (take) frame->pixels = frame_spare_detach(slot).pixels;
    rt_exit_critical();
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    if (take) rt_kprintf("reader pixel_buffer event=reuse reason=spare session=%u intent=%u page=%u bytes=%u base=%p data=%p capacity=%u\n",
        (unsigned)frame->owner->session, (unsigned)frame->intent, frame->number,
        (unsigned)frame->image.data_size, frame->pixels.allocation, frame->pixels.data,
        (unsigned)frame->pixels.capacity);
#else
    if (take) rt_kprintf("reader pixel_buffer event=reuse session=%u page=%u bytes=%u\n",
        (unsigned)frame->owner->session, frame->number, (unsigned)frame->image.data_size);
#endif
    return take;
}
static void *frame_cold_allocate(size_t bytes, void *unused)
{
    (void)unused;
    size_t charge = epd_app_allocation_peak(bytes);
    rt_enter_critical(); bool fits = charge <= VIEW_FRAME_BUDGET && frame_bytes <= VIEW_FRAME_BUDGET - charge; rt_exit_critical();
    if (!fits) return NULL;
    void *memory = view_allocate(bytes, EPD_APP_PSRAM);
    if (memory) { rt_enter_critical(); frame_bytes += charge; rt_exit_critical(); }
    return memory;
}
static void frame_cold_free(void *memory, size_t bytes, void *unused)
{
    (void)unused;
    epd_app_free(memory);
    rt_enter_critical(); frame_bytes -= epd_app_allocation_peak(bytes); rt_exit_critical();
}
static const reader_frame_allocator_t frame_cold_allocator = {
    frame_cold_allocate, frame_cold_free, epd_app_allocation_peak, NULL
};
static void frame_cold_clear(view_frame_t *frame)
{
    reader_frame_store_clear(&frame->cold, &frame_cold_allocator);
    frame->thaw_next = NULL; frame->thaw_offset = 0;
}
static void frame_release(view_frame_t *frame)
{
    if (!frame) return;
    rt_enter_critical(); bool last = --frame->references == 0; rt_exit_critical();
    if (!last) return;
    frame_drop_note(frame);
    if (frame->renderer) {
        if (render_frame == frame) render_frame = NULL;
        rt_enter_critical(); frame->next = mailbox.cleanup; mailbox.cleanup = frame; rt_exit_critical();
        return;
    }
    image_release(frame->render_image);
    epd_app_font_page_release(frame->pins);
    if (frame->backing) frame_release(frame->backing);
    else {
        frame_cold_clear(frame);
        if (!frame_spare_keep(frame)) {
            rt_enter_critical(); frame_bytes -= frame->pixels.capacity; rt_exit_critical();
            frame_pixels_free(&frame->pixels, "last_reference", frame->owner->session, frame->intent);
        }
    }
    rt_enter_critical(); --frame->owner->references; rt_exit_critical();
    window_uncharge_bytes(frame->window_charge);
    epd_app_free(frame);
}
static void frame_cache_clear(void)
{
    frame_release(view.rendering); view.rendering = NULL;
    while (view.frames) {
        view_frame_t *next = view.frames->next;
        frame_release(view.frames); view.frames = next;
    }
}
static bool render_cancelled(void *user)
{
    view_frame_t *frame = user;
    rt_enter_critical();
    bool cancelled = frame->cancelled || mailbox.stop || mailbox.suspended || (mailbox.paused && !frame->required) ||
        mailbox.session != frame->owner->session ||
        (mailbox.intent != frame->intent && !frame_turn_candidate(frame, &mailbox.command));
    if (cancelled) frame->cancelled = true;
    uint32_t handoff = !cancelled && mailbox.intent != frame->intent ? mailbox.intent : 0;
    rt_exit_critical();
    if (handoff) frame_handoff_note(frame, handoff);
    return cancelled;
}
static bool frame_admission_error(view_frame_t *frame, const char *stage, int result, size_t bytes)
{
    frame->admission_stage = stage; frame->admission_result = result; frame->admission_need = bytes;
    return false;
}
static int frame_memory_failure(const view_frame_t *frame, const char *stage)
{
    if (frame->required)
        rt_kprintf("reader frame_memory stage=%s page=%u need=%u free=%u frames=%u layout=%u images=%u index=%u\n",
            stage, frame->number, (unsigned)frame->image.data_size, (unsigned)epd_app_psram_available(),
            (unsigned)frame_bytes, (unsigned)layout_bytes, (unsigned)image_bytes, (unsigned)index_bytes);
    return -3;
}
static int render_admit(view_frame_t *frame, bool required)
{
    epd_compute_wait_t wait = NULL;
    int result = epd_compute_wait_create(&wait);
    if (result != EPD_COMPUTE_OK) {
        frame_admission_error(frame, "render_wait", result, 0);
        return frame_memory_failure(frame, "render_wait");
    }
    rt_enter_critical();
    render_required = required; render_wait = wait;
    if (render_cancelled(frame)) epd_compute_wait_cancel(wait);
    rt_exit_critical();
    rr_frame format = {frame->image.header.w, frame->image.header.h, frame->image.header.stride,
                       RR_FORMAT_L8};
    result = epd_reader_render_open_wait(&frame->pixels, format,
        required ? EPD_COMPUTE_FOREGROUND : EPD_COMPUTE_BACKGROUND, wait, &frame->renderer);
    rt_enter_critical(); render_wait = NULL; rt_exit_critical();
    int destroyed = epd_compute_wait_destroy(wait);
    RT_ASSERT(destroyed == EPD_COMPUTE_OK);
    (void)destroyed;
    if (frame->renderer) render_frame = frame;
    if (render_cancelled(frame)) return -2;
    if (result != EPD_COMPUTE_OK)
        frame_admission_error(frame, "render_open", result, VIEW_RENDER_WORKSPACE);
    if (result == EPD_COMPUTE_NO_MEMORY) {
        frame_admission_error(frame, "render_open", result, VIEW_RENDER_WORKSPACE);
        return frame_memory_failure(frame, "render_open");
    }
    return result == EPD_COMPUTE_OK ? 1 : -1;
}
static int render_submit(view_frame_t *frame, const rr_command *command, const void *pixels, size_t bytes)
{
    rt_enter_critical();
    render_required = frame->required; render_active = frame->renderer;
    if (render_cancelled(frame)) epd_reader_render_cancel(render_active);
    rt_exit_critical();
    int result;
    if (command->operation == RR_BLIT_A8) {
        result = epd_reader_render_a8_enqueue(frame->renderer, command, pixels, bytes);
        if (result == EPD_COMPUTE_MORE) {
            result = epd_reader_render_a8_flush(frame->renderer, render_cancelled, frame);
            if (result == EPD_COMPUTE_OK)
                result = epd_reader_render_a8_enqueue(frame->renderer, command, pixels, bytes);
        }
        if (result == EPD_COMPUTE_MORE)
            result = epd_reader_render_command(frame->renderer, command, pixels, bytes, render_cancelled, frame, NULL);
    } else {
        result = epd_reader_render_a8_flush(frame->renderer, render_cancelled, frame);
        if (result == EPD_COMPUTE_OK) {
            if (frame->render_image)
                result = epd_reader_render_command_shared(frame->renderer, command,
                    &frame->render_image->picture.pixels, bytes, render_cancelled, frame, NULL);
            else
                result = epd_reader_render_command(frame->renderer, command, pixels, bytes, render_cancelled, frame, NULL);
        }
    }
    rt_enter_critical(); render_active = NULL; rt_exit_critical();
    return result;
}
static int render_flush(view_frame_t *frame)
{
    rt_enter_critical();
    render_required = frame->required; render_active = frame->renderer;
    if (render_cancelled(frame)) epd_reader_render_cancel(render_active);
    rt_exit_critical();
    int result = epd_reader_render_a8_flush(frame->renderer, render_cancelled, frame);
    rt_enter_critical(); render_active = NULL; rt_exit_critical();
    return result;
}
static rr_command render_command(uint32_t operation, const view_frame_t *frame)
{
    rr_command command = {0};
    command.width = frame->image.header.w; command.height = frame->image.header.h;
    command.operation = operation;
    if (frame->appearance & UI_READER_NEGATIVE) {
        if (operation == RR_INIT) command.flags = RR_INIT_BLACK;
        if (operation == RR_BLIT_A8) command.flags = RR_A8_WHITE_INK;
        if (operation == RR_FILLRECT) command.fill_rgb565 = UINT16_MAX;
    }
    command.clip_w = frame->image.header.w; command.clip_h = frame->image.header.h;
    return command;
}
static view_frame_t *frame_find(document_cursor_t start)
{
    for (view_frame_t *frame = view.frames; frame; frame = frame->next)
        if (frame->layout == view.layout && frame->appearance == view.appearance && cursor_equal(start,
            (document_cursor_t){frame->page.start.record, frame->page.start.byte})) return frame;
    return NULL;
}
static bool frame_pixels_ready(const view_frame_t *frame)
{
    return frame && frame->complete && !frame->cancelled && frame->pixels.data &&
        frame->image.data == frame->pixels.data &&
        (!frame->cold.first || frame->cold.raw != frame->image.data_size);
}
/* Only the worker walks mutable layout caches. Page numbers bound the walk;
 * every edge and the final frame must also match both cursor components. */
static bool frame_window_contains(const view_frame_t *frame, const epd_reader_page_t *target, unsigned number)
{
    if (!frame || frame->owner != state || frame->layout != view.layout || frame->cancelled) return false;
    unsigned distance = frame->number > number ? frame->number - number : number - frame->number;
    if (!distance || distance > 3) return false;
    bool forward = frame->number > number, eof = target->eof;
    document_cursor_t edge = forward ? (document_cursor_t){target->cursor.record, target->cursor.byte} :
                                      (document_cursor_t){target->start.record, target->start.byte};
    reader_cached_page_t *page = NULL;
    for (unsigned i = 0; i < distance; ++i) {
        number = forward ? number + 1 : number - 1;
        page = reader_page_cache_find(&view.cache, number, view.layout);
        if (!page || (forward ? eof : page->eof) || !cursor_equal(edge,
            forward ? (document_cursor_t){page->start.record, page->start.byte} :
                      (document_cursor_t){page->end.record, page->end.byte})) return false;
        edge = forward ? (document_cursor_t){page->end.record, page->end.byte} :
                         (document_cursor_t){page->start.record, page->start.byte};
        eof = page->eof;
    }
    return cursor_equal((document_cursor_t){page->start.record, page->start.byte},
                        (document_cursor_t){frame->page.start.record, frame->page.start.byte}) &&
        cursor_equal((document_cursor_t){page->end.record, page->end.byte},
                     (document_cursor_t){frame->page.cursor.record, frame->page.cursor.byte}) &&
        page->eof == frame->page.eof;
}
static bool layout_ahead_expired(void)
{
    unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
    return view.ahead_attempted && view.ahead_page == wanted && view.ahead_layout == view.layout &&
        view.ahead_intent == view.intent && elapsed_ms(view.ahead_started) >= VIEW_AHEAD_WAIT_MS;
}
static bool layout_ahead_ready(void)
{
    if (view.failure || !view.selected || !view.status.ready || view.background_paused || view.direction < 0 ||
        view.preview_back_scanning || core_waiting() || view_interrupted()) return false;
    if (window_neighbor_work(state)) return false;
    unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
    if (wanted > UINT_MAX - VIEW_LAYOUT_AHEAD) return false;
    for (unsigned d = 1; d <= 3; ++d) {
        reader_cached_page_t *page = reader_page_cache_find(&view.cache, wanted + d, view.layout);
        if (!page || page->eof ||
            !frame_find((document_cursor_t){page->start.record, page->start.byte})) return false;
    }
    if (!view.ahead_attempted || view.ahead_page != wanted || view.ahead_layout != view.layout ||
        view.ahead_intent != view.intent) {
        view.ahead_attempted = true; view.ahead_page = wanted;
        view.ahead_layout = view.layout; view.ahead_intent = view.intent;
        view.ahead_started = rt_tick_get();
    }
    return !layout_ahead_expired();
}
static void frame_trim(void)
{
    unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
    view_frame_t **at = &view.frames;
    while (*at) {
        view_frame_t *frame = *at;
        unsigned distance = frame->number > wanted ? frame->number - wanted : wanted - frame->number;
        bool keep = frame->layout == view.layout && distance <= 3;
        if (keep) at = &frame->next;
        else { *at = frame->next; frame_release(frame); }
    }
}
static view_frame_t *frame_create(const epd_reader_page_t *page, unsigned number, uint32_t generation)
{
    size_t charge = view.window_state_charge ? sizeof(view_frame_t) : 0;
    if (charge && !window_charge(charge)) return NULL;
    view_frame_t *frame = view_allocate(sizeof(*frame), EPD_APP_PSRAM);
    if (!frame) { window_uncharge_bytes(charge); return NULL; }
    memset(frame, 0, sizeof(*frame)); frame->window_charge = charge;
    frame->owner = state; frame->references = 1; frame->number = number;
    frame->intent = view.intent; frame->layout = view.layout; frame->generation = generation;
    frame->preparation_started = rt_tick_get();
    frame->appearance = view.appearance;
    rt_enter_critical(); ++view.references; rt_exit_critical();
    if (!epd_reader_page_copy(&frame->page, page)) { frame_release(frame); return NULL; }
    frame->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    frame->image.header.cf = LV_COLOR_FORMAT_L8;
    frame->full_picture = page->count == 1 && page->runs[0].image &&
        (page->runs[0].image_bytes & EPD_READER_IMAGE_FULL);
    frame->image.header.w = frame->full_picture ? view.font_area.width : width_available();
    frame->image.header.h = frame->full_picture ? view.font_area.height : VIEW_HEIGHT;
    frame->image.header.stride = (frame->image.header.w + 3u) & ~3u;
    frame->image.data_size = frame->image.header.stride * frame->image.header.h;
    return frame;
}
static unsigned frame_priority(const view_state_t *owner, unsigned number)
{
    unsigned global = window_priority(owner, number);
    if (global != UINT_MAX) return global;
    unsigned wanted = owner->preview_cache_active ? owner->preview_page : owner->wanted;
    unsigned distance = number > wanted ? number - wanted : wanted - number;
    if (distance > 1000) distance = 1000;
    bool behind = owner->direction < 0 ? number > wanted : number < wanted;
    /* Prepare the next three turns before opposite-direction pixels. */
    unsigned priority = distance <= 3 ? distance + (behind ? 3u : 0u) :
        distance * 2u + (behind ? 1u : 0u);
    return priority + (owner == primary_state ? 0 : VIEW_NEIGHBOR_PRIORITY);
}
static unsigned frame_distance(const view_frame_t *frame)
{ return frame_priority(frame->owner, frame->number); }
/* Decode scheduling rank without its direction preference. Off-window
 * owners retain their separate tier; unknown routes remain least valuable. */
static unsigned frame_retention_distance(unsigned priority)
{
    if (priority == UINT_MAX) return UINT_MAX;
    unsigned tier = priority >= VIEW_NEIGHBOR_PRIORITY ? VIEW_NEIGHBOR_PRIORITY : 0;
    unsigned local = priority - tier;
    return tier + (local <= 6u ? (local > 3u ? local - 3u : local) : local / 2u);
}
static bool frame_bitmap_less_useful(unsigned candidate, unsigned retained)
{
    unsigned candidate_distance = frame_retention_distance(candidate);
    unsigned retained_distance = frame_retention_distance(retained);
    return candidate_distance > retained_distance ||
        (candidate_distance == retained_distance && candidate > retained);
}
#ifndef VIEW_RECLAIM_TRACE
#define VIEW_RECLAIM_TRACE 1
#endif
static void frame_reclaim_note(const view_frame_t *victim, const view_state_t *actor,
                               unsigned number, unsigned priority, const char *reason)
{
#if VIEW_RECLAIM_TRACE
    static uint32_t session, intent;
    static bool reported;
    if (!primary_state || (reported && session == primary_state->session && intent == primary_state->intent)) return;
    session = primary_state->session; intent = primary_state->intent; reported = true;
    rt_kprintf("reader frame_reclaim session=%u intent=%u cause=%s victim=%p:%u start=%u:%u victim_d=%u actor=%p:%u request_d=%u rank=%u refs=%u free=%u frames=%u\n",
        (unsigned)session, (unsigned)intent, reason, (void *)victim->owner, victim->number,
        victim->page.start.record, victim->page.start.byte,
        frame_retention_distance(frame_distance(victim)), (const void *)actor, number,
        frame_retention_distance(priority), priority, victim->references,
        (unsigned)epd_app_psram_available(), (unsigned)frame_bytes);
#else
    (void)victim; (void)actor; (void)number; (void)priority; (void)reason;
#endif
}
static bool frame_reclaim_layout(size_t bytes, unsigned request_priority)
{
    while (!bytes || !view_memory_room(bytes)) {
        unsigned score = frame_retention_distance(request_priority);
        if (bytes && score < 3) score = 3;
        view_frame_t **victim = NULL;
        view_state_t *contexts[7]; unsigned context_count = window_contexts(primary_state, contexts);
        for (unsigned i = 0; i < context_count; ++i)
            for (view_frame_t **at = &contexts[i]->frames; *at; at = &(*at)->next) {
                rt_enter_critical(); unsigned references = (*at)->references; rt_exit_critical();
                unsigned priority = frame_retention_distance(frame_distance(*at));
                if (references == 1 && priority > score) { victim = at; score = priority; }
            }
        if (!victim) { if (window_reclaim(request_priority)) continue; return false; }
        view_frame_t *old = *victim; *victim = old->next;
        frame_reclaim_note(old, state, window_number(state), request_priority, "layout");
        frame_release(old);
        if (!bytes) return true;
    }
    return true;
}
static bool frame_reclaim_pages(size_t bytes)
{
    view_state_t *contexts[7]; unsigned context_count = window_contexts(primary_state, contexts);
    for (unsigned i = 0; i < context_count && (!bytes || !view_memory_room(bytes)); ++i) {
        view_state_t *owner = contexts[i];
        if (!owner) continue;
        unsigned wanted = owner->preview_cache_active ? owner->preview_page : owner->wanted;
        reader_page_cache_t *cache = &owner->cache;
        reader_cached_page_t **at = &cache->first;
        while (*at && (!bytes || !view_memory_room(bytes))) {
            reader_cached_page_t *page = *at;
            unsigned distance = page->number > wanted ? page->number - wanted : wanted - page->number;
            if (page->layout == owner->layout && distance <= 3) { at = &page->next; continue; }
            reader_page_cache_remove(cache, page); cache->window_dirty = true;
            if (!bytes) return true;
        }
    }
    return bytes && view_memory_room(bytes);
}
static size_t frame_storage_minimum(const view_frame_t *frame)
{
    return ((frame->image.data_size + 31u) & ~(size_t)31u) + 31u + VIEW_RENDER_WORKSPACE;
}
static epd_activity_work_t *allocation_activity;
static bool frame_nearby_growth(const view_frame_t *frame)
{
    if (!allocation_activity || frame_retention_distance(frame_distance(frame)) > 3) return false;
    size_t bytes = (frame->image.data_size + 31u) & ~(size_t)31u;
    rt_enter_critical(); size_t resident = frame_bytes; rt_exit_critical();
    if (bytes > VIEW_FRAME_BUDGET || resident > VIEW_FRAME_BUDGET - bytes) return false;
    epd_resource_activity_snapshot_t activity;
    return epd_resource_activity_get(epd_activity_work_activity(allocation_activity), &activity) == EPD_RESOURCE_OK &&
        activity.requested.kind == EPD_RESOURCE_NEARBY;
}
static void frame_admission_report(const view_frame_t *frame, int result)
{
    static struct {
        uint32_t session, intent;
        unsigned page;
        const char *stage;
        int result;
        size_t need;
        bool attempted;
        bool ticket;
        unsigned recovery_state;
        int recovery, reason;
    } last;
    const char *stage = frame->admission_stage ? frame->admission_stage : "frame_render";
    int raw = frame->admission_stage ? frame->admission_result : result;
    size_t need = frame->admission_stage ? frame->admission_need : frame->image.data_size;
    if (last.stage && last.session == frame->owner->session && last.intent == frame->intent &&
        last.page == frame->number && !strcmp(last.stage, stage) && last.result == raw &&
        last.need == need && last.attempted == frame->allocation_attempted &&
        last.ticket == frame->admission_recovery.used && last.recovery_state == (unsigned)frame->admission_recovery.state &&
        last.recovery == (int)frame->admission_recovery.result && last.reason == (int)frame->admission_recovery.reason) return;
    last.session = frame->owner->session; last.intent = frame->intent; last.page = frame->number;
    last.stage = stage; last.result = raw; last.need = need; last.attempted = frame->allocation_attempted;
    last.ticket = frame->admission_recovery.used; last.recovery_state = (unsigned)frame->admission_recovery.state;
    last.recovery = (int)frame->admission_recovery.result; last.reason = (int)frame->admission_recovery.reason;
    rt_kprintf("reader frame_admission session=%u intent=%u page=%u activity=%llu stage=%s raw=%d need=%u alloc=%u ticket=%u state=%u recovery=%d reason=%d gen=%u\n",
        last.session, last.intent, last.page, (unsigned long long)epd_activity_work_activity(allocation_activity),
        stage, raw, (unsigned)need, (unsigned)last.attempted, (unsigned)last.ticket, last.recovery_state,
        last.recovery, last.reason, frame->admission_recovery.generation);
}
static bool frame_workspace_ready(view_frame_t *frame)
{
    int result = epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM, VIEW_RENDER_WORKSPACE);
    return result == EPD_RESOURCE_OK || frame_admission_error(frame, "workspace", result, VIEW_RENDER_WORKSPACE);
}
static bool frame_cold_cancelled(const view_frame_t *frame)
{
    rt_enter_critical();
    bool cancelled = mailbox.stop || mailbox.suspended || mailbox.replace ||
        (mailbox.paused && !frame->required) || mailbox.session != frame->owner->session ||
        mailbox.intent != frame->intent || frame->layout != frame->owner->layout;
    rt_exit_critical();
    return cancelled;
}
/* Drop only unfinished conversions on input replacement. Completed cold
 * content survives direction changes and remains in the same logical window. */
static void frame_cold_maintain(void)
{
    view_state_t *contexts[7]; unsigned count = window_contexts(primary_state, contexts);
    for (unsigned i = 0; i < count; ++i) for (view_frame_t *f = contexts[i]->frames; f; f = f->next) {
        if (!f->cold.first) continue;
        rt_enter_critical(); bool alone = f->references == 1 && !f->renderer && !f->backing; rt_exit_critical();
        if (!alone) continue;
        bool stopped = mailbox.stop || mailbox.suspended || mailbox.replace || f->layout != f->owner->layout;
        if (f->cold.raw != f->image.data_size) {
            if (stopped || f->cold_intent != mailbox.intent) frame_cold_clear(f);
        } else if (f->pixels.data && (stopped || f->thaw_intent != mailbox.intent)) {
            if (!frame_spare_keep(f)) {
                rt_enter_critical(); frame_bytes -= f->pixels.capacity; rt_exit_critical();
                frame_pixels_free(&f->pixels, "cold_thaw_cancel", f->owner->session, f->intent);
            }
            f->image.data = NULL; f->thaw_next = NULL; f->thaw_offset = 0;
        }
    }
}
static view_frame_t *frame_cold_candidate(const view_frame_t *incoming)
{
    /* All background preparation must grow the hot window. Rendering a new
     * neighbor must not consume an already prepared neighbor's pixels either. */
    if (!incoming->required) return NULL;
    view_frame_t *donor = NULL;
    unsigned rank = 0;
    size_t capacity = (incoming->image.data_size + 31u) & ~(size_t)31u;
    view_state_t *contexts[7]; unsigned count = window_contexts(primary_state, contexts);
    for (unsigned i = 0; i < count; ++i) for (view_frame_t *f = contexts[i]->frames; f; f = f->next) {
        rt_enter_critical(); bool alone = f->references == 1; rt_exit_critical();
        unsigned distance = frame_distance(f);
        if (f == incoming || !alone || !f->complete || f->cancelled || f->renderer || f->backing ||
            !f->pixels.data || f->pixels.capacity < capacity || ((uintptr_t)f->pixels.data & 31u) ||
            f->owner->session != incoming->owner->session || f->layout != f->owner->layout ||
            !distance || frame_retention_distance(distance) > 3 ||
            (f->cold.first && f->cold.raw == f->image.data_size)) continue;
        if (f->cold_attempted && f->cold_intent == incoming->intent && !f->cold.first) continue;
        if (f->cold.first && f->cold_intent == incoming->intent) { donor = f; break; }
        if ((!donor || !donor->cold.first) && (!donor || frame_bitmap_less_useful(distance, rank))) {
            donor = f; rank = distance;
        }
    }
    return donor;
}
/* Convert at most 64 KiB of source per worker slice, also yielding after the
 * normal render time slice. No pointer moves until the whole lossless copy is
 * admitted. A failed attempt is not restarted during the same input intent. */
static int frame_cold_reuse(view_frame_t *incoming)
{
    if (frame_cold_cancelled(incoming) || !frame_workspace_ready(incoming)) return -1;
    view_frame_t *donor = frame_cold_candidate(incoming);
    if (!donor) return -1;
    if (donor->cold_intent != incoming->intent) frame_cold_clear(donor);
    donor->cold_intent = incoming->intent; donor->cold_attempted = true;
    rt_tick_t started = rt_tick_get();
    for (unsigned blocks = 0; blocks < 16; ++blocks) {
        if (frame_cold_cancelled(incoming) || !reader_frame_store_append(&donor->cold,
            donor->pixels.data, donor->image.data_size, donor->pixels.capacity, &frame_cold_allocator)) {
            frame_cold_clear(donor); return -1;
        }
        if (frame_cold_cancelled(incoming)) { frame_cold_clear(donor); return -1; }
        if (donor->cold.raw == donor->image.data_size) {
            incoming->pixels = donor->pixels;
            memset(&donor->pixels, 0, sizeof(donor->pixels)); donor->image.data = NULL;
            rt_kprintf("reader frame_cold page=%u raw=%u stored=%u event=store\n", donor->number,
                (unsigned)donor->image.data_size, (unsigned)donor->cold.charge);
            return 1;
        }
        if (elapsed_ms(started) >= VIEW_RENDER_SLICE_MS) break;
    }
    return 0;
}
static view_frame_t **frame_pixel_victim(const view_frame_t *incoming, bool matching);
static bool frame_reserve(view_frame_t *incoming)
{
    frame_spare_trim(incoming);
    size_t bytes = (incoming->image.data_size + 31u) & ~(size_t)31u;
    size_t minimum = frame_storage_minimum(incoming);
    if (bytes > VIEW_FRAME_BUDGET) return frame_admission_error(incoming, "frame_budget", 0, bytes);
    for (;;) {
        rt_enter_critical(); size_t resident = frame_bytes; rt_exit_critical();
        /* Keep reusable storage intact while preparing small glyph/layout data. */
        if (resident <= VIEW_FRAME_BUDGET &&
            (frame_spare_available(incoming) || frame_pixel_victim(incoming, true) ||
             (!incoming->resources_ready && frame_cold_candidate(incoming))) &&
            frame_workspace_ready(incoming)) return true;
        if (resident <= VIEW_FRAME_BUDGET - bytes && view_memory_room(minimum)) return true;
        unsigned score = frame_distance(incoming);
        view_frame_t **victim = NULL;
        view_state_t *contexts[7]; unsigned context_count = window_contexts(primary_state, contexts);
        for (unsigned i = 0; i < context_count; ++i)
            for (view_frame_t **at = &contexts[i]->frames; *at; at = &(*at)->next) {
                rt_enter_critical(); unsigned references = (*at)->references; rt_exit_critical();
                unsigned priority = frame_distance(*at);
                if (references == 1 && frame_retention_distance(priority) > 3 &&
                    frame_bitmap_less_useful(priority, score)) { victim = at; score = priority; }
            }
        if (!victim) {
            if (window_reclaim(frame_distance(incoming))) continue;
            bool reusable = frame_spare_available(incoming) || frame_pixel_victim(incoming, true) ||
                (!incoming->resources_ready && frame_cold_candidate(incoming));
            if (resident > VIEW_FRAME_BUDGET - (reusable ? 0 : bytes))
                return frame_admission_error(incoming, "frame_budget", 0, bytes);
            if (reusable) return frame_workspace_ready(incoming);
            if (frame_reclaim_pages(minimum)) return true;
            return frame_admission_error(incoming, "reserve", view_reserve_result, epd_app_allocation_peak(minimum));
        }
        view_frame_t *old = *victim; *victim = old->next;
        frame_reclaim_note(old, incoming->owner, incoming->number, frame_distance(incoming), "reserve");
        frame_release(old);
    }
}
/* Cache-only frames have no UI owner. An active renderer or an adopted
 * backing buffer still owns its pixels and cannot donate or release them. */
static view_frame_t **frame_pixel_victim(const view_frame_t *incoming, bool matching)
{
    unsigned score = frame_distance(incoming);
    view_frame_t **victim = NULL;
    view_state_t *contexts[7]; unsigned context_count = window_contexts(primary_state, contexts);
    size_t bytes = (incoming->image.data_size + 31u) & ~(size_t)31u;
    for (unsigned i = 0; i < context_count; ++i)
        for (view_frame_t **at = &contexts[i]->frames; *at; at = &(*at)->next) {
            view_frame_t *old = *at;
            rt_enter_critical(); unsigned references = old->references; rt_exit_critical();
            if (references != 1 || !old->complete || old->renderer || old->backing || !old->pixels.data ||
                old->cold.first) continue;
            /* RR_INIT clears the new frame using its own geometry. Only raw
             * capacity and alignment constrain storage reuse across reflows. */
            if (matching && (old->pixels.capacity < bytes || ((uintptr_t)old->pixels.data & 31u))) continue;
            unsigned priority = frame_distance(old);
            if (frame_retention_distance(priority) > 3 && frame_bitmap_less_useful(priority, score)) {
                victim = at; score = priority;
            }
        }
    return victim;
}
static bool frame_reuse_pixels(view_frame_t *frame)
{
    view_frame_t **victim = frame_pixel_victim(frame, true);
    if (!victim) return false;
    if (frame_retention_distance(frame_distance(*victim)) <= 3) return false;
    view_frame_t *old = *victim; *victim = old->next;
    frame_reclaim_note(old, frame->owner, frame->number, frame_distance(frame), "reuse");
    frame->pixels = old->pixels;
    memset(&old->pixels, 0, sizeof(old->pixels));
    frame_release(old); /* The transferred capacity remains in frame_bytes. */
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    rt_kprintf("reader pixel_buffer event=reuse reason=donor session=%u intent=%u page=%u bytes=%u base=%p data=%p capacity=%u\n",
        (unsigned)frame->owner->session, (unsigned)frame->intent, frame->number,
        (unsigned)frame->image.data_size, frame->pixels.allocation, frame->pixels.data,
        (unsigned)frame->pixels.capacity);
#endif
    return true;
}
static bool frame_allocate_pixels(view_frame_t *frame)
{
    if (!frame_workspace_ready(frame)) return false;
    if (frame_spare_take(frame)) return true;
    view_frame_t **donor = frame_pixel_victim(frame, true);
    /* Recycle storage outside the six-page window first. Within the window,
     * grow under the same credit/budget before sacrificing a reverse page. */
    if (donor && frame_retention_distance(frame_distance(*donor)) > 3 && frame_reuse_pixels(frame)) return true;
    size_t bytes = (frame->image.data_size + 31u) & ~(size_t)31u;
    rt_enter_critical(); size_t resident = frame_bytes; rt_exit_critical();
    bool admitted = bytes <= VIEW_FRAME_BUDGET && resident <= VIEW_FRAME_BUDGET - bytes &&
        view_memory_room(frame_storage_minimum(frame));
    bool recover = frame_nearby_growth(frame) && !frame->memory_reclaimed;
    if (!admitted) {
        if (!recover && frame_reuse_pixels(frame)) return true;
        bool budget = bytes > VIEW_FRAME_BUDGET || resident > VIEW_FRAME_BUDGET - bytes;
        return frame_admission_error(frame, budget ? "frame_budget" : "reserve",
            budget ? 0 : view_reserve_result, epd_app_allocation_peak(frame_storage_minimum(frame)));
    }
    frame->allocation_attempted = true;
    if (!epd_compute_buffer_alloc(&frame->pixels, frame->image.data_size)) {
        frame_admission_error(frame, "pixels_alloc", 0, epd_app_allocation_peak(bytes + 31u));
        /* A fragmented heap can still supply an intact lower-priority buffer.
         * Transfer it directly: freeing it can lose the alignment padding. */
        if (!recover && frame_reuse_pixels(frame)) return true;
        if (recover) return false;
        view_frame_t **victim = frame_pixel_victim(frame, false);
        if (!victim || frame_retention_distance(frame_distance(*victim)) <= 3) return false;
        view_frame_t *old = *victim; *victim = old->next;
        frame_reclaim_note(old, frame->owner, frame->number, frame_distance(frame), "alloc");
        frame_release(old);
        frame_spare_trim(frame);
        if (frame_spare_take(frame)) return true;
        /* Total free bytes do not prove that a contiguous block fits. Retry
         * once after a real release; do not wait for unrelated pressure work. */
        if (!epd_compute_buffer_alloc(&frame->pixels, frame->image.data_size)) return false;
    }
    int adopted = epd_resource_cache_adopt(&view_resource, frame->pixels.allocation);
    if (adopted != EPD_RESOURCE_OK) {
        frame_pixels_free(&frame->pixels, "cache_adopt_failed", frame->owner->session, frame->intent);
        return frame_admission_error(frame, "cache_adopt", adopted, epd_app_allocation_peak(bytes + 31u));
    }
    rt_enter_critical(); frame_bytes += frame->pixels.capacity; rt_exit_critical();
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    rt_kprintf("reader pixel_buffer event=alloc session=%u intent=%u page=%u bytes=%u base=%p data=%p capacity=%u\n",
        (unsigned)frame->owner->session, (unsigned)frame->intent, frame->number,
        (unsigned)frame->image.data_size, frame->pixels.allocation, frame->pixels.data,
        (unsigned)frame->pixels.capacity);
#endif
    return true;
}
/* A bound frame stays pinned until its replacement is displayed. Only known
 * retirement/close work can release additional storage without that display. */
static int frame_storage(view_frame_t *frame, bool required)
{
    if (frame->pixels.data) return 1;
    if (frame->memory_intent != frame->intent) {
        /* A handed-off preparation belongs to the new input's activity.
         * Never poll a recovery ticket owned by the retired input credit. */
        memset(&frame->admission_recovery, 0, sizeof(frame->admission_recovery));
        frame->memory_reclaimed = frame->memory_waiting = false;
        frame->memory_intent = frame->intent;
        frame->cold_reusing = false;
        frame->admission_stage = NULL; frame->allocation_attempted = false;
    }
    if (frame->cold_reusing) {
        int reused = frame_cold_reuse(frame);
        frame->cold_reusing = reused == 0;
        if (reused >= 0) { if (reused) frame->image.data = frame->pixels.data; return reused; }
        /* A partial conversion can fail while generic recovery is still
         * pending. The intact donor remains usable. Yield before polling or
         * choosing another donor so one call still has only one copy slice. */
        return 0;
    }
    bool nearby = frame_nearby_growth(frame);
    /* A real spare can arrive while recovery is pending or after it failed.
     * Taking it requires only workspace, not a new contiguous allocation. */
    if ((frame_spare_available(frame) || frame_pixel_victim(frame, true)) &&
        frame_workspace_ready(frame) &&
        (frame_spare_take(frame) || frame_reuse_pixels(frame))) {
        frame->image.data = frame->pixels.data; return 1;
    }
    if (frame->admission_recovery.used) {
        epd_memory_recovery_result_t result = epd_memory_recovery_poll(&frame->admission_recovery, false);
        if (result == EPD_MEMORY_RECOVERY_PENDING) goto recovery_pending;
        if (result != EPD_MEMORY_RECOVERY_READY) {
            if (!frame->admission_stage)
                frame_admission_error(frame, "recovery", frame->admission_recovery.result, frame->admission_recovery.physical);
            goto cold_fallback;
        }
    }
    bool reserved = frame_reserve(frame);
    if (!reserved && ((required && frame->owner == primary_state) || nearby)) {
        size_t minimum = epd_app_allocation_peak(frame_storage_minimum(frame));
        epd_memory_recovery_result_t result = view_memory_recover(&frame->admission_recovery,
            minimum, false);
        if (result == EPD_MEMORY_RECOVERY_PENDING) goto recovery_pending;
        if (result != EPD_MEMORY_RECOVERY_READY && !frame->admission_stage)
            frame_admission_error(frame, "recovery", frame->admission_recovery.result, minimum);
        if (result == EPD_MEMORY_RECOVERY_READY) reserved = frame_reserve(frame);
    }
    if (reserved) {
        frame->memory_waiting = false;
        /* Acquire the contiguous pixels before font/image preparation can
         * split the admitted free space into smaller allocations. */
        bool allocated = frame_allocate_pixels(frame);
        if (!allocated && (required || nearby) && !frame->memory_reclaimed) {
            frame->memory_reclaimed = true;
            /* The credit already covers pixels plus workspace. Fragmentation
             * recovery needs one contiguous pixel block, not both together. */
            size_t payload = frame_storage_minimum(frame) - VIEW_RENDER_WORKSPACE;
            size_t physical = epd_app_allocation_peak(payload);
            epd_memory_recovery_result_t result;
            if (frame->admission_recovery.used) {
                frame->admission_recovery.physical = physical;
                result = epd_memory_recovery_failed_allocation(&frame->admission_recovery, payload);
            } else {
                epd_memory_backend_t backend;
                result = epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend) ?
                    epd_memory_recovery_begin_allocation(&frame->admission_recovery,
                        backend, physical, payload, true) : EPD_MEMORY_RECOVERY_DENIED;
            }
            if (result == EPD_MEMORY_RECOVERY_PENDING) goto recovery_pending;
            if (result == EPD_MEMORY_RECOVERY_READY) allocated = frame_allocate_pixels(frame);
        }
        if (!allocated) goto cold_fallback;
        frame->admission_stage = NULL;
        frame->image.data = frame->pixels.data;
        return 1;
    }
 cold_fallback:
    /* Only a foreground request can trade a cache-only neighbor's pixels.
     * Background rendering and thawing both preserve the prepared window. */
    if (required) {
        int reused = frame_cold_reuse(frame);
        frame->cold_reusing = reused == 0;
        if (reused >= 0) { if (reused) frame->image.data = frame->pixels.data; return reused; }
    }
    if (!required) return -3;
    rt_enter_critical();
    bool transient = mailbox.cleanup || mailbox.retired || mailbox.ack;
    rt_exit_critical();
    if (!transient) return frame_memory_failure(frame, frame->resources_ready ? "pixels_alloc" : "pixels_reserve");
    if (!frame->memory_waiting) {
        frame->memory_waiting = true; frame->memory_wait_started = rt_tick_get();
    }
    return elapsed_ms(frame->memory_wait_started) < VIEW_FRAME_WAIT_MS ? 0 : frame_memory_failure(frame, "pixels_retiring");
 recovery_pending:
    /* Fragmented free space may never fit a raw frame. An admitted lossless
     * transfer supplies an existing continuous buffer without waiting for the
     * generic recovery timeout. Keep that request alive if transfer fails. */
    if (required) {
        int reused = frame_cold_reuse(frame);
        frame->cold_reusing = reused == 0;
        if (reused >= 0) { if (reused) frame->image.data = frame->pixels.data; return reused; }
    }
    return 0;
}

/* A cold cache hit becomes displayable only after its continuous buffer has
 * been restored. completed_at records that transition, not the earlier raster. */
static int frame_materialize(view_frame_t *frame, bool required)
{
    if (!frame->cold.first) return frame->pixels.data ? 1 : -1;
    if (frame->cold.raw != frame->image.data_size) {
        frame_cold_clear(frame); return frame->pixels.data ? 1 : -1;
    }
    frame->intent = frame->owner->intent; frame->required = required;
    frame->resources_ready = true;
    if (frame_cold_cancelled(frame)) return -2;
    int result = frame_storage(frame, required);
    if (result <= 0) return result;
    if (!frame->thaw_next) {
        frame->thaw_next = frame->cold.first; frame->thaw_offset = 0;
        frame->thaw_intent = frame->intent;
    }
    rt_tick_t started = rt_tick_get();
    for (unsigned blocks = 0; blocks < 16 && frame->thaw_next; ++blocks) {
        if (frame_cold_cancelled(frame)) return -2;
        reader_frame_block_t *block = frame->thaw_next;
        if (!reader_frame_block_read(block, (uint8_t *)frame->pixels.data + frame->thaw_offset,
            frame->image.data_size - frame->thaw_offset)) return -1;
        frame->thaw_offset += block->raw; frame->thaw_next = block->next;
        if (elapsed_ms(started) >= VIEW_RENDER_SLICE_MS) break;
    }
    if (frame->thaw_next) return 0;
    if (frame->thaw_offset != frame->image.data_size) return -1;
    if (frame_cold_cancelled(frame)) return -2;
    size_t stored = frame->cold.charge;
    frame_cold_clear(frame);
    frame->image.data = frame->pixels.data; frame->completed_at = rt_tick_get();
    rt_kprintf("reader frame_cold page=%u raw=%u stored=%u event=restore\n", frame->number,
        (unsigned)frame->image.data_size, (unsigned)stored);
    return 1;
}
static int frame_glyph_progress(view_frame_t *frame, bool required)
{
    if (frame->pin_ready_pass > frame->pin_ready_peak) {
        frame->pin_ready_peak = frame->pin_ready_pass;
        frame->pin_progress_at = rt_tick_get();
    }
    frame->pin_ready_pass = frame->pin_run = frame->pin_byte = 0;
    frame->pins_pending = false;
    if (elapsed_ms(frame->pin_progress_at) < VIEW_PRESSURE_WAIT_MS) return 0;
    /* Repeating a scan is not progress. Preserve the reusable spare, but
     * release a further idle bitmap if glyph preparation needs real space. */
    if (!required) {
        frame_admission_error(frame, "glyph_pending", 0, 0); return -3;
    }
    if (frame->owner == primary_state && !frame->pin_recovery_used) {
        frame->pin_recovery_used = true;
        view_frame_t **victim;
        while ((victim = frame_pixel_victim(frame, false)) != NULL) {
            view_frame_t *old = *victim; *victim = old->next;
            frame_reclaim_note(old, frame->owner, frame->number, frame_distance(frame), "glyph_wait");
            rt_enter_critical(); size_t before = frame_bytes; rt_exit_critical();
            frame_release(old);
            rt_enter_critical(); bool released = frame_bytes < before; rt_exit_critical();
            if (released) { frame->pin_progress_at = rt_tick_get(); return 0; }
        }
    }
    frame_admission_error(frame, "glyph_pending", 0, 0);
    return frame_memory_failure(frame, "glyph_pending");
}
/* Return 0 pending, 1 complete, -2 cancelled, -3 storage exhausted, -1 failed.
 * Each call emits at most VIEW_RENDER_COMMANDS component commands and yields
 * between commands after the slice expires. A command itself may take longer. */
static int frame_render(view_frame_t *frame, bool required)
{
    if (required && !frame->required && frame->renderer &&
        epd_reader_render_set_purpose(frame->renderer, EPD_COMPUTE_FOREGROUND) != EPD_COMPUTE_OK) return -1;
    frame->required = required;
    rt_enter_critical();
    if (render_frame == frame) render_required = required;
    rt_exit_critical();
    if (render_cancelled(frame)) return -2;
    if (!frame->pins) {
        int storage = frame_storage(frame, required);
        if (storage <= 0) return storage;
    }
    if (!frame->pins) {
        if (!frame->pin_creating) {
            frame->pin_creating = true;
            frame->pin_progress_at = rt_tick_get();
        }
        frame->pins = epd_app_font_page_create();
        if (!frame->pins) {
            frame_admission_error(frame, "pins_create", 0, 0);
            return elapsed_ms(frame->pin_progress_at) < VIEW_FRAME_WAIT_MS ?
                0 : frame_memory_failure(frame, "pins_create");
        }
        frame->pin_creating = false;
        frame->admission_stage = NULL;
        frame->pin_progress_at = rt_tick_get();
    }
    rt_tick_t started = rt_tick_get();
    unsigned operations = 0;
    if (!frame->resources_ready) {
        while (frame->pin_run < frame->page.count) {
            const view_run_t *run = &frame->page.runs[frame->pin_run];
            if ((unsigned)run->text + run->bytes > frame->page.bytes) return -1;
            if (run->image || frame->pin_byte >= run->bytes) {
                ++frame->pin_run; frame->pin_byte = 0; continue;
            }
            if (render_cancelled(frame)) return -2;
            const lv_font_t *font = font_for(run->style);
            uint32_t letter;
            unsigned pin_byte = frame->pin_byte;
            frame->pin_byte += utf8_letter(frame->page.text + run->text + frame->pin_byte,
                                          run->bytes - frame->pin_byte, &letter);
            if (letter != '\r' && letter != '\n') {
                if (letter == '\t') letter = ' ';
                int pinned = epd_app_font_page_pin(frame->pins, font, letter, 0, true);
                if (pinned == EPD_APP_FONT_NO_MEMORY) {
                    frame_admission_error(frame, "glyph_pin", pinned, 0); return -3;
                }
                if (pinned == EPD_APP_FONT_PIN_PENDING) {
                    frame->pin_byte = pin_byte;
                    return elapsed_ms(frame->pin_progress_at) < VIEW_PRESSURE_WAIT_MS ?
                        0 : frame_glyph_progress(frame, required);
                }
                if (pinned < 0) {
                    int ready = epd_app_font_glyph_ready(font, letter, 0, true);
                    if (ready == EPD_APP_FONT_NO_MEMORY) {
                        frame_admission_error(frame, "glyph_ready", ready, 0); return -3;
                    }
                    if (ready < 0) return -1;
                    frame_admission_error(frame, "glyph_pin", pinned, 0);
                    return frame_memory_failure(frame, "glyph_pin");
                }
                int ready = epd_app_font_glyph_ready(font, letter, 0, true);
                if (ready == EPD_APP_FONT_NO_MEMORY) {
                    frame_admission_error(frame, "glyph_ready", ready, 0); return -3;
                }
                if (ready < 0) return -1;
                if (!ready) frame->pins_pending = true;
                else ++frame->pin_ready_pass;
            }
            if (++operations >= 32u || elapsed_ms(started) >= VIEW_RENDER_SLICE_MS) return 0;
        }
        if (frame->pins_pending) {
            return frame_glyph_progress(frame, required);
        }
        int images = images_prepare_runs(frame->page.runs, frame->page.count, required);
        if (images <= 0) return images;
        frame->resources_ready = true;
        frame->resources_at = rt_tick_get();
    }
    if (render_cancelled(frame)) return -2;
    int storage = frame_storage(frame, required);
    if (storage <= 0) return storage;
    if (!frame->renderer) {
        int admitted = render_admit(frame, required);
        if (admitted <= 0) return admitted;
    }
    operations = 0;
    for (;;) {
        if (render_cancelled(frame)) return -2;
        if (operations >= VIEW_RENDER_COMMANDS || (operations && elapsed_ms(started) >= VIEW_RENDER_SLICE_MS)) {
            int result = render_flush(frame);
            if (render_cancelled(frame)) return -2;
            return result == EPD_COMPUTE_OK ? 0 : -1;
        }
        rr_command command;
        const void *pixels = NULL;
        size_t bytes = 0;
        epd_app_font_glyph_t glyph = {0};
        bool acquired = false;
        if (!frame->render_initialized) {
            command = render_command(RR_INIT, frame);
        } else if (frame->render_run < frame->page.count) {
            const view_run_t *run = &frame->page.runs[frame->render_run];
            if (run->image) {
                view_image_t *image = frame->render_image;
                if (!image) {
                    image = image_find(run);
                    if (!image || image->state != 1) return -1;
                    /* Keep the shared source alive until the renderer closes on failure. */
                    ++image->references;
                    frame->render_image = image;
                }
                command = render_command(RR_SCALE_GRAY4_TO_RGB565, frame);
                command.x = run->x; command.y = run->y;
                command.width = run->width; command.height = run->height;
                if (frame->full_picture) {
                    unsigned w = frame->image.header.w, h = frame->image.header.h;
                    unsigned sw = image->picture.width, sh = image->picture.height;
                    if ((uint64_t)w * sh <= (uint64_t)h * sw) h = (uint64_t)w * sh / sw;
                    else w = (uint64_t)h * sw / sh;
                    command.width = w ? w : 1u; command.height = h ? h : 1u;
                    command.x = (frame->image.header.w - command.width) / 2u;
                    command.y = (frame->image.header.h - command.height) / 2u;
                }
                if (frame->render_row >= command.height) {
                    image_release(frame->render_image); frame->render_image = NULL;
                    ++frame->render_run; frame->render_row = frame->render_byte = 0; continue;
                }
                if (command.width == image->picture.width && command.height == image->picture.height)
                    command.operation = RR_BLIT_GRAY4;
                command.src_width = image->picture.width; command.src_height = image->picture.height;
                command.src_stride = (command.src_width + 1u) / 2u;
                command.clip_y = command.y + frame->render_row;
                command.clip_h = command.height - frame->render_row;
                if (command.clip_h > RR_ROWS_PER_STEP) command.clip_h = RR_ROWS_PER_STEP;
                bytes = image->bytes;
                frame->render_row += command.clip_h;
            } else {
                const lv_font_t *font = font_for(run->style);
                const epd_app_font_metrics_t *metrics = run->style & BOOK_STYLE_ITALIC ?
                    &view.italic_metrics[font_slot(run->style)] : &view.metrics[font_slot(run->style)];
                if (!frame->render_byte) frame->render_x = run->x;
                if (!frame->render_decorated && (run->style & BOOK_STYLE_HEADING) &&
                    (!frame->render_run || frame->page.runs[frame->render_run - 1].image ||
                     frame->page.runs[frame->render_run - 1].y != run->y ||
                     !(frame->page.runs[frame->render_run - 1].style & BOOK_STYLE_HEADING))) {
                    command = render_command(RR_FILLRECT, frame);
                    command.x = 0; command.y = run->y;
                    command.width = epd_reader_heading_bar_width(run->height);
                    if (command.width > run->x) command.width = run->x;
                    command.height = run->height;
                    frame->render_decorated = true;
                    if (!command.width) continue;
                } else if (frame->render_byte < run->bytes) {
                    uint32_t letter;
                    frame->render_byte += utf8_letter(frame->page.text + run->text + frame->render_byte,
                                                     run->bytes - frame->render_byte, &letter);
                    if (letter == '\n' || letter == '\r') { ++operations; continue; }
                    if (letter == '\t') letter = ' ';
                    int ready = epd_app_font_glyph_acquire(font, letter, 0, true, &glyph);
                    if (ready == EPD_APP_FONT_NO_MEMORY) {
                        frame_admission_error(frame, "glyph_acquire", ready, 0); return -3;
                    }
                    if (ready != 1) return -1;
                    acquired = true;
                    command = render_command(RR_BLIT_A8, frame);
#ifdef BOOKS_READER_UI
                    if (!(frame->appearance & UI_READER_ANTIALIAS))
                        command.flags |= RR_A8_MONO;
#endif
                    command.x = frame->render_x + glyph.ofs_x;
                    command.y = (int32_t)run->y + metrics->line_height - metrics->base_line - glyph.box_h - glyph.ofs_y;
                    command.width = command.src_width = glyph.box_w;
                    command.height = command.src_height = glyph.box_h; command.src_stride = glyph.stride;
                    pixels = glyph.pixels; bytes = glyph.stride * glyph.box_h;
                    frame->render_x += glyph.adv_w;
                    if (!glyph.box_w || !glyph.box_h) {
                        epd_app_font_glyph_release(&glyph); ++operations; continue;
                    }
                } else if (!frame->render_struck && (run->style & BOOK_STYLE_STRIKE)) {
                    frame->render_struck = true;
                    command = render_command(RR_FILLRECT, frame);
                    command.x = run->x; command.y = run->y + run->height / 2u;
                    command.width = run->width; command.height = 1;
                } else {
                    const view_run_t *next = frame->render_run + 1 < frame->page.count ?
                        run + 1 : NULL;
                    unsigned guide = frame->appearance & UI_READER_GUIDE_MASK;
                    bool line_end = !next || next->image || next->y != run->y;
                    unsigned baseline = run->y + metrics->line_height - metrics->base_line;
                    if (guide && line_end && !frame->render_guided) {
                        frame->render_guided = true;
                        command = render_command(RR_FILLRECT, frame);
                        if (guide == UI_READER_GUIDE_DOTTED) command.flags = RR_FILL_DOTTED;
                        if (!(frame->appearance & UI_READER_NEGATIVE)) command.fill_rgb565 = 0xad55u;
                        command.y = run->y + run->height;
                        command.width = frame->image.header.w; command.height = 1;
                    } else {
                        ++frame->render_run; frame->render_byte = 0; frame->render_decorated = false;
                        frame->render_struck = frame->render_guided = false;
                        if (!run->link && !(run->style & BOOK_STYLE_UNDERLINE)) continue;
                        command = render_command(RR_FILLRECT, frame);
                        command.x = run->x; command.y = baseline + 1u;
                        command.width = run->width; command.height = 1;
                    }
                }
            }
        } else if (!frame->render_finished) {
            command = render_command(RR_FINISH, frame);
        } else break;
        int result = render_submit(frame, &command, pixels, bytes);
        if (acquired) epd_app_font_glyph_release(&glyph);
        ++operations;
        if (render_cancelled(frame)) return -2;
        if (result != EPD_COMPUTE_OK) return -1;
        if (command.operation == RR_INIT) frame->render_initialized = true;
        if (command.operation == RR_FINISH) frame->render_finished = true;
    }
    int closed = epd_reader_render_shutdown(&frame->renderer);
    if (render_cancelled(frame)) return -2;
    if (closed != EPD_COMPUTE_OK) return -1;
    render_frame = NULL;
    epd_app_font_page_release(frame->pins); frame->pins = NULL;
    frame->complete = true;
    frame->completed_at = rt_tick_get();
    frame->info = view.status;
    frame->info.ready = true; frame->info.saved = false;
    frame->info.page = view.wanted + 1;
    frame->info.font = NULL; frame->info.text = "";
    snprintf(frame->title, sizeof(frame->title), "%s", view.title);
    frame->info.title = frame->title;
    frame->position.cursor = (document_cursor_t){frame->page.start.record, frame->page.start.byte};
    frame->position.conversion_version = DOCUMENT_CONVERSION_VERSION;
    frame->position.layout = view.layout;
    document_source_position(view.document, frame->page.start.record, frame->page.start.byte, &frame->position.source);
    frame->info.percent = document_source_percent(view.document, &view.progress, &frame->position.source);
    pagination_snapshot(state, &frame->info);
    frame->position.page = frame->info.page; frame->position.pages = frame->info.pages;
    frame->position.percent = frame->info.percent;
    rt_kprintf("reader frame session=%u intent=%u required=%u source=%u:%u:%u resources_ms=%u raster_ms=%u bytes=%u\n",
        (unsigned)view.session, (unsigned)view.intent, required,
        (unsigned)frame->position.source.unit, (unsigned)frame->position.source.chapter,
        (unsigned)frame->position.source.offset,
        duration_ms(frame->resources_at - frame->preparation_started), elapsed_ms(frame->resources_at),
        (unsigned)frame->image.data_size);
    return 1;
}
static void frame_publish(view_frame_t *cached)
{
    if (!cached->pixels.data || cached->cold.first) return;
    if (view.frame_published && view.published_intent == view.intent &&
        view.published_generation == cached->generation) return;
    view_frame_t *frame = view_allocate(sizeof(*frame), EPD_APP_PSRAM);
    if (!frame) { fail(READER_ERROR_IMAGE_MEMORY); return; }
    *frame = *cached; frame->window_charge = 0;
    frame->backing = cached; frame->next = NULL; frame->references = 1; frame->renderer = NULL;
    frame->intent = view.intent; frame->pins = NULL;
    frame_ref(cached);
    rt_enter_critical(); ++view.references; rt_exit_critical();
    frame->info = view.status; frame->info.ready = true; frame->info.saved = false;
    frame->info.font = NULL; frame->info.text = ""; frame->info.title = frame->title;
    frame->info.page = view.wanted + 1;
    frame->info.percent = document_source_percent(view.document, &view.progress, &frame->position.source);
    pagination_snapshot(state, &frame->info);
    frame->position.page = frame->info.page; frame->position.pages = frame->info.pages;
    frame->position.percent = frame->info.percent;
    rt_tick_t ready_at = 0, requested_at = 0;
    rt_enter_critical();
    view_frame_t *old = mailbox.ready;
    bool accepted = mailbox.session == view.session && mailbox.intent == view.intent;
    if (accepted) {
        frame->requested_at = mailbox.requested_at; frame->ready_at = rt_tick_get();
        requested_at = frame->requested_at; ready_at = frame->ready_at;
        mailbox.ready = frame;
    }
    else old = frame;
    rt_exit_critical();
    if (accepted) {
        unsigned images = 0, text_bytes = 0;
        for (unsigned i = 0; i < frame->page.count; ++i) {
            if (frame->page.runs[i].image) ++images;
            else text_bytes += frame->page.runs[i].bytes;
        }
        rt_kprintf("reader ready session=%u intent=%u request_to_ready_ms=%u tick=%u runs=%u text_bytes=%u images=%u source=%u:%u:%u prepared=%u\n",
            (unsigned)view.session, (unsigned)view.intent, duration_ms(ready_at - requested_at), (unsigned)ready_at,
            (unsigned)frame->page.count, text_bytes, images, (unsigned)frame->position.source.unit,
            (unsigned)frame->position.source.chapter, (unsigned)frame->position.source.offset,
            (unsigned)(frame->complete && (int32_t)(requested_at - frame->completed_at) > 0));
    }
    frame_release(old);
    view.frame_published = true; view.published_intent = view.intent; view.published_generation = cached->generation;
    if (accepted) {
        epd_reader_ui_wake();
        first_font_ready(view.session, view.intent, view.font_id);
    }
}
static void frame_accept(view_frame_t *frame);
static bool optional_pixels_ready(void)
{
    if (!view.pixels_blocked || view.pixels_blocked_intent != view.intent) return true;
    /* Global credit/owner changes do not establish progress for this failed
     * optional frame. A new intent or a better-ranked page remains eligible. */
    return view.pixels_memory_blocked && frame_bytes < view.pixels_blocked_bytes;
}
static bool optional_pixels_ready_for(unsigned number)
{
    if (optional_pixels_ready() || frame_priority(state, number) < view.pixels_blocked_priority) return true;
    if (!view.pixels_memory_blocked) return false;
    if (view.pixels_retry_ready) return true;
    for (view_frame_t *f = view.frames; f; f = f->next) {
        /* Frames in one layout share geometry, including a not-yet-created
         * neighbor. A matching spare supplies it without heap growth. */
        if (f->layout == view.layout && frame_spare_available(f)) return true;
    }
    /* A failed first render has no cached frame. Physical capacity progress
     * must rearm it too. Latch one opportunity before opening its work credit;
     * owner/credit epochs alone do not establish usable heap progress. */
    size_t available = epd_app_psram_available();
    if (available <= view.pixels_blocked_available) return false;
    view.pixels_blocked_available = available;
    view.pixels_retry_ready = true;
    return true;
}
static void optional_pixels_block(int result, unsigned number)
{
    size_t available = epd_app_psram_available();
    /* Keep the high-water mark across unsuccessful attempts. Closing the new
     * activity or its temporary frame cannot manufacture another retry. */
    if (!view.pixels_memory_blocked || view.pixels_blocked_intent != view.intent ||
        available > view.pixels_blocked_available) view.pixels_blocked_available = available;
    view.pixels_blocked = true; view.pixels_memory_blocked = result == -3;
    view.pixels_retry_ready = false;
    view.pixels_blocked_intent = view.intent;
    view.pixels_blocked_priority = frame_priority(state, number);
    view.pixels_blocked_bytes = frame_bytes;
    rt_kprintf("reader near_block session=%u intent=%u page=%u rank=%u result=%d free=%u frames=%u\n",
        (unsigned)view.session, (unsigned)view.intent, number, view.pixels_blocked_priority, result,
        (unsigned)epd_app_psram_available(), (unsigned)view.pixels_blocked_bytes);
}
static int frame_prewarm(view_frame_t *frame)
{
    if (!optional_pixels_ready_for(frame->number)) return -3;
    if (view.pixels_blocked) {
        /* Eligibility came from a new input or actual reusable capacity.
         * Retire the old bounded recovery result before making one attempt. */
        view.pixels_blocked = false;
        memset(&frame->admission_recovery, 0, sizeof(frame->admission_recovery));
        frame->memory_reclaimed = false;
    }
    int result = frame_materialize(frame, false);
    if (result < 0 && result != -2) { frame_admission_report(frame, result); optional_pixels_block(result, frame->number); }
    else if (result > 0) view.pixels_memory_blocked = view.pixels_retry_ready = false;
    return result;
}
static bool frame_adopt(view_frame_t *frame, const view_page_t *page)
{
    if (!frame || frame->cancelled || (frame->renderer && frame->intent != view.intent) ||
        frame->owner != state || frame->layout != view.layout ||
        !cursor_equal((document_cursor_t){frame->page.start.record, frame->page.start.byte}, page->start) ||
        !cursor_equal((document_cursor_t){frame->page.cursor.record, frame->page.cursor.byte}, page->cursor) ||
        frame->page.eof != page->eof) return false;
    if (frame->intent != view.intent) frame_handoff_note(frame, view.intent);
    frame->intent = view.intent; frame->generation = page->generation;
    frame->number = view.preview_cache_active ? view.preview_page : view.wanted;
    return true;
}
static void frame_prepare(void)
{
    frame_cold_maintain();
    if (request_memory_failed(state)) return;
    if (view.failure || !view.initialized || view.target_pending || view.segment_turns || data_retiring) return;
    bool target = view.display_pending;
    unsigned target_number = view.preview_cache_active ? view.preview_page : view.wanted;
    /* Layout and completed bitmaps can cover a wider window than speculative
     * rasterization, which favors the current reading direction. */
    if (view.rendering && !(target && view.selected) &&
        !window_prewarm_rank(window_priority(state, view.rendering->number))) {
        frame_release(view.rendering); view.rendering = NULL;
    }
    if (target && !view.selected && (!window_prewarm_rank(window_priority(state, target_number)) ||
        !optional_pixels_ready_for(target_number))) return;
    if (target) {
        view_frame_t *cached = frame_find(view.building->start);
        if (cached && (!cursor_equal((document_cursor_t){cached->page.cursor.record, cached->page.cursor.byte},
                                     view.building->cursor) || cached->page.eof != view.building->eof)) cached = NULL;
        if (cached) {
            if (!frame_window_contains(view.rendering, (epd_reader_page_t *)view.building, target_number)) {
                frame_release(view.rendering); view.rendering = NULL;
            }
            if (view.selected) {
                int ready = frame_materialize(cached, true);
                if (ready > 0) frame_publish(cached);
                else if (ready == -3) { frame_admission_report(cached, ready); page_memory_block(view.building, "frame_restore", READER_ERROR_IMAGE_MEMORY); }
                else if (ready < 0 && ready != -2) fail(READER_ERROR_IMAGE_READ);
            } else if (frame_pixels_ready(cached) || frame_prewarm(cached) > 0) frame_accept(cached);
            return;
        }
        if (view.rendering && !frame_adopt(view.rendering, view.building)) {
            frame_release(view.rendering); view.rendering = NULL;
        }
        if (!view.rendering) view.rendering = frame_create((epd_reader_page_t *)view.building,
            view.preview_cache_active ? view.preview_page : view.wanted, view.building->generation);
        if (!view.rendering) {
            if (view.selected) page_memory_block(view.building, "frame_create", READER_ERROR_IMAGE_MEMORY);
            else optional_pixels_block(-3, target_number);
            return;
        }
    } else if (!view.background_paused && view.status.ready && !core_waiting() && !view.rendering) {
        unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
        reader_cached_page_t *candidate = NULL;
        unsigned priority = UINT_MAX;
        for (unsigned d = 1; d <= 3; ++d) {
            unsigned numbers[2] = {wanted <= UINT_MAX - d ? wanted + d : UINT_MAX,
                                   wanted >= d ? wanted - d : UINT_MAX};
            for (unsigned i = 0; i < 2; ++i) {
                if (numbers[i] == UINT_MAX) continue;
                reader_cached_page_t *p = reader_page_cache_find(&view.cache, numbers[i], view.layout);
                if (!p || (view.preview_back_scanning && p->number < view.preview_back_base) ||
                    !optional_pixels_ready_for(p->number) ||
                    frame_pixels_ready(frame_find((document_cursor_t){p->start.record, p->start.byte}))) continue;
                unsigned rank = window_priority(state, p->number);
                if (!window_prewarm_rank(rank)) continue;
                if (rank < priority) { candidate = p; priority = rank; }
            }
        }
        if (candidate) {
            view_frame_t *cached = frame_find((document_cursor_t){candidate->start.record, candidate->start.byte});
            if (cached) { frame_prewarm(cached); return; }
        }
        /* Reuse the idle scratch page only after its layout batch has ended. */
        if (candidate && view.compute_page != view.building) {
            page_reset(view.building, (document_cursor_t){candidate->start.record, candidate->start.byte});
            reader_page_cache_load(candidate, (epd_reader_page_t *)view.building);
            view.rendering = frame_create((epd_reader_page_t *)view.building, candidate->number, view.building->generation);
            if (!view.rendering) { optional_pixels_block(-3, candidate->number); return; }
        }
    }
    if (!view.rendering) return;
    int result = frame_render(view.rendering, target && view.selected);
    if (!result) return;
    view_frame_t *frame = view.rendering; view.rendering = NULL;
    if (result < 0) {
        if (result != -2) frame_admission_report(frame, result);
        unsigned number = frame->number;
        size_t retiring = frame->renderer && !frame->backing && frame->pixels.capacity ?
            epd_app_allocation_peak(frame->pixels.capacity + 31u) : 0;
        rt_enter_critical();
        size_t retained = frame_bytes - (frame->backing ? 0 : frame->pixels.capacity);
        rt_exit_critical();
        frame_release(frame);
        if (target && view.selected && result == -3)
            page_memory_block(view.building, "frame_prepare", READER_ERROR_IMAGE_MEMORY);
        else if (target && view.selected && result != -2) fail(READER_ERROR_IMAGE_READ);
        else if (result != -2) {
            optional_pixels_block(result, number);
            /* Its own deferred CLOSE/spare release is not new capacity. */
            if (view.pixels_blocked_bytes > retained) view.pixels_blocked_bytes = retained;
            size_t after_close = epd_app_psram_available();
            if (retiring <= SIZE_MAX - after_close && after_close + retiring > view.pixels_blocked_available)
                view.pixels_blocked_available = after_close + retiring;
        }
        return;
    }
    view.pixels_blocked = view.pixels_memory_blocked = view.pixels_retry_ready = false;
    /* A seek can assign the same window page number to a different cursor.
     * Keep one cached frame per slot; the UI retains its own reference until
     * it has finished displaying the superseded frame. */
    view_frame_t **at = &view.frames;
    while (*at) {
        view_frame_t *old = *at;
        if (old->number != frame->number || old->layout != frame->layout) {
            at = &old->next;
            continue;
        }
        *at = old->next;
        old->discard_pixels = true;
        frame_release(old);
    }
    frame->next = view.frames; view.frames = frame;
    if (target) { if (view.selected) frame_publish(frame); else frame_accept(frame); }
    frame_trim();
}
static void layout_accept(void)
{
    view_page_t *old = view.display; view.display = view.building; view.building = old;
    view.display_layout = view.layout; view.status.ready = true;
    view.numeric_target = false;
    view.status.page = view.wanted + 1;
    view.display_pending = view.notified = false;
    frame_trim();
    cache_store(view.wanted, view.display);
    page_reset(view.building, (document_cursor_t){UINT32_MAX, 0});
    document_flow();
    view.trace_pending = false;
}
static void frame_accept(view_frame_t *frame)
{
    if (!frame || frame->owner != state || frame->layout != view.layout ||
        frame->appearance != view.appearance || !view.display_pending ||
        !cursor_equal(view.building->start, frame->position.cursor)) return;
    view.status.percent = frame->info.percent;
    layout_accept();
}
static void frame_remember(const view_frame_t *frame)
{
    if (frame && frame->complete && !frame->cancelled)
        visited_record(frame->owner, &frame->page, frame->layout);
}

static void state_retire(view_state_t *retired)
{
    if (!retired || retired->parked) return;
    document_target_require(retired->document, false);
    document_navigation_release(retired->document);
    retired->navigation_link = retired->navigation_toc = 0;
    if (retired->displayed) {
        frame_release(retired->rendering); retired->rendering = NULL;
        view_frame_t **at = &retired->frames;
        while (*at) {
            rt_enter_critical(); unsigned references = (*at)->references; rt_exit_critical();
            if (references == 1) {
                view_frame_t *old = *at; *at = old->next; frame_release(old);
            } else at = &(*at)->next;
        }
        images_free(retired->images); retired->images = NULL;
        retired->parked = true; retired->previous = retired->following = NULL;
        parked_state = retired;
        return;
    }
    view_state_t *saved = state; state = retired;
    frame_cache_clear(); core_release();
    state = saved;
    rt_enter_critical(); retired->next = mailbox.retired; mailbox.retired = retired; rt_exit_critical();
}
static void state_dispose(view_state_t *retired)
{
    document_close(retired->document);
    for (unsigned i = 0; i < 3; ++i) {
        document_font_free(retired->oblique[i]); document_font_free(retired->retired_oblique[i]);
        epd_app_font_destroy(retired->fonts[i]); epd_app_font_destroy(retired->fallbacks[i]);
    }
    for (unsigned i = 0; i < 6; ++i) epd_app_font_destroy(retired->retired[i]);
    window_uncharge_bytes(retired->window_state_charge); retired->window_state_charge = 0;
    epd_app_free(retired);
}
static void cluster_retire(view_state_t *current)
{
    window_clear();
    if (!current) return;
    state_retire(current->previous); state_retire(current->following); state_retire(current);
}
static void window_uncharge(view_state_t *owner)
{
    window_uncharge_bytes(owner->window_state_charge + owner->window_workspace_charge);
    owner->window_state_charge = owner->window_workspace_charge = 0;
    for (view_frame_t *f = owner->frames; f; f = f->next) {
        window_uncharge_bytes(f->window_charge); f->window_charge = 0;
    }
    if (owner->rendering) {
        window_uncharge_bytes(owner->rendering->window_charge); owner->rendering->window_charge = 0;
    }
}
static void window_clear(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        view_state_t *owner = window_slots[i].owner;
        window_slots[i].owner = NULL; window_slots[i].hop = 0;
        state_retire(owner);
    }
}
static bool window_store(view_state_t *owner, int hop)
{
    if (!owner || owner->displayed || owner->selected || hop < -3 || hop > 3 ||
        (hop >= -1 && hop <= 1)) return false;
    unsigned slot = 4;
    for (unsigned i = 0; i < 4; ++i) {
        if (window_slots[i].owner == owner ||
            (window_slots[i].owner && window_slots[i].hop == hop)) return false;
        if (!window_slots[i].owner && slot == 4) slot = i;
    }
    if (slot == 4) return false;
    size_t workspace = owner->initialized ? 4u * sizeof(view_page_t) +
        sizeof(epd_reader_batch_t) + owner->capacity * sizeof(*owner->pages) + 512u : 0;
    size_t bytes = sizeof(*owner) + workspace;
    for (view_frame_t *f = owner->frames; f; f = f->next) bytes += sizeof(*f);
    if (owner->rendering) bytes += sizeof(*owner->rendering);
    if (!window_charge(bytes)) return false;
    owner->window_state_charge = sizeof(*owner); owner->window_workspace_charge = workspace;
    for (view_frame_t *f = owner->frames; f; f = f->next) f->window_charge = sizeof(*f);
    if (owner->rendering) owner->rendering->window_charge = sizeof(*owner->rendering);
    owner->previous = owner->following = NULL;
    window_slots[slot].owner = owner; window_slots[slot].hop = hop;
    return true;
}
static void window_shift(view_state_t *current, view_state_t *next, int direction)
{
    view_state_t *opposite = direction > 0 ? current->previous : current->following;
    current->previous = current->following = NULL;
    if (direction > 0) next->previous = current; else next->following = current;
    for (unsigned i = 0; i < 4; ++i) if (window_slots[i].owner) {
        view_state_t *owner = window_slots[i].owner;
        int hop = window_slots[i].hop - direction;
        if (hop == -1 || hop == 1) {
            window_slots[i].owner = NULL; window_slots[i].hop = 0;
            window_uncharge(owner);
            if (hop < 0) next->previous = owner; else next->following = owner;
        } else if (hop < -3 || hop > 3) {
            window_slots[i].owner = NULL; window_slots[i].hop = 0; state_retire(owner);
        } else window_slots[i].hop = hop;
    }
    if (opposite && !window_store(opposite, -2 * direction)) state_retire(opposite);
}
static bool window_work_allowed(void)
{
    if (view.failure || request_memory_failed(state) ||
        (view.font_memory_blocked && view.font_blocked_intent == view.intent)) return false;
    if (view.init_blocked && view.init_blocked_intent == view.intent &&
        epd_memory_capacity_epoch() == view.init_blocked_epoch) return false;
    if (!view.status.ready && ((view.building && !page_work_allowed(view.building)) ||
        (view.index && !page_work_allowed(view.index)))) return false;
    return true;
}
static bool window_pixels_pending_for(view_state_t *owner, int direction)
{
    if (owner->rendering) return true;
    window_route_t route = window_route(direction);
    for (unsigned i = 0; i < route.count; ++i) if (route.pages[i].owner == owner) {
        view_state_t *saved = state; state = owner;
        unsigned number = route.pages[i].number;
        document_cursor_t start; bool eof;
        bool known = window_page(owner, number, &start, &eof);
        bool pending = window_work_allowed() && optional_pixels_ready_for(number) &&
            (known ? !frame_pixels_ready(frame_find(start)) :
            (number != owner->prefetch_number || !owner->prefetch || page_work_allowed(owner->prefetch)) &&
            (!owner->status.ready || (owner->cache_blocked != number &&
            !(number < window_number(owner) && owner->preview_back_blocked))));
        state = saved;
        if (pending) return true;
    }
    return false;
}
static bool window_pixels_pending(view_state_t *owner)
{
    return window_pixels_pending_for(owner, -1) || window_pixels_pending_for(owner, 1);
}
static bool window_runnable(view_state_t *owner, view_state_t *current)
{
    if (!owner) return false;
    view_state_t *saved = state; uint32_t intent = owner->intent;
    state = owner; view.intent = current->intent;
    bool runnable = window_work_allowed() && window_pixels_pending(owner);
    view.intent = intent; state = saved;
    return runnable;
}
static document_t *window_conversion_document(view_state_t *current)
{
    view_state_t *contexts[9]; unsigned count = window_contexts(current, contexts);
    if (parked_state) contexts[count++] = parked_state;
    if (pagination.counter) contexts[count++] = pagination.counter;
    document_t *pending = NULL;
    bool readable = false;
    for (unsigned i = 0; i < count; ++i) {
        document_progress_t progress;
        document_progress(contexts[i]->document, &progress);
        if (progress.complete || progress.result) continue;
        /* Prefer the converter that has already supplied input over an
         * unopened context. A foreground command can leave cancelled input
         * awaiting its terminal publication for one worker round. */
        if (!pending || (!readable && progress.end)) {
            pending = contexts[i]->document; readable = progress.end != 0;
        }
    }
    return pending;
}
static bool window_conversion_ready(view_state_t *current)
{
    document_t *pending = window_conversion_document(current);
    if (!pending) return true;
    /* Pixels can finish before conversion reaches EOF. Restore its optional
     * input gate after window_quiesce, so deferring admission cannot strand
     * the converter that the new document would otherwise cancel. */
    view_state_t *contexts[9]; unsigned count = window_contexts(current, contexts);
    if (parked_state) contexts[count++] = parked_state;
    if (pagination.counter) contexts[count++] = pagination.counter;
    for (unsigned i = 0; i < count; ++i) if (contexts[i]->document == pending) {
        view_input_set(contexts[i], true);
        break;
    }
    return false;
}
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
static unsigned restore_view_flags(const view_state_t *owner)
{
    return (owner->initialized ? 1u : 0u) | (owner->target_pending ? 2u : 0u) |
        (owner->input_enabled ? 4u : 0u) | (owner->status.ready ? 8u : 0u) |
        (owner->failure ? 16u : 0u);
}
#endif
static bool window_prepare(view_state_t *current, bool pixels)
{
    int first = current->direction < 0 ? -1 : 1;
    view_state_t *visited[6]; unsigned visited_count = 0;
    for (unsigned side = 0; side < 2; ++side) {
        int direction = side ? -first : first;
        window_route_t route = window_route(direction);
        for (unsigned i = 0; i < route.count; ++i) {
            view_state_t *owner = route.pages[i].owner;
            if (owner == current || (!pixels && (owner->display_pending || owner->rendering))) continue;
            bool seen = false;
            for (unsigned j = 0; j < visited_count; ++j) if (visited[j] == owner) seen = true;
            if (seen) continue;
            visited[visited_count++] = owner;
            if (window_runnable(owner, current)) {
                state = owner; view.intent = current->intent; view.background_paused = false;
                if (pixels && view.status.ready && !view.display_pending && !view.rendering && !frame_pixels_ready(frame_find(view.display->start))) {
                    page_reset(view.building, view.display->start);
                    epd_reader_page_copy((epd_reader_page_t *)view.building, (epd_reader_page_t *)view.display);
                    view.display_pending = true;
                }
                view.direction = direction;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
                unsigned restore_before = restore_view_flags(owner);
#endif
                bool advanced = neighbor_step(pixels);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
                if (direction < 0 && window_extra(owner))
                    document_trace_restore(owner->document, DOC_RESTORE_STEP, owner,
                        restore_before, restore_view_flags(owner), advanced);
#endif
                state = current;
                if (advanced) return true;
            }
        }
        if (pixels && route.missing_hop) {
            bool conversion_ready = window_conversion_ready(current);
            bool direct = route.missing_hop == direction;
            /* The regular neighbor scheduler handles direct opens. While a
             * converter is occupied, reuse any completed chapter on the route. */
            if (direct && conversion_ready) continue;
            view_state_t *parent = route.predecessor;
            /* A published boundary can precede the document's final EOF. */
            document_progress_t progress; document_progress(parent->document, &progress);
            if (!progress.complete || progress.result || parent->rendering) continue;
            unsigned slot = direction > 0;
            unsigned bit = 1u << slot;
            epd_memory_recovery_t *recovery = &parent->window_recovery[slot];
            if (parent->window_attempt_intent != current->intent) {
                parent->window_attempt_intent = current->intent; parent->window_attempts = 0;
                memset(parent->window_recovery, 0, sizeof(parent->window_recovery));
            }
            size_t epoch = epd_memory_capacity_epoch();
            if (epoch != parent->window_attempt_epoch) {
                parent->window_attempt_epoch = epoch;
                parent->window_attempts = 0;
            }
            if (!conversion_ready) {
                uint32_t revision = document_worker_revision();
                if (parent->window_probe_revision != revision) {
                    parent->window_probe_revision = revision;
                    parent->window_attempts &= 3u;
                }
                bit <<= 2;
            }
            bool recovering = recovery->used && recovery->state == EPD_MEMORY_RECOVERY_PENDING;
            if ((parent->window_attempts & bit) && !recovering) continue;
            parent->window_attempts |= bit;
            document_t *document = NULL;
            if (!conversion_ready) {
                document = document_adjacent_ready(parent->document, direction);
                if (!document) continue;
            }
            size_t minimum = sizeof(view_state_t) + 4u * sizeof(view_page_t) + sizeof(epd_reader_batch_t) + 256u * sizeof(document_cursor_t) + 512u;
            unsigned priority = route.count + 1 + (side ? 3u : 0u);
            state = parent;
            bool admitted = !recovering && window_admit(minimum, priority);
            if (!admitted && layout_bytes <= VIEW_CACHE_BUDGET - minimum &&
                (recovering || !recovery->used)) {
                epd_memory_recovery_result_t result = view_memory_recover(recovery,
                    epd_app_allocation_peak(minimum), false);
                if (result == EPD_MEMORY_RECOVERY_READY) admitted = window_admit(minimum, priority);
            }
            state = current;
            if (!admitted) { if (document) document_close(document); continue; }
            memset(recovery, 0, sizeof(*recovery));
            state = parent;
            if (!document) document = document_adjacent_prefetch(parent->document, direction);
            view_state_t *owner = state_neighbor(document, direction < 0);
            state = current;
            if (!owner) continue;
            if (direct) {
                if (direction < 0) parent->previous = owner;
                else parent->following = owner;
            } else if (!window_store(owner, route.missing_hop)) { state_retire(owner); continue; }
            return true;
        }
    }
    return false;
}

static bool window_reclaim(unsigned priority)
{
    unsigned victim = 4, worst = frame_retention_distance(priority);
    for (unsigned i = 0; i < 4; ++i) {
        view_state_t *owner = window_slots[i].owner;
        if (!owner || owner == state || owner->selected || owner->displayed) continue;
        unsigned score = UINT_MAX;
        for (int direction = -1; direction <= 1; direction += 2) {
            window_route_t route = window_route_limit(direction, VIEW_NEAR_PAGES);
            for (unsigned p = 0; p < route.count; ++p)
                if (route.pages[p].owner == owner && route.pages[p].distance < score)
                    score = route.pages[p].distance;
        }
        if (score > worst) { victim = i; worst = score; }
    }
    if (victim == 4) return false;
    view_state_t *owner = window_slots[victim].owner;
    for (view_frame_t *frame = owner->frames; frame; frame = frame->next) {
        if (frame->references != 1) continue;
        frame_reclaim_note(frame, state, window_number(state), priority, "context"); break;
    }
    window_slots[victim].owner = NULL; window_slots[victim].hop = 0;
    state_retire(owner);
    return true;
}
static bool window_admit(size_t bytes, unsigned priority)
{
    if (bytes > VIEW_CACHE_BUDGET) return false;
    view_state_t *contexts[7]; unsigned count = window_contexts(primary_state, contexts);
    for (unsigned i = 0; i < count && layout_bytes > VIEW_CACHE_BUDGET - bytes; ++i) {
        reader_page_cache_t *cache = &contexts[i]->cache;
        reader_cached_page_t **at = &cache->first;
        while (*at && layout_bytes > VIEW_CACHE_BUDGET - bytes) {
            reader_cached_page_t *page = *at;
            if (frame_retention_distance(frame_priority(contexts[i], page->number)) <= frame_retention_distance(priority)) { at = &page->next; continue; }
            reader_page_cache_remove(cache, page); cache->window_dirty = true;
        }
    }
    for (unsigned n = 0; n < 4 && layout_bytes > VIEW_CACHE_BUDGET - bytes; ++n)
        if (!window_reclaim(priority)) break;
    return layout_bytes <= VIEW_CACHE_BUDGET - bytes && frame_reclaim_layout(bytes, priority);
}
/* The resource executor only sends an event. rdview owns these mutable lists. */
static int view_resource_shrink(void *context, uint32_t backend, size_t requested, size_t *released)
{
    (void)context; *released = 0;
    rt_enter_critical();
    if (!mailbox.thread || mailbox.stop || view_resource_pending) {
        rt_exit_critical(); return EPD_RESOURCE_STOPPED;
    }
    view_resource_pending = true; view_resource_requested = requested; view_resource_backend = backend;
    rt_exit_critical(); wake_view(); return EPD_RESOURCE_PENDING;
}
static size_t view_resource_bytes(void)
{
    rt_enter_critical();
    size_t bytes = layout_bytes + image_bytes + frame_bytes + index_bytes;
    if (frame_spare_extra) bytes += sizeof(*frame_spare_extra);
    rt_exit_critical();
    return bytes;
}
static void view_resource_publish(void)
{
    if (!view_resource.provider && mailbox.thread && !mailbox.stop)
        epd_resource_cache_open(&view_resource, EPD_RESOURCE_WORKER, view_resource_shrink, NULL);
    size_t reclaimable = 0;
    rt_enter_critical(); unsigned retained = mailbox.suspended ? 0u : 3u; rt_exit_critical();
    if (primary_state && rt_thread_self() == mailbox.thread) {
        view_state_t *contexts[7]; unsigned count = window_contexts(primary_state, contexts);
        view_state_t *saved = state;
        for (unsigned i = 0; i < count; ++i) {
            view_state_t *owner = contexts[i];
            state = owner;
            for (reader_cached_page_t *p = owner->cache.first; p; p = p->next)
                if (frame_retention_distance(frame_priority(owner, p->number)) > 3) reclaimable += p->allocation;
            for (view_frame_t *f = owner->frames; f; f = f->next) {
                rt_enter_critical(); bool free = f->references == 1 && !f->renderer && !f->backing; rt_exit_critical();
                if (free && frame_retention_distance(frame_distance(f)) > retained)
                    reclaimable += f->window_charge + f->pixels.capacity + f->cold.charge;
            }
            for (view_image_t *image = owner->images; image; image = image->next)
                if (image->references == 1 && !image->request &&
                    !(owner->display_pending && page_uses_image(owner->building, image)))
                    reclaimable += document_picture_charge(image->bytes);
        }
        state = saved;
    }
    reclaimable += frame_spare_reclaimable();
    size_t total = view_resource_bytes();
    if (reclaimable > total) reclaimable = total;
    epd_resource_cache_update_region(&view_resource, EPD_MEMORY_REGION_HCPU_PSRAM, total, reclaimable, 4, 0);
}
static size_t view_resource_released(size_t before)
{
    size_t now = view_resource_bytes();
    return before > now ? before - now : 0;
}

static void view_resource_step(void)
{
    rt_enter_critical(); bool pending = view_resource_pending;
    unsigned retained = mailbox.suspended ? 0u : 3u;
    size_t requested = view_resource_requested; uint32_t backend = view_resource_backend; rt_exit_critical();
    if (!pending) return;
    size_t before = view_resource_bytes();
    if (primary_state) {
        view_state_t *saved = state; state = primary_state;
        while (view_resource_released(before) < requested && frame_reclaim_layout(0, retained)) {}
        while (view_resource_released(before) < requested && frame_reclaim_pages(0)) {}
        if (view_resource_released(before) < requested) {
            view_state_t *contexts[7]; unsigned count = window_contexts(primary_state, contexts);
            for (unsigned i = 0; i < count && view_resource_released(before) < requested; ++i) {
                state = contexts[i]; images_prune(true);
            }
        }
        state = saved;
    }
    size_t physical_extra = 0;
    size_t released = view_resource_released(before);
    while (released < requested && frame_spare_reclaim(requested - released, &physical_extra))
        released = view_resource_released(before) + physical_extra;
    view_resource_publish();
    rt_enter_critical(); view_resource_pending = false; rt_exit_critical();
    epd_resource_provider_cache_complete(view_resource.provider, backend, released, EPD_RESOURCE_OK);
}

static void state_reset_layout(document_cursor_t anchor)
{
    visited_clear(state);
    frame_cache_clear();
    view.frame_published = false;
    compute_cancel();
    view.failure = READER_ERROR_NONE; view.status.error_code = READER_ERROR_NONE;
    view.count = view.toc_count = view.toc_scanned = 0;
    view.sparse_stride = view.sparse_pages = 0; view.sparse_seek = view.toc_limited = false;
    view.count_loaded = view.count_saved = view.numeric_target = false;
    view.complete = view.index_loaded = view.index_loading = view.index_saved = view.from_cache = false;
    view.index_saved_count = 0; view.index_save_failed = false;
    link_clear();
    view.display_pending = view.notified = false;
    view.status.pages = 0; view.status.indexing = true;
    page_reset(view.index, (document_cursor_t){0, 0});
    preview_start(anchor);
    images_cancel_pending(); images_prune(true);
    trace_start();
}

static void state_reflow(view_state_t *configuration)
{
    for (unsigned i = 0; i < 3; ++i) {
        lv_font_t *normal = view.fonts[i], *italic = view.oblique[i], *fallback = view.fallbacks[i];
        view.fonts[i] = configuration->fonts[i]; view.oblique[i] = configuration->oblique[i];
        view.fallbacks[i] = configuration->fallbacks[i];
        configuration->fonts[i] = normal; configuration->oblique[i] = italic; configuration->fallbacks[i] = fallback;
    }
    view.font_id = configuration->font_id; view.font_revision = configuration->font_revision;
    view.font_identity = configuration->font_identity;
    view.font_area = configuration->font_area;
    memcpy(view.settings, configuration->settings, sizeof(view.settings));
    view.appearance = configuration->appearance;
    view.layout = configuration->layout; view.font_metrics_ready = false;
    view.status.margin = configuration->status.margin; view.status.content_width = configuration->status.content_width;
    view.status.top = configuration->status.top; view.status.bottom = configuration->status.bottom;
    view.status.line_space = configuration->status.line_space;
    state_reset_layout(view.status.ready ? view.display->start : view.anchor);
    rt_enter_critical(); configuration->next = mailbox.retired; mailbox.retired = configuration; rt_exit_critical();
}
static view_state_t *state_neighbor(document_t *document, bool backward)
{
    if (!document) return NULL;
    view_state_t *neighbor = view_allocate(sizeof(*neighbor), EPD_APP_PSRAM);
    if (!neighbor) { document_close(document); return NULL; }
    memset(neighbor, 0, sizeof(*neighbor));
    neighbor->document = document; neighbor->session = view.session; neighbor->intent = view.intent;
    neighbor->layout = view.layout; neighbor->appearance = view.appearance;
    neighbor->font_id = view.font_id; neighbor->font_revision = view.font_revision;
    neighbor->font_identity = view.font_identity;
    neighbor->font_area = view.font_area; neighbor->font_metrics_ready = view.font_metrics_ready;
    neighbor->backward_target = backward;
    neighbor->status = view.status; neighbor->status.ready = false; neighbor->status.page = neighbor->status.pages = 0;
    neighbor->status.saved = false; neighbor->status.error_code = READER_ERROR_NONE;
    memcpy(neighbor->settings, view.settings, sizeof(view.settings));
    memcpy(neighbor->metrics, view.metrics, sizeof(view.metrics));
    memcpy(neighbor->italic_metrics, view.italic_metrics, sizeof(view.italic_metrics));
    snprintf(neighbor->title, sizeof(neighbor->title), "%s", view.title);
    for (unsigned i = 0; i < 3; ++i) {
        if (!epd_app_font_ref(view.fonts[i])) { state_dispose(neighbor); return NULL; }
        neighbor->fonts[i] = view.fonts[i];
        if (!epd_app_font_ref(view.oblique[i])) { state_dispose(neighbor); return NULL; }
        neighbor->oblique[i] = view.oblique[i];
        if (view.fallbacks[i]) {
            if (!epd_app_font_ref(view.fallbacks[i])) { state_dispose(neighbor); return NULL; }
            neighbor->fallbacks[i] = view.fallbacks[i];
        }
    }
    return neighbor;
}
static void pagination_drop_counter(void)
{
    state_retire(pagination.counter);
    pagination.counter = NULL;
}
static void pagination_reset(void)
{
    pagination_drop_counter();
    index_bytes -= pagination.bytes;
    epd_app_free(pagination.ends);
    memset(&pagination, 0, sizeof(pagination));
}
static unsigned pagination_chapter(const view_state_t *context)
{ return context->progress.chapters ? context->progress.chapter : 0; }
static bool pagination_matches(const view_state_t *context, unsigned chapter)
{
    return context && context->layout == pagination.layout && context->complete && !context->failure &&
        context->progress.complete && !context->progress.result && (context->whole_source || !context->progress.source_begin) &&
        pagination_chapter(context) == chapter &&
        (context->progress.chapters || !context->progress.source_total ||
         context->progress.source_next >= context->progress.source_total);
}
static bool pagination_add(unsigned pages)
{
    if (pages > UINT_MAX - pagination.total) { pagination.failed = true; return false; }
    pagination.total += pages;
    pagination.ends[pagination.next] = pagination.total;
    pagination.complete = ++pagination.next == pagination.chapters;
    if (pagination.complete && primary_state)
        document_page_summary_save(primary_state->document, pagination.layout, pagination.ends, pagination.chapters);
    return true;
}
static void pagination_snapshot(view_state_t *context, reader_view_t *info)
{
    bool version = pagination.session == context->session && pagination.layout == context->layout;
    info->pages = version && pagination.complete ? pagination.total : 0;
    info->indexing = !version || (!pagination.complete && !pagination.failed);
    info->page = 0;
    unsigned chapter = pagination_chapter(context);
    if (!version || chapter >= pagination.next || !indexed_pages(context) ||
        (context->progress.source_begin && !context->whole_source)) return;
    /* A cold preview has no accurate page number until the index reaches it. */
    document_cursor_t cursor = context->display_pending ? context->building->start : context->display->start;
    if (context->sparse_stride) {
        if (context->numbered_page && cursor_equal(cursor, context->numbered_cursor))
            info->page = (chapter ? pagination.ends[chapter - 1] : 0) + context->numbered_page;
        return;
    }
    if (!context->complete && !cursor_before(cursor, context->pages[context->count])) return;
    unsigned lo = 0, hi = context->count;
    while (lo + 1 < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (cursor_before(cursor, context->pages[mid])) hi = mid; else lo = mid;
    }
    info->page = (chapter ? pagination.ends[chapter - 1] : 0) + lo + 1;
}
static bool pagination_locate(unsigned page, unsigned *chapter, unsigned *local)
{
    if (!pagination.complete || !page || page > pagination.total) return false;
    unsigned lo = 0, hi = pagination.chapters;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (pagination.ends[mid] < page) lo = mid + 1; else hi = mid;
    }
    *chapter = lo;
    *local = page - (lo ? pagination.ends[lo - 1] : 0);
    return true;
}
static void pagination_prepare(view_state_t *current)
{
    if (!current->font_metrics_ready) return;
    if (pagination.session != current->session || pagination.layout != current->layout) {
        pagination_reset();
        pagination.session = current->session; pagination.layout = current->layout;
        pagination.chapters = current->progress.chapters ? current->progress.chapters : 1;
        size_t bytes = (size_t)pagination.chapters * sizeof(*pagination.ends);
        if (pagination.chapters > VIEW_INDEX_LIMIT / sizeof(*pagination.ends) ||
            index_bytes + bytes > VIEW_INDEX_GLOBAL) {
            pagination.failed = true;
            return;
        }
        pagination.ends = view_allocate(bytes, EPD_APP_PSRAM);
        if (!pagination.ends) { pagination.failed = true; return; }
        pagination.bytes = bytes; index_bytes += bytes;
        if (document_page_summary_load(current->document, pagination.layout, pagination.ends, pagination.chapters)) {
            pagination.total = pagination.ends[pagination.chapters - 1];
            pagination.next = pagination.chapters;
            pagination.complete = true;
        }
    }
}
static bool pagination_requested(void)
{
    rt_enter_critical();
    bool requested = mailbox.count_requested;
    rt_exit_critical();
    return requested;
}
static void pagination_step(view_state_t *current)
{
    if (!current->font_metrics_ready) return;
    pagination_prepare(current);
    if (!pagination.ends) return;
    if (pagination.complete || pagination.failed) return;
    view_state_t *contexts[3] = {current, current->previous, current->following};
    for (unsigned i = 0; i < 3; ++i) if (pagination_matches(contexts[i], pagination.next)) {
        state = contexts[i]; count_cache_save(); state = current;
        pagination_add(indexed_pages(contexts[i]));
        pagination_drop_counter();
        return;
    }
    if (!pagination.counter) {
        /* Opening a separate document for exact counts is driven by the progress panel. */
        if (!pagination_requested() || !window_conversion_ready(current)) return;
        document_t *document = document_pagination_open(current->document, pagination.next);
        if (!document) { pagination.failed = true; return; }
        pagination.counter = state_neighbor(document, false);
        if (!pagination.counter) { pagination.failed = true; return; }
        pagination.counter->counting_only = true;
        pagination.counter->whole_source = true;
    }
    state = pagination.counter; view.intent = current->intent;
    if (!view.init_attempted) core_initialize();
    if (view.initialized && !view.failure && fonts_ready()) {
        document_progress(view.document, &view.progress);
        view.background_paused = false;
        view_input_set(state, true);
        view.compute_foreground = false; view.index->collect = false;
        bool count_waiting = count_cache_load();
        if (!count_waiting && !view.complete) index_load();
        font_priority(false);
        rt_tick_t started = rt_tick_get();
        while (!view.complete && !view.progress.result && !view.index_loading && !count_waiting) {
            int result = page_step(view.index, true, NULL, &view.progress);
            if (result < 0) { view.failure = READER_ERROR_LAYOUT; break; }
            if (view.index->done) {
                if (view.index->count || !view.count) ++view.count;
                view.complete = view.index->eof;
                document_cursor_t next = view.index->cursor;
                page_reset(view.index, next); view.index->collect = false;
            }
            if (!result || elapsed_ms(started) >= 2) break;
        }
    }
    if (view.failure || view.progress.result) pagination.failed = true;
    if (view.complete) {
        /* The counting document must cover the whole chapter. */
        if (pagination_matches(state, pagination.next)) { count_cache_save(); pagination_add(view.count); }
        else pagination.failed = true;
    }
    state = current;
    if (pagination.complete || pagination.failed || pagination.counter->complete)
        pagination_drop_counter();
}
static void target_resolve(void)
{
    if (!view.initialized || !view.target_pending) return;
    /* Pausing retained contexts may transfer the input gate to the old page. */
    document_flow();
    document_progress(view.document, &view.progress);
    uint32_t record = 0, byte = 0;
    int ready = view.chapter_percent ? document_progress_cursor(view.document, view.chapter_percent,
        &view.progress_request, &record, &byte) :
        document_target_cursor(view.document, &record, &byte);
    /* A reused complete spine retains its original opening target. Reverse
     * entry must start at its end, not before the first body record. */
    if (view.backward_target && !view.boundary_target && !view.link_target[0] &&
        !view.progress.result && view.progress.chapters && !view.progress.source_begin) {
        if (!view.progress.complete) return;
        rt_kprintf("reader back_entry session=%u intent=%u target=%u:%u end=%u\n",
            (unsigned)view.session, (unsigned)view.intent, (unsigned)record, (unsigned)byte,
            (unsigned)view.progress.end);
        record = view.progress.end; byte = 0;
        ready = 1;
    }
    if (view.boundary_target) {
        record = view.boundary_anchor.record; byte = view.boundary_anchor.byte; ready = 1;
        view.boundary_target = false;
    }
    if (view.link_target[0]) {
        /* Link anchors are published as the target chapter streams in. Keep
         * required input moving even when optional prefetch is paused. */
        int complete = document_target_require(view.document, true);
        if (document_anchor_find(view.document, view.link_target, &record)) { ready = 1; byte = 0; }
        else ready = complete != 0 || ready < 0 ? -1 : 0;
    }
    if (!ready) return;
    if (ready < 0) { fail(view.link_target[0] ? READER_ERROR_LINK_MISSING : READER_ERROR_PAGE_READ); return; }
    link_clear();
    view.target_pending = false;
    view.initial_target_ready = true; view.initial_anchor = (document_cursor_t){record, byte};
    rt_kprintf("reader anchor session=%u intent=%u record=%u byte=%u tick=%u\n",
        (unsigned)view.session, (unsigned)view.intent, (unsigned)record, (unsigned)byte, (unsigned)rt_tick_get());
    if (view.saved_position.page > 1) {
        unsigned page = view.saved_position.page;
        view.saved_position.page = 0;
        core_seek(page);
        document_flow();
        return;
    }
    bool cached_entry = cursor_equal(view.backward_target ? view.display->cursor : view.display->start, view.initial_anchor);
    if (!view.backward_target && !record && !byte && view.status.ready)
        cached_entry |= cursor_at_segment_start(view.display->start);
    if (view.backward_target && record == view.progress.end && !byte && view.display->eof)
        cached_entry = true;
    if (view.preview_cache_active && view.status.ready && view.display_layout == view.layout && cached_entry) {
        /* A cached segment entry already has the required boundaries. Keep
         * its neighboring pages instead of clearing and rebuilding them. */
        compute_cancel();
        view.preview = view.restoring = true; view.anchor = view.display->start;
        view.turn_pending = view.turn_remaining = 0; view.forward_step = false;
        view.preview_clipped = !view.display->eof; view.preview_end = view.display->cursor;
        page_reset(view.building, view.anchor);
        epd_reader_page_copy((epd_reader_page_t *)view.building, (epd_reader_page_t *)view.display);
        target_finish();
        document_flow();
        return;
    }
    preview_start(view.initial_anchor);
    if (view.backward_target && (record || byte)) {
        view.turn_pending = -1;
    }
    document_flow();
    if (!view.backward_target && view.status.ready && cursor_equal(view.display->start, view.anchor)) {
        epd_reader_page_copy((epd_reader_page_t *)view.building, (epd_reader_page_t *)view.display);
        target_finish();
    }
}
static view_state_t *chapter_select(view_state_t *current, int direction, document_t *explicit_document)
{
    uint32_t intent = current->intent;
    bool paused = current->background_paused;
    view_state_t *neighbor = explicit_document ? NULL : direction < 0 ? current->previous : current->following;
    if (neighbor) {
        /* Promote the selected cached document as well as a newly opened one. */
        document_t *promoted = document_adjacent(current->document, direction);
        if (!promoted) return current;
        if (promoted == neighbor->document) document_close(promoted);
        else if (promoted) {
            if (direction < 0) current->previous = NULL; else current->following = NULL;
            window_clear(); state_retire(neighbor);
            neighbor = state_neighbor(promoted, direction < 0);
            if (!neighbor) return current;
        }
    }
    if (!neighbor) {
        document_t *document = explicit_document ? explicit_document : document_adjacent(current->document, direction);
        neighbor = state_neighbor(document, direction < 0 && !explicit_document);
        if (!neighbor) return current;
    }
    document_progress_t origin_progress, neighbor_progress;
    document_progress(current->document, &origin_progress);
    document_progress(neighbor->document, &neighbor_progress);
    bool boundary_entry = !explicit_document && origin_progress.chapters &&
        neighbor_progress.chapters && origin_progress.chapter != neighbor_progress.chapter &&
        neighbor_progress.complete && !neighbor_progress.result && !neighbor_progress.source_begin;
    frame_release(current->rendering); current->rendering = NULL;
    current->selected = false; neighbor->selected = true;
    if (current->document != neighbor->document)
        document_set_foreground(current->document, false);
    document_set_foreground(neighbor->document, true);
    if (explicit_document) {
        cluster_retire(current);
    } else window_shift(current, neighbor, direction);
    state = primary_state = neighbor;
    view.intent = intent; view.background_paused = paused; view.frame_published = false;
    if (!view.init_attempted) core_initialize();
    if (boundary_entry) {
        view.progress = neighbor_progress;
        view.boundary_target = view.target_pending = true; view.backward_target = direction < 0;
        view.boundary_anchor = (document_cursor_t){direction < 0 ? neighbor_progress.end : 0, 0};
        view.display_pending = view.notified = false;
    } else if (view.status.ready) {
        page_reset(view.building, view.display->start);
        epd_reader_page_copy((epd_reader_page_t *)view.building, (epd_reader_page_t *)view.display);
        view.display_pending = true; view.notified = false;
    }
    return neighbor;
}
static void pagination_seek(unsigned page, view_state_t **current)
{
    unsigned chapter, local;
    if (pagination.session != view.session || pagination.layout != view.layout ||
        !pagination_locate(page, &chapter, &local)) return;
    if (pagination_chapter(state) != chapter || (view.progress.source_begin && !view.whole_source) ||
        (!view.progress.chapters && view.progress.source_total && view.progress.source_next < view.progress.source_total)) {
        document_t *document = document_pagination_open(view.document, chapter);
        if (!document) { fail(READER_ERROR_SESSION_MEMORY); return; }
        view_state_t *next = chapter_select(*current, 1, document);
        if (next == *current) { fail(READER_ERROR_SESSION_MEMORY); return; }
        *current = state = primary_state = next;
        view.whole_source = true;
    }
    view.boundary_target = view.target_pending = false;
    view.status.pages = chapter ? pagination.ends[chapter] - pagination.ends[chapter - 1] : pagination.ends[0];
    if (!view.initialized) {
        view.saved_position.page = local;
        view.target_pending = true;
        return;
    }
    core_seek(local);
}
static bool neighbor_pending(view_state_t *neighbor)
{
    if (!neighbor) return false;
    uint32_t intent = neighbor->intent, current_intent = view.intent;
    view_state_t *saved = state; state = neighbor;
    view.intent = current_intent;
    bool pending = window_work_allowed() && (!view.status.ready || view.target_pending || near_frames_pending());
    view.intent = intent;
    state = saved;
    return pending;
}
static bool neighbor_index_pending(view_state_t *neighbor)
{
    if (!neighbor || !neighbor->initialized || !index_work_needed(neighbor)) return false;
    uint32_t intent = neighbor->intent, current_intent = view.intent;
    view_state_t *saved = state; state = neighbor; view.intent = current_intent;
    bool pending = window_work_allowed() && view.index && page_work_allowed(view.index);
    view.intent = intent; state = saved;
    return pending;
}
static view_state_t *neighbor_choose(view_state_t *current, unsigned turn)
{
    view_state_t *first = current->direction < 0 ? current->previous : current->following;
    view_state_t *second = current->direction < 0 ? current->following : current->previous;
    if (neighbor_pending(first)) return first;
    if (neighbor_pending(second)) return second;
    if (!(turn & 1)) { view_state_t *swap = first; first = second; second = swap; }
    if (neighbor_index_pending(first)) return first;
    return neighbor_index_pending(second) ? second : NULL;
}
static void neighbors_prepare(view_state_t *current)
{
    /* Prepare one optional neighbor after the current conversion completes. */
    if (!view.progress.complete || view.progress.result) return;
    if (!window_conversion_ready(current)) return;
    uint32_t epoch = epd_memory_capacity_epoch();
    if (view.neighbor_intent != view.intent || view.neighbor_publication != view.progress.publication ||
        epoch != view.neighbor_epoch) {
        view.neighbor_attempts = 0;
        view.neighbor_intent = view.intent;
        view.neighbor_publication = view.progress.publication;
        view.neighbor_epoch = epoch;
    }
    view_state_t **first = view.direction < 0 ? &current->previous : &current->following;
    view_state_t **second = view.direction < 0 ? &current->following : &current->previous;
    int direction = view.direction < 0 ? -1 : 1;
    unsigned first_bit = direction < 0 ? 1u : 2u, second_bit = direction < 0 ? 2u : 1u;
    window_route_t route = window_route(direction);
    /* A chapter outside the near-page window must not take the converter
     * from a chapter needed on the other side. */
    if (route.missing_hop == direction && !*first && !(view.neighbor_attempts & first_bit)) {
        view.neighbor_attempts |= first_bit;
        view.neighbor_epoch = epoch;
        *first = state_neighbor(document_adjacent_prefetch(view.document, direction), direction < 0);
        if (*first) return;
    }
    /* Opening the other side may transfer conversion ownership. Finish the
     * admitted direction's pixels before starting that optional input. */
    if (route.count && route.pages[route.count - 1].owner != current && neighbor_pending(*first)) return;
    if (window_route(-direction).missing_hop == -direction &&
        !*second && !(view.neighbor_attempts & second_bit)) {
        view.neighbor_attempts |= second_bit;
        view.neighbor_epoch = epoch;
        *second = state_neighbor(document_adjacent_prefetch(view.document, -direction), direction > 0);
    }
}
static void window_service(view_state_t *current, unsigned *turn)
{
    pagination_prepare(current);
    bool missing_neighbor = window_route(-1).missing_hop || window_route(1).missing_hop;
    if (pagination.counter && (missing_neighbor || !pagination_requested())) {
        /* Give the conversion slot to the visible page window. Unfinished
         * counting can resume from the last saved chapter afterwards. */
        pagination_drop_counter();
    }
    if (window_prepare(current, true)) {
        /* The last prepared page may expose another chapter boundary. */
        current->work_exhausted = true;
        window_conversion_ready(current);
        return;
    }
    /* The converter's gate must be the last optional-flow decision this round.
     * A neighbor step or a fresh pagination source could otherwise undo it. */
    if (!window_conversion_ready(current)) return;
    neighbors_prepare(current);
    view_state_t *neighbor = neighbor_choose(current, ++*turn);
    if (neighbor) {
        state = neighbor; view.intent = current->intent; view.background_paused = false;
        neighbor_step(true);
        state = current;
    }
    if (!missing_neighbor && !window_pixels_pending(current) && view.complete &&
        !neighbor_pending(current->previous) && !neighbor_index_pending(current->previous) &&
        !neighbor_pending(current->following) && !neighbor_index_pending(current->following)) {
        bool extra_pending = false;
        for (unsigned i = 0; i < sizeof(window_slots) / sizeof(window_slots[0]); ++i)
            if (window_runnable(window_slots[i].owner, current)) { extra_pending = true; break; }
        if (!extra_pending) pagination_step(current);
    }
    /* Completed or admission-blocked neighbor work must not leave NAV behind
     * its old input gate. Active pixels and unfinished BODY input keep priority. */
    if (!render_frame && (!neighbor || !neighbor->rendering) &&
        window_conversion_ready(current)) document_flow();
}
static bool cursor_at_segment_start(document_cursor_t cursor)
{
    if (!cursor.record && !cursor.byte) return true;
    document_anchor_t anchor;
    return document_source_position(view.document, cursor.record, cursor.byte, &anchor) &&
        anchor.offset <= view.progress.source_begin && !anchor.skip_utf8;
}
static void turn_continue(view_state_t **current)
{
    int remaining = view.segment_turns;
    if (!remaining || view.failure) return;
    int direction = remaining < 0 ? -1 : 1;
    if (direction > 0 && !view.progress.complete) return;
    bool adjacent = direction < 0 ? view.progress.source_begin ||
        (view.progress.chapters && view.progress.chapter) : view.progress.chapters ?
        view.progress.chapter + 1 < view.progress.chapters : view.progress.source_next < view.progress.source_total;
    view.segment_turns = 0;
    if (!adjacent) {
        if (!view.building->count && !cursor_equal(view.building->start, view.anchor)) {
            page_reset(view.building, view.anchor);
            if (cursor_equal(view.display->start, view.anchor)) {
                epd_reader_page_copy((epd_reader_page_t *)view.building, (epd_reader_page_t *)view.display);
                target_finish();
            }
        } else { view.display_pending = true; view.notified = false; }
        return;
    }
    view_state_t *next = chapter_select(*current, direction, NULL);
    if (next == *current) { fail(READER_ERROR_SESSION_MEMORY); return; }
    *current = state = primary_state = next;
    /* Re-enter at the adjacent document's source target, even when its last
     * displayed page was in the middle. Crossing consumes exactly one turn. */
    compute_cancel();
    frame_release(view.rendering); view.rendering = NULL;
    view.direction = direction; view.backward_target = direction < 0;
    view.target_pending = true; view.segment_turns = 0;
    view.display_pending = view.notified = view.frame_published = false;
    view.deferred_turns = remaining - direction;
    link_clear();
}
static void target_select(document_t *document, const char *target, view_state_t **current)
{
    if (!document) { fail(READER_ERROR_LINK_MISSING); return; }
    view_state_t *next = chapter_select(*current, 1, document);
    if (next == *current) { fail(READER_ERROR_SESSION_MEMORY); return; }
    *current = state = primary_state = next;
    compute_cancel();
    frame_release(view.rendering); view.rendering = NULL;
    snprintf(view.link_target, sizeof(view.link_target), "%s", target);
    view.target_pending = true; view.backward_target = false;
    view.boundary_target = false;
    view.segment_turns = view.deferred_turns = 0;
    view.forward_step = false;
    view.display_pending = view.notified = view.frame_published = false;
}
static void progress_seek(unsigned percent, view_state_t **current)
{
    unsigned chapter_percent;
    document_t *document = document_progress_open(view.document, percent, &chapter_percent);
    if (!document) { fail(READER_ERROR_PAGE_READ); return; }
    view_state_t *previous = *current;
    target_select(document, "", current);
    if (*current == previous) return;
    view.chapter_percent = chapter_percent;
    view.backward_target = percent == 100;
}
static void follow_link(uint32_t record, view_state_t **current)
{
    char target[DOCUMENT_TEXT_LIMIT + 1], error[96] = {0};
    if (!record || !document_link_target(view.document, record - 1, target, sizeof(target))) {
        fail(READER_ERROR_LINK_MISSING); return;
    }
    if (strstr(target, "://") || !strncmp(target, "mailto:", 7)) return;
    document_t *document = document_link_open(view.document, record - 1, error, sizeof(error));
    if (!document && document_navigation_request(view.document) == 0) {
        view.navigation_link = record; return;
    }
    navigation_clear();
    target_select(document, target, current);
}
static void follow_toc(unsigned index, view_state_t **current)
{
    char target[DOCUMENT_TEXT_LIMIT + 1], error[96] = {0};
    if (!document_toc_entry(view.document, index, NULL, 0, target, sizeof(target), NULL)) {
        if (document_navigation_request(view.document) == 0 && index < UINT_MAX) {
            view.navigation_toc = index + 1; return;
        }
        navigation_clear(); fail(READER_ERROR_LINK_MISSING); return;
    }
    navigation_clear();
    uint32_t record;
    if (document_anchor_find(view.document, target, &record)) {
        document_cursor_t cursor = {record, 0};
        if (!view.status.ready || view.display_layout != view.layout ||
            !cursor_equal(view.display->start, cursor))
            core_seek_cursor(cursor);
        return;
    }
    document_t *document = document_toc_open(view.document, index, error, sizeof(error));
    target_select(document, target, current);
}
static void navigation_resolve(view_state_t **current)
{
    if (view.navigation_link || view.navigation_toc) document_prefetch_enable(view.document, false);
    if (view.navigation_link) follow_link(view.navigation_link, current);
    else if (view.navigation_toc) follow_toc(view.navigation_toc - 1, current);
}
static bool frame_turn_cached(const view_frame_t *frame, const view_command_t *command)
{
    if (!frame_turn_candidate(frame, command) || frame->owner != state) return false;
    const view_frame_t *origin = command->origin;
    unsigned number = command->value > 0 ? origin->number + 1 : origin->number - 1;
    reader_cached_page_t *page = reader_page_cache_find(&view.cache, number, view.layout);
    if (!page || (command->value > 0 ? origin->page.eof : page->eof) ||
        !cursor_equal(command->value > 0 ?
            (document_cursor_t){origin->page.cursor.record, origin->page.cursor.byte} :
            (document_cursor_t){origin->page.start.record, origin->page.start.byte},
            command->value > 0 ? (document_cursor_t){page->start.record, page->start.byte} :
                                (document_cursor_t){page->end.record, page->end.byte})) return false;
    view_frame_t *target = frame_find((document_cursor_t){page->start.record, page->start.byte});
    /* Keep the single render slot only when the new foreground has pixels. */
    return target && target->complete && target->pixels.data && !target->cold.first && target->number == number &&
        cursor_equal((document_cursor_t){target->page.cursor.record, target->page.cursor.byte},
                     (document_cursor_t){page->end.record, page->end.byte}) && target->page.eof == page->eof &&
        frame_window_contains(frame, &target->page, number);
}
static bool command_restores_origin(const view_command_t *command, const view_state_t *current)
{
    const view_state_t *origin = command->origin ? command->origin->owner : NULL;
    return current && command->kind != INTENT_NONE && command->session == current->session &&
        origin && origin != current && origin->session == current->session &&
        origin->initialized && origin->displayed;
}
static void command_apply(view_command_t command, view_state_t **current)
{
    if (!state || command.session != view.session || command.kind == INTENT_NONE) return;
    bool restore_origin = command_restores_origin(&command, state);
    if (!view.initialized && !restore_origin) return;
    rt_kprintf("reader target session=%u intent=%u kind=%u tick=%u\n",
        (unsigned)command.session, (unsigned)command.serial, (unsigned)command.kind, (unsigned)rt_tick_get());
    link_clear();
    /* UI commands are relative to the page still bound on screen. A prepared
     * chapter must not become the origin of a newer cumulative turn. */
    if (restore_origin) {
        view_state_t *origin = command.origin->owner;
        if (view.document != origin->document) document_set_foreground(view.document, false);
        document_set_foreground(origin->document, true);
        if ((*current)->previous == origin) (*current)->previous = NULL;
        if ((*current)->following == origin) (*current)->following = NULL;
        cluster_retire(*current);
        origin->previous = origin->following = NULL;
        *current = state = origin; origin->parked = false; parked_state = NULL;
        origin->selected = true;
    }
    view.intent = command.serial;
    navigation_clear();
    view.frame_published = false;
    view_state_t *contexts[7]; unsigned count = window_contexts(*current, contexts);
    for (unsigned i = 0; i < count; ++i) {
        view_state_t *owner = contexts[i];
        if (owner->appearance != command.appearance) {
            view_state_t *saved = state; state = owner;
            frame_cache_clear(); owner->appearance = command.appearance;
            owner->frame_published = false;
            state = saved;
        } else if (owner != state) {
            frame_release(owner->rendering); owner->rendering = NULL;
        }
    }
    if (frame_turn_matches(view.rendering, &command) || frame_turn_cached(view.rendering, &command)) {
        frame_handoff_note(view.rendering, command.serial);
        view.rendering->intent = view.intent;
    }
    else { frame_release(view.rendering); view.rendering = NULL; }
    document_record_cancel(view.back_request); view.back_request = NULL;
    view.segment_turns = view.deferred_turns = 0;
    view.forward_step = false;
    view.numeric_target = false;
    switch (command.kind) {
    case INTENT_SEEK: pagination_seek((unsigned)command.value, current); break;
    case INTENT_PROGRESS: progress_seek((unsigned)command.value, current); break;
    case INTENT_SOURCE: target_select(document_reopen_at(view.document, &command.source), "", current); break;
    case INTENT_CURSOR: view.boundary_target = view.target_pending = false; core_seek_cursor(command.cursor); break;
    case INTENT_RESIZE:
        if (command.origin && command.origin->owner == state) {
            pagination_reset();
            window_clear(); state_retire(view.previous); state_retire(view.following);
            view.previous = view.following = NULL;
            view.boundary_target = view.target_pending = false;
            layout_for_area(state, command.geometry);
            state_reset_layout(command.origin->position.cursor);
        }
        break;
    case INTENT_REPAINT:
        /* The bound frame owns the exact runs even if the worker was ahead. */
        if (command.origin && command.origin->owner == state && command.origin->layout == view.layout) {
            view.boundary_target = view.target_pending = false;
            view.turn_pending = view.turn_remaining = 0;
            view.anchor = command.origin->position.cursor;
            if (view.preview_cache_active) view.preview_page = command.origin->number;
            else view.wanted = command.origin->number;
            page_reset(view.building, view.anchor);
            epd_reader_page_copy((epd_reader_page_t *)view.building, &command.origin->page);
            view.display_pending = true; view.notified = false;
        }
        break;
    case INTENT_REFLOW:
    case INTENT_CANCEL:
        if (view.status.ready) view.boundary_target = view.target_pending = false;
        view.deferred_turns = 0;
        if (!view.status.ready && view.initial_target_ready) core_seek_cursor(view.initial_anchor);
        core_cancel_pending(); break;
    case INTENT_TURN:
        if (!view.status.ready) {
            if (view.initial_target_ready) core_seek_cursor(view.initial_anchor);
            view.deferred_turns = command.value;
            break;
        }
        view.boundary_target = view.target_pending = false;
        core_turn(command.value);
        break;
    case INTENT_CHAPTER: *current = chapter_select(*current, command.value, NULL); break;
    case INTENT_TOC: follow_toc((unsigned)command.value, current); break;
    case INTENT_LINK: follow_link((uint32_t)command.value, current); break;
    default: break;
    }
    /* Reflow restores this origin before replacing its layout. Publishing the
     * old bitmap with the new intent would complete the UI request too early. */
    if (command.kind != INTENT_REFLOW && state && !view.navigation_link && !view.navigation_toc &&
        view.initialized && !core_waiting() && view.status.ready) {
        view_frame_t *visible = frame_find(view.display->start);
        if (visible) frame_publish(visible);
    }
}
static bool render_cleanup(void)
{
    rt_enter_critical(); view_frame_t *frames = mailbox.cleanup; mailbox.cleanup = NULL; rt_exit_critical();
    bool pending = false;
    while (frames) {
        view_frame_t *next = frames->next;
        epd_reader_render_shutdown(&frames->renderer);
        if (frames->renderer) pending = true;
        frames->references = 1; frame_release(frames);
        frames = next;
    }
    return pending;
}
static void font_priority(bool foreground)
{
    unsigned purpose = foreground ? EPD_COMPUTE_FOREGROUND : EPD_COMPUTE_BACKGROUND;
    for (unsigned i = 0; i < 3; ++i) {
        epd_app_font_set_purpose(view.fonts[i], purpose);
        epd_app_font_set_purpose(view.oblique[i], purpose);
    }
}
static void worker_retry_fonts(view_state_t *current)
{
    const lv_font_t *seen[6]; unsigned count = 0;
    for (unsigned i = 0; i < 6; ++i) {
        const lv_font_t *font = i < 3 ? current->fonts[i] : current->oblique[i - 3];
        if (!font) continue;
        unsigned j = 0;
        while (j < count && seen[j] != font) ++j;
        if (j != count) continue;
        seen[count++] = font;
        epd_app_font_retry_memory(font);
    }
}
static bool worker_target_needed(void);
static void worker_context_pause(bool pause_input)
{
    near_trace_finish("pause");
    if (pause_input) view_input_set(state, false);
    frame_release(view.rendering); view.rendering = NULL;
    images_cancel_pending();
    document_record_cancel(view.back_request); view.back_request = NULL;
    view_page_t *pages[3] = {view.index, view.building, view.prefetch};
    for (unsigned p = 0; p < 3; ++p) if (pages[p]) {
        document_record_cancel(pages[p]->read_request); pages[p]->read_request = NULL;
    }
    view_page_t *computing = view.compute_page;
    compute_cancel();
    reader_compute_release(view.compute_spare); view.compute_spare = NULL;
    if (computing) {
        bool collect = computing->collect;
        page_reset(computing, computing->start); computing->collect = collect;
    }
}
static void window_quiesce(view_state_t *current)
{
    if (!current || !current->initialized) return;
    view_state_t *saved = state;
    document_t *draining = window_conversion_document(current);
    bool needed[4] = {false};
    for (int direction = -1; direction <= 1; direction += 2) {
        window_route_t route = window_route(direction);
        for (unsigned p = 0; p < route.count; ++p)
            for (unsigned i = 0; i < 4; ++i)
                if (window_slots[i].owner == route.pages[p].owner) needed[i] = true;
    }
    /* Finished or displaced windows must return their outstanding job slots. */
    for (unsigned i = 0; i < 4; ++i) {
        view_state_t *owner = window_slots[i].owner;
        if (!owner || !owner->initialized || (needed[i] && window_runnable(owner, current))) continue;
        state = owner;
        /* Layout can stop while this BODY converter drains. Leave its input
         * gate for window_conversion_ready instead of toggling it off/on. */
        worker_context_pause(owner->document != draining);
    }
    state = saved;
}
static void worker_quiesce(view_state_t *current, bool paused, bool suspended, bool foreground_only)
{
    view_state_t *saved = state;
    bool urgent = current && worker_target_needed();
    document_t *draining = current && current->initialized && current->status.ready &&
        !paused && !suspended && !urgent ? window_conversion_document(current) : NULL;
    view_state_t *contexts[9]; unsigned count = window_contexts(current, contexts);
    if (parked_state) contexts[count++] = parked_state;
    if (pagination.counter) contexts[count++] = pagination.counter;
    for (unsigned i = 0; i < count; ++i) if (contexts[i]->initialized) {
        state = contexts[i];
        if (!paused && !suspended && state != parked_state && !(state == pagination.counter && urgent)) {
            if (i == 0) core_background_pause(foreground_only);
            continue;
        }
        core_background_pause(true);
        if (!suspended && i == 0 && worker_target_needed()) continue;
        worker_context_pause(!draining || view.document != draining);
    }
    if (paused || suspended) reader_compute_idle();
    state = saved;
}
static bool worker_target_needed(void)
{
    if (request_memory_failed(state)) return false;
    if (view.navigation_link || view.navigation_toc) return !view.failure;
    return core_waiting() && !(view.frame_published && view.published_intent == view.intent && view.display_pending);
}
static void worker_input_commit(view_state_t *current)
{
    view_state_t *contexts[9]; unsigned count = window_contexts(current, contexts);
    view_state_t *decisions[9]; unsigned decision_count = 0;
    if (parked_state) contexts[count++] = parked_state;
    if (pagination.counter) contexts[count++] = pagination.counter;
    /* Layout, pixels and conversion share a document. Publish only its last
     * decision this round; intermediate off/on transitions restart deferral. */
    for (unsigned i = 0; i < count; ++i) {
        unsigned j = 0;
        while (j < i && contexts[j]->document != contexts[i]->document) ++j;
        if (j < i) continue;
        view_state_t *last = NULL;
        for (j = i; j < count; ++j) {
            view_state_t *owner = contexts[j];
            if (owner->document == contexts[i]->document && owner->input_order &&
                (!last || (int32_t)(owner->input_order - last->input_order) > 0)) last = owner;
        }
        if (last) {
            unsigned at = decision_count++;
            while (at && (int32_t)(decisions[at - 1]->input_order - last->input_order) > 0) {
                decisions[at] = decisions[at - 1]; --at;
            }
            decisions[at] = last;
        }
    }
    /* Each call also transfers the document-wide input owner, even when its
     * gate is unchanged. Preserve decision order across different documents. */
    for (unsigned i = 0; i < decision_count; ++i)
        document_prefetch_enable(decisions[i]->document, decisions[i]->input_enabled);
    for (unsigned i = 0; i < count; ++i) contexts[i]->input_order = 0;
}
static bool neighbor_step(bool pixels)
{
    bool initialized = view.initialized, target = view.target_pending;
    if (!view.init_attempted) core_initialize();
    if (!view.initialized) return false;
    target_resolve();
    bool advanced = !initialized || target != view.target_pending;
    if (!view.target_pending) {
        font_priority(false);
        advanced |= core_step(2, true, false) != READER_WORK_IDLE;
        if (pixels) {
            frame_prepare();
            if (view.display_pending) { layout_accept(); advanced = true; }
        }
    }
    return advanced || view.rendering != NULL;
}
static bool near_direction_pending(void)
{
    /* The forward-pixels gate precedes reverse layout creation. Waiting for
     * missing reverse layouts at that gate would wait on its own work. */
    return view.status.ready && window_pixels_pending_for(state, view.direction < 0 ? -1 : 1);
}
static bool near_frames_pending(void)
{
    if (!view.status.ready) return false;
    /* Both directions use the same global three-page routes, including short
     * chapters. A cold hit is cached content, but still needs NEARBY work. */
    return window_pixels_pending(state);
}
static rt_tick_t worker_idle_delay(view_state_t *current)
{
    if (request_memory_failed(current)) {
        rt_enter_critical(); bool cleanup = mailbox.cleanup != NULL; rt_exit_critical();
        if (cleanup) return rt_tick_from_millisecond(20);
        return RT_WAITING_FOREVER;
    }
    /* A consumed slice has more work to drain. Still block for a tick so the
     * document worker can run; pending providers retain the polling fallback. */
    if (current->work_exhausted && !current->failure && !current->background_paused)
        return rt_tick_from_millisecond(1);
    if (worker_target_needed() || render_frame) return rt_tick_from_millisecond(4);
    /* A complete chapter index does not imply that nearby page layouts exist. */
    if (!current->failure && !current->background_paused && near_frames_pending())
        return rt_tick_from_millisecond(4);
    if (preview_ahead_pending()) return rt_tick_from_millisecond(4);
    if (!current->background_paused && !current->failure && pagination.ends &&
        !pagination.complete && !pagination.failed && pagination_requested())
        return rt_tick_from_millisecond(4);
    view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned side = 0; side < 2; ++side) {
            const epd_memory_recovery_t *recovery = &contexts[i]->window_recovery[side];
            if (contexts[i]->window_attempt_intent == current->intent &&
                recovery->used && recovery->state == EPD_MEMORY_RECOVERY_PENDING)
                return rt_tick_from_millisecond(20);
        }
        view_state_t *owner = contexts[i];
        /* A saved index can be complete before the neighboring pixels are. */
        if (owner != current && window_runnable(owner, current)) return rt_tick_from_millisecond(4);
        if (!window_extra(owner) && ((!owner->init_blocked && index_work_needed(owner)) || owner->rendering))
            return rt_tick_from_millisecond(4);
    }
    return RT_WAITING_FOREVER;
}
static void worker_trace_wait(bool cleanup, bool paused, bool suspended)
{
    if (!state || !view.initialized || view.failure || suspended ||
        (view.frame_published && view.published_intent == view.intent)) return;
    rt_enter_critical();
    rt_tick_t requested = mailbox.requested_at;
    bool active = mailbox.session == view.session && mailbox.intent == view.intent;
    rt_exit_critical();
    if (!active) return;
    unsigned elapsed = elapsed_ms(requested);
    unsigned bit = elapsed >= 15000 ? 2u : elapsed >= 5000 ? 1u : 0u;
    if (view.wait_trace_intent != view.intent) {
        view.wait_trace_intent = view.intent; view.wait_trace_mask = 0;
    }
    if (!bit || (view.wait_trace_mask & bit)) return;
    view.wait_trace_mask |= bit;
    view_frame_t *frame = view.rendering;
    rt_kprintf("reader wait session=%u intent=%u ms=%u cleanup=%u paused=%u target=%u turns=%d display=%u notified=%u metrics=%u frame=%u pins=%u pin_run=%u pin_byte=%u resources=%u renderer=%u\n",
        (unsigned)view.session, (unsigned)view.intent, elapsed, cleanup, paused,
        view.target_pending, view.segment_turns, view.display_pending, view.notified,
        view.font_metrics_ready, frame != NULL, frame && frame->pins != NULL,
        frame ? frame->pin_run : 0, frame ? frame->pin_byte : 0,
        frame && frame->resources_ready, frame && frame->renderer != NULL);
    document_progress_t progress;
    document_progress(view.document, &progress);
    uint32_t target_record = 0, target_byte = 0;
    int target_query = document_target_cursor(view.document, &target_record, &target_byte);
    view_page_t *page = view.building;
    rt_kprintf("reader wait_detail session=%u intent=%u document=%p chapter=%u end=%u complete=%u result=%d target_query=%d target_cursor=%u:%u boundary=%u backward=%u input=%u input_order=%u wanted=%u deferred=%d direction=%d compute=%u job=%p compute_page=%p read=%p wait_kind=%u cursor=%u:%u measured=%u prepared=%u\n",
        (unsigned)view.session, (unsigned)view.intent, (void *)view.document,
        (unsigned)progress.chapter, (unsigned)progress.end, progress.complete, progress.result,
        target_query, (unsigned)target_record, (unsigned)target_byte,
        view.boundary_target, view.backward_target, view.input_enabled, (unsigned)view.input_order,
        view.wanted, view.deferred_turns, view.direction, (unsigned)reader_compute_state(NULL),
        (void *)view.compute_job, (void *)view.compute_page, page ? (void *)page->read_request : NULL,
        page ? page->wait_kind : 0, page ? page->cursor.record : 0, page ? page->cursor.byte : 0,
        page ? page->measured : 0, page ? page->prepared : 0);
}
static bool worker_request_reject(const view_command_t *command, uint32_t intent)
{
    const view_state_t *origin = command->origin ? command->origin->owner : NULL;
    if (command->kind == INTENT_NONE || command->kind == INTENT_REFLOW ||
        !origin || !origin->initialized || !origin->displayed || origin->failure ||
        origin->session != command->session) return false;
    rt_enter_critical();
    if (mailbox.session == command->session && mailbox.intent == intent) {
        mailbox.rejected = true;
        mailbox.error = READER_ERROR_LAYOUT_MEMORY;
        mailbox.result_intent = intent;
        mailbox.work = READER_WORK_IDLE;
    }
    rt_exit_critical();
    rt_kprintf("reader request_rejected session=%u intent=%u kind=%u reason=admission\n",
        (unsigned)command->session, (unsigned)intent, (unsigned)command->kind);
    epd_reader_ui_wake();
    return true;
}
static void worker_nearby_inputs(view_state_t *current, uint64_t parent)
{
    if (!current) return;
    view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
    bool needed[7] = {false};
    if (parent && current->initialized && !current->failure && !request_memory_failed(current))
        for (int direction = -1; direction <= 1; direction += 2) {
            window_route_t route = window_route(direction);
            for (unsigned p = 0; p < route.count; ++p)
                for (unsigned i = 0; i < count; ++i)
                    if (contexts[i] != current && contexts[i] == route.pages[p].owner &&
                        window_runnable(contexts[i], current)) needed[i] = true;
        }
    uint64_t demand = ((uint64_t)current->session << 32) | current->intent;
    for (unsigned i = 0; i < count; ++i) {
        unsigned j = 0;
        while (j < i && contexts[j]->document != contexts[i]->document) ++j;
        if (j < i) continue;
        bool requested = false;
        for (j = i; j < count; ++j)
            if (contexts[j]->document == contexts[i]->document && needed[j]) requested = true;
        document_nearby_request(contexts[i]->document, demand,
            requested && contexts[i]->document != current->document ? parent : 0);
    }
}
static epd_resource_kind_t worker_background_kind(view_state_t *current)
{
    if (near_frames_pending() || preview_ahead_pending() ||
        neighbor_pending(current->previous) || neighbor_pending(current->following)) return EPD_RESOURCE_NEARBY;
    view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
    for (unsigned i = 0; i < count; ++i)
        if (contexts[i] != current && window_runnable(contexts[i], current)) return EPD_RESOURCE_NEARBY;
    return EPD_RESOURCE_OPTIONAL;
}
typedef struct {
    epd_resource_activity_t parent, retiring;
    uint64_t generation;
    uint32_t session, intent;
    epd_activity_demand_t demand;
    epd_activity_capacity_t capacity;
    bool sampled, early_used;
} view_near_retry_t;

static void worker_near_block(view_near_retry_t *retry, view_state_t *current,
                              epd_activity_work_t *parent, epd_activity_work_t *work)
{
    epd_resource_activity_snapshot_t anchor;
    retry->sampled = false;
    if (!epd_activity_work_demand(work, &retry->demand) && !retry->demand.psram)
        retry->demand = (epd_activity_demand_t){65536u, 4096u, EPD_MEMORY_OWNER_INVALID};
    retry->parent = 0;
    retry->retiring = epd_activity_work_activity(work);
    epd_resource_activity_t id = epd_activity_work_activity(parent);
    if (current && id && epd_resource_activity_get(id, &anchor) == EPD_RESOURCE_OK &&
        anchor.requested.kind == EPD_RESOURCE_INTERACTIVE) {
        retry->parent = id; retry->generation = anchor.instance_generation;
        retry->session = current->session; retry->intent = current->intent;
    }
    epd_activity_work_close(work);
    /* A failed credit cannot drive unfinished font pins, image tickets or a
     * renderer. Keep completed window frames and retire only unfinished work. */
    view_state_t *saved = state, *contexts[7];
    unsigned count = window_contexts(current, contexts);
    for (unsigned i = 0; i < count; ++i) if (contexts[i]->initialized) {
        state = contexts[i]; worker_context_pause(true);
    }
    state = saved;
}
static bool worker_near_retry(view_near_retry_t *retry, view_state_t *current,
                              epd_activity_work_t *parent)
{
    if (!retry->parent || !current || current->failure || request_memory_failed(current) ||
        current->session != retry->session || current->intent != retry->intent ||
        epd_activity_work_activity(parent) != retry->parent) return false;
    rt_enter_critical();
    bool live = !mailbox.stop && !mailbox.suspended && !mailbox.paused && !mailbox.replace &&
        !mailbox.cleanup && mailbox.session == retry->session && mailbox.intent == retry->intent;
    rt_exit_critical();
    if (!live) return false;
    epd_resource_activity_snapshot_t anchor, retired;
    if (epd_resource_activity_get(retry->parent, &anchor) != EPD_RESOURCE_OK ||
        anchor.instance_generation != retry->generation || anchor.state != EPD_RESOURCE_ADMITTED ||
        anchor.requested.kind != EPD_RESOURCE_INTERACTIVE) return false;
    if (retry->retiring) {
        int result = epd_resource_activity_get(retry->retiring, &retired);
        if (result != EPD_RESOURCE_STALE && (result != EPD_RESOURCE_OK || retired.state != EPD_RESOURCE_ENDED)) return false;
        retry->retiring = 0;
    }
    if (retry->sampled && epd_memory_capacity_epoch() == retry->capacity.revision) return false;
    epd_activity_capacity_t capacity;
    if (epd_activity_work_capacity(EPD_RESOURCE_NEARBY, retry->parent,
        retry->demand.psram, retry->demand.sram, &capacity) != EPD_RESOURCE_OK) return false;
    bool progress = capacity.ready && ((!retry->sampled && !retry->early_used) ||
        (retry->sampled && !retry->capacity.ready));
    /* Capacity may recover before retirement completes. Give that early
     * restoration one attempt per input; repeated self-cleanup at an already
     * sufficient capacity must not create another retry/deadline. */
    if (progress) retry->early_used = true;
    /* Sample after our own activity/renderer has drained. Update on decreases
     * too: a later recovery must not beat a lifetime free-space high-water mark. */
    retry->capacity = capacity; retry->sampled = true;
    return progress;
}
static void worker_near_resume(view_state_t *current, epd_memory_credit_t ended_credit)
{
    view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
    for (unsigned i = 0; i < count; ++i) {
        /* A cached cold frame can have a partial thaw without owner->rendering.
         * The next attempt gets a fresh recovery ticket under the new credit. */
        for (view_frame_t *frame = contexts[i]->frames; frame; frame = frame->next) {
            if (frame_pixels_ready(frame) || !frame->admission_recovery.used ||
                frame->admission_recovery.owner != ended_credit) continue;
            memset(&frame->admission_recovery, 0, sizeof(frame->admission_recovery));
            frame->memory_reclaimed = frame->memory_waiting = frame->cold_reusing = false;
            frame->admission_stage = NULL; frame->allocation_attempted = false;
        }
        worker_retry_fonts(contexts[i]);
    }
}
static void worker_recovery_continue(epd_memory_recovery_t *recovery, epd_memory_credit_t previous)
{
    if (!recovery->used || recovery->owner != previous ||
        recovery->state == EPD_MEMORY_RECOVERY_DENIED) return;
    epd_memory_recovery_t next = {0};
    epd_memory_recovery_continue(&next, recovery);
    if (next.used) *recovery = next;
}
static void worker_credit_changed(view_state_t *current, epd_memory_credit_t previous)
{
    view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
    for (unsigned i = 0; i < count; ++i) {
        view_state_t *owner = contexts[i];
        worker_recovery_continue(&owner->init_recovery, previous);
        worker_recovery_continue(&owner->image_recovery, previous);
        if (owner->window_attempt_intent == current->intent)
            for (unsigned side = 0; side < 2; ++side)
                worker_recovery_continue(&owner->window_recovery[side], previous);
        if (owner->rendering && !frame_pixels_ready(owner->rendering))
            worker_recovery_continue(&owner->rendering->admission_recovery, previous);
        for (view_frame_t *frame = owner->frames; frame; frame = frame->next)
            if (!frame_pixels_ready(frame)) worker_recovery_continue(&frame->admission_recovery, previous);
    }
}
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
static void window_snapshot_row(view_window_row_t *row, view_state_t *owner, unsigned number)
{
    row->mapped = true; row->context = (uintptr_t)owner;
    row->number = number; row->layout = owner->layout; row->chapter = owner->progress.chapter;
    bool eof;
    if (!window_page(owner, number, &row->cursor, &eof)) return;
    row->known = true;
    if (owner->status.ready && owner->display && number == window_number(owner))
        row->end = owner->display->cursor;
    else {
        reader_cached_page_t *page = reader_page_cache_find(&owner->cache, number, owner->layout);
        if (!page) { row->known = false; return; }
        row->end = (document_cursor_t){page->end.record, page->end.byte};
    }
    /* Rendering frames enter the cache only after completion. */
    for (unsigned list = 0; list < 2; ++list)
        for (view_frame_t *frame = list ? owner->rendering : owner->frames;
             frame; frame = list ? NULL : frame->next) {
            if (frame->owner != owner || frame->layout != owner->layout || frame->number != number ||
                !cursor_equal(row->cursor, (document_cursor_t){frame->page.start.record, frame->page.start.byte}) ||
                !cursor_equal(row->end, (document_cursor_t){frame->page.cursor.record, frame->page.cursor.byte}) ||
                window_end(owner, (document_cursor_t){frame->page.cursor.record, frame->page.cursor.byte}, frame->page.eof) != eof)
                continue;
            row->present = true; row->generation = frame->generation;
            row->complete = frame->complete; row->pixelsready = frame_pixels_ready(frame);
            row->cold = frame->cold.first != NULL; row->cancelled = frame->cancelled;
            row->hot = row->pixelsready && !row->cold;
            return;
        }
}

static void window_snapshot_publish(view_state_t *current)
{
    if (!current || current != primary_state || current != state) return;
    view_window_snapshot_t snapshot = {0};
    snapshot.session = current->session; snapshot.intent = current->intent;
    snapshot.published = current->frame_published;
    snapshot.published_intent = current->published_intent;
    snapshot.published_generation = current->published_generation;
    snapshot.ready = current->status.ready;
    window_snapshot_row(&snapshot.pages[3], current, window_number(current));
    for (int direction = -1; direction <= 1; direction += 2) {
        window_route_t route = window_route(direction);
        for (unsigned i = 0; i < route.count; ++i) {
            view_state_t *owner = route.pages[i].owner;
            window_snapshot_row(&snapshot.pages[3 + direction * (int)route.pages[i].distance],
                                owner, route.pages[i].number);
            if (direction < 0 && window_extra(owner))
                document_trace_restore(owner->document, DOC_RESTORE_ROUTE, owner,
                    restore_view_flags(owner), 0, route.pages[i].distance);
        }
    }
    for (unsigned i = 0; i < 7; ++i) if (snapshot.pages[i].hot) {
        bool duplicate = false;
        for (unsigned j = 0; j < i; ++j)
            duplicate |= snapshot.pages[j].hot && snapshot.pages[i].context == snapshot.pages[j].context &&
                cursor_equal(snapshot.pages[i].cursor, snapshot.pages[j].cursor);
        if (!duplicate) ++snapshot.hot;
    }
    snapshot.tick = rt_tick_get();
    /* The UI retains its initial context until close, including across chapter
     * switches. Only that allocation has trailing snapshot storage. Session
     * checks precede every access, so retired workers cannot publish into a
     * replacement session or a freed initial context. */
    rt_enter_critical();
    if (ui.session && ui.id == snapshot.session && ui.session->session == snapshot.session &&
        mailbox.session == snapshot.session && mailbox.intent == snapshot.intent &&
        !mailbox.stop && !mailbox.replace) {
        view_window_snapshot_t *published = (view_window_snapshot_t *)(ui.session + 1);
        snapshot.serial = published->serial + 1u;
        snapshot.paused = mailbox.paused; snapshot.suspended = mailbox.suspended;
        *published = snapshot;
    }
    rt_exit_critical();
}

static bool window_snapshot_copy(view_window_snapshot_t *snapshot)
{
    bool current = false;
    rt_enter_critical();
    if (ui.session && ui.session->session == ui.id) {
        *snapshot = *(const view_window_snapshot_t *)(ui.session + 1);
        current = snapshot->serial && snapshot->session == ui.id &&
            snapshot->session == mailbox.session && snapshot->intent == mailbox.intent &&
            !mailbox.stop && !mailbox.replace && snapshot->paused == mailbox.paused &&
            snapshot->suspended == mailbox.suspended;
    }
    rt_exit_critical();
    return current;
}

void epd_reader_window_diagnostic(void)
{
    view_window_snapshot_t snapshot = {0};
    bool current = window_snapshot_copy(&snapshot);
    rt_kprintf("reader window_snapshot session=%u intent=%u serial=%u tick=%u age_ms=%u current=%u ready=%u hot=%u published=%u published_intent=%u published_generation=%u paused=%u suspended=%u\n",
        (unsigned)snapshot.session, (unsigned)snapshot.intent, (unsigned)snapshot.serial,
        (unsigned)snapshot.tick, snapshot.serial ? elapsed_ms(snapshot.tick) : 0u,
        (unsigned)current, (unsigned)snapshot.ready, snapshot.hot, (unsigned)snapshot.published,
        (unsigned)snapshot.published_intent, (unsigned)snapshot.published_generation,
        (unsigned)snapshot.paused, (unsigned)snapshot.suspended);
    for (unsigned i = 0; i < 7; ++i) {
        const view_window_row_t *row = &snapshot.pages[i];
        rt_kprintf("reader window_page serial=%u rel=%d context=%p chapter=%u page=%u layout=%u generation=%u cursor=%u:%u end=%u:%u mapped=%u known=%u present=%u complete=%u pixelsready=%u cold=%u cancelled=%u hot=%u\n",
            (unsigned)snapshot.serial, (int)i - 3, (void *)row->context, row->chapter, row->number,
            (unsigned)row->layout, (unsigned)row->generation,
            (unsigned)row->cursor.record, (unsigned)row->cursor.byte,
            (unsigned)row->end.record, (unsigned)row->end.byte,
            (unsigned)row->mapped, (unsigned)row->known, (unsigned)row->present,
            (unsigned)row->complete, (unsigned)row->pixelsready, (unsigned)row->cold,
            (unsigned)row->cancelled, (unsigned)row->hot);
    }
}
static void worker_wait(view_state_t *current, rt_int32_t timeout)
{
    window_snapshot_publish(current);
    rt_sem_take(mailbox.wake, timeout);
}
#else
#define worker_wait(current, timeout) rt_sem_take(mailbox.wake, timeout)
#endif

static void view_worker(void *unused)
{
    (void)unused;
    view_state_t *current = NULL;
    unsigned speculative = 0;
    epd_activity_work_t *activity = NULL, *nearby = NULL;
    epd_resource_activity_t entered_activity = 0;
    epd_memory_credit_t entered_credit = EPD_MEMORY_OWNER_INVALID;
    epd_resource_kind_t background_kind = EPD_RESOURCE_OPTIONAL;
    bool settled = false, near_denied = false, admission_retried = false, request_denied = false;
    view_near_retry_t near_retry = {0};
    uint32_t font_session = 0, font_intent = 0;
    bool spare_attempted = false;
    for (;;) {
        view_resource_publish(); view_resource_step();
        rt_enter_critical();
        epd_activity_work_t *accepted = mailbox.activity; mailbox.activity = NULL;
        bool stopping = mailbox.stop, suspend_work = mailbox.suspended;
        rt_exit_critical();
        if (accepted) {
            epd_activity_work_close(nearby); nearby = NULL;
            epd_activity_work_close(activity); activity = accepted;
            settled = near_denied = false;
            memset(&near_retry, 0, sizeof(near_retry));
            admission_retried = request_denied = false;
        }
        if (stopping || suspend_work) {
            epd_activity_work_close(nearby); nearby = NULL;
            if (!settled) { epd_activity_work_close(activity); activity = NULL; }
        }
        epd_activity_guard_t guard;
        bool entered = false, admission_failed = false;
        if (!stopping && !suspend_work && !request_denied) {
            if (!activity && !settled && current && (!current->init_attempted || worker_target_needed())) activity = view_activity_new();
            epd_resource_kind_t kind = current ? worker_background_kind(current) : EPD_RESOURCE_OPTIONAL;
            bool retrying = settled && near_denied && kind == EPD_RESOURCE_NEARBY &&
                worker_near_retry(&near_retry, current, activity);
            if (retrying) near_denied = false;
            if (settled && nearby && kind != background_kind) {
                epd_activity_work_close(nearby); nearby = NULL;
            }
            if (settled && !nearby && !near_denied && current &&
                worker_idle_delay(current) < rt_tick_from_millisecond(100)) {
                background_kind = kind;
                nearby = epd_activity_work_queue(kind, kind == EPD_RESOURCE_NEARBY ? epd_activity_work_activity(activity) : 0,
                    retrying ? near_retry.demand.psram : 65536u,
                    retrying ? near_retry.demand.sram : 4096u, view_activity_wake, NULL);
                if (!nearby)
                    rt_kprintf("reader prepare_admission session=%u intent=%u kind=%u parent=%u fallback=1\n",
                        current->session, current->intent, (unsigned)kind,
                        (unsigned)epd_activity_work_activity(activity));
                if (!nearby && !retrying) nearby = epd_activity_work_queue(EPD_RESOURCE_OPTIONAL, 0, 65536u, 4096u, view_activity_wake, NULL);
                if (!nearby) {
                    near_denied = true;
                    if (kind == EPD_RESOURCE_NEARBY) worker_near_block(&near_retry, current, activity, NULL);
                } else if (retrying) worker_near_resume(current, near_retry.demand.credit);
            }
            epd_activity_work_t *running = settled ? nearby : activity;
            if (running) {
                int ready = epd_activity_work_prepare(running, 65536u, 4096u);
                if (!ready) {
                    bool cleanup = render_cleanup(); cleanup |= index_client_cleanup(false);
                    worker_wait(current, cleanup ? (int)rt_tick_from_millisecond(20) : RT_WAITING_FOREVER);
                    continue;
                }
                entered = ready > 0 && epd_activity_work_enter(running, &guard);
                if (entered) {
                    allocation_activity = running;
                    epd_activity_demand_t demand;
                    epd_resource_activity_t id = epd_activity_work_activity(running);
                    if (epd_activity_work_demand(running, &demand)) {
                        /* Track the last executed credit, including updates
                         * that wait through several prepare calls. A new work
                         * identity follows the separate input/retry lifecycle. */
                        if (id == entered_activity && demand.credit != entered_credit)
                            worker_credit_changed(current, entered_credit);
                        entered_activity = id; entered_credit = demand.credit;
                    }
                }
                else if (settled) {
                    if (background_kind == EPD_RESOURCE_NEARBY) worker_near_block(&near_retry, current, activity, nearby);
                    else epd_activity_work_close(nearby);
                    nearby = NULL; near_denied = true;
                }
                else {
                    /* A queued input owns its command until admission. Allow
                     * one new request after the old credit has been closed. */
                    if (!admission_retried && current && current->initialized) {
                        admission_retried = true;
                        epd_activity_work_close(activity); activity = view_activity_new();
                        if (activity) {
                            worker_wait(current, rt_tick_from_millisecond(20));
                            continue;
                        }
                    }
                    admission_failed = true;
                }
            }
        }
        rt_enter_critical();
        bool stop = mailbox.stop, replace = mailbox.replace, paused = mailbox.paused, suspended = mailbox.suspended;
        view_state_t *incoming = mailbox.incoming, *configuration = NULL;
        view_command_t command = {0};
        view_frame_t *ack = mailbox.ack;
        mailbox.incoming = NULL; mailbox.replace = false; mailbox.ack = NULL;
        uint32_t intent = mailbox.intent;
        rt_exit_critical();
        if (stop || replace || suspended)
            frame_spare_clear(stop ? "session_stop" : replace ? "session_replace" : "session_suspend",
                stop || replace);
        if (replace || stop) {
            spare_attempted = false;
            pagination_reset();
            if (parked_state) {
                view_state_t *old = parked_state; parked_state = NULL;
                old->parked = old->displayed = false; state_retire(old);
            }
            if (current) {
                current->displayed = false;
                view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
                for (unsigned i = 0; i < count; ++i) contexts[i]->displayed = false;
            }
            cluster_retire(current); current = NULL;
            if (replace && !incoming) {
                if (entered) { allocation_activity = NULL; epd_activity_work_leave(&guard); entered = false; }
                epd_activity_work_close(nearby); nearby = NULL;
                epd_activity_work_close(activity); activity = NULL; settled = near_denied = false;
                memset(&near_retry, 0, sizeof(near_retry));
            }
            if (stop) state_retire(incoming); else current = incoming;
        }
        state = primary_state = current;
        if (current) { view.selected = true; view.intent = intent; }
        if (current && !view.init_attempted && !suspended && entered) core_initialize();
        /* Decide against the installed context, after initialization can have
         * completed. Pending input stays owned by the mailbox; a displayed
         * origin can still receive cancellation after a cold chapter fails. */
        rt_enter_critical();
        if ((!suspended || stop) && (stop || !current || current->initialized ||
            command_restores_origin(&mailbox.command, current))) {
            configuration = mailbox.configuration; mailbox.configuration = NULL;
            command = mailbox.command;
            mailbox.command.kind = INTENT_NONE; mailbox.command.origin = NULL;
        }
        rt_exit_critical();
        if (pagination.counter && (command.kind != INTENT_NONE || !pagination_requested()))
            pagination_drop_counter();
        if (current && admission_failed) {
            request_denied = worker_request_reject(&command, intent);
            if (!request_denied) { view.failure = READER_ERROR_LAYOUT_MEMORY; view.init_attempted = true; }
        }
        if (current) {
            view.selected = true; view.intent = intent;
            if (!admission_failed && !view.initialized && command_restores_origin(&command, current)) {
                command_apply(command, &current);
                state = primary_state = current; view.intent = intent; command.kind = INTENT_NONE;
            }
            if (configuration) {
                pagination_reset();
                if (view.initialized && command.kind == INTENT_REFLOW) {
                    command_apply(command, &current); state = current; command.kind = INTENT_NONE;
                }
                window_clear(); state_retire(current->previous); state_retire(current->following);
                current->previous = current->following = NULL;
                if (view.initialized) state_reflow(configuration); else state_retire(configuration);
            }
            if (view.initialized && !admission_failed) {
                if (ack) {
                    if (parked_state && parked_state != ack->owner) {
                        view_state_t *old = parked_state; parked_state = NULL;
                        old->displayed = old->parked = false; state_retire(old);
                    }
                    view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
                    for (unsigned i = 0; i < count; ++i)
                        if (contexts[i] != ack->owner) contexts[i]->displayed = false;
                    ack->owner->displayed = true;
                    frame_remember(ack);
                }
                frame_accept(ack);
                command_apply(command, &current);
                state = primary_state = current; view.intent = intent;
            }
        } else state_retire(configuration);
        frame_release(ack); frame_release(command.origin);
        if (current && (font_session != current->session || font_intent != intent)) {
            worker_retry_fonts(current); font_session = current->session; font_intent = intent;
        }
        if (admission_failed && current && !request_denied) {
            rt_enter_critical(); mailbox.error = view.failure; mailbox.result_intent = view.intent; rt_exit_critical();
            epd_reader_ui_wake();
        }
        window_quiesce(current);
        worker_quiesce(current, paused, suspended, !settled);
        worker_nearby_inputs(current, entered && settled && !paused && !suspended ?
            epd_activity_work_activity(activity) : 0);
        bool cleanup_pending = render_cleanup();
        cleanup_pending |= index_client_cleanup(stop);
        if (stop) {
            if (entered) { allocation_activity = NULL; epd_activity_work_leave(&guard); entered = false; }
            if (!cleanup_pending) break;
            worker_wait(current, rt_tick_from_millisecond(20));
            continue;
        }
        if (cleanup_pending) {
            if (entered) { allocation_activity = NULL; epd_activity_work_leave(&guard); }
            worker_input_commit(current);
            worker_trace_wait(true, paused, suspended);
            worker_wait(current, rt_tick_from_millisecond(20));
            continue;
        }
        /* Finish nearby glyph preparation before queuing more speculative text.
         * A new foreground target still needs core_step to resolve its layout. */
        bool prepare_near = current && current->initialized && !current->failure &&
            !request_memory_failed(current) &&
            !paused && !suspended && current->rendering && !worker_target_needed();
        bool resolve_target = current && worker_target_needed() && !current->display_pending;
        if (entered && (render_frame || prepare_near) && !resolve_target) {
            state = render_frame ? render_frame->owner : current;
            view_input_set(state, false);
            view_frame_t *preparing = view.rendering;
            unsigned pin_run = preparing ? preparing->pin_run : 0;
            unsigned pin_byte = preparing ? preparing->pin_byte : 0;
            frame_prepare();
            bool advanced = view.rendering && view.rendering == preparing &&
                (view.rendering->pin_run != pin_run || view.rendering->pin_byte != pin_byte);
            state = current;
            if (render_frame || (prepare_near && current->rendering)) {
                if (settled && prepare_near &&
                    (!render_frame || render_frame->owner == current))
                    window_prepare(current, false);
                worker_trace_wait(false, paused, suspended);
                allocation_activity = NULL; epd_activity_work_leave(&guard);
                worker_input_commit(current);
                worker_wait(current, rt_tick_from_millisecond(render_frame || advanced ? 1 : 4));
                continue;
            }
        }
        if (current && !view.initialized && !suspended && !request_denied) {
            rt_enter_critical();
            bool notify = mailbox.error != view.failure ||
                (view.failure && mailbox.result_intent != view.intent);
            mailbox.error = view.failure; mailbox.result_intent = view.intent; mailbox.work = READER_WORK_IDLE;
            rt_exit_critical();
            if (notify) epd_reader_ui_wake();
        }
        if (entered && current && view.initialized && !suspended && !request_memory_failed(current) &&
            (!paused || worker_target_needed())) {
            navigation_resolve(&current);
            target_resolve();
            reader_work_t work = READER_WORK_TARGET;
            if (view.initialized && !view.target_pending && !view.navigation_link && !view.navigation_toc) {
                font_priority(core_waiting());
                /* Near layouts still require BODY input while speculative
                 * prefetch is gated; their credit remains NEARBY/OPTIONAL. */
                work = core_step(3, !settled || background_kind == EPD_RESOURCE_NEARBY, true);
                turn_continue(&current);
                if (view.initialized) frame_prepare();
            }
            reader_view_t published = view.status;
            reader_error_t request_error = view.failure ? view.failure :
                request_memory_failed(current) ? current->request_failure : READER_ERROR_NONE;
            pagination_snapshot(state, &published);
            bool notify = false;
            rt_enter_critical();
            if (mailbox.session == view.session && mailbox.intent == view.intent) {
                notify = mailbox.error != request_error || (request_error && mailbox.result_intent != view.intent) ||
                    (request_memory_failed(current) && !mailbox.rejected) ||
                    (!published.indexing && (mailbox.indexing || mailbox.pages != published.pages));
                mailbox.error = request_error; mailbox.result_intent = view.intent;
                mailbox.work = request_memory_failed(current) ? READER_WORK_IDLE : work;
                if (request_memory_failed(current)) mailbox.rejected = true;
                mailbox.pages = published.pages; mailbox.indexing = published.indexing;
                mailbox.document = view.document; mailbox.layout = view.display_layout;
                if (view.display) mailbox.cursor = view.display->start;
                mailbox.page = published.page;
                mailbox.percent = view.status.percent;
            }
            rt_exit_critical();
            if (notify) epd_reader_ui_wake();
            if (settled && view.initialized && !render_frame && !view.failure && !request_memory_failed(current) && !paused && view.status.ready && !view.navigation_link &&
                !view.navigation_toc && !core_waiting()) {
                if (near_frames_pending() || preview_ahead_pending()) {
                    /* Neighbor input and layout can advance between local
                     * frames. Pixel preparation keeps its normal turn order. */
                    neighbors_prepare(current);
                    window_prepare(current, false);
                } else window_service(current, &speculative);
            }
        }
        if (entered) {
            if (settled && !paused && !suspended) {
                if (current && view.status.ready && !worker_target_needed() && !spare_attempted) {
                    spare_attempted = true;
                    frame_spare_install(current->session);
                }
                worker_nearby_inputs(current, epd_activity_work_activity(activity));
            }
            allocation_activity = NULL; epd_activity_work_leave(&guard);
        }
        if (current && request_memory_failed(current)) {
            rt_enter_critical();
            bool notify = mailbox.session == current->session && mailbox.intent == current->intent &&
                (!mailbox.rejected || mailbox.error != current->request_failure || mailbox.result_intent != current->intent);
            if (notify) {
                mailbox.rejected = true;
                mailbox.error = current->request_failure; mailbox.result_intent = current->intent;
                mailbox.work = READER_WORK_IDLE;
            }
            rt_exit_critical();
            if (notify) epd_reader_ui_wake();
        }
        if (request_denied) {
            epd_activity_work_close(nearby); nearby = NULL;
            epd_activity_work_close(activity); activity = NULL;
            settled = near_denied = true;
        }
        if (activity && !settled && current && (view.failure || request_memory_failed(current) ||
            (view.frame_published && view.published_intent == view.intent && !render_frame && !worker_target_needed()))) {
            if (!view.failure && !request_memory_failed(current) && epd_activity_work_settle(activity)) settled = true;
            else { epd_activity_work_close(activity); activity = NULL; settled = true; }
        }
        if (settled && current && !paused && !suspended) core_background_pause(false);
        if (nearby && current && (paused || suspended || view.failure || request_memory_failed(current) ||
            worker_idle_delay(current) >= rt_tick_from_millisecond(100))) {
            epd_activity_work_close(nearby); nearby = NULL;
        }
        if (!request_denied) worker_trace_wait(false, paused, suspended);
        worker_input_commit(current);
        bool idle = !current || suspended || request_denied || !view.initialized ||
            (paused && !worker_target_needed());
        if (current && !suspended && !request_denied && !view.init_attempted)
            worker_wait(current, rt_tick_from_millisecond(20));
        else if (idle) worker_wait(current, RT_WAITING_FOREVER);
        else worker_wait(current, near_denied ? rt_tick_from_millisecond(100) : worker_idle_delay(current));
    }
    epd_activity_work_close(nearby); epd_activity_work_close(activity);
    state = primary_state = NULL;
    view_resource_step(); view_resource_publish(); epd_resource_cache_close(&view_resource);
    /* Suspend before waking the joining owner. The owner deletes this thread,
     * so no module instruction can execute after shutdown reports completion. */
    rt_enter_critical();
    rt_thread_suspend(rt_thread_self());
    rt_sem_release(mailbox.stopped);
    rt_exit_critical();
}
static bool worker_start(void)
{
    if (mailbox.thread) return true;
    mailbox.wake = rt_sem_create("rdvwake", 0, RT_IPC_FLAG_FIFO);
    mailbox.stopped = rt_sem_create("rdvstop", 0, RT_IPC_FLAG_FIFO);
    if (!mailbox.wake || !mailbox.stopped) goto failed;
    mailbox.stop = false;
    mailbox.thread = epd_thread_create_psram("rdview", view_worker, NULL, 12288, RT_THREAD_PRIORITY_MIDDLE + 2, 10);
    if (!mailbox.thread) goto failed;
    if (rt_thread_startup(mailbox.thread) == RT_EOK) return true;
    rt_thread_delete(mailbox.thread); mailbox.thread = NULL;
failed:
    if (mailbox.wake) rt_sem_delete(mailbox.wake);
    if (mailbox.stopped) rt_sem_delete(mailbox.stopped);
    mailbox.wake = mailbox.stopped = NULL;
    return false;
}
void epd_reader_view_worker_shutdown(void)
{
    if (!mailbox.thread) return;
    rt_enter_critical(); mailbox.stop = true; ++mailbox.intent; rt_exit_critical();
    view_cancel(); wake_view();
    rt_sem_take(mailbox.stopped, RT_WAITING_FOREVER);
    rt_thread_delete(mailbox.thread); mailbox.thread = NULL;
    rt_enter_critical();
    rt_sem_t wake = mailbox.wake, stopped = mailbox.stopped;
    mailbox.wake = mailbox.stopped = NULL;
    rt_exit_critical();
    rt_sem_delete(wake); rt_sem_delete(stopped);
}

static void ui_configuration(view_state_t *prepared)
{
    ui.font_id = prepared->font_id; ui.font_revision = prepared->font_revision;
    ui.area = prepared->font_area;
    ui.appearance = prepared->appearance;
    ui.appearance_pending = false;
    memcpy(ui.settings, prepared->settings, sizeof(ui.settings));
}
static void ui_root_deleted(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) != ui.root) return;
    epd_reader_note_close();
    ui.root = ui.bitmap = NULL;
}
void epd_reader_detach(void)
{
    lv_draw_wait_for_finish();
    if (ui.root) lv_obj_delete(ui.root);
}
void epd_reader_collect(void)
{
    rt_enter_critical(); view_state_t *retired = mailbox.retired; mailbox.retired = NULL; rt_exit_critical();
    while (retired) {
        view_state_t *next = retired->next;
        rt_enter_critical(); unsigned references = retired->references; rt_exit_critical();
        if (!references) state_dispose(retired);
        else {
            rt_enter_critical(); retired->next = mailbox.retired; mailbox.retired = retired; rt_exit_critical();
        }
        retired = next;
    }
    document_service_process();
    if (!mailbox.thread) { view_resource_publish(); epd_resource_cache_close(&view_resource); }
}
bool epd_reader_save(void)
{
    if (!ui.bound || !ui.info.ready || !ui.bound->position.source.unit) return false;
    reader_position_t position = ui.bound->position;
    position.pages = ui.info.pages; position.page = ui.info.page;
    position.percent = ui.info.percent;
    ui.info.saved = !ui.save || ui.save(ui.user, &position);
    return ui.info.saved;
}
static void font_request_cancel(void)
{
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    font_trace_stop();
#endif
    view_state_t *context = font_request.context;
    if (!context) return;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    rt_enter_critical();
#endif
    if (font_request.opening && ui.session == context) ui.session = NULL;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    rt_exit_critical();
#endif
    frame_release(font_request.origin);
    epd_activity_work_close(font_request.activity);
    memset(&font_request, 0, sizeof(font_request));
    state_dispose(context);
}

static void font_configuration_snapshot(view_state_t *configuration)
{
    configuration->font_id = epd_app_reading_font_id();
    configuration->font_revision = epd_app_reading_font_revision();
    configuration->font_area = reader_geometry();
    configuration->appearance = ui_settings_index(UI_SETTING_READER_APPEARANCE);
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        configuration->settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
    ui_configuration(configuration);
}

void epd_reader_close(void)
{
    font_request_cancel();
    first_font_cancel();
    if (ui.session) epd_reader_save();
    epd_reader_detach();
    frame_release(ui.bound); frame_release(ui.ready); ui.bound = ui.ready = NULL;
    rt_enter_critical();
    epd_activity_work_t *activity = mailbox.activity; mailbox.activity = NULL;
    view_state_t *incoming = mailbox.incoming, *configuration = mailbox.configuration;
    view_frame_t *ready = mailbox.ready, *ack = mailbox.ack;
    mailbox.incoming = mailbox.configuration = NULL; mailbox.ready = mailbox.ack = NULL;
    mailbox.replace = true; mailbox.session = ++ui.id; ++mailbox.intent;
    mailbox.burst = false;
    ui.last_turn_valid = false;
    view_frame_t *origin = mailbox.command.origin; mailbox.command.origin = NULL;
    mailbox.command.kind = INTENT_NONE;
    mailbox.error = READER_ERROR_NONE; mailbox.work = READER_WORK_IDLE;
    mailbox.rejected = false;
    if (ui.session) --ui.session->references;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    /* incoming can be the UI's initial context, including its snapshot. */
    ui.session = NULL;
#endif
    rt_exit_critical();
    epd_activity_work_close(activity);
    /* These contexts have not been admitted to the worker. */
    if (incoming) state_dispose(incoming);
    if (configuration) state_dispose(configuration);
    frame_release(ready); frame_release(ack); frame_release(origin);
    ui.session = NULL; ui.save = NULL; ui.user = NULL; ui.pending = false; ui.turn_delta = 0;
    ui.info = (reader_view_t){0};
    view_cancel(); wake_view(); epd_reader_collect();
}
bool epd_reader_attach(document_t *document, const char *title,
    const reader_position_t *restore, reader_save_fn save,
    document_t *(*reopen)(void *, char *, size_t), void *user)
{
    (void)reopen;
    epd_reader_close();
    if (!document) return false;
    if (!epd_reader_render_image_init()) {
        document_close(document); epd_reader_set_error_code(READER_ERROR_WIDGET_MEMORY); return false;
    }
    ui.requested_at = rt_tick_get();
    size_t prepared_size = sizeof(view_state_t);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    prepared_size += sizeof(view_window_snapshot_t);
#endif
    view_state_t *prepared = view_allocate(prepared_size, EPD_APP_PSRAM);
    if (!prepared) { document_close(document); epd_reader_set_error_code(READER_ERROR_LAYOUT_MEMORY); return false; }
    memset(prepared, 0, prepared_size); prepared->document = document;
    if (restore && !restore->source.unit && restore->conversion_version != DOCUMENT_CONVERSION_VERSION &&
        document_history_version(restore->conversion_version))
        prepared->saved_position.page = restore->page;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    rt_tick_t activity_requested = rt_tick_get();
#endif
    epd_activity_work_t *activity = view_activity_new();
    if (!activity) { state_dispose(prepared); epd_reader_set_error_code(READER_ERROR_LAYOUT_MEMORY); return false; }
    first_font_begin(ui.id);
    if (!worker_start()) {
        first_font_cancel(); epd_activity_work_close(activity);
        state_dispose(prepared); epd_reader_set_error_code(READER_ERROR_FONT_MEMORY); return false;
    }
    snprintf(prepared->title, sizeof(prepared->title), "%s", title ? title : "");
    snprintf(ui.title, sizeof(ui.title), "%s", prepared->title);
    font_configuration_snapshot(prepared);
    ui.info = prepared->status; ui.info.font = epd_app_font_role(EPD_FONT_BODY);
    ui.info.title = ui.title; ui.info.text = ""; ui.info.indexing = true;
    ui.info.revision = 1; ui.pending = true; ui.paused = false; ui.configuration_failed = false;
    ui.save = save; ui.user = user; ui.session = prepared;
    prepared->session = ui.id;
    font_request.activity = activity;
    font_request.context = prepared; font_request.session = ui.id; font_request.opening = true;
    rt_enter_critical();
    mailbox.paused = mailbox.suspended = false;
    ui.intent = mailbox.intent;
    rt_exit_critical();
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    font_trace_arm(activity, activity_requested);
#endif
    rt_kprintf("reader request session=%u intent=%u kind=open tick=%u enqueue_ms=%u\n",
        (unsigned)ui.id, (unsigned)ui.intent, (unsigned)ui.requested_at, elapsed_ms(ui.requested_at));
    wake_view();
    return true;
}
bool epd_reader_active(void) { return ui.session != NULL; }
const reader_view_t *epd_reader_get(void) { return &ui.info; }
void epd_reader_set_error_code(reader_error_t error)
{
    if (error) ui.appearance_pending = false;
    ui.info.error_code = error;
    snprintf(ui.info.error, sizeof(ui.info.error), "%s", reader_error_text(error));
    ++ui.info.revision;
}
bool epd_reader_settings_changed(void)
{
    if (!ui.session) return false;
    if (ui.appearance != ui_settings_index(UI_SETTING_READER_APPEARANCE)) return true;
    if (ui.font_id != epd_app_reading_font_id() || ui.font_revision != epd_app_reading_font_revision()) return true;
    reader_geometry_t area = reader_geometry();
    if (ui.area.width != area.width || ui.area.top != area.top || ui.area.bottom != area.bottom) return true;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        if (ui.settings[i - UI_SETTING_FONT_SIZE] != ui_settings_index(i)) return true;
    return false;
}
static void ui_configuration_rejected(void)
{
    ui.font_id = epd_app_reading_font_id(); ui.font_revision = epd_app_reading_font_revision();
    ui.area = reader_geometry();
    ui.appearance = ui_settings_index(UI_SETTING_READER_APPEARANCE);
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        ui.settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
    ui.configuration_failed = true;
    epd_reader_set_error_code(READER_ERROR_FONT_MEMORY);
}
bool epd_reader_reflow(void)
{
    if (!ui.session) return false;
    if (font_request.context && font_request.opening) {
        /* Nothing has been admitted to the view worker yet. The same document
         * can take the latest settings without reopening or losing its anchor. */
        font_configuration_snapshot(font_request.context);
        return true;
    }
    epd_reader_note_close();
    ui.requested_at = rt_tick_get();
    view_state_t *configuration = view_allocate(sizeof(*configuration), EPD_APP_PSRAM);
    if (!configuration) { ui_configuration_rejected(); return false; }
    memset(configuration, 0, sizeof(*configuration));
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    rt_tick_t activity_requested = rt_tick_get();
#endif
    epd_activity_work_t *activity = view_activity_new();
    if (!activity) { epd_app_free(configuration); ui_configuration_rejected(); return false; }
    view_command_t command = font_request.command;
    font_request_cancel();
    font_configuration_snapshot(configuration);
    frame_ref(ui.bound);
    font_request.activity = activity;
    font_request.context = configuration; font_request.origin = ui.bound;
    font_request.session = ui.id; font_request.opening = false;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    font_trace_arm(activity, activity_requested);
#endif
    font_request.command = command;
    if (!font_request.command.kind) font_request.command.kind = ui.bound ? INTENT_REFLOW : INTENT_NONE;
    frame_release(ui.ready); ui.ready = NULL;
    ui.configuration_failed = false; ui.pending = true;
    ui.info.error_code = READER_ERROR_NONE; ui.info.error[0] = 0;
    view_cancel(); wake_view();
    return true;
}

/* A full batch owns six shells. A retiring generation may temporarily occupy
 * those slots: leave the request pending instead of reporting a font failure.
 * This function never waits for file reads, remote metrics, or glyph pixels. */
static bool font_request_process(void)
{
    view_state_t *prepared = font_request.context;
    if (!prepared) return true;
    if (font_request.session != ui.id || !ui.session) { font_request_cancel(); return false; }
    int admission = epd_activity_work_ready(font_request.activity);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    if (!admission) font_trace_once(FONT_TRACE_ACTIVITY_WAIT, "activity_wait", 0);
    if (admission > 0) { font_trace_probe(false); font_trace_ready(); }
#endif
    if (!admission) return false;
    if (admission < 0) { font_request_cancel(); ui.pending = false; ui_configuration_rejected(); return false; }
    if (!epd_app_reading_font_can_create(6)) {
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
        font_trace_once(FONT_TRACE_SLOTS_WAIT, "font_slots_wait", 0);
#endif
        return false;
    }
    epd_activity_guard_t guard;
    if (!epd_activity_work_enter(font_request.activity, &guard)) { font_request_cancel(); ui.pending = false; ui_configuration_rejected(); return false; }
    bool opening = font_request.opening;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    font_trace_once(FONT_TRACE_BEGIN, "font_begin", 0);
#endif
    int begun = opening ? epd_reader_font_begin() : 1;
    bool ready_fonts = begun > 0 && prepare_fonts(prepared);
    epd_activity_work_leave(&guard);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    if (!begun) font_trace_once(FONT_TRACE_SERVICE_WAIT, "font_service_wait", 0);
    else font_trace_once(FONT_TRACE_END, "font_end", ready_fonts ? 1 : -1);
#endif
    if (!begun) return false;
    if (!ready_fonts) {
        font_request_cancel(); ui.pending = false;
        if (opening) first_font_cancel();
        ui_configuration_rejected();
        return false;
    }
    ui_configuration(prepared);
    view_frame_t *origin = font_request.origin;
    view_command_t command = font_request.command;
    epd_activity_work_t *activity = font_request.activity;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    font_trace_stop();
#endif
    memset(&font_request, 0, sizeof(font_request));
    rt_enter_critical();
    view_frame_t *ready = mailbox.ready; mailbox.ready = NULL;
    view_frame_t *old_origin = mailbox.command.origin;
    view_state_t *old = mailbox.configuration; mailbox.configuration = NULL;
    epd_activity_work_t *old_activity = mailbox.activity; mailbox.activity = activity;
    ui.intent = ++mailbox.intent;
    mailbox.session = ui.id; mailbox.requested_at = ui.requested_at;
    mailbox.error = READER_ERROR_NONE;
    mailbox.rejected = false;
    command.appearance = prepared->appearance;
    if (opening) {
        prepared->references = 1; prepared->session = ui.id; prepared->intent = ui.intent;
        prepared->selected = true;
        mailbox.incoming = prepared; mailbox.replace = true;
        mailbox.pages = 0; mailbox.indexing = true;
        command.session = ui.id; command.serial = ui.intent;
        mailbox.command = command;
    } else {
        mailbox.configuration = prepared;
        if (!command.kind) command.kind = origin ? INTENT_REFLOW : INTENT_NONE;
        command.session = ui.id; command.serial = ui.intent; command.origin = origin;
        mailbox.command = command; origin = NULL;
    }
    rt_exit_critical();
    epd_activity_work_close(old_activity);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    font_trace_emit("font_handoff", rt_tick_get(), 1);
#endif
    if (old) state_dispose(old);
    frame_release(old_origin); frame_release(origin); frame_release(ready);
    if (!opening)
        rt_kprintf("reader request session=%u intent=%u kind=reflow tick=%u enqueue_ms=%u\n",
            (unsigned)ui.id, (unsigned)ui.intent, (unsigned)ui.requested_at, elapsed_ms(ui.requested_at));
    wake_view();
    return true;
}
static bool ui_command_at(view_intent_t kind, int value, document_cursor_t cursor, const document_anchor_t *source)
{
    if (!ui.session) return false;
    if (font_request.context) {
        if (kind != INTENT_TURN) ui.turn_delta = 0;
        font_request.command = (view_command_t){kind, value, cursor, ui.id, 0, NULL, {0}};
        if (source) font_request.command.source = *source;
        return true;
    }
    epd_activity_work_t *activity = view_activity_new();
    if (!activity) {
        epd_reader_set_error_code(READER_ERROR_LAYOUT_MEMORY);
        return false;
    }
    epd_reader_note_close();
    ui.requested_at = rt_tick_get();
    if (kind != INTENT_TURN) ui.turn_delta = 0;
    reader_geometry_t geometry = kind == INTENT_RESIZE ? reader_geometry() : (reader_geometry_t){0};
    frame_ref(ui.bound);
    rt_enter_critical();
    epd_activity_work_t *old_activity = mailbox.activity; mailbox.activity = activity;
    ui.intent = ++mailbox.intent;
    mailbox.requested_at = ui.requested_at;
    mailbox.rejected = false;
    mailbox.burst = kind == INTENT_TURN && ui.last_turn_valid &&
        (rt_tick_t)(ui.requested_at - ui.last_turn) <= rt_tick_from_millisecond(VIEW_BURST_TURN_MS);
    ui.last_turn_valid = kind == INTENT_TURN;
    ui.last_turn = ui.requested_at;
    view_frame_t *origin = mailbox.command.origin;
    mailbox.command = (view_command_t){kind, value, cursor, ui.id, ui.intent, ui.bound, {0},
        kind == INTENT_REPAINT ? (unsigned)value : ui.appearance_pending ? ui.requested_appearance : ui.appearance,
        geometry};
    if (source) mailbox.command.source = *source;
    view_frame_t *ready = mailbox.ready; mailbox.ready = NULL;
    rt_exit_critical();
    epd_activity_work_close(old_activity);
    frame_release(origin); frame_release(ready); frame_release(ui.ready); ui.ready = NULL;
    rt_kprintf("reader request session=%u intent=%u kind=%u value=%d tick=%u enqueue_ms=%u\n",
        (unsigned)ui.id, (unsigned)ui.intent, (unsigned)kind, value, (unsigned)ui.requested_at, elapsed_ms(ui.requested_at));
    ui.pending = true;
    if (kind == INTENT_TURN) turn_cancel(); else view_cancel();
    wake_view();
    return true;
}
bool epd_reader_resize(void)
{
    if (!ui.bound || !ui.info.ready || ui.pending || ui.info.error_code || font_request.context) return false;
    reader_geometry_t area = reader_geometry();
    if (!ui_command_at(INTENT_RESIZE, area.width, (document_cursor_t){0, 0}, NULL)) return false;
    ui.area = area;
    return true;
}

bool epd_reader_request_appearance(unsigned appearance)
{
    if (!ui.bound || !ui.info.ready || ui.info.error_code || font_request.context ||
        epd_reader_settings_changed() || !ui_reader_appearance_valid(appearance)) return false;
    if (ui.appearance_pending && ui.requested_appearance == appearance) return true;
    if (!ui_command_at(INTENT_REPAINT, (int)appearance, (document_cursor_t){0, 0}, NULL)) return false;
    ui.requested_appearance = appearance; ui.appearance_pending = true;
    return true;
}
static void ui_command(view_intent_t kind, int value, document_cursor_t cursor)
{
    ui_command_at(kind, value, cursor, NULL);
}
bool epd_reader_position(document_anchor_t *anchor)
{
    if (!anchor || !ui.bound || !ui.bound->position.source.unit) return false;
    *anchor = ui.bound->position.source;
    return true;
}
bool epd_reader_seek_source(const document_anchor_t *anchor)
{
    if (!anchor || !anchor->unit || !ui.bound || !ui.session) return false;
    ui_command_at(INTENT_SOURCE, 0, (document_cursor_t){0, 0}, anchor);
    return true;
}

void epd_reader_seek(unsigned page)
{ if (page) ui_command(INTENT_SEEK, page > READER_PAGE_MAX ? READER_PAGE_MAX : page, (document_cursor_t){0,0}); }
void epd_reader_turn(int direction)
{
    if (!direction) return;
    int64_t turns = (int64_t)ui.turn_delta + direction;
    ui.turn_delta = turns > INT_MAX ? INT_MAX : turns < INT_MIN + 1 ? INT_MIN + 1 : (int)turns;
    ui_command(ui.turn_delta ? INTENT_TURN : INTENT_CANCEL, ui.turn_delta, (document_cursor_t){0,0});
}
void epd_reader_seek_cursor(document_cursor_t cursor) { ui_command(INTENT_CURSOR, 0, cursor); }
void epd_reader_cancel_pending(void) { ui_command(INTENT_CANCEL, 0, (document_cursor_t){0,0}); }
bool epd_reader_waiting(void) { return ui.session && !ui.info.error_code && ui.pending; }
void epd_reader_pagination_request(bool active)
{
    rt_enter_critical();
    bool changed = mailbox.count_requested != active;
    mailbox.count_requested = active;
    rt_exit_critical();
    if (changed) wake_view();
}

void epd_reader_background_pause(bool paused)
{
    ui.paused = paused;
    rt_enter_critical(); bool changed = mailbox.paused != paused; mailbox.paused = paused; rt_exit_critical();
    if (!changed) return;
    if (paused) { index_cancel(); render_cancel_optional(); }
    wake_view();
}
void epd_reader_suspend(bool suspended)
{
    if (suspended) epd_reader_note_close();
    rt_enter_critical(); bool changed = mailbox.suspended != suspended; mailbox.suspended = suspended; rt_exit_critical();
    if (!changed) return;
    if (suspended) view_cancel();
    epd_reader_worker_wake(); wake_view();
}
bool epd_reader_suspended(void)
{
    rt_enter_critical(); bool suspended = mailbox.suspended; rt_exit_critical();
    return suspended;
}
reader_work_t epd_reader_step(uint32_t budget_ms, bool foreground, bool commit)
{
    (void)budget_ms;
    if (foreground && commit) reader_note_ui_process();
    epd_reader_collect();
    if (!ui.session || epd_reader_suspended()) return READER_WORK_IDLE;
    if (foreground && commit && ui.bound) {
        char chapter[sizeof(visible_chapter)] = {0};
        document_current_title(ui.bound->owner->document, ui.bound->page.start.record, chapter, sizeof(chapter), NULL);
        if (strcmp(chapter, visible_chapter)) {
            memcpy(visible_chapter, chapter, sizeof(visible_chapter));
            ++ui.info.revision;
        }
    }
    if (foreground && commit && !ui.paused && epd_reader_settings_changed()) epd_reader_reflow();
    if (font_request.context) {
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
        if (foreground) font_trace_gate(commit);
#endif
        /* An initial open owns no displayed reader frame. Its private font
         * shells and worker handoff can overlap the loading screen refresh. */
        if (!foreground || (!commit && (!font_request.opening || ui.bound || ui.root)))
            return READER_WORK_TARGET;
        if (!font_request_process()) return ui.info.error_code ? READER_WORK_IDLE : READER_WORK_TARGET;
        if (!commit) return READER_WORK_TARGET;
    }
    rt_enter_critical();
    reader_work_t work = mailbox.work;
    reader_error_t error = mailbox.error;
    bool current_result = mailbox.result_intent == ui.intent;
    bool rejected = mailbox.rejected;
    unsigned pages = mailbox.pages, page = mailbox.page, percent = mailbox.percent;
    bool indexing = mailbox.indexing;
    document_t *document = mailbox.document; document_cursor_t cursor = mailbox.cursor;
    uint32_t layout = mailbox.layout;
    view_frame_t *ready = foreground && commit ? mailbox.ready : NULL;
    if (ready) mailbox.ready = NULL;
    rt_exit_critical();
    if (commit && current_result && rejected) {
        frame_release(ready);
        if (error != ui.info.error_code) epd_reader_set_error_code(error);
        ui.pending = false; ui.turn_delta = 0;
        return READER_WORK_IDLE;
    }
    if (commit && current_result && !ui.configuration_failed && error != ui.info.error_code) epd_reader_set_error_code(error);
    if (current_result && error) ui.pending = false;
    if (commit && current_result && ui.bound && !ui.pending && ui.bound->owner->document == document &&
        ui.bound->layout == layout && cursor_equal(ui.bound->position.cursor, cursor) &&
        (pages != ui.info.pages || page != ui.info.page || percent != ui.info.percent || indexing != ui.info.indexing)) {
        ui.info.pages = pages; ui.info.page = page; ui.info.percent = percent;
        ui.info.indexing = indexing; ++ui.info.revision;
    }
    if (ready) {
        if (ready->owner->session != ui.id || ready->intent != ui.intent) frame_release(ready);
        else if (ready == ui.bound) { frame_release(ready); ui.pending = false; }
        else if (ready == ui.ready) frame_release(ready);
        else { frame_release(ui.ready); ui.ready = ready; ++ui.info.revision; }
    }
    return ui.pending && !ui.ready ? READER_WORK_TARGET : work;
}
static unsigned page_notes(unsigned index, const view_run_t **selected)
{
    if (!ui.bound || !ui.root) return 0;
    unsigned count = 0;
    uint32_t previous = 0;
    for (unsigned i = 0; i < ui.bound->page.count; ++i) {
        const view_run_t *run = &ui.bound->page.runs[i];
        if (run->image || !(run->style & BOOK_STYLE_NOTEREF) || !run->link) {
            previous = 0;
            continue;
        }
        if (previous == run->link) continue;
        previous = run->link;
        if (count++ == index && selected) { *selected = run; break; }
    }
    return count;
}
unsigned epd_reader_note_count(void) { return page_notes(UINT_MAX, NULL); }

bool epd_reader_open_note(unsigned index)
{
    const view_run_t *run = NULL;
    if (epd_reader_waiting()) return false;
    page_notes(index, &run);
    if (!run) return false;
    lv_area_t anchor;
    lv_obj_get_coords(ui.root, &anchor);
    anchor.x1 += run->x; anchor.x2 = anchor.x1 + run->width - 1;
    anchor.y1 += run->y; anchor.y2 = anchor.y1 + run->height - 1;
    reader_note_ui_open(lv_obj_get_screen(ui.root), ui.bound->owner->document, run->link - 1, &anchor);
    return epd_reader_note_visible();
}

static void ui_link_clicked(lv_event_t *event)
{
    view_widget_t *widget = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        view_widget_t **at = &ui.widgets;
        while (*at && *at != widget) at = &(*at)->next;
        if (*at) *at = widget->next;
        epd_app_free(widget);
        return;
    }
    if (reader_view_touch_input(event)) reader_view_touch_event(&widget->touch, event);
    bool tap = !widget->touch.moved && !widget->touch.held;
    if (widget->note && tap && lv_event_get_code(event) == LV_EVENT_RELEASED)
        lv_event_stop_bubbling(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && tap && widget->link && !epd_reader_waiting()) {
        lv_event_stop_bubbling(event);
        if (widget->note && ui.bound) {
            lv_area_t anchor;
            lv_obj_get_coords(widget->object, &anchor);
            reader_note_ui_open(lv_obj_get_screen(widget->object), ui.bound->owner->document,
                                 widget->link - 1, &anchor);
        } else epd_reader_follow_link(widget->link);
    }
}
bool epd_reader_bind(lv_obj_t *body)
{
    if (!ui.session || ui.info.error_code || epd_reader_settings_changed()) return false;
    view_frame_t *frame = ui.ready ? ui.ready : ui.bound;
    if (!frame) return false;
    if (ui.ready && frame->appearance != (ui.appearance_pending ? ui.requested_appearance : ui.appearance)) return false;
    if (!ui.ready) {
        if (ui.root) return true;
        if (ui.pending) return false;
    }
    rt_tick_t bind_started = rt_tick_get();
    lv_draw_wait_for_finish();
    unsigned draw_wait_ms = elapsed_ms(bind_started);
    /* During an appearance transaction the old tree and frame remain usable
     * until both the replacement widgets and the settings write are admitted. */
    lv_obj_t *previous_root = ui.appearance_pending ? ui.root : NULL;
    lv_obj_t *previous_bitmap = ui.bitmap;
    if (!previous_root && ui.root) lv_obj_delete(ui.root);
    reader_error_t bind_error = READER_ERROR_WIDGET_MEMORY;
    ui.root = lv_obj_create(body);
    if (!ui.root) goto bind_failed;
    epd_app_object_init(ui.root);
    lv_obj_set_style_pad_all(ui.root, 0, 0); lv_obj_set_style_border_width(ui.root, 0, 0);
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(ui.root, frame->appearance & UI_READER_NEGATIVE ? lv_color_black() : lv_color_white(), 0);
    lv_obj_remove_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui.root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(ui.root, ui_root_deleted, LV_EVENT_DELETE, NULL);
    ui.bitmap = lv_image_create(ui.root);
    if (!ui.bitmap) goto bind_failed;
    lv_image_set_src(ui.bitmap, &frame->image);
    lv_obj_add_flag(ui.bitmap, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(ui.bitmap, LV_OBJ_FLAG_CLICKABLE);
    for (unsigned i = 0; i < frame->page.count; ++i) {
        const view_run_t *run = &frame->page.runs[i];
        if (!run->link) continue;
        view_widget_t *widget = view_allocate(sizeof(*widget), EPD_APP_PSRAM);
        if (!widget) goto bind_failed;
        memset(widget, 0, sizeof(*widget)); widget->link = run->link;
        widget->note = !run->image && (run->style & BOOK_STYLE_NOTEREF) != 0;
        widget->object = lv_obj_create(ui.root);
        if (!widget->object) { epd_app_free(widget); goto bind_failed; }
        lv_obj_remove_style_all(widget->object);
        lv_obj_set_pos(widget->object, run->x, run->y);
        lv_obj_set_size(widget->object, run->width, run->height);
        if (widget->note) lv_obj_set_ext_click_area(widget->object, 7);
        lv_obj_add_flag(widget->object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(widget->object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(widget->object, ui_link_clicked, LV_EVENT_ALL, widget);
        widget->next = ui.widgets; ui.widgets = widget;
    }
    if (ui.ready && ui.appearance_pending) {
        if (!ui_settings_set(UI_SETTING_READER_APPEARANCE, frame->appearance)) {
            bind_error = READER_ERROR_IO;
            goto bind_failed;
        }
        ui.appearance = frame->appearance; ui.appearance_pending = false;
    }
    if (previous_root) lv_obj_delete(previous_root);
    lv_obj_set_pos(body, frame->full_picture ? 0 : frame->info.margin, frame->full_picture ? 0 : frame->info.top);
    lv_obj_set_size(body, frame->image.header.w, frame->image.header.h);
    lv_obj_set_size(ui.root, frame->image.header.w, frame->image.header.h);
    if (ui.ready) {
        view_frame_t *old = ui.bound; ui.bound = ui.ready; ui.ready = NULL;
        uint32_t revision = ui.info.revision;
        ui.info = frame->info; ui.info.font = epd_app_font_role(EPD_FONT_BODY);
        ui.info.revision = revision; ui.info.title = ui.title;
        snprintf(ui.title, sizeof(ui.title), "%s", frame->title);
        ui.pending = false; ui.turn_delta = 0;
        frame_ref(frame);
        rt_enter_critical(); view_frame_t *previous = mailbox.ack; mailbox.ack = frame; rt_exit_critical();
        frame_release(previous); frame_release(old); wake_view();
        epd_reader_save();
        rt_kprintf("reader commit session=%u intent=%u request_to_ready_ms=%u ready_to_ui_ms=%u bind_ms=%u draw_wait_ms=%u total_ms=%u\n",
            (unsigned)ui.id, (unsigned)ui.intent, duration_ms(frame->ready_at - frame->requested_at),
            duration_ms(bind_started - frame->ready_at), elapsed_ms(bind_started), draw_wait_ms, elapsed_ms(frame->requested_at));
    }
    return true;
bind_failed:
    if (ui.root) lv_obj_delete(ui.root);
    ui.root = previous_root; ui.bitmap = previous_root ? previous_bitmap : NULL;
    frame_release(ui.ready); ui.ready = NULL; ui.pending = false;
    epd_reader_set_error_code(bind_error);
    return false;
}
const lv_image_dsc_t *epd_reader_bound_image(lv_obj_t *body)
{
    return body && ui.root && ui.bound && !ui.bound->full_picture && lv_obj_get_parent(ui.root) == body &&
        ui.bound->appearance == ui_settings_index(UI_SETTING_READER_APPEARANCE) ? &ui.bound->image : NULL;
}
bool epd_reader_full_picture(void) { return ui.bound && ui.root && ui.bound->full_picture; }
bool epd_reader_page_text(char *text, size_t capacity)
{
    if (!text || !capacity) return false;
    text[0] = 0;
    if (!ui.bound || !ui.root) return false;
    const epd_reader_page_t *page = &ui.bound->page;
    size_t used = 0;
    for (unsigned i = 0; i < page->count; ++i) {
        const view_run_t *run = &page->runs[i];
        if (run->image || !run->bytes) continue;
        if ((size_t)run->text + run->bytes > page->bytes || used + run->bytes + 2 > capacity) { text[0] = 0; return false; }
        if (used && i && run->y != page->runs[i - 1].y) text[used++] = '\n';
        memcpy(text + used, page->text + run->text, run->bytes); used += run->bytes;
    }
    text[used] = 0; return true;
}
bool epd_reader_page_excerpt(char *text, size_t capacity)
{
    if (!text || !capacity) return false;
    text[0] = 0;
    if (!ui.bound || !ui.root) return false;
    const epd_reader_page_t *page = &ui.bound->page;
    size_t used = 0;
    for (unsigned i = 0; i < page->count && used + 1 < capacity; ++i) {
        const view_run_t *run = &page->runs[i];
        if (run->image || !run->bytes || (size_t)run->text + run->bytes > page->bytes) continue;
        const char *source = page->text + run->text;
        size_t bytes = capacity - used - 1;
        if (bytes > run->bytes) bytes = run->bytes;
        while (bytes && bytes < run->bytes && ((unsigned char)source[bytes] & 0xc0u) == 0x80u) --bytes;
        for (size_t j = 0; j < bytes; ++j)
            text[used++] = source[j] == '\n' || source[j] == '\r' ? ' ' : source[j];
        if (bytes < run->bytes) break;
    }
    text[used] = 0;
    return used > 0;
}
static document_t *ui_document(void)
{ return ui.bound ? ui.bound->owner->document : ui.session ? ui.session->document : NULL; }
bool epd_reader_can_seek_progress(void)
{ return ui.session && document_can_seek_progress(ui_document()); }
bool epd_reader_seek_progress(unsigned percent)
{
    if (percent > 100 || !epd_reader_can_seek_progress()) return false;
    ui_command(INTENT_PROGRESS, (int)percent, (document_cursor_t){0, 0});
    return true;
}
int epd_reader_toc_prepare(bool active)
{
    document_t *d = ui_document();
    if (active) {
        document_prefetch_enable(d, false);
        return document_navigation_request(d);
    }
    document_navigation_release(d); return 0;
}
unsigned epd_reader_toc_count(void) { return document_toc_count(ui_document()); }
unsigned epd_reader_toc_level(unsigned index)
{
    unsigned level = 0; document_toc_entry(ui_document(), index, NULL, 0, NULL, 0, &level); return level;
}
bool epd_reader_toc_title(unsigned index, char *title, size_t size, unsigned *level)
{ return document_toc_entry(ui_document(), index, title, size, NULL, 0, level); }
bool epd_reader_toc_seek(unsigned index)
{
    if (!ui.session || index > INT_MAX ||
        (index >= epd_reader_toc_count() && epd_reader_toc_prepare(true) != 0)) return false;
    ui_command(INTENT_TOC, (int)index, (document_cursor_t){0,0}); return true;
}
bool epd_reader_chapter_move(int direction)
{
    if (!ui.session || !direction) return false;
    ui_command(INTENT_CHAPTER, direction, (document_cursor_t){0,0}); return true;
}
bool epd_reader_chapter_title(char *title, size_t capacity, unsigned *toc_index)
{
    return ui.bound && document_current_title(ui_document(), ui.bound->page.start.record, title, capacity, toc_index);
}
bool epd_reader_follow_link(uint32_t record)
{
    if (!ui.session || !record) return false;
    ui_command(INTENT_LINK, (int)record, (document_cursor_t){0,0}); return true;
}
