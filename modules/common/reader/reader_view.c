/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_memory.h"
#include "platform/epd_app_font.h"
#include "platform/epd_reader_session.h"
#include "reader_internal.h"
#include "reader_i18n.h"
#include "reader_font.h"
#include "reader_geometry.h"
#include "reader_view_touch_ui.h"
#include "reader_compute_client.h"
#include "reader_page_cache.h"
#include "epd_compute.h"
#include "ui_settings.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define VIEW_TEXT EPD_READER_TEXT
#define VIEW_RUNS EPD_READER_RUNS
#define VIEW_HEIGHT (view.view.bottom - view.view.top)
#define VIEW_BLOCK EPD_READER_BATCH
#define VIEW_METRICS_BATCH 32u
#define VIEW_GLYPH_BURST 8u
#define VIEW_GLYPH_BURST_MS 4u
#define VIEW_INDEX_MAGIC 0x58445642u
#define VIEW_INDEX_VERSION 2u
#define VIEW_INDEX_CHECKPOINT_PAGES 32u
#define VIEW_INDEX_CHECKPOINT_MS 30000u
#define VIEW_LAYOUT_VERSION 2u
#define VIEW_CACHE_BUDGET (2u * 1024u * 1024u)
#define VIEW_CACHE_RESERVE (1024u * 1024u)
#define VIEW_IMAGE_BUDGET (2u * 1024u * 1024u)

typedef epd_reader_run_t view_run_t;
typedef struct {
    EPD_READER_PAGE_FIELDS(document_cursor_t);
    uint32_t loaded, generation, measured;
    document_record_t record;
    char data[DOCUMENT_TEXT_LIMIT + 1];
    epd_reader_glyph_t decoded[DOCUMENT_TEXT_LIMIT];
    document_image_t image;
    bool has_record;
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
    rt_tick_t started;
} view_image_t;
typedef struct view_widget {
    struct view_widget *next;
    lv_obj_t *object;
    uint32_t link;
    bool image, used;
    reader_view_touch_t touch;
} view_widget_t;
typedef struct {
    uint32_t magic, version, layout, end, count, toc_count, toc_scanned, complete, checksum;
} view_index_header_t;
static struct {
    document_t *document;
    document_reader_t *index_reader, *page_reader, *aux_reader;
    document_progress_t progress;
    reader_view_t view;
    char title[DOCUMENT_TITLE_MAX];
    unsigned wanted, count, capacity, toc_count, toc_capacity;
    reader_save_fn save;
    document_t *(*reopen)(void *user, char *error, size_t size);
    void *user;
    reader_position_t saved_position;
    uint32_t toc_scanned, layout, display_layout;
    bool complete, restoring, display_pending, notified, index_loaded, index_saved, from_cache;
    unsigned index_saved_count;
    rt_tick_t index_saved_at;
    bool index_save_failed;
    int direction;
    reader_error_t failure;
    document_cursor_t anchor;
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
    size_t blocked_available;
    uint32_t cache_begin, cache_end;
    bool allow_pixels, allow_glyphs, glyph_turn, glyph_waited, background_paused;
    unsigned glyph_steps, work_text_steps, work_glyph_calls;
    rt_tick_t glyph_ticks, work_glyph_ticks, work_draw_ticks, work_logged_at;
    rt_tick_t published_at;
    bool trace_published;
    rt_tick_t turn_started, layout_ready_at, images_ready_at, cache_logged_at;
    rt_tick_t trace_read_ticks, trace_metric_ticks, trace_layout_ticks, compute_started;
    rt_tick_t work_started, work_budget;
    unsigned trace_metric_calls;
    bool compute_traced;
    bool trace_pending, trace_hit, trace_layout_ready, trace_images_ready;
    unsigned cache_hits, cache_misses, cache_logged_count;
    uint32_t prefetch_record, prefetch_publication;
    lv_font_t *fonts[3], *fallbacks[3], *retired[6];
    lv_font_t *oblique[3], *retired_oblique[3];
    uint32_t font_id, font_revision;
    /* Failed preparation keeps the active layout and suppresses automatic retries
     * of this configuration. Explicit reflow or a new configuration can retry. */
    uint32_t attempted_font_id, attempted_font_revision;
    reader_geometry_t attempted_area;
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    lv_obj_t *root;
    view_image_t *images;
    view_widget_t *widgets;
    char link_target[DOCUMENT_TEXT_LIMIT + 1];
    uint32_t link_scan;
    reader_compute_job_t *compute_job, *compute_spare;
    bool compute_foreground;
    view_page_t *compute_page;
} view;

static unsigned elapsed_ms(rt_tick_t start);

static void compute_cancel(void)
{
    reader_compute_cancel(view.compute_job);
    view.compute_job = NULL;
    view.compute_page = NULL;
    view.compute_traced = false;
}

static void fail(reader_error_t error)
{
    view.failure = error;
}
static bool cursor_before(document_cursor_t a, document_cursor_t b)
{ return a.record < b.record || (a.record == b.record && a.byte < b.byte); }
static bool cursor_equal(document_cursor_t a, document_cursor_t b)
{ return a.record == b.record && a.byte == b.byte; }
static unsigned font_slot(unsigned style)
{ return style & BOOK_STYLE_HEADING ? 2 : style & BOOK_STYLE_BOLD ? 1 : 0; }
static const lv_font_t *font_for(unsigned style)
{
    unsigned slot = font_slot(style);
    return (style & BOOK_STYLE_ITALIC) && view.oblique[slot] ? view.oblique[slot] : view.fonts[slot];
}
static unsigned width_available(void) { return view.view.content_width; }
static void page_reset(view_page_t *p, document_cursor_t start)
{
    if (view.compute_page == p) compute_cancel();
    memset(p, 0, offsetof(view_page_t, runs));
    p->done = p->eof = 0;
    p->has_record = false; p->loaded = UINT32_MAX; p->measured = 0;
    memset(&p->record, 0, sizeof(p->record));
    memset(&p->image, 0, sizeof(p->image));
    p->start = p->cursor = start;
    p->collect = true;
    p->generation = ++page_generation;
}
static bool reserve_pages(unsigned n)
{
    if (n > READER_PAGE_MAX + 1 || n > SIZE_MAX / sizeof(*view.pages)) return false;
    if (n <= view.capacity) return true;
    unsigned capacity = view.capacity ? view.capacity * 2 : 256;
    if (capacity < n) capacity = n;
    if (capacity > SIZE_MAX / sizeof(*view.pages)) capacity = n;
    void *data = epd_app_realloc(view.pages, capacity * sizeof(*view.pages), EPD_APP_PSRAM);
    if (!data) return false;
    view.pages = data; view.capacity = capacity;
    return true;
}
static bool add_toc(uint32_t record, unsigned level)
{
    if (view.toc_count == view.toc_capacity)
    {
        if (view.toc_count >= SIZE_MAX / sizeof(*view.toc))
        { fail(READER_ERROR_TOC_RANGE); return false; }
        unsigned capacity = view.toc_capacity ? view.toc_capacity * 2 : 32;
        if (capacity > SIZE_MAX / sizeof(*view.toc)) capacity = view.toc_count + 1;
        void *p = epd_app_realloc(view.toc, capacity * sizeof(*view.toc), EPD_APP_PSRAM);
        if (!p) { fail(READER_ERROR_TOC_MEMORY); return false; }
        view.toc = p; view.toc_capacity = capacity;
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
static void decode_at(view_page_t *p, unsigned at, bool trace)
{
    if (p->decoded[at].bytes) return;
    uint32_t letter;
    unsigned bytes = utf8_letter(p->data + at, p->record.length - at, &letter);
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    if (letter != '\r' && letter != '\n')
    {
        lv_font_get_glyph_dsc(font_for(p->record.style), &glyph, letter == '\t' ? ' ' : letter, 0);
        if (trace) ++view.trace_metric_calls;
    }
    p->decoded[at].letter = letter;
    p->decoded[at].bytes = bytes;
    p->decoded[at].width = glyph.adv_w;
}
static int page_step(view_page_t *p, bool indexing, document_reader_t *reader,
                     const document_progress_t *status)
{
    bool target = view.compute_foreground && (p == view.building ||
        (p == view.index && (view.restoring || view.link_target[0] || view.wanted >= view.count)));
    bool trace = target && view.trace_pending && !view.trace_layout_ready;
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
            if (p->cursor.record != p->loaded) p->has_record = false;
        }
        else result = EPD_COMPUTE_COMPONENT_ERROR;
        if (result == EPD_COMPUTE_OK) view.compute_spare = view.compute_job;
        else reader_compute_release(view.compute_job);
        view.compute_job = NULL; view.compute_page = NULL;
        return result == EPD_COMPUTE_OK ? 1 : -1;
    }
    if (p->done) return 1;
    bool eof = p->cursor.record == status->end;
    if (eof && !status->complete) return 0;
    if (!eof && (!p->has_record || p->loaded != p->cursor.record))
    {
        rt_tick_t started = trace ? rt_tick_get() : 0;
        if (p->cursor.record > status->end || status->end - p->cursor.record < sizeof(document_record_t) ||
            !document_reader_record(reader, p->cursor.record, &p->record) ||
            p->record.length > status->end - p->cursor.record - sizeof(document_record_t)) return -1;
        p->loaded = p->cursor.record; p->has_record = true;
        if (p->record.kind < DOCUMENT_TEXT || p->record.kind > DOCUMENT_IMAGE ||
            (p->cursor.byte && p->record.kind != DOCUMENT_TEXT) || p->cursor.byte > p->record.length) return -1;
        if (p->record.kind != DOCUMENT_IMAGE)
        {
            if (p->record.length > DOCUMENT_TEXT_LIMIT ||
                !document_reader_read(reader, p->cursor.record + sizeof(document_record_t), p->data, p->record.length)) return -1;
            p->data[p->record.length] = 0;
            if (p->cursor.byte < p->record.length && p->cursor.byte &&
                ((unsigned char)p->data[p->cursor.byte] & 0xc0) == 0x80) return -1;
            memset(p->decoded, 0, sizeof(p->decoded));
        }
        else if (!document_reader_image_info(reader, p->cursor.record, &p->image)) return -1;
        p->measured = p->cursor.byte;
        if (trace) view.trace_read_ticks += rt_tick_get() - started;
    }
    if (!eof && p->record.kind == DOCUMENT_TEXT)
    {
        rt_tick_t started = trace ? rt_tick_get() : 0;
        for (unsigned i = 0; i < VIEW_METRICS_BATCH && p->measured < p->record.length; ++i)
        {
            decode_at(p, p->measured, trace);
            p->measured += p->decoded[p->measured].bytes;
            if (view.work_budget && rt_tick_get() - view.work_started >= view.work_budget) break;
        }
        if (trace) view.trace_metric_ticks += rt_tick_get() - started;
        if (p->measured < p->record.length) return 1;
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
    if (!job) job = reader_compute_create(&result);
    if (!job) return result == EPD_COMPUTE_BUSY ? 0 : -1;
    epd_reader_batch_t *batch = reader_compute_data(job);
    if (!epd_reader_page_copy(&batch->page, (const epd_reader_page_t *)p))
    { reader_compute_release(job); return -1; }
    batch->generation = p->generation;
    batch->kind = eof ? EPD_READER_EOF : p->record.kind;
    batch->length = eof ? 0 : p->record.length;
    batch->style = p->record.style; batch->link = p->record.link;
    batch->next_record = p->cursor.record + sizeof(document_record_t) + p->record.length;
    batch->width = width_available(); batch->height = VIEW_HEIGHT;
    batch->line_space = view.view.line_space;
    batch->base_height = view.fonts[0]->line_height;
    batch->font_height = font_for(p->record.style)->line_height;
    batch->image_width = p->image.width; batch->image_height = p->image.height;
    batch->limit = VIEW_BLOCK;
    if (!eof && p->record.kind == DOCUMENT_TEXT) {
        memcpy(batch->data, p->data, p->record.length + 1);
        memcpy(batch->glyphs, p->decoded, p->record.length * sizeof(*p->decoded));
    }
    view.compute_job = job; view.compute_page = p;
    view.compute_traced = trace;
    if (trace) view.compute_started = rt_tick_get();
    reader_compute_submit_target(job, target);
    return 0;
}

static void save_position(void)
{
    if (!view.view.ready) return;
    reader_position_t p = {0};
    p.page = view.view.page; p.pages = view.view.pages;
    p.percent = view.view.percent; p.layout = view.display_layout;
    p.conversion_version = DOCUMENT_CONVERSION_VERSION;
    if (view.display) p.cursor = view.display->start;
    if (view.view.saved && !memcmp(&view.saved_position, &p, sizeof(p))) return;
    view.view.saved = !view.save || view.save(view.user, &p);
    if (view.view.saved) view.saved_position = p;
}

static bool prepare_fonts(void)
{
    static const unsigned sizes[] = {28, 30, 32, 34, 36, 40, 44, 48};
    static const unsigned margins[] = {4, 8, 12, 16, 20};
    lv_font_t *fonts[3] = {0}, *fallbacks[3] = {0};
    lv_font_t *oblique[3] = {0};
    uint32_t identity = 0, font_identity = 2166136261u;
    uint32_t font_id = epd_app_reading_font_id();
    uint32_t font_revision = epd_app_reading_font_revision();
    view.attempted_font_id = font_id;
    view.attempted_font_revision = font_revision;
    view.attempted_area = reader_geometry();
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        view.settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
    unsigned size = sizes[ui_settings_index(UI_SETTING_FONT_SIZE)];
    for (unsigned i = 0; i < 3; ++i)
    {
        fonts[i] = epd_app_font_create(font_id, size + (i == 2 ? 6 : 0),
                     i ? 1 : ui_settings_index(UI_SETTING_FONT_WEIGHT), &identity, &fallbacks[i]);
        if (fonts[i]) oblique[i] = document_font_oblique(fonts[i]);
        if (!fonts[i] || !oblique[i])
        {
            for (unsigned j = 0; j <= i; ++j)
            { document_font_free(oblique[j]); epd_app_font_destroy(fonts[j]); epd_app_font_destroy(fallbacks[j]); }
            return false;
        }
        fonts[i]->kerning = LV_FONT_KERNING_NONE;
        if (fallbacks[i]) fallbacks[i]->kerning = LV_FONT_KERNING_NONE;
        font_identity = (font_identity ^ identity) * 16777619u;
    }
    lv_draw_wait_for_finish();
    for (unsigned i = 0; i < 3; ++i)
    {
        if (view.retired[i])
        { document_font_free(view.oblique[i]); epd_app_font_destroy(view.fonts[i]); epd_app_font_destroy(view.fallbacks[i]); }
        else { view.retired[i] = view.fonts[i]; view.retired[i + 3] = view.fallbacks[i]; view.retired_oblique[i] = view.oblique[i]; }
        view.fonts[i] = fonts[i]; view.fallbacks[i] = fallbacks[i];
        view.oblique[i] = oblique[i];
    }
    view.view.font = view.fonts[0];
    view.font_id = font_id;
    view.font_revision = font_revision;
    view.view.margin = 24 + margins[ui_settings_index(UI_SETTING_MARGIN)];
    reader_geometry_t area = reader_geometry();
    view.view.content_width = area.width - 2 * view.view.margin;
    view.view.top = area.top;
    view.view.bottom = area.bottom;
    view.view.line_space = view.fonts[0]->line_height * ui_settings_index(UI_SETTING_LINE_SPACING) / 5;
    uint32_t layout = font_identity ^ VIEW_LAYOUT_VERSION;
    layout = (layout ^ font_id) * 16777619u;
    layout = (layout ^ font_revision) * 16777619u;
    layout = (layout ^ width_available()) * 16777619u;
    layout = (layout ^ VIEW_HEIGHT) * 16777619u;
    layout = (layout ^ (unsigned)view.view.line_space) * 16777619u;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
    {
        view.settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
        layout = (layout ^ ui_settings_index(i)) * 16777619u;
    }
    view.layout = layout;
    return true;
}

static uint32_t index_checksum(const void *data, size_t size)
{
    const unsigned char *p = data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i)
        if (i < offsetof(view_index_header_t, checksum) || i >= sizeof(view_index_header_t))
            hash = (hash ^ p[i]) * 16777619u;
    return hash;
}
static bool index_size(uint32_t count, uint32_t toc_count, size_t *size)
{
    if (!count || count > READER_PAGE_MAX ||
        (uint64_t)(count + 1u) * sizeof(document_cursor_t) +
        (uint64_t)toc_count * sizeof(toc_entry_t) > SIZE_MAX - sizeof(view_index_header_t)) return false;
    *size = sizeof(view_index_header_t) + (size_t)(count + 1u) * sizeof(document_cursor_t) +
            (size_t)toc_count * sizeof(toc_entry_t);
    return true;
}
static void index_load(void)
{
    if (view.index_loaded || !view.progress.complete) return;
    view.index_loaded = true;
    if (view.count || view.index->cursor.record || view.index->cursor.byte) return;
    rt_tick_t started = rt_tick_get();
    void *blob = NULL;
    size_t size = 0, expected;
    if (!document_index_load(view.document, view.layout, &blob, &size)) goto done;
    if (!blob || size < sizeof(view_index_header_t)) goto done;
    view_index_header_t header;
    memcpy(&header, blob, sizeof(header));
    if (header.magic != VIEW_INDEX_MAGIC || header.version != VIEW_INDEX_VERSION || header.layout != view.layout ||
        header.end != view.progress.end || header.complete > 1 || header.toc_scanned > header.end ||
        !index_size(header.count, header.toc_count, &expected) || expected != size ||
        header.checksum != index_checksum(blob, size)) goto done;
    document_cursor_t *pages = (document_cursor_t *)((char *)blob + sizeof(header));
    toc_entry_t *toc = (toc_entry_t *)(pages + header.count + 1u);
    if (pages[0].record || pages[0].byte || pages[header.count].record > header.end) goto done;
    for (unsigned i = 0; i <= header.count; ++i)
    {
        if (pages[i].record > header.end || pages[i].byte > DOCUMENT_TEXT_LIMIT ||
            (pages[i].record == header.end && pages[i].byte)) goto done;
        if (i && !cursor_before(pages[i - 1], pages[i]) &&
            !(header.complete && header.count == 1 && !header.end && !pages[i - 1].record && !pages[i - 1].byte)) goto done;
    }
    for (unsigned i = 0; i < header.toc_count; ++i)
        if (toc[i].record >= header.toc_scanned || (i && toc[i - 1].record >= toc[i].record)) goto done;
    toc_entry_t *copy = NULL;
    if (header.toc_count)
    {
        copy = epd_app_alloc((size_t)header.toc_count * sizeof(*copy), EPD_APP_PSRAM);
        if (!copy) goto done;
        memcpy(copy, toc, (size_t)header.toc_count * sizeof(*copy));
    }
    if (!reserve_pages(header.count + 1u)) { epd_app_free(copy); goto done; }
    memcpy(view.pages, pages, (size_t)(header.count + 1u) * sizeof(*pages));
    epd_app_free(view.toc); view.toc = copy;
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
    if (!view.document || !view.count || !view.view.ready || view.failure || view.index_save_failed) return;
    if (!force && epd_reader_waiting()) return;
    if (view.index_saved_count == view.count && view.index_saved == view.complete) return;
    if (!force && !view.complete && view.index_saved_count &&
        (view.count - view.index_saved_count < VIEW_INDEX_CHECKPOINT_PAGES ||
         elapsed_ms(view.index_saved_at) < VIEW_INDEX_CHECKPOINT_MS)) return;
    document_progress(view.document, &view.progress);
    if (!view.progress.complete || view.progress.result) return;
    size_t size;
    if (!index_size(view.count, view.toc_count, &size)) return;
    view_index_header_t *blob = epd_app_alloc(size, EPD_APP_PSRAM);
    if (!blob) { view.index_save_failed = true; return; }
    *blob = (view_index_header_t){VIEW_INDEX_MAGIC, VIEW_INDEX_VERSION, view.layout,
                                view.progress.end, view.count, view.toc_count,
                                view.complete ? view.progress.end : view.toc_scanned, view.complete, 0};
    document_cursor_t *pages = (document_cursor_t *)(blob + 1);
    memcpy(pages, view.pages, (size_t)(view.count + 1u) * sizeof(*pages));
    if (view.toc_count) memcpy(pages + view.count + 1u, view.toc, (size_t)view.toc_count * sizeof(*view.toc));
    blob->checksum = index_checksum(blob, size);
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
static bool readers_create(void)
{
    view.index_reader = document_reader_create(view.document);
    view.page_reader = document_reader_create(view.document);
    view.aux_reader = document_reader_create(view.document);
    return view.index_reader && view.page_reader && view.aux_reader;
}
static void *cache_allocate(size_t size) { return epd_app_alloc(size, EPD_APP_PSRAM); }
static void readers_destroy(void)
{
    document_reader_destroy(view.index_reader); view.index_reader = NULL;
    document_reader_destroy(view.page_reader); view.page_reader = NULL;
    document_reader_destroy(view.aux_reader); view.aux_reader = NULL;
}

bool epd_reader_attach(document_t *document, const char *title,
                   const reader_position_t *restore, reader_save_fn save,
                   document_t *(*reopen)(void *user, char *error, size_t size), void *user)
{
    rt_tick_t started = rt_tick_get();
    epd_reader_close();
    if (!document) return false;
    view.save = save; view.reopen = reopen; view.user = user;
    snprintf(view.title, sizeof(view.title), "%.*s", (int)sizeof(view.title) - 1, title ? title : "");
    view.view.title = view.title; view.view.text = "";
    view.view.margin = 32; view.view.font = epd_app_font_role(EPD_FONT_BODY);
    view.document = document;
    document_prefetch_enable(document, false);
    view.cache = (reader_page_cache_t){.budget = VIEW_CACHE_BUDGET, .reserve = VIEW_CACHE_RESERVE,
        .allocate = cache_allocate, .release = epd_app_free, .available = epd_app_psram_available};
    view.cache_blocked = view.prefetch_number = UINT_MAX;
    view.prefetch_record = UINT32_MAX;
    if (!epd_app_reading_font_begin())
    { epd_reader_close(); epd_reader_set_error_code(READER_ERROR_FONT_MEMORY); return false; }
    int compute_status;
    view.compute_spare = reader_compute_create(&compute_status);
    view.index = epd_app_alloc(sizeof(*view.index), EPD_APP_PSRAM);
    view.building = epd_app_alloc(sizeof(*view.building), EPD_APP_PSRAM);
    view.display = epd_app_alloc(sizeof(*view.display), EPD_APP_PSRAM);
    view.prefetch = epd_app_alloc(sizeof(*view.prefetch), EPD_APP_PSRAM);
    if ((!view.compute_spare && compute_status != EPD_COMPUTE_BUSY) ||
        !view.index || !view.building || !view.display || !view.prefetch || !reserve_pages(2) || !prepare_fonts() || !readers_create())
    { epd_reader_close(); epd_reader_set_error_code(READER_ERROR_LAYOUT_MEMORY); return false; }
    view.pages[0] = (document_cursor_t){0, 0};
    page_reset(view.index, view.pages[0]); page_reset(view.building, view.pages[0]); page_reset(view.display, view.pages[0]);
    page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
    if (restore && restore->conversion_version == DOCUMENT_CONVERSION_VERSION) view.anchor = restore->cursor;
    view.restoring = true; view.view.indexing = true; view.direction = 1;
    view.trace_pending = true; view.turn_started = started;
    rt_kprintf("reader attach setup_ms=%u\n", elapsed_ms(started));
    ++view.view.revision;
    return true;
}

static void images_free(view_image_t *images)
{
    while (images)
    {
        view_image_t *next = images->next;
        if (images->request) document_image_cancel(images->request);
        epd_app_free(images->bitmap.memory); epd_app_free(images); images = next;
    }
}
static void root_deleted(lv_event_t *event)
{
    if (view.root != lv_event_get_target_obj(event)) return;
    view.root = NULL;
    while (view.widgets)
    {
        view_widget_t *next = view.widgets->next;
        epd_app_free(view.widgets); view.widgets = next;
    }
    for (view_image_t *image = view.images; image; image = image->next) image->bound = false;
}
void epd_reader_detach(void)
{
    lv_draw_wait_for_finish();
    if (view.root) { lv_obj_delete(view.root); view.root = NULL; }
    images_free(view.images); view.images = NULL;
}
void epd_reader_close(void)
{
    compute_cancel();
    reader_compute_release(view.compute_spare);
    view.compute_spare = NULL;
    if (view.document) { save_position(); index_save(true); }
    epd_reader_detach();
    readers_destroy();
    document_close(view.document);
    for (unsigned i = 0; i < 3; ++i)
    { document_font_free(view.oblique[i]); document_font_free(view.retired_oblique[i]);
      epd_app_font_destroy(view.fonts[i]); epd_app_font_destroy(view.fallbacks[i]); }
    for (unsigned i = 0; i < 6; ++i) epd_app_font_destroy(view.retired[i]);
    epd_app_free(view.pages); epd_app_free(view.toc);
    epd_app_free(view.index); epd_app_free(view.building); epd_app_free(view.display);
    epd_app_free(view.prefetch);
    reader_page_cache_clear(&view.cache);
    memset(&view, 0, sizeof(view));
}
bool epd_reader_active(void) { return view.document != NULL; }
const reader_view_t *epd_reader_get(void)
{
    if (view.view.error_code)
        snprintf(view.view.error, sizeof(view.view.error), "%s", reader_error_text(view.view.error_code));
    return &view.view;
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
    return !view.restoring && !view.link_target[0] && view.wanted < view.count &&
        (!view.view.ready || view.display_layout != view.layout || view.view.page != view.wanted + 1 ||
         !cursor_equal(view.display->start, view.pages[view.wanted]));
}
static void images_prune(bool optional)
{
    view_image_t **at = &view.images;
    while (*at)
    {
        view_image_t *image = *at;
        bool keep = image->bound || (view.display_pending && page_uses_image(view.building, image)) ||
                    (view.view.ready && view.display_layout == view.layout && page_uses_image(view.display, image));
        if (!optional)
        {
            if (view.view.ready && view.display_layout == view.layout && page_uses_image(view.display, image)) keep = true;
            for (reader_cached_page_t *p = view.cache.first; p; p = p->next)
            {
                unsigned distance = p->number > view.wanted ? p->number - view.wanted : view.wanted - p->number;
                if (p->layout != view.layout || distance > 3) continue;
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
static view_image_t *image_find(const view_run_t *run)
{
    for (view_image_t *image = view.images; image; image = image->next)
        if (image_matches(image, run)) return image;
    return NULL;
}
static void images_cancel_pending(void)
{
    view_image_t **at = &view.images;
    while (*at)
    {
        view_image_t *image = *at;
        if (image->request && !image->bound)
        { *at = image->next; image->next = NULL; images_free(image); }
        else at = &image->next;
    }
}
/* Pending pixels are polled once per service call, never in the compute loop. */
static int images_prepare_runs(const view_run_t *runs, unsigned count, bool required)
{
    int ready = 1;
    for (unsigned i = 0; i < count; ++i)
    {
        const view_run_t *run = &runs[i];
        if (!run->image) continue;
        view_image_t *image = image_find(run);
        if (!image)
        {
            if (!required) {
                size_t bytes = (size_t)run->width * run->height * 2u;
                size_t used = 0;
                for (view_image_t *p = view.images; p; p = p->next)
                    used += (size_t)p->width * p->height * 2u;
                if (bytes > VIEW_IMAGE_BUDGET || used > VIEW_IMAGE_BUDGET - bytes ||
                    epd_app_psram_available() < bytes + VIEW_CACHE_RESERVE) return 0;
            }
            image = epd_app_alloc(sizeof(*image), EPD_APP_PSRAM);
            if (!image && required)
            { images_prune(true); image = epd_app_alloc(sizeof(*image), EPD_APP_PSRAM); }
            if (!image) { if (required) fail(READER_ERROR_IMAGE_MEMORY); return -1; }
            memset(image, 0, sizeof(*image));
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
            images_prune(true);
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
static unsigned elapsed_ms(rt_tick_t start)
{ return (unsigned)((uint64_t)(rt_tick_get() - start) * 1000 / RT_TICK_PER_SECOND); }

static void trace_start(void)
{
    document_prefetch_enable(view.document, false);
    view.trace_pending = true; view.trace_hit = false;
    view.trace_layout_ready = view.trace_images_ready = false;
    view.trace_read_ticks = view.trace_metric_ticks = view.trace_layout_ticks = 0;
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
        rt_kprintf("reader target_detail page=%u read_ms=%u layout_ms=%u metric_ms=%u metric_calls=%u warm_bytes=%u\n",
            view.wanted + 1, (unsigned)((uint64_t)view.trace_read_ticks * 1000 / RT_TICK_PER_SECOND),
            (unsigned)((uint64_t)view.trace_layout_ticks * 1000 / RT_TICK_PER_SECOND),
            (unsigned)((uint64_t)view.trace_metric_ticks * 1000 / RT_TICK_PER_SECOND),
            view.trace_metric_calls, warmed);
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
    uint32_t record = view.restoring ? view.anchor.record :
        view.wanted < view.count ? view.pages[view.wanted].record : view.index->cursor.record;
    document_chapter_range_t range;
    view.cache_begin = 0;
    view.cache_end = view.progress.end;
    if (document_chapter_range(view.document, record, &range)) {
        view.cache_begin = range.previous_begin;
        view.cache_end = range.next_end;
        if (view.prefetch_record != record || view.prefetch_publication != range.publication) {
            document_chapter_prefetch(view.document, record);
            view.prefetch_record = record; view.prefetch_publication = range.publication;
        }
    }
    if (view.cache.wanted != view.wanted) view.cache_blocked = UINT_MAX;
    reader_page_cache_window(&view.cache, view.wanted, view.cache_begin, view.cache_end);
    if (view.cache_blocked != UINT_MAX && epd_app_psram_available() > view.blocked_available + 128u * 1024u)
        view.cache_blocked = UINT_MAX;
}
static reader_cached_page_t *cache_store(unsigned number, view_page_t *page)
{
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
    if (!view.allow_glyphs || (epd_reader_waiting() && !view.display_pending)) return NULL;
    if (view.display_pending) {
        reader_cached_page_t *p = reader_page_cache_find(&view.cache, view.wanted, view.layout);
        if (p && p->warm_run < p->count) return p;
    }
    /* The displayed page has already drawn its glyphs. Warm the next turns. */
    for (unsigned d = 1; d <= 3; ++d) {
        unsigned numbers[2] = {view.wanted + d, view.wanted >= d ? view.wanted - d : UINT_MAX};
        if (view.direction < 0) { unsigned swap = numbers[0]; numbers[0] = numbers[1]; numbers[1] = swap; }
        for (unsigned i = 0; i < 2; ++i) {
            reader_cached_page_t *p = reader_page_cache_find(&view.cache, numbers[i], view.layout);
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
    const view_run_t *run = &p->runs[p->warm_run];
    if (run->image || p->warm_byte >= run->bytes) {
        ++p->warm_run; p->warm_byte = 0;
        if (p->warm_run == p->count)
            rt_kprintf("reader glyph page=%u done=1 failed=%u\n", p->number + 1, p->warm_failed);
        return true;
    }
    uint32_t letter;
    p->warm_byte += utf8_letter(reader_page_cache_text(p) + run->text + p->warm_byte,
                                run->bytes - p->warm_byte, &letter);
    rt_tick_t started = rt_tick_get();
    if (!view.glyph_waited) {
        lv_draw_wait_for_finish(); view.glyph_waited = true;
        view.work_draw_ticks += rt_tick_get() - started;
    }
    rt_tick_t prepared = rt_tick_get();
    if (!epd_app_font_prepare(font_for(run->style), letter, true)) ++p->warm_failed;
    rt_tick_t finished = rt_tick_get();
    view.work_glyph_ticks += finished - prepared;
    ++view.work_glyph_calls;
    if (view.glyph_turn) {
        view.glyph_ticks += finished - started;
        rt_tick_t limit = rt_tick_from_millisecond(VIEW_GLYPH_BURST_MS);
        if (view.glyph_ticks >= (limit ? limit : 1)) view.glyph_turn = false;
    }
    return true;
}
static bool cache_target_pending(void)
{
    return view.allow_glyphs && view.display_pending && !cache_contains(view.wanted) &&
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
    rt_kprintf("reader work page=%u text_steps=%u glyph_calls=%u glyph_ms=%u draw_wait_ms=%u\n",
        view.wanted + 1, view.work_text_steps, view.work_glyph_calls,
        (unsigned)((uint64_t)view.work_glyph_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)view.work_draw_ticks * 1000 / RT_TICK_PER_SECOND));
    view.work_text_steps = view.work_glyph_calls = 0;
    view.work_glyph_ticks = view.work_draw_ticks = 0;
    view.work_logged_at = rt_tick_get();
}
static bool page_runnable(const view_page_t *page)
{
    return !page->done && (page->cursor.record < view.progress.end || view.progress.complete);
}
static void restore_target(void)
{
    if (view.restoring && view.count && (view.complete || cursor_before(view.anchor, view.pages[view.count])))
    {
        unsigned lo = 0, hi = view.count;
        while (lo + 1 < hi)
        {
            unsigned mid = lo + (hi - lo) / 2;
            if (!cursor_before(view.anchor, view.pages[mid])) lo = mid; else hi = mid;
        }
        view.wanted = lo; view.restoring = false;
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
static void index_finish(void)
{
    if (!view.index->count && view.index->eof && view.count) { view.complete = true; return; }
    if (!reserve_pages(view.count + 2)) { fail(READER_ERROR_PAGE_MEMORY); return; }
    unsigned number = view.count++;
    view.pages[view.count] = view.index->cursor;
    view.complete = view.index->eof;
    restore_target();
    if (view.index->collect)
    {
        if (!view.restoring && !view.link_target[0] && number == view.wanted)
        {
            view_page_t *old = view.building;
            view.building = view.index; view.index = old;
            view.display_pending = true; view.notified = false;
        }
        else cache_store(number, view.index);
    }
    page_reset(view.index, view.pages[view.count]);
    view.index->collect = view.restoring || view.count <= view.wanted + 3 ||
        (view.pages[view.count].record >= view.cache_begin && view.pages[view.count].record < view.cache_end);
}

static bool link_process(void)
{
    if (!view.link_target[0]) return false;
    if (view.link_scan == view.progress.end)
    {
        if (view.progress.complete) { view.link_target[0] = 0; fail(READER_ERROR_LINK_MISSING); }
        return false;
    }
    document_record_t r;
    if (view.link_scan > view.progress.end || view.progress.end - view.link_scan < sizeof(r) ||
        !document_reader_record(view.aux_reader, view.link_scan, &r) ||
        r.length > view.progress.end - view.link_scan - sizeof(r))
    { fail(READER_ERROR_LINK_READ); return false; }
    if (r.kind == DOCUMENT_ANCHOR && r.length <= DOCUMENT_TEXT_LIMIT)
    {
        char text[DOCUMENT_TEXT_LIMIT + 1];
        if (!document_reader_read(view.aux_reader, view.link_scan + sizeof(r), text, r.length))
        { fail(READER_ERROR_LINK_READ); return false; }
        text[r.length] = 0;
        size_t id_length = strlen(text);
        bool definition = id_length + 1 < r.length && !text[id_length + 1];
        if (definition && (!strcmp(text, view.link_target) ||
            (view.link_target[0] == '#' && !strcmp(text, view.link_target + 1))))
        {
            view.anchor = (document_cursor_t){view.link_scan, 0};
            view.restoring = true; view.link_target[0] = 0;
            view.display_pending = view.notified = false;
            return true;
        }
    }
    view.link_scan += sizeof(r) + r.length;
    return true;
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

static bool compute_one(bool foreground)
{
    view.compute_foreground = foreground || (view.background_paused && epd_reader_waiting());
    if (cache_target_pending()) cache_store(view.wanted, view.building);
    if (view.compute_job)
    {
        int status;
        /* The worker owns a separate batch; nearby glyphs can progress while it runs. */
        if (!reader_compute_poll(view.compute_job, &status)) return cache_warm();
        view_page_t *p = view.compute_page;
        int result = page_step(p, p == view.index, NULL, &view.progress);
        if (p == view.index)
        {
            if (result < 0) { fail(READER_ERROR_LAYOUT); return false; }
            if (p->done && (!view.background_paused || view.restoring ||
                view.link_target[0] || view.wanted >= view.count)) index_finish();
        }
        else if (p == view.building)
        {
            if (result < 0 || (p->done && !cursor_equal(p->cursor, view.pages[view.wanted + 1])))
                return page_read_failed();
            if (p->done) { view.display_pending = true; view.notified = false; }
        }
        else if (p == view.prefetch) {
            if (result < 0 || (p->done && !cursor_equal(p->cursor, view.pages[view.prefetch_number + 1]))) {
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
    if (view.background_paused && !epd_reader_waiting()) return false;
    /* A paused batch can finish an index page before its boundary is appended. */
    if (!view.complete && view.index->done && (!view.background_paused ||
        view.restoring || view.link_target[0] || view.wanted >= view.count)) {
        index_finish();
        return true;
    }
    bool pending = view.display_pending;
    restore_target();
    if (!pending && view.display_pending) return true;
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
    if (!view.background_paused && !view.restoring && !view.link_target[0] && view.wanted < view.count) {
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
            int result = page_step(view.prefetch, false, view.page_reader, &view.progress);
            if (result < 0) {
                view.cache_blocked = number; view.blocked_available = epd_app_psram_available();
                rt_kprintf("reader prefetch page=%u error=read\n", number + 1);
                view.prefetch_number = UINT_MAX;
                page_reset(view.prefetch, (document_cursor_t){UINT32_MAX, 0});
            }
            return result > 0;
        }
    }
    if (!view.complete && page_runnable(view.index) && (!view.background_paused ||
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
    if (view.link_target[0]) return view.link_scan < view.progress.end || view.progress.complete;
    if (view.restoring)
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
    if (!view.restoring && !view.link_target[0] && view.wanted < view.count) {
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
        if (view.view.error_code != view.failure)
        { epd_reader_set_error_code(view.failure); ++view.view.revision; }
        view.view.indexing = false;
        return;
    }
    bool finished = view.complete && view.view.pages != view.count;
    view.view.indexing = !view.complete;
    if (view.complete) view.view.pages = view.count;
    if (foreground && view.display_pending && !view.notified && images_ready(view.building))
    {
        view.notified = true; ++view.view.revision;
        if (view.trace_pending) {
            view.published_at = rt_tick_get(); view.trace_published = true;
            rt_kprintf("reader publish page=%u ready_wait_ms=%u\n", view.wanted + 1,
                view.trace_images_ready ? (unsigned)((uint64_t)(view.published_at - view.images_ready_at) *
                    1000 / RT_TICK_PER_SECOND) : 0);
        }
    }
    if (finished && foreground && view.view.ready && view.display_layout == view.layout) save_position();
}
void epd_reader_background_pause(bool paused)
{
    view.background_paused = paused;
    if (paused) {
        view.allow_glyphs = false;
        reader_compute_idle();
        if (view.document) document_prefetch_enable(view.document, false);
    }
}

reader_work_t epd_reader_step(uint32_t budget_ms, bool foreground, bool commit)
{
    if (!view.document) return READER_WORK_IDLE;
    if (!view.background_paused && epd_reader_settings_changed())
    {
        if (!foreground || !commit) return READER_WORK_IDLE;
        epd_reader_reflow();
    }
    rt_tick_t started = rt_tick_get();
    rt_tick_t budget = rt_tick_from_millisecond(budget_ms > INT_MAX ? INT_MAX : (int)budget_ms);
    if (!budget) budget = 1;
    document_progress(view.document, &view.progress);
    if (view.progress.result)
    {
        document_status_t status;
        document_status(view.document, &status);
        fail(status.error_code ? status.error_code : READER_ERROR_IO);
    }
    document_prefetch_enable(view.document, !view.background_paused && view.view.ready && !epd_reader_waiting());
    if (!view.background_paused || epd_reader_waiting()) index_load();
    cache_sync();
    view.allow_pixels = commit;
    /* First publication returns immediately; later calls may fill the bind wait. */
    bool present = foreground && commit && epd_reader_waiting() && !view.notified;
    view.allow_glyphs = !view.background_paused;
    view.glyph_waited = false;
    view.work_started = started; view.work_budget = budget;
    if (budget_ms && !view.failure)
        while ((!present || !view.display_pending) && compute_one(foreground)) {
            if (view.display_pending && !view.trace_layout_ready) trace_ready();
            if (view.failure || rt_tick_get() - started >= budget) break;
        }
    view.work_budget = 0;
    if (!view.failure)
    {
        trace_ready();
        if (view.allow_pixels) {
            if (!view.background_paused) images_prune(false);
            if (view.display_pending) images_prepare(view.building, true);
            else if (!view.background_paused && view.view.ready && !view.root) images_prepare(view.display, true);
        }
        /* Publish the target before starting optional I/O. */
        if (!view.background_paused && view.view.ready && !epd_reader_waiting())
        {
            if (view.allow_pixels)
                for (reader_cached_page_t *p = view.cache.first; p && !view.failure; p = p->next) {
                    unsigned d = p->number > view.wanted ? p->number - view.wanted : view.wanted - p->number;
                    if (d <= 3) images_prepare_runs(p->runs, p->count, false);
                }
            index_save(false);
        }
        trace_ready();
        if (!present) trace_work();
        if (view.cache_logged_count != view.cache.count && !view.background_paused && elapsed_ms(view.cache_logged_at) >= 1000) {
            unsigned ahead = 0, back = 0;
            reader_page_cache_span(&view.cache, view.wanted, view.layout, &ahead, &back);
            rt_kprintf("reader cache page=%u pages=%u ahead=%u back=%u bytes=%u window=%u..%u\n",
                view.wanted + 1, view.cache.count, ahead, back, (unsigned)view.cache.bytes,
                view.cache_begin, view.cache_end);
            view.cache_logged_count = view.cache.count; view.cache_logged_at = rt_tick_get();
        }
    }
    if (view.failure) { compute_cancel(); images_cancel_pending(); }
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
    if (view.background_paused || !view.compute_job) reader_compute_idle();
    return READER_WORK_IDLE;
}

static void link_clicked(lv_event_t *event)
{
    view_widget_t *widget = lv_event_get_user_data(event);
    if (reader_view_touch_input(event)) reader_view_touch_event(&widget->touch, event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && !widget->touch.moved && !widget->touch.held &&
        widget->link && !epd_reader_waiting())
        epd_reader_follow_link(widget->link);
}
static view_widget_t *widget_take(bool image)
{
    for (view_widget_t *w = view.widgets; w; w = w->next)
        if (!w->used && w->image == image) { w->used = true; return w; }
    view_widget_t *w = epd_app_alloc(sizeof(*w), EPD_APP_PSRAM);
    if (!w) return NULL;
    memset(w, 0, sizeof(*w)); w->image = image; w->used = true;
    w->object = image ? lv_image_create(view.root) : lv_label_create(view.root);
    if (!w->object) { epd_app_free(w); return NULL; }
    lv_obj_add_flag(w->object, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(w->object, link_clicked, LV_EVENT_ALL, w);
    w->next = view.widgets; view.widgets = w;
    return w;
}
bool epd_reader_bind(lv_obj_t *body)
{
    if (!view.document || view.failure) return false;
    if (!view.display_pending && view.root) return true;
    if (!view.display_pending && view.display_layout != view.layout) return false;
    if ((!view.display_pending && !view.view.ready) || (view.display_pending && !view.notified)) return false;
    view_page_t *page = view.display_pending ? view.building : view.display;
    if (!images_ready(page)) return false;
    if (!view.root)
    {
        view.root = lv_obj_create(body);
        if (!view.root) { fail(READER_ERROR_WIDGET_MEMORY); publish_state(true); return false; }
        epd_app_object_init(view.root);
        lv_obj_set_style_pad_all(view.root, 0, 0); lv_obj_set_style_border_width(view.root, 0, 0);
        lv_obj_set_style_bg_opa(view.root, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(view.root, lv_color_white(), 0);
        lv_obj_remove_flag(view.root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(view.root, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(view.root, root_deleted, LV_EVENT_DELETE, NULL);
    }
    for (view_widget_t *w = view.widgets; w; w = w->next) w->used = false;
    for (unsigned i = 0; i < page->count; ++i)
        if (!widget_take(page->runs[i].image != 0))
        { fail(READER_ERROR_WIDGET_MEMORY); publish_state(true); return false; }
    lv_draw_wait_for_finish();
    for (view_image_t *image = view.images; image; image = image->next) image->bound = false;
    for (view_widget_t *w = view.widgets; w; w = w->next) w->used = false;
    for (unsigned i = 0; i < page->count; ++i)
    {
        view_run_t *run = &page->runs[i];
        view_widget_t *w = widget_take(run->image != 0);
        if (!w) { fail(READER_ERROR_WIDGET_MEMORY); publish_state(true); return false; }
        lv_obj_t *object = w->object;
        w->link = run->link;
        if (run->image)
        {
            view_image_t *image = image_find(run);
            if (!image) { fail(READER_ERROR_IMAGE_READ); publish_state(true); return false; }
            image->bound = true;
            lv_image_set_src(object, &image->bitmap.image);
        }
        else
        {
            lv_obj_set_style_text_font(object, font_for(run->style), 0);
            lv_obj_set_style_text_color(object, lv_color_black(), 0);
            lv_obj_set_style_text_line_space(object, 0, 0);
            lv_obj_set_style_text_letter_space(object, 0, 0);
            lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
            lv_obj_set_size(object, run->width + 2, run->height);
            lv_label_set_text_static(object, page->text + run->text);
            lv_obj_set_style_text_decor(object, run->link ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE, 0);
        }
        lv_obj_set_pos(object, run->x, run->y);
        if (run->link) lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    for (view_widget_t *w = view.widgets; w; w = w->next)
        if (!w->used)
        {
            w->link = 0;
            lv_obj_add_flag(w->object, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(w->object, LV_OBJ_FLAG_CLICKABLE);
            if (w->image) lv_image_set_src(w->object, NULL);
            else
            {
                lv_label_set_text_static(w->object, "");
                lv_obj_set_style_text_font(w->object, epd_app_font(), 0);
            }
        }
    lv_obj_set_size(view.root, width_available(), VIEW_HEIGHT);
    lv_obj_set_pos(body, view.view.margin, view.view.top);
    lv_obj_set_size(body, width_available(), VIEW_HEIGHT);
    lv_obj_remove_flag(view.root, LV_OBJ_FLAG_HIDDEN);
    if (view.display_pending)
    {
        view_page_t *old = view.display; view.display = view.building; view.building = old;
        view.display_layout = view.layout;
        view.view.page = view.wanted + 1;
        view.view.percent = view.progress.complete && view.progress.end ?
            (unsigned)((uint64_t)view.display->cursor.record * 100 / view.progress.end) : 0;
        view.view.ready = true; view.display_pending = view.notified = false;
        document_prefetch_enable(view.document, !view.background_paused);
        reader_cached_page_t *saved = cache_store(view.wanted, view.display);
        if (view.trace_pending) {
            rt_kprintf("reader bind page=%u total_ms=%u ui_wait_ms=%u bind_wait_ms=%u glyph_runs=%u/%u glyph_failed=%u\n",
                view.wanted + 1, elapsed_ms(view.turn_started),
                view.trace_images_ready ? elapsed_ms(view.images_ready_at) : 0,
                view.trace_published ? elapsed_ms(view.published_at) : 0,
                saved ? saved->warm_run : 0, view.display->count, saved ? saved->warm_failed : 0);
            view.trace_pending = false;
        }
        page_reset(view.building, (document_cursor_t){UINT32_MAX, 0});
        save_position();
    }
    images_prune(false);
    for (unsigned i = 0; i < 3; ++i) { document_font_free(view.retired_oblique[i]); view.retired_oblique[i] = NULL; }
    for (unsigned i = 0; i < 6; ++i) { epd_app_font_destroy(view.retired[i]); view.retired[i] = NULL; }
    return true;
}

bool epd_reader_save(void) { save_position(); index_save(true); return view.view.saved; }
bool epd_reader_reflow(void)
{
    if (!view.document) return false;
    index_save(true);
    document_status_t status;
    document_status(view.document, &status);
    if (status.error[0])
    {
        char error[96] = {0};
        document_t *replacement = view.reopen ? view.reopen(view.user, error, sizeof(error)) : NULL;
        if (!replacement)
        {
            reader_error_t reason = view.reopen ? document_open_error() : READER_ERROR_UNAVAILABLE;
            fail(reason ? reason : READER_ERROR_UNAVAILABLE);
            return false;
        }
        compute_cancel();
        images_cancel_pending();
        readers_destroy();
        document_close(view.document);
        view.document = replacement;
        if (!readers_create()) { fail(READER_ERROR_CURSOR_MEMORY); publish_state(true); return false; }
        document_service_process();
        view.toc_count = view.toc_scanned = 0;
    }
    if (!prepare_fonts()) return false;
    compute_cancel();
    save_position();
    view.anchor = view.view.ready ? view.display->start : view.anchor;
    view.view.error[0] = 0;
    view.view.error_code = READER_ERROR_NONE;
    view.failure = READER_ERROR_NONE;
    view.link_target[0] = 0;
    view.link_scan = 0;
    view.count = 0; view.complete = false; view.restoring = true; view.display_pending = view.notified = false;
    view.index_loaded = view.index_saved = view.from_cache = false;
    view.index_saved_count = 0; view.index_save_failed = false;
    view.toc_count = view.toc_scanned = 0;
    reader_page_cache_clear(&view.cache); view.cache_blocked = UINT_MAX;
    view.prefetch_number = UINT_MAX;
    trace_start();
    page_reset(view.index, (document_cursor_t){0, 0});
    page_reset(view.building, (document_cursor_t){0, 0});
    images_cancel_pending();
    images_prune(true);
    view.view.pages = 0; view.view.indexing = true;
    ++view.view.revision;
    return true;
}
bool epd_reader_settings_changed(void)
{
    if (view.attempted_font_id != epd_app_reading_font_id() ||
        view.attempted_font_revision != epd_app_reading_font_revision()) return true;
    reader_geometry_t area = reader_geometry();
    if (view.attempted_area.width != area.width || view.attempted_area.top != area.top ||
        view.attempted_area.bottom != area.bottom) return true;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        if (view.settings[i - UI_SETTING_FONT_SIZE] != ui_settings_index(i)) return true;
    return false;
}
void epd_reader_seek(unsigned page)
{
    if (!page || !view.document || view.failure) return;
    if (view.view.pages && page > view.view.pages) page = view.view.pages;
    if (page > READER_PAGE_MAX) page = READER_PAGE_MAX;
    if (!view.restoring && !view.link_target[0] && view.wanted == page - 1) return;
    compute_cancel();
    trace_start();
    if (page - 1 != view.wanted) view.direction = page - 1 > view.wanted ? 1 : -1;
    view.restoring = false; view.wanted = page - 1; view.link_target[0] = 0;
    view.display_pending = view.notified = false;
    page_reset(view.building, view.wanted < view.count ? view.pages[view.wanted] : (document_cursor_t){0, 0});
    restore_target();
    images_prune(false);
}
void epd_reader_cancel_pending(void)
{
    if (!view.view.ready || view.failure) return;
    compute_cancel();
    if (view.display_layout != view.layout)
    {
        /* The visible page number belongs to the previous layout. Resolve its
         * original text cursor again, even if a new page is already prepared. */
        view.anchor = view.display->start;
        view.restoring = true;
        view.link_target[0] = 0; view.link_scan = 0;
        view.display_pending = view.notified = false;
        images_cancel_pending();
        images_prune(true);
        return;
    }
    epd_reader_seek(view.view.page);
}
bool epd_reader_waiting(void)
{
    return !view.failure && (!view.view.ready || view.restoring || view.link_target[0] ||
        view.wanted + 1 != view.view.page || view.display_pending ||
        view.display_layout != view.layout);
}
unsigned epd_reader_toc_count(void) { return view.toc_count; }
unsigned epd_reader_toc_level(unsigned index) { return index < view.toc_count ? view.toc[index].level : 0; }
bool epd_reader_page_text(char *text, size_t capacity)
{
    if (!text || !capacity) return false;
    text[0] = 0;
    const view_page_t *page = view.display;
    if (!view.root || !view.view.ready || !page || !page->collect) return false;
    size_t used = 0;
    for (unsigned i = 0; i < page->count; ++i)
    {
        const view_run_t *run = &page->runs[i];
        if (run->image || !run->bytes) continue;
        if ((size_t)run->text + run->bytes > page->bytes || used + run->bytes + 2 > capacity)
        { text[0] = 0; return false; }
        if (used && i && run->y != page->runs[i - 1].y) text[used++] = '\n';
        memcpy(text + used, page->text + run->text, run->bytes);
        used += run->bytes;
    }
    text[used] = 0;
    return true;
}
bool epd_reader_toc_title(unsigned index, char *title, size_t size, unsigned *level)
{
    if (index >= view.toc_count) return false;
    document_record_t r;
    char data[DOCUMENT_TEXT_LIMIT + 1];
    if (!document_reader_record(view.aux_reader, view.toc[index].record, &r) || r.kind != DOCUMENT_ANCHOR ||
        r.style != view.toc[index].level || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_reader_read(view.aux_reader, view.toc[index].record + sizeof(r), data, r.length)) return false;
    data[r.length] = 0;
    size_t start = strlen(data) + 1;
    if (start >= r.length || !data[start] || !memchr(data + start, 0, r.length - start)) return false;
    snprintf(title, size, "%s", data + start);
    if (level) *level = view.toc[index].level;
    return true;
}
bool epd_reader_toc_seek(unsigned index)
{
    if (index >= view.toc_count) return false;
    document_record_t r;
    if (!document_reader_record(view.aux_reader, view.toc[index].record, &r) || r.kind != DOCUMENT_ANCHOR ||
        r.style != view.toc[index].level || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_reader_read(view.aux_reader, view.toc[index].record + sizeof(r), view.link_target, r.length)) return false;
    view.link_target[r.length] = 0;
    trace_start();
    compute_cancel();
    view.link_scan = 0;
    view.display_pending = view.notified = false;
    images_cancel_pending();
    images_prune(true);
    return true;
}
bool epd_reader_chapter_move(int direction)
{
    document_chapter_range_t range;
    if (!view.document || !view.display || !view.view.ready ||
        !document_chapter_range(view.document, view.display->start.record, &range) || !range.indexed) return false;
    uint32_t target = direction < 0 ? range.previous_begin : range.end;
    if ((direction < 0 && target == range.begin) || target >= view.progress.end) return false;
    epd_reader_seek_cursor((document_cursor_t){target, 0});
    return true;
}
bool epd_reader_chapter_title(char *title, size_t capacity, unsigned *toc_index)
{
    if (!title || !capacity) return false;
    title[0] = 0;
    document_chapter_range_t range;
    if (!view.document || !view.display || !view.view.ready ||
        !document_chapter_range(view.document, view.display->start.record, &range) || !range.indexed) return false;
    if (view.chapter_cached && view.chapter_begin == range.begin && view.chapter_count == view.toc_count)
    {
        snprintf(title, capacity, "%s", view.chapter_title);
        if (toc_index) *toc_index = view.chapter_toc;
        return title[0] != 0;
    }
    char root[DOCUMENT_TEXT_LIMIT + 1], text[DOCUMENT_TEXT_LIMIT + 1];
    document_record_t record;
    uint32_t offset = range.begin;
    bool found = false;
    for (unsigned n = 0; n < 64 && offset < range.end; ++n)
    {
        if (range.end - offset < sizeof(record) || !document_reader_record(view.aux_reader, offset, &record) ||
            record.length > range.end - offset - sizeof(record)) return false;
        if (record.kind == DOCUMENT_ANCHOR && record.length <= DOCUMENT_TEXT_LIMIT)
        {
            if (!document_reader_read(view.aux_reader, offset + sizeof(record), root, record.length)) return false;
            root[record.length] = 0;
            size_t length = strlen(root);
            if (length && length + 1 < record.length && !root[length + 1] && !strchr(root, '#'))
            { found = true; break; }
        }
        offset += sizeof(record) + record.length;
    }
    if (!found) return false;
    size_t length = strlen(root);
    for (unsigned i = 0; i < view.toc_count; ++i)
    {
        if (!document_reader_record(view.aux_reader, view.toc[i].record, &record) || record.length > DOCUMENT_TEXT_LIMIT ||
            !document_reader_read(view.aux_reader, view.toc[i].record + sizeof(record), text, record.length)) return false;
        text[record.length] = 0;
        if (strncmp(root, text, length) || (text[length] && text[length] != '#')) continue;
        size_t name = strlen(text) + 1;
        if (name >= record.length || !memchr(text + name, 0, record.length - name)) continue;
        snprintf(title, capacity, "%s", text + name);
        snprintf(view.chapter_title, sizeof(view.chapter_title), "%s", text + name);
        view.chapter_begin = range.begin; view.chapter_count = view.toc_count; view.chapter_toc = i;
        view.chapter_cached = true;
        if (toc_index) *toc_index = i;
        return title[0] != 0;
    }
    view.chapter_title[0] = 0; view.chapter_cached = true;
    view.chapter_begin = range.begin; view.chapter_count = view.toc_count; view.chapter_toc = UINT_MAX;
    return false;
}
bool epd_reader_follow_link(uint32_t record)
{
    document_record_t r;
    if (!record || !document_reader_record(view.aux_reader, record - 1, &r) || r.kind != DOCUMENT_LINK || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_reader_read(view.aux_reader, record - 1 + sizeof(r), view.link_target, r.length)) return false;
    view.link_target[r.length] = 0;
    if (strstr(view.link_target, "://") || !strncmp(view.link_target, "mailto:", 7))
    { view.link_target[0] = 0; return false; }
    trace_start();
    compute_cancel();
    view.link_scan = 0;
    view.display_pending = view.notified = false;
    images_cancel_pending();
    images_prune(true);
    return true;
}

void epd_reader_seek_cursor(document_cursor_t cursor)
{
    if (!view.document || view.failure) return;
    trace_start();
    compute_cancel();
    view.anchor = cursor; view.restoring = true;
    view.link_target[0] = 0; view.link_scan = 0;
    view.display_pending = view.notified = false;
    images_cancel_pending();
    page_reset(view.building, (document_cursor_t){0, 0});
    restore_target();
    images_prune(true);
}

void epd_reader_collect(void) { document_service_process(); }
void epd_reader_set_error_code(reader_error_t error)
{
    view.view.error_code = error;
    snprintf(view.view.error, sizeof(view.view.error), "%s", reader_error_text(error));
}
