/* SPDX-License-Identifier: Apache-2.0 */
/* Production page control and layout; host-only font metrics and job transport. */
#include "epd_reader_layout.h"
#include "reader_page_type.inc"
/* Win32 legacy memory-model macros collide with production's local names. */
#undef near
#undef far

typedef struct { unsigned unused; } lv_font_t;
typedef struct { uint32_t adv_w; } epd_app_font_glyph_t;
typedef struct { unsigned line_height; } flow_metrics_t;
typedef struct reader_compute_job {
    epd_reader_batch_t batch;
    bool submitted;
    int result;
} reader_compute_job_t;
static unsigned flow_jobs, flow_eofs;
static bool flow_hold_compute;
static reader_compute_job_t *reader_compute_create(int *result)
{
    reader_compute_job_t *job = calloc(1, sizeof(*job)); assert(job);
    ++flow_jobs; *result = EPD_COMPUTE_OK; return job;
}
static epd_reader_batch_t *reader_compute_data(reader_compute_job_t *job) { return &job->batch; }
static void reader_compute_submit_target(reader_compute_job_t *job, bool foreground)
{
    (void)foreground;
    assert(!job->submitted);
    if (job->batch.kind == EPD_READER_EOF) ++flow_eofs;
    job->result = epd_reader_layout(&job->batch) == 0 ? EPD_COMPUTE_OK : EPD_COMPUTE_COMPONENT_ERROR;
    job->submitted = true;
}
static bool reader_compute_poll(reader_compute_job_t *job, int *result)
{ assert(job->submitted); if (flow_hold_compute) return false; job->submitted = false; *result = job->result; return true; }
static void reader_compute_release(reader_compute_job_t *job)
{ if (job) { assert(flow_jobs); --flow_jobs; free(job); } }
static void reader_compute_cancel(reader_compute_job_t *job) { reader_compute_release(job); }
static int epd_app_font_prepare_text(const lv_font_t *font, const char *s, unsigned n, bool pixels)
{ (void)font; (void)s; (void)n; (void)pixels; return 1; }
static int epd_app_font_prepare_text_preview(const lv_font_t *font, const char *s, unsigned n)
{ return epd_app_font_prepare_text(font, s, n, false); }
static int epd_app_font_glyph_ready(const lv_font_t *font, uint32_t letter, unsigned next, bool pixels)
{ (void)font; (void)letter; (void)next; (void)pixels; return 1; }
static int epd_app_font_glyph_acquire(const lv_font_t *font, uint32_t letter, unsigned next,
                                     bool pixels, epd_app_font_glyph_t *glyph)
{ (void)font; (void)letter; (void)next; (void)pixels; glyph->adv_w = 8; return 1; }
static void epd_app_font_glyph_release(epd_app_font_glyph_t *glyph) { (void)glyph; }
static bool add_toc(uint32_t record, unsigned level) { (void)record; (void)level; return true; }

static struct {
    document_t *document;
    document_progress_t progress;
    view_page_t *building, *prefetch, *index, *compute_page;
    reader_compute_job_t *compute_job, *compute_spare;
    bool compute_foreground, compute_traced, preview_back_scanning, preview_clipped;
    bool trace_pending, trace_layout_ready, restoring, selected, preview_cache_active;
    bool preview, complete, background_paused, numeric_target;
    int turn_pending;
    unsigned wanted, count, toc_scanned, session, intent, trace_metric_calls, preview_page, prefetch_number;
    char link_target[2];
    document_cursor_t preview_back_end, preview_end;
    rt_tick_t trace_layout_ticks, trace_metric_ticks, trace_read_ticks, compute_started, work_started, work_budget;
    rt_tick_t trace_record_wait, trace_glyph_wait;
    struct { unsigned content_width, top, bottom, line_space; bool ready; } status;
    flow_metrics_t metrics[3], italic_metrics[3];
    lv_font_t *fonts[3], *oblique[3];
} view;
static uint32_t page_generation;
#define VIEW_HEIGHT (view.status.bottom - view.status.top)
#define VIEW_BLOCK EPD_READER_BATCH
#define VIEW_METRICS_BATCH 32u
static bool flow_near_pending;
static bool core_waiting(void) { return false; }
static bool near_frames_pending(void) { return flow_near_pending; }
#define compute_cancel flow_compute_cancel
#include "reader_page_functions.inc"
#undef compute_cancel

static view_page_t flow_pages[3];
static unsigned flow_workers, flow_waits, flow_metadata_waits, flow_consumed_waits, flow_terminal_waits;
static unsigned flow_resumed_demands;
static document_record_request_t *flow_demand_request;
static unsigned flow_first_pipe;
static const char *flow_case;
static bool flow_background;

static unsigned flow_request_count(void)
{
    unsigned n = 0;
    for (document_record_request_t *r = record_requests; r; r = r->next) ++n;
    return n;
}
static void flow_worker(void)
{
    assert(view.document && view.document->demand_controlled &&
           (flow_background || !view.document->prefetch_enabled));
    ++flow_workers;
    assert(flow_workers < 4096);
    unsigned before = pipe_steps;
    document_worker_step(&service);
    assert(pipe_steps - before <= 1);
    document_service_process();
    assert(view.document->status.result == BOOK_OK);
    assert(pipe_opens <= flow_first_pipe + 1 && (flow_background || !view.document->following));
    document_progress(view.document, &view.progress);
}
static void flow_fixture(bool image, unsigned records, bool metadata)
{
    assert(!wire_data); wire_bytes = 0;
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR, 0, 0, 0,
                  BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_BEGIN, NULL, 0);
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR, 0, 0, 0,
                  BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_TARGET, NULL, 0);
    if (image) picture_pixels(1);
    else for (unsigned i = 0; i < records; ++i)
        picture_event(BOOK_DOCUMENT_TEXT, 0, 0, 0, 0, "abcdefghij", 10);
    if (metadata)
        picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR, 0, records * 10, 0,
                      BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_END, NULL, 0);
}
static void flow_load(const char *path)
{
    FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END);
    wire_bytes = (size_t)ftell(f); rewind(f);
    wire_data = malloc(wire_bytes); assert(wire_data);
    assert(fread(wire_data, 1, wire_bytes, f) == wire_bytes); fclose(f);
}
static void flow_open_at(const char *name, const document_anchor_t *anchor)
{
    assert(!documents && !record_requests && !allocated_bytes && !flow_jobs);
    flow_case = name;
    memset(&view, 0, sizeof(view)); memset(flow_pages, 0, sizeof(flow_pages));
    flow_near_pending = false;
    view.building = &flow_pages[0]; view.index = &flow_pages[1]; view.prefetch = &flow_pages[2];
    view.compute_foreground = true;
    view.status.content_width = 684; view.status.bottom = 1216; view.status.line_space = 2;
    for (unsigned i = 0; i < 3; ++i) view.metrics[i].line_height = view.italic_metrics[i].line_height = 16;
    flow_workers = flow_waits = flow_metadata_waits = flow_consumed_waits = flow_terminal_waits = flow_eofs = 0;
    flow_resumed_demands = 0; flow_demand_request = NULL;
    flow_first_pipe = pipe_opens;
    /* Advance the synthetic clock so native cache publication flushes each event.
     * This also limits convert_step to one pipe step; it is not hardware timing. */
    pipe_events_per_step = 1; pipe_step_ticks = 60;
    bookshelf_item_t file = {0}; char error[96];
    strcpy(file.path, "/flash/page-flow.epub"); file.format = DOCUMENT_EPUB;
    file.size = 10000; file.modified = 1;
    committed = false; _unlink(persistent_path);
    view.document = anchor ? document_open_at(&file, anchor, error, sizeof(error)) :
                             document_open(&file, error, sizeof(error)); assert(view.document);
    document_prefetch_enable(view.document, false);
    uint32_t record = 0, byte = 0;
    for (unsigned i = 0; ; ++i) {
        assert(i < 2048);
        flow_worker();
        if (document_target_cursor(view.document, &record, &byte) == 1 && view.progress.end > record) break;
    }
    assert(!view.progress.complete && view.document->converting);
    for (unsigned i = 0; i < 3; ++i) page_reset(&flow_pages[i], (document_cursor_t){record, byte});
}
static void flow_open(const char *name) { flow_open_at(name, NULL); }
static document_t *flow_open_neighbor(const char *path)
{
    const char *previous = getenv("HCPU_PREVIOUS_WIRE");
    if (!previous) {
        flow_load(path);
        document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 2, 0, 0};
        flow_open_at("real-chapter2", &anchor);
        return NULL;
    }
    flow_load(previous);
    document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 1, 0, 0};
    flow_open_at("real-chapter2", &anchor);
    flow_background = true;
    document_prefetch_enable(view.document, true);
    while (!view.progress.complete) flow_worker();
    document_prefetch_enable(view.document, false);
    flow_background = false;
    document_t *origin = view.document;
    assert(origin->chapter == 1 && origin->status.complete);
    free(wire_data); wire_data = NULL; wire_bytes = 0;
    flow_load(path);
    /* The host persistent-cache stub has a single path for all chapters. */
    committed = false;
    view.document = document_adjacent_internal(origin, 1, false);
    assert(view.document && origin->following == view.document);
    flow_first_pipe = pipe_opens;
    document_prefetch_enable(view.document, false);
    uint32_t record = 0, byte = 0;
    for (unsigned i = 0; ; ++i) {
        assert(i < 2048);
        flow_worker();
        if (document_target_cursor(view.document, &record, &byte) == 1 && view.progress.end > record) break;
    }
    assert(view.progress.chapter == 2 && !view.progress.complete);
    for (unsigned i = 0; i < 3; ++i) page_reset(&flow_pages[i], (document_cursor_t){record, byte});
    return origin;
}
static void flow_close(void)
{
    unsigned workers = flow_workers, demands = flow_waits, metadata = flow_metadata_waits;
    unsigned consumed = flow_consumed_waits, terminal = flow_terminal_waits;
    for (unsigned i = 0; i < 3; ++i) page_reset(&flow_pages[i], (document_cursor_t){0, 0});
    reader_compute_release(view.compute_spare); view.compute_spare = NULL;
    document_service_process(); assert(!flow_request_count() && !flow_jobs);
    document_close(view.document);
    /* Cleanup may finish cancellation; all live-page assertions kept prefetch off. */
    pump();
    assert(!documents && !record_requests && !allocated_bytes);
    assert_compute_released();
    pipe_step_ticks = pipe_events_per_step = 0;
    free(wire_data); wire_data = NULL; wire_bytes = 0;
    flow_demand_request = NULL;
    printf("HCPU page-flow %s passed: worker_steps=%u demands=%u metadata_demands=%u consumed_records=%u resumed_demands=%u terminal_IO_waits=%u requests=0\n",
           flow_case, workers, demands, metadata, consumed, flow_resumed_demands, terminal);
}
static void flow_step(view_page_t *p)
{
    document_progress(view.document, &view.progress);
    document_record_request_t *prior = p->read_request;
    bool frontier = p->cursor.record == view.progress.end && !view.progress.complete;
    bool runnable = page_runnable(p) || view.compute_page == p;
    uint32_t cursor = p->cursor.record;
    if (runnable) {
        int result = page_step(p, p == view.index, NULL, &view.progress);
        if (result < 0) fprintf(stderr, "page_step failed case=%s cursor=%u end=%u complete=%u\n",
                                flow_case, cursor, view.progress.end, view.progress.complete);
        assert(result >= 0);
    }
    if (prior && prior != p->read_request) {
        assert(prior->released);
        if (prior->done && prior->result == BOOK_OK) ++flow_consumed_waits;
        if (prior == flow_demand_request) {
            if (prior->done && prior->result == BOOK_OK) ++flow_resumed_demands;
            flow_demand_request = NULL;
        }
    }
    if (!prior && p->read_request && frontier) {
        ++flow_waits;
        assert(!flow_demand_request); flow_demand_request = p->read_request;
        if (p->record.kind == DOCUMENT_METADATA) ++flow_metadata_waits;
        assert(p->read_request->foreground && p->read_request->offset == cursor);
        assert(!page_runnable(p));
        document_record_request_t *wait = p->read_request;
        for (unsigned i = 0; i < 3; ++i) {
            assert(page_step(p, p == view.index, NULL, &view.progress) == 0);
            assert(p->read_request == wait && flow_request_count() == 1);
        }
    }
    document_service_process();
    bool complete = view.progress.complete;
    flow_worker();
    if (!complete && view.progress.complete && p->read_request && p->cursor.record == view.progress.end) {
        /* Exercise the race where record_work reports EOF as IO_ERROR before UI runs. */
        flow_worker();
        assert(p->read_request->done && p->read_request->result == BOOK_IO_ERROR);
        assert(view.progress.result == BOOK_OK);
        ++flow_terminal_waits;
    }
}
static void flow_drive(view_page_t *p, bool stop_at_wait)
{
    unsigned idle = 0;
    for (unsigned i = 0; i < 2048; ++i) {
        unsigned steps = pipe_steps;
        document_cursor_t cursor = p->cursor;
        flow_step(p);
        if (p->done || (stop_at_wait && p->read_request && p->read_request->offset == p->cursor.record &&
                       p->cursor.record == view.progress.end && !view.progress.complete)) return;
        if (steps == pipe_steps && cursor_equal(cursor, p->cursor) && !view.compute_job && !p->read_request) ++idle;
        else idle = 0;
        if (idle == 32) {
            fprintf(stderr, "FRONTIER_STALL case=%s cursor=%u end=%u complete=%u prefetch=%u requests=%u worker_steps=%u\n",
                    flow_case, p->cursor.record, view.progress.end, view.progress.complete,
                    view.document->prefetch_enabled, flow_request_count(), flow_workers);
            exit(2);
        }
    }
    fprintf(stderr, "page-flow budget exhausted: %s\n", flow_case); assert(false);
}
static void flow_eof_case(const char *name, bool image, bool metadata, const char *path)
{
    if (path) flow_load(path); else flow_fixture(image, 1, metadata);
    flow_open(name);
    flow_drive(view.building, false);
    assert(view.building->done && view.building->eof && view.building->count);
    assert(view.progress.complete && view.progress.result == BOOK_OK);
    assert(flow_waits && flow_terminal_waits == 1 && flow_eofs == 1);
    assert(!view.building->read_request && !flow_request_count());
    if (metadata) assert(flow_metadata_waits && flow_waits >= 2 && flow_resumed_demands);
    if (image) {
        assert(!view.building->bytes);
        bool found = false;
        for (unsigned i = 0; i < view.building->count; ++i) found |= view.building->runs[i].image != 0;
        assert(found);
    } else assert(view.building->bytes == 11 && !strcmp(view.building->text, "abcdefghij"));
    flow_close();
}
static void flow_idle(void)
{
    unsigned steps = pipe_steps, opens = pipe_opens;
    uint32_t end = view.document->status.end;
    for (unsigned i = 0; i < 20; ++i) flow_worker();
    assert(pipe_steps == steps && pipe_opens == opens && view.document->status.end == end);
    assert(!view.progress.complete && !flow_request_count());
}
static void flow_next_page(void)
{
    flow_fixture(false, 128, true); flow_open("next-page-demand");
    view.status.content_width = 80; view.status.bottom = 36;
    flow_drive(view.building, false);
    assert(view.building->done && !view.building->eof && flow_waits && flow_resumed_demands);
    assert(view.document->pipe && view.document->pipe->at < wire_bytes);
    flow_idle();
    unsigned steps = pipe_steps, waits = flow_waits;
    document_cursor_t next = view.building->cursor;
    page_reset(view.building, next); flow_drive(view.building, false);
    assert(view.building->done && !view.building->eof && flow_waits > waits && pipe_steps > steps);
    assert(pipe_opens == flow_first_pipe + 1 && view.building->cursor.record > next.record);
    flow_idle(); flow_close();
}
static void flow_cancel_or_clip(bool clip)
{
    flow_fixture(false, 2, true); flow_open(clip ? "clipped-eof" : "cancel-wait");
    flow_drive(view.building, true);
    assert(view.building->read_request && flow_request_count() == 1);
    document_record_request_t *wait = view.building->read_request;
    assert(!wait->done && wait->foreground);
    if (clip) {
        view.preview_clipped = true; view.preview_end = view.building->cursor;
        assert(page_step(view.building, false, NULL, &view.progress) == 0);
        assert(wait->released && !view.building->read_request && view.compute_job);
        document_service_process(); assert(!flow_request_count());
        assert(page_step(view.building, false, NULL, &view.progress) == 1);
        assert(view.building->done && !view.building->eof && flow_eofs == 1);
    } else {
        page_reset(view.building, view.building->start);
        assert(wait->released && !view.building->read_request);
        document_service_process(); assert(!flow_request_count());
    }
    flow_idle(); flow_close();
}
static void flow_index_target(void)
{
    flow_fixture(false, 1, true); flow_open("index-target");
    view.restoring = true; view.count = 1;
    flow_drive(view.index, false);
    assert(view.index->done && view.index->eof && flow_waits >= 2);
    assert(flow_resumed_demands && flow_terminal_waits == 1 && !flow_request_count());
    flow_close();
}
static void flow_target_roles(void)
{
#if HCPU_HAS_PAGE_TARGET
    flow_fixture(false, 1, true); flow_open("target-roles");
    assert(page_target(view.building));
    view.count = 1;
    assert(!page_target(view.index) && !page_target(view.prefetch));
    view.restoring = true; assert(page_target(view.index)); view.restoring = false;
    view.link_target[0] = 'x'; assert(page_target(view.index)); view.link_target[0] = 0;
    view.wanted = 1; assert(page_target(view.index)); view.wanted = 0;
    view.preview_back_scanning = true;
    view.preview_back_end = (document_cursor_t){UINT32_MAX, 0};
    assert(!page_target(view.prefetch));
    view.turn_pending = -1; assert(page_target(view.prefetch));
    view.compute_foreground = false;
    assert(!page_target(view.building) && !page_target(view.prefetch) && !page_target(view.index));
    for (unsigned i = 0; i < 3; ++i) {
        page_reset(&flow_pages[i], (document_cursor_t){view.progress.end, 0});
        assert(!page_runnable(&flow_pages[i]));
        assert(page_step(&flow_pages[i], i == 1, NULL, &view.progress) == 0);
        assert(!flow_pages[i].read_request && !flow_request_count());
    }
    flow_idle();
    page_reset(view.building, (document_cursor_t){view.document->target_record, 0});
    view.compute_foreground = true; view.turn_pending = 0;
    page_reset(view.prefetch, (document_cursor_t){view.progress.end, 0});
    assert(!page_runnable(view.prefetch));
    assert(page_step(view.prefetch, false, NULL, &view.progress) == 0);
    assert(!view.prefetch->read_request && !flow_request_count());
    view.prefetch->cursor.record = view.progress.end + 1;
    assert(!page_runnable(view.prefetch));
    view.prefetch->cursor.record = view.progress.end;
    view.turn_pending = -1; assert(page_runnable(view.prefetch));
    assert(page_step(view.prefetch, false, NULL, &view.progress) == 0);
    assert(view.prefetch->read_request && view.prefetch->read_request->foreground);
    page_reset(view.prefetch, view.building->start); document_service_process();
    /* The actual backward-prefetch page can drive the same stream to normal EOF. */
    flow_drive(view.prefetch, false);
    assert(view.prefetch->done && !view.prefetch->eof && flow_terminal_waits == 1 && !flow_request_count());
    flow_close();
#endif
}
static void flow_near_input(bool indexed)
{
    flow_fixture(false, 4, true); flow_open(indexed ? "near-index-input" : "near-preview-input");
    view.selected = view.status.ready = true;
    view.preview = view.preview_cache_active = !indexed;
    view.preview_page = 100; view.prefetch_number = 101;
    view.count = 1; view.wanted = 0;
    view_page_t *page = indexed ? view.index : view.prefetch;
    if (!page_target(page)) { fprintf(stderr, "NEAR_REQUIRED_INPUT\n"); exit(1); }
    flow_drive(page, false);
    assert(page->done && page->eof && page->count && flow_resumed_demands);
    assert(view.progress.complete && !view.document->prefetch_enabled);
    assert(!view.document->persistent && view.document->finish_stage == DOCUMENT_FINISH_PREPARE);
    assert(!view.document->converting && !conversion_document);
    for (unsigned i = 0; i < 20; ++i) assert(!document_worker_step(&service));
    flow_close();
}
static void flow_near_resume(void)
{
#if HCPU_HAS_DOCUMENT_FLOW
    flow_fixture(false, 128, true); flow_open("near-resume-chapter");
    view.selected = view.status.ready = true;
    view.preview = view.preview_cache_active = true;
    view.preview_page = 100; view.prefetch_number = 101;
    view.status.content_width = 80; view.status.bottom = 36;
    flow_near_pending = true; document_flow();
    flow_drive(view.prefetch, false);
    assert(view.prefetch->done && !view.prefetch->eof && flow_resumed_demands);
    assert(!view.document->prefetch_enabled && !view.progress.complete);
    flow_idle();
    /* Finishing the near window admits the rest of the chapter without any
     * further page request, followed by its optional cache publication. */
    unsigned before = pipe_steps;
    flow_near_pending = false; document_flow(); flow_background = true;
    assert(view.document->prefetch_enabled);
    while (!view.progress.complete) flow_worker();
    assert(pipe_steps > before && view.progress.result == BOOK_OK && !flow_request_count());
    for (unsigned i = 0; view.document->finish_stage; ++i) { assert(i < 20); flow_worker(); }
    assert(view.document->persistent && !conversion_document);
    flow_background = false; document_prefetch_enable(view.document, false);
    flow_close();
#endif
}
static void flow_real_chapter(const char *path, bool reading)
{
    document_t *origin = flow_open_neighbor(path);
    flow_case = reading ? "real-chapter2-read" : "real-chapter2";
    printf("REAL_CHAPTER_START record=%u byte=%u end=%u complete=%u chapter=%u\n",
           view.building->start.record, view.building->start.byte, view.progress.end,
           view.progress.complete, view.progress.chapter);
    document_cursor_t start = view.building->start;
    view.compute_foreground = false;
    flow_background = true;
    document_prefetch_enable(view.document, true);
    /* The neighboring view is still measuring its first page when selected. */
    flow_hold_compute = true;
    for (unsigned i = 0; ; ++i) {
        assert(i < 2048);
        flow_step(view.building);
        if (reading ? view.building->read_request && view.building->read_request->done :
                      view.compute_job != NULL) break;
    }
    assert(!view.progress.complete);
    if (!reading) assert(view.compute_page == view.building);
    /* Advance through the device's published frontier without finishing the chapter. */
    for (unsigned i = 0; view.progress.end < 23382; ++i) {
        assert(i < 256);
        flow_worker();
    }
    assert(!view.progress.complete);
    printf("REAL_CHAPTER_PROMOTE case=%s pending_read=%u pending_compute=%u end=%u complete=%u\n",
           flow_case, view.building->read_request != NULL, view.compute_job != NULL,
           view.progress.end, view.progress.complete);
    if (origin) {
        document_t *promoted = document_adjacent(origin, 1);
        assert(promoted == view.document);
        document_close(promoted);
    }
    view.compute_foreground = true;
    page_reset(view.building, start);
    assert(!view.compute_job && !view.building->read_request);
    document_prefetch_enable(view.document, false);
    flow_background = false;
    flow_hold_compute = false;
    flow_drive(view.building, false);
    assert(view.building->done && view.building->count && view.building->bytes);
    assert(cursor_equal(view.building->start, start));
    printf("REAL_CHAPTER_READY start=%u:%u cursor=%u:%u runs=%u bytes=%u end=%u complete=%u\n",
           view.building->start.record, view.building->start.byte,
           view.building->cursor.record, view.building->cursor.byte,
           view.building->count, view.building->bytes, view.progress.end, view.progress.complete);
    if (origin) document_close(origin);
    flow_close();
}
static void page_flow_tests(const char *cover)
{
    const char *only = getenv("HCPU_PAGE_CASE");
#define FLOW_CASE(name) (!only || !strcmp(only, name))
    if (FLOW_CASE("short-text")) flow_eof_case("short-text", false, false, NULL);
    if (FLOW_CASE("metadata-tail")) flow_eof_case("metadata-tail", false, true, NULL);
    if (FLOW_CASE("picture-only")) flow_eof_case("picture-only", true, true, NULL);
    if (FLOW_CASE("real-cover") && cover) flow_eof_case("real-cover", true, true, cover);
    if (FLOW_CASE("next-page-demand")) flow_next_page();
    if (FLOW_CASE("cancel-wait")) flow_cancel_or_clip(false);
    if (FLOW_CASE("clipped-eof")) flow_cancel_or_clip(true);
    if (FLOW_CASE("index-target")) flow_index_target();
    if (FLOW_CASE("target-roles")) flow_target_roles();
    if (FLOW_CASE("near-preview-input")) flow_near_input(false);
    if (FLOW_CASE("near-index-input")) flow_near_input(true);
    if (FLOW_CASE("near-resume-chapter")) flow_near_resume();
    if (FLOW_CASE("real-chapter2") && getenv("HCPU_CHAPTER_WIRE"))
        flow_real_chapter(getenv("HCPU_CHAPTER_WIRE"), false);
    if (FLOW_CASE("real-chapter2-read") && getenv("HCPU_CHAPTER_WIRE"))
        flow_real_chapter(getenv("HCPU_CHAPTER_WIRE"), true);
#undef FLOW_CASE
}
