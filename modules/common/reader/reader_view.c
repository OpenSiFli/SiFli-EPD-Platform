/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_memory.h"
#include "app_memory_pressure.h"
#include "platform/epd_app_font.h"
#include "reader_internal.h"
#include "reader_i18n.h"
#include "reader_font.h"
#include "reader_geometry.h"
#include "reader_view_touch_ui.h"
#include "reader_note_ui.h"
#include "reader_compute_client.h"
#include "reader_page_cache.h"
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
#define VIEW_AHEAD_WAIT_MS 2000u
#define VIEW_GLYPH_BURST 8u
#define VIEW_GLYPH_BURST_MS 4u
#define VIEW_INDEX_MAGIC 0x58445642u
#define VIEW_INDEX_VERSION 2u
#define VIEW_INDEX_CHECKPOINT_PAGES 32u
#define VIEW_INDEX_CHECKPOINT_MS 30000u
#define VIEW_LAYOUT_VERSION 3u
#define VIEW_CACHE_BUDGET (2u * 1024u * 1024u)
#define VIEW_CACHE_RESERVE (1024u * 1024u)
#define VIEW_IMAGE_BUDGET (2u * 1024u * 1024u)
#define VIEW_FRAME_BUDGET (6u * 1024u * 1024u)
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
#define VIEW_PREVIEW_ORIGIN 0x40000000u
#define VIEW_BACK_WINDOW_MIN 4096u
#define VIEW_BACK_WINDOW_MAX 16384u

typedef epd_reader_run_t view_run_t;
typedef struct {
    EPD_READER_PAGE_FIELDS(document_cursor_t);
    uint32_t loaded, generation, measured, prepared;
    document_record_t record;
    char data[DOCUMENT_TEXT_LIMIT + 1];
    epd_reader_glyph_t decoded[DOCUMENT_TEXT_LIMIT];
    document_image_t image;
    bool has_record;
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
    document_bitmap_t bitmap;
    document_image_request_t *request;
    uint32_t layout, record;
    unsigned width, height;
    int state;
    bool bound, retried;
    size_t reserved;
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
typedef struct view_frame view_frame_t;
static view_frame_t *render_frame;
typedef struct view_state {
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
    bool complete, restoring, display_pending, notified, index_loaded, index_loading, index_saved, from_cache;
    document_record_request_t *back_request;
    uint32_t back_request_offset;
    unsigned index_saved_count;
    rt_tick_t index_saved_at;
    bool index_save_failed;
    int direction;
    reader_error_t failure;
    document_cursor_t anchor;
    /* Preview publication is independent of the chapter's page-number pass. */
    bool preview;
    bool preview_cache_active;
    unsigned preview_page;
    bool preview_back_scanning, preview_back_ready;
    bool preview_back_seeking;
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
    size_t blocked_available;
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
    uint32_t font_id, font_revision;
    reader_geometry_t font_area;
    /* A rejected request is distinct from the configuration still on screen. */
    bool font_failed, font_metrics_ready;
    uint32_t failed_font_id, failed_font_revision;
    reader_geometry_t failed_font_area;
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
    document_cursor_t boundary_anchor;
    bool initial_target_ready;
    document_cursor_t initial_anchor;
    int deferred_turns, segment_turns;
    bool forward_step;
    document_cursor_t forward_from;
    struct view_state *previous, *following;
    epd_app_font_metrics_t metrics[3], italic_metrics[3];
    bool initialized, init_attempted, target_pending;
    bool init_blocked;
    uint32_t init_blocked_intent;
    size_t init_blocked_available;
    uint32_t published_intent, published_generation;
    bool frame_published;
    bool pixels_blocked, pixels_memory_blocked;
    uint32_t pixels_blocked_intent;
    unsigned pixels_blocked_priority;
    size_t pixels_blocked_available, pixels_blocked_bytes;
    bool image_memory_reclaimed, image_pressure_requested;
    uint32_t image_memory_intent, image_memory_layout, image_pressure_generation;
    rt_tick_t image_memory_started;
    unsigned neighbor_attempts;
    uint32_t neighbor_publication, neighbor_intent;
    size_t neighbor_available;
    size_t window_state_charge, window_workspace_charge;
    unsigned window_attempts;
    uint32_t window_attempt_intent;
    size_t window_attempt_available;
    uint32_t navigation_link, navigation_toc;
} view_state_t;
static view_state_t *state, *primary_state, *parked_state;
#define view (*state)

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
static void neighbor_step(void);
static void window_clear(void);
static bool window_pixels_pending(view_state_t *owner);
static bool window_prepare(view_state_t *current);
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
    if (bytes > VIEW_CACHE_BUDGET || epd_app_psram_available() < bytes + VIEW_CACHE_RESERVE) return false;
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
static window_route_t window_route(int direction)
{
    window_route_t route = {0};
    view_state_t *owner = primary_state;
    if (!owner || !owner->status.ready) return route;
    unsigned number = window_number(owner);
    int hop = 0;
    for (unsigned distance = 1; distance <= 3; ++distance) {
        document_cursor_t start; bool eof;
        if (!window_page(owner, number, &start, &eof)) break;
        bool boundary = direction > 0 ? eof : window_start(owner, start);
        if (boundary) {
            hop += direction;
            if (hop < -3 || hop > 3) break;
            view_state_t *next = window_owner(hop);
            if (!next) { route.missing_hop = hop; route.predecessor = owner; break; }
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
static unsigned window_priority(const view_state_t *owner, unsigned number)
{
    if (!primary_state) return UINT_MAX;
    int first = primary_state->direction < 0 ? -1 : 1;
    for (unsigned side = 0; side < 2; ++side) {
        window_route_t route = window_route(side ? -first : first);
        for (unsigned i = 0; i < route.count; ++i)
            if (route.pages[i].owner == owner && route.pages[i].number == number)
                return route.pages[i].distance + (side ? 3u : 0u);
    }
    return UINT_MAX;
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
    if (index_bytes + delta > VIEW_INDEX_GLOBAL || epd_app_psram_available() < delta + VIEW_CACHE_RESERVE) return false;
    size_t charge = view.window_state_charge ? delta : 0;
    if (charge && !window_charge(charge)) return false;
    void *data = epd_app_realloc(view.pages, capacity * sizeof(*view.pages), EPD_APP_PSRAM);
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
        if (capacity <= view.toc_count || index_bytes + delta > VIEW_INDEX_GLOBAL ||
            epd_app_psram_available() < delta + VIEW_CACHE_RESERVE)
        { view.toc_limited = true; return true; }
        void *p = epd_app_realloc(view.toc, capacity * sizeof(*view.toc), EPD_APP_PSRAM);
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
static int page_error(const view_page_t *p, const char *stage, int result)
{
    if (view_interrupted()) return -1;
    rt_kprintf("reader page_error session=%u intent=%u stage=%s result=%d generation=%u record=%u byte=%u kind=%u length=%u\n",
        (unsigned)view.session, (unsigned)view.intent, stage, result, (unsigned)p->generation,
        (unsigned)p->cursor.record, (unsigned)p->cursor.byte, (unsigned)p->record.kind, (unsigned)p->record.length);
    return -1;
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
        if (ready < 0) return page_error(p, "glyph_ready", (int)letter);
        if (!ready) return 0;
        ready = epd_app_font_glyph_acquire(font_for(p->record.style), letter == '\t' ? ' ' : letter, 0, false, &glyph);
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
        }
        else if (result == EPD_COMPUTE_OK) result = EPD_COMPUTE_COMPONENT_ERROR;
        if (result != EPD_COMPUTE_OK) page_error(p, "layout_result", result);
        if (result == EPD_COMPUTE_OK) view.compute_spare = view.compute_job;
        else reader_compute_release(view.compute_job);
        view.compute_job = NULL; view.compute_page = NULL;
        return result == EPD_COMPUTE_OK ? 1 : -1;
    }
    if (p->done) return 1;
    bool clipped = clip_page && cursor_equal(p->cursor, clip_end);
    bool eof = p->cursor.record == status->end || clipped;
    if (eof && !status->complete && !clipped) {
        /* Required input must advance even while speculative prefetch is paused. */
        if (target && !p->read_request) {
            p->read_request = document_record_request(view.document, p->cursor.record, true);
            if (!p->read_request) return page_error(p, "record_request", -1);
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
        if (!p->read_request) p->read_request = document_record_request(view.document, p->cursor.record, target);
        if (!p->read_request) return page_error(p, "record_request", -1);
        document_record_data_t data;
        int loaded = document_record_take(p->read_request, &data);
        if (!loaded) {
            if (trace) { p->wait_started = rt_tick_get(); p->wait_kind = 1; }
            return 0;
        }
        document_record_cancel(p->read_request); p->read_request = NULL;
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
        while (end < p->record.length && events < VIEW_METRICS_WINDOW) {
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
            if (ready < 0) return page_error(p, "font_prepare", -1);
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
    unsigned slot = font_slot(p->record.style);
    batch->font_height = (p->record.style & BOOK_STYLE_ITALIC) ? view.italic_metrics[slot].line_height : view.metrics[slot].line_height;
    batch->image_width = p->image.width; batch->image_height = p->image.height;
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
    rt_kprintf("reader near_detail session=%u intent=%u page=%u chapter=%u end=%s start=%u:%u cursor=%u:%u total_ms=%u read_ms=%u metric_ms=%u layout_ms=%u record_wait_ms=%u glyph_wait_ms=%u service_ms=%u calls=%u done=%u\n",
        (unsigned)view.session, (unsigned)view.near_intent, view.near_number, (unsigned)view.progress.chapter,
        reason, p->start.record, p->start.byte, p->cursor.record, p->cursor.byte, elapsed_ms(view.near_started),
        (unsigned)((uint64_t)view.trace_read_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.trace_metric_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.trace_layout_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.trace_record_wait * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.trace_glyph_wait * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.near_service_ticks * 1000 / RT_TICK_PER_SECOND),
        view.near_steps, (unsigned)p->done);
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

static bool prepare_fonts(view_state_t *state)
{
    static const unsigned sizes[] = {28, 30, 32, 34, 36, 40, 44, 48};
    static const unsigned margins[] = {4, 8, 12, 16, 20};
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
    view.font_area = area;
    view.font_failed = false;
    view.font_metrics_ready = false;
    view.status.margin = 24 + margins[settings[UI_SETTING_MARGIN - UI_SETTING_FONT_SIZE]];
    view.status.content_width = area.width - 2 * view.status.margin;
    view.status.top = area.top;
    view.status.bottom = area.bottom;
    view.status.line_space = view.fonts[0]->line_height * settings[UI_SETTING_LINE_SPACING - UI_SETTING_FONT_SIZE] / 5;
    uint32_t layout = font_identity ^ VIEW_LAYOUT_VERSION;
    layout = (layout ^ font_id) * 16777619u;
    layout = (layout ^ font_revision) * 16777619u;
    layout = (layout ^ view.status.content_width) * 16777619u;
    layout = (layout ^ VIEW_HEIGHT) * 16777619u;
    layout = (layout ^ (unsigned)view.status.line_space) * 16777619u;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
    {
        view.settings[i - UI_SETTING_FONT_SIZE] = settings[i - UI_SETTING_FONT_SIZE];
        layout = (layout ^ settings[i - UI_SETTING_FONT_SIZE]) * 16777619u;
    }
    view.layout = layout;
    return true;
}

static bool fonts_ready(void)
{
    if (view.font_metrics_ready) return true;
    for (unsigned i = 0; i < 3; ++i) {
        int normal = epd_app_font_metrics(view.fonts[i], &view.metrics[i]);
        int oblique = epd_app_font_metrics(view.oblique[i], &view.italic_metrics[i]);
        if (normal < 0 || oblique < 0) { fail(READER_ERROR_FONT_MEMORY); return false; }
        if (!normal || !oblique) return false;
    }
    view.status.line_space = view.metrics[0].line_height *
        view.settings[UI_SETTING_LINE_SPACING - UI_SETTING_FONT_SIZE] / 5;
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
    if (index_bytes - old_toc_bytes + new_toc_bytes > VIEW_INDEX_GLOBAL ||
        epd_app_psram_available() < new_toc_bytes + VIEW_CACHE_RESERVE) goto done;
    toc_entry_t *copy = new_toc_bytes ? epd_app_alloc(new_toc_bytes, EPD_APP_PSRAM) : NULL;
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
    if (!index_size(view.count, view.toc_count, &size) ||
        epd_app_psram_available() < size + VIEW_CACHE_RESERVE) return;
    view_index_header_t *blob = epd_app_alloc(size, EPD_APP_PSRAM);
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
    document_count_save(view.document, view.layout, &header, sizeof(header));
    view.count_saved = true;
}
typedef union { size_t bytes; uint64_t alignment; } cache_allocation_t;
static void *cache_allocate(size_t size)
{
    if (size > VIEW_CACHE_BUDGET || layout_bytes > VIEW_CACHE_BUDGET - size ||
        epd_app_psram_available() < size + sizeof(cache_allocation_t) + VIEW_CACHE_RESERVE) return NULL;
    if (!window_charge(size)) return NULL;
    cache_allocation_t *allocation = epd_app_alloc(sizeof(*allocation) + size, EPD_APP_PSRAM);
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

static bool core_initialize(void)
{
    size_t available = epd_app_psram_available();
    if (view.init_blocked && !view.selected && view.init_blocked_intent == view.intent &&
        available <= view.init_blocked_available + 128u * 1024u) return false;
    size_t required = (view.counting_only ? 1u : 4u) * sizeof(view_page_t) +
        sizeof(epd_reader_batch_t) + (view.counting_only ? 0u : 256u * sizeof(*view.pages)) + 512u;
    /* Selection is updated before primary_state during a chapter handoff. */
    if (!frame_reclaim_layout(required, view.selected ? 0u : VIEW_NEIGHBOR_PRIORITY)) {
        view.init_blocked = true; view.init_blocked_intent = view.intent;
        view.init_blocked_available = epd_app_psram_available();
        if (view.selected) { view.init_attempted = true; fail(READER_ERROR_LAYOUT_MEMORY); }
        return false;
    }
    if (view.window_state_charge && !view.window_workspace_charge) {
        size_t workspace = required - (view.counting_only ? 0u : 256u * sizeof(*view.pages));
        if (!window_charge(workspace)) {
            view.init_blocked = true; view.init_blocked_intent = view.intent;
            view.init_blocked_available = epd_app_psram_available(); return false;
        }
        view.window_workspace_charge = workspace;
    }
    view.init_blocked = false;
    view.init_attempted = true;
    rt_tick_t started = rt_tick_get();
    view.status.title = view.title; view.status.text = "";
    document_prefetch_enable(view.document, false);
    view.cache = (reader_page_cache_t){.budget = VIEW_CACHE_BUDGET / 3u, .reserve = VIEW_CACHE_RESERVE,
        .allocate = cache_allocate, .release = cache_free, .available = epd_app_psram_available};
    view.cache_blocked = view.prefetch_number = UINT_MAX;
    view.prefetch_record = UINT32_MAX;
    int compute_status;
    view.compute_spare = compute_acquire(&compute_status);
    view.index = epd_app_alloc(sizeof(*view.index), EPD_APP_PSRAM);
    if (view.index) memset(view.index, 0, sizeof(*view.index));
    if (view.counting_only) {
        if ((!view.compute_spare && compute_status != EPD_COMPUTE_BUSY) || !view.index)
        { fail(READER_ERROR_LAYOUT_MEMORY); return false; }
        view.pages = view.counting_origin;
        page_reset(view.index, (document_cursor_t){0, 0}); view.index->collect = false;
        view.initialized = true;
        return true;
    }
    view.building = epd_app_alloc(sizeof(*view.building), EPD_APP_PSRAM);
    view.display = epd_app_alloc(sizeof(*view.display), EPD_APP_PSRAM);
    view.prefetch = epd_app_alloc(sizeof(*view.prefetch), EPD_APP_PSRAM);
    if (view.building) memset(view.building, 0, sizeof(*view.building));
    if (view.display) memset(view.display, 0, sizeof(*view.display));
    if (view.prefetch) memset(view.prefetch, 0, sizeof(*view.prefetch));
    if ((!view.compute_spare && compute_status != EPD_COMPUTE_BUSY) ||
        !view.index || !view.building || !view.display || !view.prefetch || !reserve_pages(2))
    { fail(READER_ERROR_LAYOUT_MEMORY); return false; }
    view.pages[0] = (document_cursor_t){0, 0};
    page_reset(view.index, view.pages[0]); page_reset(view.building, view.pages[0]); page_reset(view.display, view.pages[0]);
    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
    view.restoring = true; view.target_pending = true; view.status.indexing = true; view.direction = 1;
    view.trace_pending = true; view.turn_started = started;
    view.initialized = true;
    return true;
}

static void images_free(view_image_t *images)
{
    while (images)
    {
        view_image_t *next = images->next;
        if (images->request) document_image_cancel(images->request);
        image_bytes -= images->reserved;
        epd_app_free(images->bitmap.memory); epd_app_free(images); images = next;
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
    reader_page_cache_clear(&view.cache);
    window_uncharge_bytes(view.window_workspace_charge); view.window_workspace_charge = 0;
}

static bool image_matches(const view_image_t *image, const view_run_t *run)
{
    return image->document == view.document && image->layout == view.layout && image->record == run->image - 1 &&
           image->width == run->width && image->height == run->height;
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
        bool keep = image == pending || image->bound || (view.display_pending && page_uses_image(view.building, image));
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
    view_image_t **at = &view.images;
    while (*at)
    {
        view_image_t *image = *at;
        if (image->request && !image->bound)
        { *at = image->next; image->next = NULL; images_free(image); }
        else at = &image->next;
    }
}
/* A target gets one reclamation pass and one pressure request, even if it has
 * several images. A wait with no submitted decoder must have its own deadline. */
static int image_memory_room(size_t bytes, bool required)
{
    size_t extra = bytes + sizeof(view_image_t) + 64u;
    size_t minimum = extra + VIEW_CACHE_RESERVE;
    if (image_bytes <= VIEW_IMAGE_BUDGET - bytes && epd_app_psram_available() >= minimum) return 1;
    if (!required) return -3;
    if (!view.image_memory_reclaimed || view.image_memory_intent != view.intent ||
        view.image_memory_layout != view.layout) {
        view.image_memory_reclaimed = true;
        view.image_pressure_requested = false;
        view.image_memory_intent = view.intent; view.image_memory_layout = view.layout;
        view.image_memory_started = rt_tick_get();
        images_prune(true);
        view_state_t *others[7]; unsigned contexts = window_contexts(primary_state, others);
        for (unsigned n = 0; n < contexts; ++n) if (others[n] != state) {
            view_image_t **at = &others[n]->images;
            while (*at) {
                view_image_t *image = *at;
                if (image->bound) { at = &image->next; continue; }
                *at = image->next; image->next = NULL; images_free(image);
            }
        }
        if (image_bytes <= VIEW_IMAGE_BUDGET - bytes) {
            if (!frame_reclaim_layout(extra, 0u) && !frame_reclaim_pages(extra))
                epd_app_font_reclaim_pixels(minimum);
        }
    }
    if (image_bytes > VIEW_IMAGE_BUDGET - bytes) goto exhausted;
    if (epd_app_psram_available() >= minimum) return 1;
    if (!view.image_pressure_requested) {
        view.image_pressure_requested = true;
        view.image_pressure_generation = app_memory_pressure_request(minimum);
    }
    if (epd_app_psram_available() >= minimum) return 1;
    app_memory_pressure_status_t pressure = app_memory_pressure_query();
    if (view.image_pressure_generation && pressure.generation == view.image_pressure_generation &&
        pressure.pending && elapsed_ms(view.image_memory_started) < VIEW_PRESSURE_WAIT_MS) return 0;
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
            size_t bytes = (((size_t)run->width * 2u + 31u) & ~(size_t)31u) * run->height;
            if (bytes > VIEW_IMAGE_BUDGET) { if (required) fail(READER_ERROR_IMAGE_MEMORY); return -3; }
            int room = image_memory_room(bytes, required);
            if (room <= 0) return room;
            image = epd_app_alloc(sizeof(*image), EPD_APP_PSRAM);
            if (!image) { if (required) fail(READER_ERROR_IMAGE_MEMORY); return -3; }
            memset(image, 0, sizeof(*image));
            image->reserved = bytes; image_bytes += bytes;
            image->document = view.document; image->layout = view.layout; image->record = run->image - 1;
            image->width = run->width; image->height = run->height;
            image->next = view.images; view.images = image;
            image->started = rt_tick_get();
            image->request = document_image_request(view.document, image->record, image->width, image->height);
            if (!image->request) image->state = -1;
            rt_kprintf("reader image record=%u size=%ux%u target=%u queued=%u\n",
                image->record, image->width, image->height, required, image->request != NULL);
        }
        if (!image->state && image->request)
        {
            image->state = document_image_take(image->request, &image->bitmap);
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
            image->request = document_image_request(view.document, image->record, image->width, image->height);
            image->state = image->request ? 0 : -1;
        }
        if (image->state < 0)
        {
            if (required) fail(READER_ERROR_IMAGE_READ);
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
    unsigned pending = page->count;
    for (unsigned i = page->warm_run; i < page->count; ++i) {
        const view_run_t *run = &page->runs[i];
        if (run->image || !run->bytes) continue;
        if ((uint32_t)run->text + run->bytes > page->bytes) { fail(READER_ERROR_LAYOUT); return false; }
        int ready = epd_app_font_prepare_text(font_for(run->style), page->text + run->text, run->bytes, true);
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
    if (view.cache.wanted != wanted) view.cache_blocked = UINT_MAX;
    reader_page_cache_window(&view.cache, wanted, view.cache_begin, view.cache_end);
    if (view.cache_blocked != UINT_MAX && epd_app_psram_available() > view.blocked_available + 128u * 1024u)
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
            *at = old->next; view.cache.bytes -= old->allocation; --view.cache.count;
            view.cache.release(old);
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
        view.blocked_available = epd_app_psram_available();
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
    return !page->done && (page->cursor.record < view.progress.end || view.progress.complete ||
        (page->cursor.record == view.progress.end && !page->read_request && page_target(page)));
}
static void preview_back_discard(void)
{
    /* Rolling entries are not predecessors of the anchor until the scan ends. */
    while (view.cache.first && view.cache.first->number < view.preview_back_base) {
        reader_cached_page_t *p = view.cache.first;
        view.cache.first = p->next; view.cache.bytes -= p->allocation; --view.cache.count;
        view.cache.release(p);
    }
}
static void preview_back_stop(bool failed)
{
    document_record_cancel(view.back_request); view.back_request = NULL;
    preview_back_discard();
    view.preview_back_scanning = false; view.preview_back_ready = true;
    view.preview_back_seeking = false;
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
    start = checkpoint.record;
    view.preview_back_end = end; view.preview_back_base = base;
    if (view.turn_pending < 0) preview_back_discard();
    view.preview_back_scanning = true;
    view.preview_back_seeking = !view.sparse_stride && start < low;
    view.preview_back_scan = start; view.preview_back_low = low;
    view.preview_back_headers = 0; view.preview_back_seek_ticks = 0;
    view.preview_back_started = rt_tick_get();
    view.prefetch_number = base - 1;
    page_reset(view.prefetch, checkpoint);
    return true;
}
static bool preview_back_seek_step(void)
{
    document_record_t record;
    uint32_t at = view.preview_back_scan;
    rt_tick_t started = rt_tick_get();
    if (view.back_request && view.back_request_offset != at)
    { document_record_cancel(view.back_request); view.back_request = NULL; }
    if (!view.back_request)
    { view.back_request = document_record_request(view.document, at, view.turn_pending < 0); view.back_request_offset = at; }
    if (!view.back_request) { preview_back_stop(true); return false; }
    document_record_data_t data;
    int ready = document_record_take(view.back_request, &data);
    if (!ready) return false;
    document_record_cancel(view.back_request); view.back_request = NULL;
    if (ready < 0) { preview_back_stop(true); return false; }
    record = data.record;
    view.preview_back_seek_ticks += rt_tick_get() - started;
    ++view.preview_back_headers;
    if (record.kind < DOCUMENT_TEXT || record.kind > DOCUMENT_METADATA ||
        at > view.preview_back_end.record ||
        view.preview_back_end.record - at < sizeof(record) ||
        record.length > view.preview_back_end.record - at - sizeof(record)) {
        preview_back_stop(true);
        return false;
    }
    uint32_t next = at + sizeof(record) + record.length;
    if (next > view.preview_back_low) {
        view.preview_back_seeking = false;
        page_reset(view.prefetch, (document_cursor_t){at, 0});
    } else view.preview_back_scan = next;
    return true;
}
static void preview_back_finish(void)
{
    view_page_t *page = view.prefetch;
    document_cursor_t next = page->cursor;
    bool target = view.turn_pending < 0 && cursor_equal(view.preview_back_end, view.anchor);
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
                *at = p->next; view.cache.bytes -= p->allocation; --view.cache.count;
                view.cache.release(p);
            } else { --p->number; at = &p->next; }
        }
        if (!reader_page_cache_put(&view.cache, view.preview_back_base - 1, view.layout, (epd_reader_page_t *)page)) {
            view.preview_back_blocked = true;
            preview_back_discard();
            if (!target) { preview_back_stop(false); return; }
        }
    }
    if (target && cursor_equal(next, view.preview_back_end) && page->count) {
        view.preview_back_scanning = false; view.preview_back_ready = true;
        view.preview_page = view.preview_back_base - 1;
        view.anchor = page->start; view.restoring = true; view.turn_pending = 0;
        view.preview_clipped = true; view.preview_end = next;
        view.prefetch = view.building; view.building = page;
        view.prefetch_number = UINT_MAX;
        page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
        target_finish();
        return;
    }
    if (cursor_equal(next, view.preview_back_end) || page->eof) {
        if (target) { preview_back_stop(true); return; }
        view.preview_back_scanning = false; view.preview_back_ready = true;
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
    if (next == view.cache_blocked || (view.progress.complete && start->record >= view.progress.end) ||
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
        view.blocked_available = epd_app_psram_available();
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
static bool compute_one(bool foreground)
{
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
                    view.blocked_available = epd_app_psram_available();
                    if (view.preview_back_scanning) preview_back_stop(true);
                } else if (p->done && !view.background_paused) preview_prefetch_finish();
            } else if (result < 0 || (p->done && !cursor_equal(p->cursor, view.pages[view.prefetch_number + 1]))) {
                view.cache_blocked = view.prefetch_number;
                view.blocked_available = epd_app_psram_available();
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
        if (!view.preview_back_scanning && back < 3 &&
            (ahead >= 3 || view.turn_pending < 0 || view.display->eof) &&
            (!view.selected || view.direction < 0 || view.turn_pending < 0 || !near_frames_pending()) &&
            !preview_ahead_pending())
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
                    view.cache_blocked = number; view.blocked_available = epd_app_psram_available();
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
                    view.cache_blocked = number; view.blocked_available = epd_app_psram_available();
                    view.prefetch_number = UINT_MAX;
                    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
                }
                return false;
            }
            int result = near_page_step();
            if (result < 0) {
                view.cache_blocked = number; view.blocked_available = epd_app_psram_available();
                rt_kprintf("reader prefetch page=%u error=read\n", number + 1);
                view.prefetch_number = UINT_MAX;
                page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
            }
            return result > 0;
        }
    }
    if (!view.index_loading && !view.complete && page_runnable(view.index) && (!view.background_paused ||
        view.restoring || view.link_target[0] || view.wanted >= view.count))
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
    return !view.complete && (view.index->done || page_runnable(view.index));
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
        reader_compute_idle();
        if (view.document) document_prefetch_enable(view.document, false);
    }
}

static void document_flow(void)
{
    document_prefetch_enable(view.document, !view.link_target[0] && (view.numeric_target ||
        (!view.background_paused && view.status.ready && !core_waiting() &&
         !near_frames_pending())));
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
    /* Keep the session across active batches. A pause requests worker-owned
     * release after the current batch, without cancelling or waiting for it. */
    if (view.background_paused) reader_compute_idle();
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
    bool complete, required, cancelled, drop_reported;
    epd_compute_buffer_t pixels;
    epd_reader_render_t renderer;
    lv_image_dsc_t image;
    reader_view_t info;
    reader_position_t position;
    char title[DOCUMENT_TITLE_MAX];
    epd_reader_page_t page;
    epd_app_font_page_t pins;
    rt_tick_t requested_at, ready_at, preparation_started, resources_at, completed_at;
    unsigned pin_run, pin_byte, render_run, render_byte, render_row;
    int32_t render_x;
    bool pins_pending, resources_ready, render_initialized, render_finished, memory_waiting, render_decorated;
    rt_tick_t memory_wait_started;
    uint32_t memory_ticket;
    rt_tick_t memory_requested;
    bool memory_reclaimed;
    bool pressure_requested;
    uint32_t pressure_generation;
    rt_tick_t pressure_started;
};
typedef enum { INTENT_NONE, INTENT_SEEK, INTENT_TURN, INTENT_CURSOR,
    INTENT_CANCEL, INTENT_TOC, INTENT_CHAPTER, INTENT_LINK, INTENT_REFLOW } view_intent_t;
typedef struct {
    view_intent_t kind;
    int value;
    document_cursor_t cursor;
    uint32_t session, serial;
    view_frame_t *origin;
} view_command_t;
static struct {
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
    bool indexing;
} mailbox;
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
    bool paused, pending, configuration_failed;
    int turn_delta;
    uint32_t font_id, font_revision;
    reader_geometry_t area;
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    char title[DOCUMENT_TITLE_MAX];
} ui;
static char visible_chapter[256];

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
void epd_reader_view_advance(void) { wake_view(); }
static void frame_ref(view_frame_t *frame)
{
    if (!frame) return;
    rt_enter_critical(); ++frame->references; rt_exit_critical();
}
static void frame_release(view_frame_t *frame)
{
    if (!frame) return;
    rt_enter_critical(); bool last = --frame->references == 0; rt_exit_critical();
    if (!last) return;
    frame_drop_note(frame);
    document_reclaim_finish(frame->memory_ticket); frame->memory_ticket = 0;
    if (frame->renderer) {
        if (render_frame == frame) render_frame = NULL;
        rt_enter_critical(); frame->next = mailbox.cleanup; mailbox.cleanup = frame; rt_exit_critical();
        return;
    }
    epd_app_font_page_release(frame->pins);
    if (frame->backing) frame_release(frame->backing);
    else {
        rt_enter_critical(); frame_bytes -= frame->pixels.capacity; rt_exit_critical();
        epd_compute_buffer_free(&frame->pixels);
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
    if (result != EPD_COMPUTE_OK) return frame_memory_failure(frame, "render_wait");
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
        if (result == EPD_COMPUTE_OK)
            result = epd_reader_render_command(frame->renderer, command, pixels, bytes, render_cancelled, frame, NULL);
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
    command.clip_w = frame->image.header.w; command.clip_h = frame->image.header.h;
    return command;
}
static unsigned source_percent(const document_progress_t *progress, const document_anchor_t *anchor)
{
    if (!anchor->unit) return 0;
    uint64_t offset = (uint64_t)anchor->offset + anchor->skip_utf8;
    if (offset > progress->source_total) offset = progress->source_total;
    uint64_t percent = progress->source_total ? offset * 100 / progress->source_total : 0;
    if (anchor->unit == 2 && progress->chapters)
        percent = ((uint64_t)anchor->chapter * 100 + percent) / progress->chapters;
    return percent > 100 ? 100 : (unsigned)percent;
}
static view_frame_t *frame_find(document_cursor_t start)
{
    for (view_frame_t *frame = view.frames; frame; frame = frame->next)
        if (frame->layout == view.layout && cursor_equal(start,
            (document_cursor_t){frame->page.start.record, frame->page.start.byte})) return frame;
    return NULL;
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
    view_frame_t *frame = epd_app_alloc(sizeof(*frame), EPD_APP_PSRAM);
    if (!frame) { window_uncharge_bytes(charge); return NULL; }
    memset(frame, 0, sizeof(*frame)); frame->window_charge = charge;
    frame->owner = state; frame->references = 1; frame->number = number;
    frame->intent = view.intent; frame->layout = view.layout; frame->generation = generation;
    frame->preparation_started = rt_tick_get();
    rt_enter_critical(); ++view.references; rt_exit_critical();
    if (!epd_reader_page_copy(&frame->page, page)) { frame_release(frame); return NULL; }
    frame->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    frame->image.header.cf = LV_COLOR_FORMAT_L8;
    frame->image.header.w = width_available(); frame->image.header.h = VIEW_HEIGHT;
    frame->image.header.stride = (width_available() + 3u) & ~3u;
    frame->image.data_size = frame->image.header.stride * VIEW_HEIGHT;
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
    while (epd_app_psram_available() < bytes + VIEW_CACHE_RESERVE) {
        unsigned score = frame_retention_distance(request_priority);
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
    }
    return true;
}
static bool frame_reclaim_pages(size_t bytes)
{
    view_state_t *contexts[7]; unsigned context_count = window_contexts(primary_state, contexts);
    for (unsigned i = 0; i < context_count && epd_app_psram_available() < bytes + VIEW_CACHE_RESERVE; ++i) {
        view_state_t *owner = contexts[i];
        if (!owner) continue;
        unsigned wanted = owner->preview_cache_active ? owner->preview_page : owner->wanted;
        reader_page_cache_t *cache = &owner->cache;
        reader_cached_page_t **at = &cache->first;
        while (*at && epd_app_psram_available() < bytes + VIEW_CACHE_RESERVE) {
            reader_cached_page_t *page = *at;
            unsigned distance = page->number > wanted ? page->number - wanted : wanted - page->number;
            if (page->layout == owner->layout && distance <= 3) { at = &page->next; continue; }
            *at = page->next; cache->bytes -= page->allocation; --cache->count;
            cache->release(page); cache->window_dirty = true;
        }
    }
    return epd_app_psram_available() >= bytes + VIEW_CACHE_RESERVE;
}
static bool frame_reserve(view_frame_t *incoming)
{
    size_t bytes = (incoming->image.data_size + 31u) & ~(size_t)31u;
    if (bytes > VIEW_FRAME_BUDGET) return false;
    for (;;) {
        rt_enter_critical(); size_t resident = frame_bytes; rt_exit_critical();
        if (resident <= VIEW_FRAME_BUDGET - bytes &&
            epd_app_psram_available() >= bytes + VIEW_CACHE_RESERVE) return true;
        unsigned score = frame_retention_distance(frame_distance(incoming));
        view_frame_t **victim = NULL;
        view_state_t *contexts[7]; unsigned context_count = window_contexts(primary_state, contexts);
        for (unsigned i = 0; i < context_count; ++i)
            for (view_frame_t **at = &contexts[i]->frames; *at; at = &(*at)->next) {
                rt_enter_critical(); unsigned references = (*at)->references; rt_exit_critical();
                unsigned priority = frame_retention_distance(frame_distance(*at));
                if (references == 1 && priority > score) { victim = at; score = priority; }
            }
        if (!victim) {
            if (window_reclaim(frame_distance(incoming))) continue;
            return resident <= VIEW_FRAME_BUDGET - bytes && frame_reclaim_pages(bytes);
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
    unsigned score = frame_retention_distance(frame_distance(incoming));
    view_frame_t **victim = NULL;
    view_state_t *contexts[7]; unsigned context_count = window_contexts(primary_state, contexts);
    size_t bytes = (incoming->image.data_size + 31u) & ~(size_t)31u;
    for (unsigned i = 0; i < context_count; ++i)
        for (view_frame_t **at = &contexts[i]->frames; *at; at = &(*at)->next) {
            view_frame_t *old = *at;
            rt_enter_critical(); unsigned references = old->references; rt_exit_critical();
            if (references != 1 || !old->complete || old->renderer || old->backing || !old->pixels.data) continue;
            if (matching && (old->pixels.capacity < bytes || ((uintptr_t)old->pixels.data & 31u) ||
                old->image.header.cf != incoming->image.header.cf ||
                old->image.header.w != incoming->image.header.w ||
                old->image.header.h != incoming->image.header.h ||
                old->image.header.stride != incoming->image.header.stride)) continue;
            unsigned priority = frame_retention_distance(frame_distance(old));
            if (priority > score) { victim = at; score = priority; }
        }
    return victim;
}
static bool frame_reuse_pixels(view_frame_t *frame)
{
    view_frame_t **victim = frame_pixel_victim(frame, true);
    if (!victim) return false;
    view_frame_t *old = *victim; *victim = old->next;
    frame_reclaim_note(old, frame->owner, frame->number, frame_distance(frame), "reuse");
    frame->pixels = old->pixels;
    memset(&old->pixels, 0, sizeof(old->pixels));
    frame_release(old); /* The transferred capacity remains in frame_bytes. */
    return true;
}
static bool frame_allocate_pixels(view_frame_t *frame)
{
    if (!epd_compute_buffer_alloc(&frame->pixels, frame->image.data_size)) {
        if (frame_reuse_pixels(frame)) return true;
        view_frame_t **victim = frame_pixel_victim(frame, false);
        if (!victim) return false;
        view_frame_t *old = *victim; *victim = old->next;
        frame_reclaim_note(old, frame->owner, frame->number, frame_distance(frame), "alloc");
        frame_release(old);
        /* Total free bytes do not prove that a contiguous block fits. Retry
         * once after a real release; do not wait for unrelated pressure work. */
        if (!epd_compute_buffer_alloc(&frame->pixels, frame->image.data_size)) return false;
    }
    rt_enter_critical(); frame_bytes += frame->pixels.capacity; rt_exit_critical();
    return true;
}
/* A bound frame stays pinned until its replacement is displayed. Only known
 * retirement/close work can release additional storage without that display. */
static int frame_storage(view_frame_t *frame, bool required)
{
    if (frame->pixels.data) return 1;
    bool reserved = frame_reserve(frame);
    if (!reserved && !frame->memory_reclaimed && frame->owner == primary_state &&
        (required || frame_distance(frame) <= 3)) {
        size_t minimum = ((frame->image.data_size + 31u) & ~(size_t)31u) + VIEW_CACHE_RESERVE;
        if (!frame->memory_ticket) {
            frame->memory_ticket = document_reclaim_request(minimum);
            frame->memory_requested = rt_tick_get();
        }
        if (frame->memory_ticket && !document_reclaim_poll(frame->memory_ticket, NULL, NULL) &&
            elapsed_ms(frame->memory_requested) < VIEW_FRAME_WAIT_MS) return 0;
        frame->memory_reclaimed = true;
        reserved = frame_reserve(frame);
        if (!reserved && epd_app_font_reclaim_pixels(minimum)) reserved = frame_reserve(frame);
    }
    if (!reserved && frame->owner == primary_state && (required || frame_distance(frame) <= 3)) {
        if (!frame->pressure_requested) {
            frame->pressure_requested = true;
            frame->pressure_started = rt_tick_get();
            frame->pressure_generation = app_memory_pressure_request(
                ((frame->image.data_size + 31u) & ~(size_t)31u) + VIEW_CACHE_RESERVE);
        }
        app_memory_pressure_status_t pressure = app_memory_pressure_query();
        if (frame->pressure_generation && pressure.generation == frame->pressure_generation &&
            pressure.pending && elapsed_ms(frame->pressure_started) < VIEW_PRESSURE_WAIT_MS) return 0;
        reserved = frame_reserve(frame);
    }
    if (reserved) {
        frame->memory_waiting = false;
        bool allocated = frame_allocate_pixels(frame);
        document_reclaim_finish(frame->memory_ticket); frame->memory_ticket = 0;
        if (!allocated) return frame_memory_failure(frame, "pixels_alloc");
        frame->image.data = frame->pixels.data;
        return 1;
    }
    document_reclaim_finish(frame->memory_ticket); frame->memory_ticket = 0;
    if (!required) return -3;
    rt_enter_critical();
    bool transient = mailbox.cleanup || mailbox.retired || mailbox.ack;
    rt_exit_critical();
    if (!transient) return frame_memory_failure(frame, "pixels_reserve");
    if (!frame->memory_waiting) {
        frame->memory_waiting = true; frame->memory_wait_started = rt_tick_get();
    }
    return elapsed_ms(frame->memory_wait_started) < VIEW_FRAME_WAIT_MS ? 0 : frame_memory_failure(frame, "pixels_retiring");
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
    int storage = frame_storage(frame, required);
    if (storage <= 0) return storage;
    if (!frame->pins) {
        frame->pins = epd_app_font_page_create();
        if (!frame->pins) return frame_memory_failure(frame, "pins_create");
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
            frame->pin_byte += utf8_letter(frame->page.text + run->text + frame->pin_byte,
                                          run->bytes - frame->pin_byte, &letter);
            if (letter != '\r' && letter != '\n') {
                if (letter == '\t') letter = ' ';
                if (epd_app_font_page_pin(frame->pins, font, letter, 0, true) < 0) return frame_memory_failure(frame, "glyph_pin");
                int ready = epd_app_font_glyph_ready(font, letter, 0, true);
                if (ready < 0) return -1;
                if (!ready) frame->pins_pending = true;
            }
            if (++operations >= 32u || elapsed_ms(started) >= VIEW_RENDER_SLICE_MS) return 0;
        }
        if (frame->pins_pending) {
            frame->pin_run = frame->pin_byte = 0; frame->pins_pending = false; return 0;
        }
        int images = images_prepare_runs(frame->page.runs, frame->page.count, required);
        if (images <= 0) return images;
        frame->resources_ready = true;
        frame->resources_at = rt_tick_get();
    }
    if (render_cancelled(frame)) return -2;
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
                view_image_t *image = image_find(run);
                if (!image || image->state != 1 || image->bitmap.image.header.cf != LV_COLOR_FORMAT_RGB565) return -1;
                unsigned stride = image->bitmap.image.header.stride;
                unsigned rows = stride ? RR_INPUT_BYTES / stride : 0;
                if (!rows) return -1;
                if (frame->render_row >= run->height) {
                    ++frame->render_run; frame->render_row = frame->render_byte = 0; continue;
                }
                unsigned height = run->height - frame->render_row < rows ? run->height - frame->render_row : rows;
                command = render_command(RR_BLIT_RGB565, frame);
                command.x = run->x; command.y = run->y + frame->render_row;
                command.width = command.src_width = run->width;
                command.height = command.src_height = height; command.src_stride = stride;
                pixels = image->bitmap.image.data + frame->render_row * stride; bytes = height * stride;
                frame->render_row += height;
            } else {
                const lv_font_t *font = font_for(run->style);
                const epd_app_font_metrics_t *metrics = run->style & BOOK_STYLE_ITALIC ?
                    &view.italic_metrics[font_slot(run->style)] : &view.metrics[font_slot(run->style)];
                if (!frame->render_byte) frame->render_x = run->x;
                if (!frame->render_decorated && (run->style & BOOK_STYLE_HEADING) &&
                    (!frame->render_run || frame->page.runs[frame->render_run - 1].y != run->y ||
                     !(frame->page.runs[frame->render_run - 1].style & BOOK_STYLE_HEADING))) {
                    command = render_command(RR_FILLRECT, frame);
                    command.x = 0; command.y = run->y;
                    command.width = epd_reader_heading_bar_width(run->height);
                    if (command.width > run->x) command.width = run->x;
                    command.height = run->height; command.fill_rgb565 = 0;
                    frame->render_decorated = true;
                    if (!command.width) continue;
                } else if (frame->render_byte < run->bytes) {
                    uint32_t letter;
                    frame->render_byte += utf8_letter(frame->page.text + run->text + frame->render_byte,
                                                     run->bytes - frame->render_byte, &letter);
                    if (letter == '\n' || letter == '\r') { ++operations; continue; }
                    if (letter == '\t') letter = ' ';
                    if (epd_app_font_glyph_acquire(font, letter, 0, true, &glyph) != 1) return -1;
                    acquired = true;
                    command = render_command(RR_BLIT_A8, frame);
                    command.x = frame->render_x + glyph.ofs_x;
                    command.y = (int32_t)run->y + metrics->line_height - metrics->base_line - glyph.box_h - glyph.ofs_y;
                    command.width = command.src_width = glyph.box_w;
                    command.height = command.src_height = glyph.box_h; command.src_stride = glyph.stride;
                    pixels = glyph.pixels; bytes = glyph.stride * glyph.box_h;
                    frame->render_x += glyph.adv_w;
                    if (!glyph.box_w || !glyph.box_h) {
                        epd_app_font_glyph_release(&glyph); ++operations; continue;
                    }
                } else {
                    ++frame->render_run; frame->render_byte = 0; frame->render_decorated = false;
                    if (!run->link) continue;
                    command = render_command(RR_FILLRECT, frame);
                    command.x = run->x; command.y = run->y + metrics->line_height - metrics->base_line + 1;
                    command.width = run->width; command.height = 1; command.fill_rgb565 = 0;
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
    frame->info.percent = source_percent(&view.progress, &frame->position.source);
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
    if (view.frame_published && view.published_intent == view.intent &&
        view.published_generation == cached->generation) return;
    view_frame_t *frame = epd_app_alloc(sizeof(*frame), EPD_APP_PSRAM);
    if (!frame) { fail(READER_ERROR_IMAGE_MEMORY); return; }
    *frame = *cached; frame->window_charge = 0;
    frame->backing = cached; frame->next = NULL; frame->references = 1; frame->renderer = NULL;
    frame->intent = view.intent; frame->pins = NULL;
    frame_ref(cached);
    rt_enter_critical(); ++view.references; rt_exit_critical();
    frame->info = view.status; frame->info.ready = true; frame->info.saved = false;
    frame->info.font = NULL; frame->info.text = ""; frame->info.title = frame->title;
    frame->info.page = view.wanted + 1;
    frame->info.percent = source_percent(&view.progress, &frame->position.source);
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
    return view.pixels_memory_blocked && (frame_bytes < view.pixels_blocked_bytes ||
        epd_app_psram_available() > view.pixels_blocked_available + 128u * 1024u);
}
static bool optional_pixels_ready_for(unsigned number)
{
    return optional_pixels_ready() || frame_priority(state, number) < view.pixels_blocked_priority;
}
static void optional_pixels_block(int result, unsigned number)
{
    view.pixels_blocked = true; view.pixels_memory_blocked = result == -3;
    view.pixels_blocked_intent = view.intent;
    view.pixels_blocked_priority = frame_priority(state, number);
    view.pixels_blocked_available = epd_app_psram_available();
    view.pixels_blocked_bytes = frame_bytes;
    rt_kprintf("reader near_block session=%u intent=%u page=%u rank=%u result=%d free=%u frames=%u\n",
        (unsigned)view.session, (unsigned)view.intent, number, view.pixels_blocked_priority, result,
        (unsigned)view.pixels_blocked_available, (unsigned)view.pixels_blocked_bytes);
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
    if (view.failure || !view.initialized || view.target_pending || view.segment_turns || data_retiring) return;
    bool target = view.display_pending;
    unsigned target_number = view.preview_cache_active ? view.preview_page : view.wanted;
    if (target && !view.selected && !optional_pixels_ready_for(target_number)) return;
    if (target) {
        view_frame_t *cached = frame_find(view.building->start);
        if (cached) {
            if (!frame_window_contains(view.rendering, (epd_reader_page_t *)view.building, target_number)) {
                frame_release(view.rendering); view.rendering = NULL;
            }
            if (view.selected) frame_publish(cached); else frame_accept(cached);
            return;
        }
        if (view.rendering && !frame_adopt(view.rendering, view.building)) {
            frame_release(view.rendering); view.rendering = NULL;
        }
        if (!view.rendering) view.rendering = frame_create((epd_reader_page_t *)view.building,
            view.preview_cache_active ? view.preview_page : view.wanted, view.building->generation);
        if (!view.rendering) { if (view.selected) fail(READER_ERROR_IMAGE_MEMORY); else optional_pixels_block(-3, target_number); return; }
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
                if (!p || !optional_pixels_ready_for(p->number) ||
                    frame_find((document_cursor_t){p->start.record, p->start.byte})) continue;
                unsigned rank = frame_priority(state, p->number);
                if (window_extra(state) && rank > 6) continue;
                if (rank < priority) { candidate = p; priority = rank; }
            }
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
        unsigned number = frame->number;
        frame_release(frame);
        if (target && view.selected && result != -2) fail(result == -3 ? READER_ERROR_IMAGE_MEMORY : READER_ERROR_IMAGE_READ);
        else if (result != -2) optional_pixels_block(result, number);
        return;
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
    if (!frame || frame->owner != state || frame->layout != view.layout || !view.display_pending ||
        !cursor_equal(view.building->start, frame->position.cursor)) return;
    view.status.percent = frame->info.percent;
    layout_accept();
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
static bool window_pixels_pending(view_state_t *owner)
{
    if (owner->rendering) return true;
    for (int direction = -1; direction <= 1; direction += 2) {
        window_route_t route = window_route(direction);
        for (unsigned i = 0; i < route.count; ++i) if (route.pages[i].owner == owner) {
            view_state_t *saved = state; state = owner;
            unsigned number = route.pages[i].number;
            document_cursor_t start; bool eof;
            bool known = window_page(owner, number, &start, &eof);
            bool pending = optional_pixels_ready_for(number) && (known ? !frame_find(start) :
                !owner->status.ready || (owner->cache_blocked != number &&
                !(number < window_number(owner) && owner->preview_back_blocked)));
            state = saved;
            if (pending) return true;
        }
    }
    return false;
}
static bool window_runnable(view_state_t *owner, view_state_t *current)
{
    if (!owner || owner->failure) return false;
    if (owner->init_blocked && owner->init_blocked_intent == current->intent &&
        epd_app_psram_available() <= owner->init_blocked_available + 128u * 1024u) return false;
    return window_pixels_pending(owner);
}
static bool window_conversion_ready(view_state_t *current)
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
    if (!pending) return true;
    /* Pixels can finish before conversion reaches EOF. Restore its optional
     * input gate after window_quiesce, so deferring admission cannot strand
     * the converter that the new document would otherwise cancel. */
    document_prefetch_enable(pending, true);
    return false;
}
static bool window_prepare(view_state_t *current)
{
    int first = current->direction < 0 ? -1 : 1;
    for (unsigned side = 0; side < 2; ++side) {
        int direction = side ? -first : first;
        window_route_t route = window_route(direction);
        for (unsigned i = 0; i < route.count; ++i) {
            view_state_t *owner = route.pages[i].owner;
            if (owner == current) continue;
            if (window_runnable(owner, current)) {
                state = owner; view.intent = current->intent; view.background_paused = false;
                if (view.status.ready && !view.display_pending && !view.rendering && !frame_find(view.display->start)) {
                    page_reset(view.building, view.display->start);
                    epd_reader_page_copy((epd_reader_page_t *)view.building, (epd_reader_page_t *)view.display);
                    view.display_pending = true;
                }
                neighbor_step(); view.direction = direction;
                state = current; return true;
            }
        }
        if (route.missing_hop < -1 || route.missing_hop > 1) {
            if (!window_conversion_ready(current)) return false;
            view_state_t *parent = route.predecessor;
            /* A published boundary can precede the document's final EOF. */
            document_progress_t progress; document_progress(parent->document, &progress);
            if (!progress.complete || progress.result || parent->rendering) continue;
            unsigned bit = direction > 0 ? 2u : 1u;
            if (parent->window_attempt_intent != current->intent ||
                epd_app_psram_available() > parent->window_attempt_available + 128u * 1024u) {
                parent->window_attempt_intent = current->intent; parent->window_attempts = 0;
            }
            if (parent->window_attempts & bit) continue;
            parent->window_attempts |= bit; parent->window_attempt_available = epd_app_psram_available();
            size_t minimum = sizeof(view_state_t) + 4u * sizeof(view_page_t) + sizeof(epd_reader_batch_t) + 256u * sizeof(document_cursor_t) + 512u;
            state = parent;
            bool admitted = window_admit(minimum, route.count + 1 + (side ? 3u : 0u));
            state = current;
            if (!admitted) continue;
            state = parent;
            view_state_t *owner = state_neighbor(document_adjacent(parent->document, direction), direction < 0);
            state = current;
            if (!owner) continue;
            if (!window_store(owner, route.missing_hop)) { state_retire(owner); continue; }
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
            window_route_t route = window_route(direction);
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
            *at = page->next; cache->bytes -= page->allocation; --cache->count;
            cache->release(page); cache->window_dirty = true;
        }
    }
    for (unsigned n = 0; n < 4 && layout_bytes > VIEW_CACHE_BUDGET - bytes; ++n)
        if (!window_reclaim(priority)) break;
    return layout_bytes <= VIEW_CACHE_BUDGET - bytes && frame_reclaim_layout(bytes, priority);
}
static void state_reflow(view_state_t *configuration)
{
    frame_cache_clear();
    view.frame_published = false;
    compute_cancel();
    for (unsigned i = 0; i < 3; ++i) {
        lv_font_t *normal = view.fonts[i], *italic = view.oblique[i], *fallback = view.fallbacks[i];
        view.fonts[i] = configuration->fonts[i]; view.oblique[i] = configuration->oblique[i];
        view.fallbacks[i] = configuration->fallbacks[i];
        configuration->fonts[i] = normal; configuration->oblique[i] = italic; configuration->fallbacks[i] = fallback;
    }
    view.font_id = configuration->font_id; view.font_revision = configuration->font_revision;
    view.font_area = configuration->font_area;
    memcpy(view.settings, configuration->settings, sizeof(view.settings));
    view.layout = configuration->layout; view.font_metrics_ready = false;
    view.status.margin = configuration->status.margin; view.status.content_width = configuration->status.content_width;
    view.status.top = configuration->status.top; view.status.bottom = configuration->status.bottom;
    view.status.line_space = configuration->status.line_space;
    view.anchor = view.status.ready ? view.display->start : view.anchor;
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
    preview_start(view.anchor);
    images_cancel_pending(); images_prune(true);
    trace_start();
    rt_enter_critical(); configuration->next = mailbox.retired; mailbox.retired = configuration; rt_exit_critical();
}
static view_state_t *state_neighbor(document_t *document, bool backward)
{
    if (!document) return NULL;
    view_state_t *neighbor = epd_app_alloc(sizeof(*neighbor), EPD_APP_PSRAM);
    if (!neighbor) { document_close(document); return NULL; }
    memset(neighbor, 0, sizeof(*neighbor));
    neighbor->document = document; neighbor->session = view.session; neighbor->intent = view.intent;
    neighbor->layout = view.layout; neighbor->font_id = view.font_id; neighbor->font_revision = view.font_revision;
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
static void pagination_reset(void)
{
    state_retire(pagination.counter);
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
            index_bytes + bytes > VIEW_INDEX_GLOBAL || epd_app_psram_available() < bytes + VIEW_CACHE_RESERVE) {
            pagination.failed = true;
            return;
        }
        pagination.ends = epd_app_alloc(bytes, EPD_APP_PSRAM);
        if (!pagination.ends) { pagination.failed = true; return; }
        pagination.bytes = bytes; index_bytes += bytes;
    }
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
        state_retire(pagination.counter); pagination.counter = NULL;
        return;
    }
    if (!pagination.counter) {
        if (!window_conversion_ready(current)) return;
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
        document_prefetch_enable(view.document, true);
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
    if (pagination.complete || pagination.failed || pagination.counter->complete) {
        state_retire(pagination.counter); pagination.counter = NULL;
    }
}
static void target_resolve(void)
{
    if (!view.target_pending) return;
    /* Pausing retained contexts may transfer the input gate to the old page. */
    document_flow();
    document_progress(view.document, &view.progress);
    uint32_t record = 0, byte = 0;
    int ready = document_target_cursor(view.document, &record, &byte);
    /* A reused complete spine retains its original opening target. Reverse
     * entry must start at its end, not before the first body record. */
    if (ready > 0 && view.backward_target && !view.boundary_target && !view.link_target[0] &&
        view.progress.complete && !view.progress.result && view.progress.chapters && !view.progress.source_begin) {
        rt_kprintf("reader back_entry session=%u intent=%u target=%u:%u end=%u\n",
            (unsigned)view.session, (unsigned)view.intent, (unsigned)record, (unsigned)byte,
            (unsigned)view.progress.end);
        record = view.progress.end; byte = 0;
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
    core_seek(local);
}
static bool neighbor_pending(view_state_t *neighbor)
{
    if (!neighbor || neighbor->failure || neighbor->init_blocked) return false;
    view_state_t *saved = state; state = neighbor;
    bool pending = !view.status.ready || view.target_pending || near_frames_pending();
    state = saved;
    return pending;
}
static view_state_t *neighbor_choose(view_state_t *current, unsigned turn)
{
    view_state_t *first = current->direction < 0 ? current->previous : current->following;
    view_state_t *second = current->direction < 0 ? current->following : current->previous;
    if (neighbor_pending(first)) return first;
    if (neighbor_pending(second)) return second;
    return turn & 1 ? first : second;
}
static void neighbors_prepare(view_state_t *current)
{
    /* document_adjacent promotes ownership and can cancel an older conversion.
     * Admit one neighboring conversion after the current conversion completes. */
    if (!view.progress.complete || view.progress.result) return;
    if (!window_conversion_ready(current)) return;
    size_t available = epd_app_psram_available();
    if (view.neighbor_intent != view.intent || view.neighbor_publication != view.progress.publication ||
        available > view.neighbor_available + 128u * 1024u) {
        view.neighbor_attempts = 0;
        view.neighbor_intent = view.intent;
        view.neighbor_publication = view.progress.publication;
        view.neighbor_available = available;
    }
    view_state_t **first = view.direction < 0 ? &current->previous : &current->following;
    view_state_t **second = view.direction < 0 ? &current->following : &current->previous;
    int direction = view.direction < 0 ? -1 : 1;
    unsigned first_bit = direction < 0 ? 1u : 2u, second_bit = direction < 0 ? 2u : 1u;
    if (!*first && !(view.neighbor_attempts & first_bit)) {
        view.neighbor_attempts |= first_bit;
        view.neighbor_available = available;
        *first = state_neighbor(document_adjacent(view.document, direction), direction < 0);
        if (*first) return;
    }
    /* Opening the other side may transfer conversion ownership. Finish the
     * admitted direction's pixels before starting that optional input. */
    if (neighbor_pending(*first)) return;
    if (!*second && !(view.neighbor_attempts & second_bit)) {
        view.neighbor_attempts |= second_bit;
        view.neighbor_available = available;
        *second = state_neighbor(document_adjacent(view.document, -direction), direction > 0);
    }
}
static void window_service(view_state_t *current, unsigned *turn)
{
    pagination_prepare(current);
    if (window_prepare(current)) {
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
        neighbor_step();
        state = current;
    }
    if (view.complete && (!current->previous || current->previous->complete || current->previous->failure) &&
        (!current->following || current->following->complete || current->following->failure))
        pagination_step(current);
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
    return target && target->complete && target->number == number &&
        cursor_equal((document_cursor_t){target->page.cursor.record, target->page.cursor.byte},
                     (document_cursor_t){page->end.record, page->end.byte}) && target->page.eof == page->eof &&
        frame_window_contains(frame, &target->page, number);
}
static void command_apply(view_command_t command, view_state_t **current)
{
    if (!state || command.session != view.session || command.kind == INTENT_NONE) return;
    rt_kprintf("reader target session=%u intent=%u kind=%u tick=%u\n",
        (unsigned)command.session, (unsigned)command.serial, (unsigned)command.kind, (unsigned)rt_tick_get());
    link_clear();
    /* UI commands are relative to the page still bound on screen. A prepared
     * chapter must not become the origin of a newer cumulative turn. */
    if (command.origin && command.origin->owner != state && command.origin->owner->displayed) {
        view_state_t *origin = command.origin->owner;
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
    for (unsigned i = 0; i < count; ++i) if (contexts[i] != state) {
        frame_release(contexts[i]->rendering); contexts[i]->rendering = NULL;
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
    case INTENT_CURSOR: view.boundary_target = view.target_pending = false; core_seek_cursor(command.cursor); break;
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
static bool worker_target_needed(void);
static void worker_context_pause(void)
{
    near_trace_finish("pause");
    document_prefetch_enable(view.document, false);
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
        worker_context_pause();
    }
    state = saved;
}
static void worker_quiesce(view_state_t *current, bool paused, bool suspended)
{
    view_state_t *saved = state;
    bool urgent = current && worker_target_needed();
    view_state_t *contexts[9]; unsigned count = window_contexts(current, contexts);
    if (parked_state) contexts[count++] = parked_state;
    if (pagination.counter) contexts[count++] = pagination.counter;
    for (unsigned i = 0; i < count; ++i) if (contexts[i]->initialized) {
        state = contexts[i];
        if (!paused && !suspended && state != parked_state && !(state == pagination.counter && urgent)) {
            if (i == 0) core_background_pause(false);
            continue;
        }
        core_background_pause(true);
        if (!suspended && i == 0 && worker_target_needed()) continue;
        worker_context_pause();
    }
    if (paused || suspended) reader_compute_idle();
    state = saved;
}
static bool worker_target_needed(void)
{
    if (view.navigation_link || view.navigation_toc) return !view.failure;
    return core_waiting() && !(view.frame_published && view.published_intent == view.intent && view.display_pending);
}
static void neighbor_step(void)
{
    if (!view.init_attempted) core_initialize();
    if (!view.initialized) return;
    target_resolve();
    if (!view.target_pending) {
        font_priority(false); core_step(2, true, false);
        frame_prepare();
        if (view.display_pending) layout_accept();
    }
}
static bool near_frames_pending(void)
{
    if (window_extra(state)) return window_pixels_pending(state);
    if (!view.status.ready) return false;
    if (view.rendering) return true;
    unsigned wanted = view.preview_cache_active ? view.preview_page : view.wanted;
    reader_cached_page_t *previous = reader_page_cache_find(&view.cache, wanted, view.layout);
    for (unsigned d = 1; d <= 3; ++d) {
        if (view.direction < 0 && wanted < d) break;
        document_cursor_t boundary = previous ?
            (document_cursor_t){previous->start.record, previous->start.byte} : view.display->start;
        if (view.direction < 0 && cursor_at_segment_start(boundary)) break;
        if (view.direction >= 0 && ((previous ? previous->eof : view.display->eof) ||
            (!view.preview_cache_active && view.complete && wanted + d >= view.count))) break;
        unsigned number = view.direction < 0 ? wanted - d : wanted + d;
        if (!optional_pixels_ready_for(number)) return false;
        reader_cached_page_t *page = reader_page_cache_find(&view.cache, number, view.layout);
        if (!page) return view.cache_blocked != number && !(view.direction < 0 && view.preview_back_blocked);
        if (!frame_find((document_cursor_t){page->start.record, page->start.byte})) return true;
        previous = page;
    }
    return false;
}
static rt_tick_t worker_idle_delay(view_state_t *current)
{
    /* A consumed slice has more work to drain. Still block for a tick so the
     * document worker can run; pending providers retain the polling fallback. */
    if (current->work_exhausted && !current->failure && !current->background_paused)
        return rt_tick_from_millisecond(1);
    if (worker_target_needed() || render_frame) return rt_tick_from_millisecond(4);
    /* A complete chapter index does not imply that nearby page layouts exist. */
    if (!current->failure && !current->background_paused && near_frames_pending())
        return rt_tick_from_millisecond(4);
    if (preview_ahead_pending()) return rt_tick_from_millisecond(4);
    if (pagination.counter && !pagination.failed) return rt_tick_from_millisecond(4);
    view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
    for (unsigned i = 0; i < count; ++i) {
        if (window_extra(contexts[i])) {
            if (window_runnable(contexts[i], current)) return rt_tick_from_millisecond(4);
        } else if ((!contexts[i]->complete && !contexts[i]->failure && !contexts[i]->init_blocked) || contexts[i]->rendering)
            return rt_tick_from_millisecond(4);
    }
    /* UI commands release the same semaphore and interrupt this wait. The
     * bounded timeout also observes providers without a completion callback. */
    return rt_tick_from_millisecond(100);
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
}
static void view_worker(void *unused)
{
    (void)unused;
    view_state_t *current = NULL;
    unsigned speculative = 0;
    for (;;) {
        rt_enter_critical();
        bool stop = mailbox.stop, replace = mailbox.replace, paused = mailbox.paused, suspended = mailbox.suspended;
        view_state_t *incoming = mailbox.incoming, *configuration = NULL;
        view_command_t command = {0};
        if (!suspended || stop) {
            configuration = mailbox.configuration; mailbox.configuration = NULL;
            command = mailbox.command;
            mailbox.command.kind = INTENT_NONE; mailbox.command.origin = NULL;
        }
        view_frame_t *ack = mailbox.ack;
        mailbox.incoming = NULL; mailbox.replace = false; mailbox.ack = NULL;
        uint32_t intent = mailbox.intent;
        rt_exit_critical();
        if (replace || stop) {
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
            if (stop) state_retire(incoming); else current = incoming;
        }
        state = primary_state = current;
        if (current && !view.init_attempted && !suspended) core_initialize();
        if (current) {
            view.selected = true; view.intent = intent;
            if (configuration) {
                pagination_reset();
                if (view.initialized && command.kind == INTENT_REFLOW) {
                    command_apply(command, &current); state = current; command.kind = INTENT_NONE;
                }
                window_clear(); state_retire(current->previous); state_retire(current->following);
                current->previous = current->following = NULL;
                if (view.initialized) state_reflow(configuration); else state_retire(configuration);
            }
            if (view.initialized) {
                if (ack) {
                    if (parked_state && parked_state != ack->owner) {
                        view_state_t *old = parked_state; parked_state = NULL;
                        old->displayed = old->parked = false; state_retire(old);
                    }
                    view_state_t *contexts[7]; unsigned count = window_contexts(current, contexts);
                    for (unsigned i = 0; i < count; ++i)
                        if (contexts[i] != ack->owner) contexts[i]->displayed = false;
                    ack->owner->displayed = true;
                }
                frame_accept(ack);
                command_apply(command, &current);
                state = primary_state = current; view.intent = intent;
                core_background_pause(paused || suspended);
            }
        } else state_retire(configuration);
        frame_release(ack); frame_release(command.origin);
        window_quiesce(current);
        worker_quiesce(current, paused, suspended);
        bool cleanup_pending = render_cleanup();
        cleanup_pending |= index_client_cleanup(stop);
        if (stop) {
            if (!cleanup_pending) break;
            rt_sem_take(mailbox.wake, rt_tick_from_millisecond(20));
            continue;
        }
        if (cleanup_pending) {
            worker_trace_wait(true, paused, suspended);
            rt_sem_take(mailbox.wake, rt_tick_from_millisecond(20));
            continue;
        }
        /* Finish nearby glyph preparation before queuing more speculative text.
         * A new foreground target still needs core_step to resolve its layout. */
        bool prepare_near = current && current->initialized && !current->failure &&
            !paused && !suspended && current->rendering && !worker_target_needed();
        bool resolve_target = current && worker_target_needed() && !current->display_pending;
        if ((render_frame || prepare_near) && !resolve_target) {
            state = render_frame ? render_frame->owner : current;
            document_prefetch_enable(view.document, false);
            view_frame_t *preparing = view.rendering;
            unsigned pin_run = preparing ? preparing->pin_run : 0;
            unsigned pin_byte = preparing ? preparing->pin_byte : 0;
            frame_prepare();
            bool advanced = view.rendering && view.rendering == preparing &&
                (view.rendering->pin_run != pin_run || view.rendering->pin_byte != pin_byte);
            state = current;
            if (render_frame || (prepare_near && current->rendering)) {
                worker_trace_wait(false, paused, suspended);
                rt_sem_take(mailbox.wake, rt_tick_from_millisecond(render_frame || advanced ? 1 : 4));
                continue;
            }
        }
        if (current && !view.initialized && !suspended) {
            rt_enter_critical();
            bool notify = mailbox.error != view.failure ||
                (view.failure && mailbox.result_intent != view.intent);
            mailbox.error = view.failure; mailbox.result_intent = view.intent; mailbox.work = READER_WORK_IDLE;
            rt_exit_critical();
            if (notify) epd_reader_ui_wake();
        }
        if (current && view.initialized && !suspended && (!paused || worker_target_needed())) {
            navigation_resolve(&current);
            target_resolve();
            reader_work_t work = READER_WORK_TARGET;
            if (!view.target_pending && !view.navigation_link && !view.navigation_toc) {
                font_priority(core_waiting());
                work = core_step(3, true, true);
                turn_continue(&current);
                frame_prepare();
            }
            reader_view_t published = view.status;
            pagination_snapshot(state, &published);
            bool notify = false;
            rt_enter_critical();
            if (mailbox.session == view.session && mailbox.intent == view.intent) {
                notify = mailbox.error != view.failure || (view.failure && mailbox.result_intent != view.intent) ||
                    (!published.indexing && (mailbox.indexing || mailbox.pages != published.pages));
                mailbox.error = view.failure; mailbox.result_intent = view.intent; mailbox.work = work;
                mailbox.pages = published.pages; mailbox.indexing = published.indexing;
                mailbox.document = view.document; mailbox.layout = view.display_layout;
                mailbox.cursor = view.display->start; mailbox.page = published.page;
                mailbox.percent = view.status.percent;
            }
            rt_exit_critical();
            if (notify) epd_reader_ui_wake();
            if (!render_frame && !view.failure && !paused && view.status.ready && !view.navigation_link &&
                !view.navigation_toc && !core_waiting() && !near_frames_pending() && !preview_ahead_pending()) {
                window_service(current, &speculative);
            }
        }
        worker_trace_wait(false, paused, suspended);
        bool idle = !current || suspended || !view.initialized ||
            (paused && !worker_target_needed());
        if (idle) rt_sem_take(mailbox.wake, RT_WAITING_FOREVER);
        else rt_sem_take(mailbox.wake, worker_idle_delay(current));
    }
    state = primary_state = NULL;
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
void epd_reader_close(void)
{
    first_font_cancel();
    if (ui.session) epd_reader_save();
    epd_reader_detach();
    frame_release(ui.bound); frame_release(ui.ready); ui.bound = ui.ready = NULL;
    rt_enter_critical();
    view_state_t *incoming = mailbox.incoming, *configuration = mailbox.configuration;
    view_frame_t *ready = mailbox.ready, *ack = mailbox.ack;
    mailbox.incoming = mailbox.configuration = NULL; mailbox.ready = mailbox.ack = NULL;
    mailbox.replace = true; mailbox.session = ++ui.id; ++mailbox.intent;
    view_frame_t *origin = mailbox.command.origin; mailbox.command.origin = NULL;
    mailbox.command.kind = INTENT_NONE;
    mailbox.error = READER_ERROR_NONE; mailbox.work = READER_WORK_IDLE;
    if (ui.session) --ui.session->references;
    rt_exit_critical();
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
    (void)restore; (void)reopen;
    epd_reader_close();
    if (!document) return false;
    if (!epd_reader_render_image_init()) {
        document_close(document); epd_reader_set_error_code(READER_ERROR_WIDGET_MEMORY); return false;
    }
    ui.requested_at = rt_tick_get();
    view_state_t *prepared = epd_app_alloc(sizeof(*prepared), EPD_APP_PSRAM);
    if (!prepared) { document_close(document); epd_reader_set_error_code(READER_ERROR_LAYOUT_MEMORY); return false; }
    memset(prepared, 0, sizeof(*prepared)); prepared->document = document;
    first_font_begin(ui.id);
    if (!epd_app_reading_font_begin() || !prepare_fonts(prepared) || !worker_start()) {
        first_font_cancel();
        state_dispose(prepared); epd_reader_set_error_code(READER_ERROR_FONT_MEMORY); return false;
    }
    snprintf(prepared->title, sizeof(prepared->title), "%s", title ? title : "");
    snprintf(ui.title, sizeof(ui.title), "%s", prepared->title);
    ui_configuration(prepared);
    ui.info = prepared->status; ui.info.font = epd_app_font_role(EPD_FONT_BODY);
    ui.info.title = ui.title; ui.info.text = ""; ui.info.indexing = true;
    ui.info.revision = 1; ui.pending = true; ui.paused = false; ui.configuration_failed = false;
    ui.save = save; ui.user = user; ui.session = prepared;
    rt_enter_critical();
    prepared->references = 1; prepared->session = ui.id; prepared->intent = mailbox.intent;
    prepared->selected = true;
    mailbox.session = ui.id; mailbox.incoming = prepared; mailbox.replace = true;
    mailbox.paused = mailbox.suspended = false;
    mailbox.error = READER_ERROR_NONE; mailbox.pages = 0; mailbox.indexing = true;
    ui.intent = mailbox.intent;
    mailbox.requested_at = ui.requested_at;
    rt_exit_critical();
    rt_kprintf("reader request session=%u intent=%u kind=open tick=%u enqueue_ms=%u\n",
        (unsigned)ui.id, (unsigned)ui.intent, (unsigned)ui.requested_at, elapsed_ms(ui.requested_at));
    wake_view();
    return true;
}
bool epd_reader_active(void) { return ui.session != NULL; }
const reader_view_t *epd_reader_get(void) { return &ui.info; }
void epd_reader_set_error_code(reader_error_t error)
{
    ui.info.error_code = error;
    snprintf(ui.info.error, sizeof(ui.info.error), "%s", reader_error_text(error));
    ++ui.info.revision;
}
bool epd_reader_settings_changed(void)
{
    if (!ui.session) return false;
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
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        ui.settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
    ui.configuration_failed = true;
    epd_reader_set_error_code(READER_ERROR_FONT_MEMORY);
}
bool epd_reader_reflow(void)
{
    if (!ui.session) return false;
    /* A replacement owns three normal/oblique pairs. Keep the requested
     * settings pending while old shells retire; this is not a font error. */
    if (!epd_app_reading_font_can_create(6)) return false;
    epd_reader_note_close();
    ui.requested_at = rt_tick_get();
    view_state_t *configuration = epd_app_alloc(sizeof(*configuration), EPD_APP_PSRAM);
    if (!configuration) { ui_configuration_rejected(); return false; }
    memset(configuration, 0, sizeof(*configuration));
    if (!prepare_fonts(configuration)) { state_dispose(configuration); ui_configuration_rejected(); return false; }
    ui_configuration(configuration); ui.configuration_failed = false;
    frame_ref(ui.bound);
    rt_enter_critical();
    view_state_t *old = mailbox.configuration; mailbox.configuration = configuration;
    view_frame_t *ready = mailbox.ready; mailbox.ready = NULL;
    view_frame_t *origin = mailbox.command.origin;
    ui.intent = ++mailbox.intent;
    mailbox.requested_at = ui.requested_at;
    mailbox.command = (view_command_t){ui.bound ? INTENT_REFLOW : INTENT_NONE, 0,
        {0, 0}, ui.id, ui.intent, ui.bound};
    rt_exit_critical();
    if (old) state_dispose(old);
    frame_release(origin);
    frame_release(ready); frame_release(ui.ready); ui.ready = NULL;
    ui.pending = true; ui.info.error_code = READER_ERROR_NONE; ui.info.error[0] = 0;
    rt_kprintf("reader request session=%u intent=%u kind=reflow tick=%u enqueue_ms=%u\n",
        (unsigned)ui.id, (unsigned)ui.intent, (unsigned)ui.requested_at, elapsed_ms(ui.requested_at));
    view_cancel(); wake_view(); return true;
}
static void ui_command(view_intent_t kind, int value, document_cursor_t cursor)
{
    if (!ui.session) return;
    epd_reader_note_close();
    ui.requested_at = rt_tick_get();
    if (kind != INTENT_TURN) ui.turn_delta = 0;
    frame_ref(ui.bound);
    rt_enter_critical();
    ui.intent = ++mailbox.intent;
    mailbox.requested_at = ui.requested_at;
    view_frame_t *origin = mailbox.command.origin;
    mailbox.command = (view_command_t){kind, value, cursor, ui.id, ui.intent, ui.bound};
    view_frame_t *ready = mailbox.ready; mailbox.ready = NULL;
    rt_exit_critical();
    frame_release(origin); frame_release(ready); frame_release(ui.ready); ui.ready = NULL;
    rt_kprintf("reader request session=%u intent=%u kind=%u value=%d tick=%u enqueue_ms=%u\n",
        (unsigned)ui.id, (unsigned)ui.intent, (unsigned)kind, value, (unsigned)ui.requested_at, elapsed_ms(ui.requested_at));
    ui.pending = true;
    if (kind == INTENT_TURN) turn_cancel(); else view_cancel();
    wake_view();
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
    rt_enter_critical();
    reader_work_t work = mailbox.work;
    reader_error_t error = mailbox.error;
    bool current_result = mailbox.result_intent == ui.intent;
    unsigned pages = mailbox.pages, page = mailbox.page, percent = mailbox.percent;
    bool indexing = mailbox.indexing;
    document_t *document = mailbox.document; document_cursor_t cursor = mailbox.cursor;
    uint32_t layout = mailbox.layout;
    view_frame_t *ready = foreground && commit ? mailbox.ready : NULL;
    if (ready) mailbox.ready = NULL;
    rt_exit_critical();
    if (commit && current_result && !ui.configuration_failed && error != ui.info.error_code) epd_reader_set_error_code(error);
    if (current_result && error) ui.pending = false;
    if (commit && current_result && ui.bound && !ui.pending && ui.bound->owner->document == document &&
        ui.bound->layout == layout && cursor_equal(ui.bound->position.cursor, cursor) &&
        (pages != ui.info.pages || page != ui.info.page || percent != ui.info.percent || indexing != ui.info.indexing)) {
        ui.info.pages = pages; ui.info.page = page; ui.info.percent = percent;
        ui.info.indexing = indexing; ++ui.info.revision;
        if (foreground) epd_reader_save();
    }
    if (ready) {
        if (ready->owner->session != ui.id || ready->intent != ui.intent) frame_release(ready);
        else if (ready == ui.bound) { frame_release(ready); ui.pending = false; }
        else if (ready == ui.ready) frame_release(ready);
        else { frame_release(ui.ready); ui.ready = ready; ++ui.info.revision; }
    }
    return ui.pending && !ui.ready ? READER_WORK_TARGET : work;
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
    if (!ui.ready && ui.root) return true;
    rt_tick_t bind_started = rt_tick_get();
    lv_draw_wait_for_finish();
    unsigned draw_wait_ms = elapsed_ms(bind_started);
    if (ui.root) lv_obj_delete(ui.root);
    ui.root = lv_obj_create(body);
    if (!ui.root) { epd_reader_set_error_code(READER_ERROR_WIDGET_MEMORY); return false; }
    epd_app_object_init(ui.root);
    lv_obj_set_style_pad_all(ui.root, 0, 0); lv_obj_set_style_border_width(ui.root, 0, 0);
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(ui.root, lv_color_white(), 0);
    lv_obj_remove_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui.root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(ui.root, ui_root_deleted, LV_EVENT_DELETE, NULL);
    ui.bitmap = lv_image_create(ui.root);
    if (!ui.bitmap) { epd_reader_detach(); return false; }
    lv_image_set_src(ui.bitmap, &frame->image);
    lv_obj_add_flag(ui.bitmap, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(ui.bitmap, LV_OBJ_FLAG_CLICKABLE);
    for (unsigned i = 0; i < frame->page.count; ++i) {
        const view_run_t *run = &frame->page.runs[i];
        if (!run->link) continue;
        view_widget_t *widget = epd_app_alloc(sizeof(*widget), EPD_APP_PSRAM);
        if (!widget) { epd_reader_detach(); epd_reader_set_error_code(READER_ERROR_WIDGET_MEMORY); return false; }
        memset(widget, 0, sizeof(*widget)); widget->link = run->link;
        widget->note = (run->style & BOOK_STYLE_NOTEREF) != 0;
        widget->object = lv_obj_create(ui.root);
        if (!widget->object) { epd_app_free(widget); epd_reader_detach(); return false; }
        lv_obj_remove_style_all(widget->object);
        lv_obj_set_pos(widget->object, run->x, run->y);
        lv_obj_set_size(widget->object, run->width, run->height);
        lv_obj_add_flag(widget->object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(widget->object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(widget->object, ui_link_clicked, LV_EVENT_ALL, widget);
        widget->next = ui.widgets; ui.widgets = widget;
    }
    lv_obj_set_pos(body, frame->info.margin, frame->info.top);
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
}
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
static document_t *ui_document(void)
{ return ui.bound ? ui.bound->owner->document : ui.session ? ui.session->document : NULL; }
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
