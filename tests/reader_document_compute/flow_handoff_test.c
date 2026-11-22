/* SPDX-License-Identifier: Apache-2.0 */
#define main restore_gate_main
#include "restore_gate_test.c"
#undef main

typedef struct {
    document_cursor_t start, cursor;
    bool collect, eof;
    document_record_request_t *read_request;
} view_page_t;
typedef view_page_t epd_reader_page_t;
typedef struct view_state {
    struct view_state *previous, *following;
    document_t *document;
    document_progress_t progress;
    view_page_t *index, *building, *prefetch, *display, *compute_page;
    document_record_request_t *back_request, *progress_request;
    void *compute_spare;
    void *rendering;
    bool initialized, selected, displayed, counting_only, complete;
    bool target_pending, backward_target, boundary_target, initial_target_ready;
    bool background_paused, allow_glyphs, numeric_target, frame_published, display_pending;
    bool preview_cache_active, preview, restoring, forward_step, preview_clipped;
    unsigned intent, published_intent, session, layout, display_layout, failure;
    unsigned chapter_percent, request_failure, request_failure_intent, input_order;
    struct { unsigned page; } saved_position;
    bool input_enabled;
    int turn_pending, turn_remaining;
    char link_target[8];
    uint32_t navigation_link, navigation_toc;
    document_cursor_t boundary_anchor, initial_anchor, anchor, preview_end;
    struct { bool ready; } status;
} view_state_t;
static view_state_t *state, *parked_state;
static uint32_t input_sequence;
static struct { view_state_t *owner; int hop; } window_slots[4];
static struct { view_state_t *counter; } pagination;
#define view (*state)
static bool core_waiting(void) { return view.target_pending || !view.status.ready; }
static bool near_frames_pending(void) { return false; }
static void reader_compute_idle(void) {}
static void frame_release(void *frame) { assert(!frame); }
static void images_cancel_pending(void) { assert(!view.rendering); }
static void near_trace_finish(const char *reason) { assert(!strcmp(reason, "pause")); }
static void reader_compute_release(void *job) { assert(!job); }
static void core_seek(unsigned page) { (void)page; assert(!"unexpected numeric seek"); }
#define compute_cancel handoff_compute_cancel
static void compute_cancel(void) { view.compute_page = NULL; }
static void epd_reader_page_copy(epd_reader_page_t *to, const epd_reader_page_t *from) { *to = *from; }
static void page_reset(view_page_t *page, document_cursor_t cursor)
{ memset(page, 0, sizeof(*page)); page->start = page->cursor = cursor; }
static bool cursor_equal(document_cursor_t a, document_cursor_t b)
{ return a.record == b.record && a.byte == b.byte; }
static bool cursor_at_segment_start(document_cursor_t cursor) { (void)cursor; return false; }
static void fail(unsigned error) { view.failure = error; }
static void target_finish(void) { assert(false && "uncached target must start layout"); }
static void preview_start(document_cursor_t cursor)
{ view.anchor = cursor; view.preview = view.restoring = true; page_reset(view.building, cursor); }
#include "flow_handoff.inc"
#undef compute_cancel

static document_t *new_source(void)
{
    committed = false;
    book_document_event_t source = target_event(true);
    book_document_event_t text = {0}; text.type = BOOK_DOCUMENT_TEXT;
    epd_paragraph_style_t style = {0};
    text.length1 = sizeof(body);
    text.length2 = sizeof(style);
    text.bytes = RT_ALIGN(sizeof(text) + text.length1 + text.length2, 4);
    wire_bytes = 2 * sizeof(source) + text.bytes * 8;
    wire_data = calloc(1, wire_bytes); assert(wire_data);
    memcpy(wire_data, &source, sizeof(source));
    size_t at = sizeof(source);
    for (unsigned i = 0; i < 8; ++i) {
        memcpy(wire_data + at, &text, sizeof(text)); at += sizeof(text);
        memcpy(wire_data + at, body, sizeof(body)); at += sizeof(body);
        memcpy(wire_data + at, &style, sizeof(style));
        at += text.bytes - sizeof(text) - sizeof(body);
    }
    source.b += 8 * sizeof(body); source.d = BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_END;
    memcpy(wire_data + at, &source, sizeof(source));
    pipe_events_per_step = 1; pipe_step_ticks = 60;
    bookshelf_item_t file = {0}; char error[160];
    strcpy(file.path, "/flash/new-chapter.epub"); file.format = DOCUMENT_EPUB;
    file.size = 100000; file.modified = 1;
    document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 1, 128, 0};
    document_t *d = document_open_at(&file, &anchor, error, sizeof(error)); assert(d);
    document_prefetch_enable(d, false);
    return d;
}

static void select_after_quiesce(document_t *old, document_t *next, bool counter)
{
    /* Old optional work must remain queued while the new target consumes input. */
    FILE *index = fopen(index_file, "wb"); assert(index);
    assert(fwrite("old-index", 1, 9, index) == 9); fclose(index);
    void *blob = NULL; size_t bytes = 0;
    assert(document_index_load_async(old, 7, &blob, &bytes) == 0);
    char save_path[512]; snprintf(save_path, sizeof(save_path), "%s/progress.bin", test_root);
    replace_enabled = true;
    assert(document_save_async(save_path, NULL, 0, "progress", 8));
    unsigned writes = replace_calls, navigation_opens = pipe_navigation_opens;
    view_page_t visible = {0}, building = {0}, old_index = {0};
    view_state_t retained = {.document=old, .initialized=true, .complete=true,
        .displayed=!counter, .counting_only=counter, .display=&visible, .index=&old_index};
    retained.status.ready = true;
    view_state_t selected = {.document=next, .initialized=true, .selected=true,
        .target_pending=true, .display=&visible, .building=&building};
    state = &selected;
    if (counter) pagination.counter = &retained; else parked_state = &retained;
    unsigned pending_skips = 0, transfers = 0;
    for (unsigned i = 0; selected.target_pending; ++i) {
        assert(i < 32 && !selected.failure && !selected.link_target[0]);
        worker_quiesce(&selected, false, false, false);
        assert(!retained.input_enabled && retained.input_order && old->status.complete && !old->cancelled);
        unsigned wakes = service_wakes;
        unsigned expected_wakes = flow_owner() == old ? 1u : 2u;
        target_resolve();
        worker_input_commit(&selected);
        if (flow_owner() != next) { fprintf(stderr, "FLOW_PENDING_OWNER\n"); exit(1); }
        assert(!next->prefetch_enabled && service_wakes == wakes + expected_wakes);
        assert(!old->index_done && replace_calls == writes && saves && !persistence_allowed());
        ++transfers;
        /* view_worker skips core_step while target_pending. No synthetic
         * layout call may repair the flow owner in this pending interval. */
        if (selected.target_pending) {
            ++pending_skips;
            assert(document_worker_step(&service)); document_service_process();
            assert(!old->index_done && replace_calls == writes && saves);
        }
    }
    assert(pending_skips && selected.initial_target_ready && next->target_ready && next->converting);
    assert(selected.anchor.record == next->target_record && !old->cancelled);
    document_record_request_t *request = document_record_request(next, selected.anchor.record, true);
    assert(request);
    for (unsigned i = 0; !request->done; ++i) {
        assert(i < 32 && document_worker_step(&service)); document_service_process();
    }
    document_record_data_t data;
    assert(document_record_take(request, &data) == 1 && data.record.kind == DOCUMENT_TEXT);
    assert(data.record.length == sizeof(body) && !memcmp(data.text, body, sizeof(body)));
    document_record_cancel(request); document_service_process();
    assert(!next->prefetch_enabled && !next->status.complete && !old->cancelled);
    assert(!old->index_done && replace_calls == writes && saves);
    assert(pipe_navigation_opens == navigation_opens);
    printf("PASS %s pending_core_skips=%u owner_transfers=%u first_body=%u pipe_steps=%u old_complete=1 old_cancelled=0 old_index_blocked=1 save_blocked=1 warm_navigation_opens=%u near_navigation_opens=0\n",
        counter ? "completed-counter" : "parked-display", pending_skips, transfers, (unsigned)sizeof(body), pipe_steps, navigation_opens);
    state = parked_state = pagination.counter = NULL;
}

static void transfer_wake(document_t *old, document_t *next)
{
    unsigned navigation_opens = pipe_navigation_opens;
    document_prefetch_enable(old, false);
    assert(flow_owner() == old && !old->prefetch_enabled && !next->prefetch_enabled);
    assert(!document_worker_step(&service) && !next->claimed);
    unsigned wakes = service_wakes;
    document_prefetch_enable(next, false);
    assert(flow_owner() == next && !persistence_allowed());
    if (service_wakes != wakes + 1) { fprintf(stderr, "FLOW_OWNER_WAKE\n"); exit(1); }
    for (unsigned i = 0; i < 20; ++i) document_prefetch_enable(next, false);
    assert(service_wakes == wakes + 1);
    assert(document_worker_step(&service) && next->claimed && next->converting && pipe_steps);
    assert(pipe_navigation_opens == navigation_opens);
    printf("PASS same-false-wake transfer_wakes=1 repeated_same_owner_wakes=0 pipe_steps=%u\n", pipe_steps);
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    assert(argc == 3); snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root);
    memset(body, 'h', sizeof(body)); make_spool(true);
    /* A v10 cached EPUB carries the body boundary before its navigation tail. */
    FILE *spool = fopen(persistent_path, "ab"); assert(spool);
    book_document_event_t end = target_event(false);
    end.b += body_records * sizeof(body); end.d = BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_END;
    write_record(spool, DOCUMENT_METADATA, &end, sizeof(end));
    fixture_end = (uint32_t)ftell(spool); fclose(spool);
    document_t *old = open_document(true);
    for (unsigned i = 0; !document_navigation_ready(old); ++i) {
        assert(i < 128 && old->status.result == BOOK_OK);
        assert(document_worker_step(&service)); document_service_process();
    }
    assert(old->status.complete && old->target_ready && old->target_record == fixture_target);
    assert(pipe_navigation_opens == 1 && old->nav_state == DOCUMENT_NAV_DONE);
    document_prefetch_enable(old, false);
    document_t *next = new_source();
    if (!strcmp(argv[2], "same-false-wake")) transfer_wake(old, next);
    else select_after_quiesce(old, next, !strcmp(argv[2], "completed-counter"));
    document_close(next); document_close(old); drain();
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    free(wire_data); wire_data = NULL;
    assert(!documents && !allocated_bytes && !fd_live && !record_requests && !saves);
    assert_compute_released();
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    return 0;
}
