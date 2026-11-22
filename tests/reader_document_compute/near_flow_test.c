/* SPDX-License-Identifier: Apache-2.0 */
#define main restore_gate_main
#include "restore_gate_test.c"
#undef main

typedef struct {
    document_t *document;
    bool selected, numeric_target, background_paused, input_enabled;
    uint32_t input_order;
    char link_target[2];
    struct { bool ready; } status;
} view_state_t;
static view_state_t view;
static view_state_t *state = &view;
static uint32_t input_sequence;
static bool waiting, near_pending;
static bool core_waiting(void) { return waiting; }
static bool near_frames_pending(void) { return near_pending; }
#include "near_flow.inc"
static void publish_flow(void)
{
    document_flow();
    if (view.input_order) {
        document_prefetch_enable(view.document, view.input_enabled);
        view.input_order = 0;
    }
}

static document_t *neighbor_open(void)
{
    bookshelf_item_t file = {0}; char error[160];
    strcpy(file.path, "/flash/neighbor.epub"); file.format = DOCUMENT_EPUB;
    file.size = 100000; file.modified = 1;
    document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 0, 128, 0};
    document_t *d = document_open_at(&file, &anchor, error, sizeof(error)); assert(d);
    document_prefetch_enable(d, true);
    return d;
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    assert(argc == 3); snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root);
    memset(body, 'b', sizeof(body)); make_spool(true);
    document_t *current = open_document(false); reach_target(current);
    bool retained = !strcmp(argv[2], "unowned-neighbor");
    document_t *neighbor;
    if (retained) {
        current->chapter_count = 2;
        /* Model chapter zero's beginning, with no preceding tail segment. */
        current->source_begin = 0;
        neighbor = document_adjacent_internal(current, 1, false); assert(neighbor);
        /* adjacent_work retains the link without a reader/view owner. */
        --neighbor->owners; assert(!neighbor->owners && neighbor->retained);
    } else neighbor = neighbor_open();
    bool existing = !retained && strcmp(argv[2], "new-neighbor") != 0;
    if (existing) {
        /* A neighbor can already own its cache and still need navigation. */
        assert(document_worker_step(&service));
        assert(neighbor->claimed && neighbor->persistent && neighbor->chapter_restore);
        neighbor->index_requested = true; neighbor->index_layout = 7;
    }
    view.document = current; view.selected = view.status.ready = true; near_pending = true;
    publish_flow();
    if (current->prefetch_enabled) { fprintf(stderr, "NEAR_LAYOUT_GATE\n"); return 1; }
    unsigned reads = io_reads, opens = io_opens, steps = pipe_steps;
    uint32_t scan = current->chapter_scan, other_scan = neighbor->chapter_scan;
    document_record_request_t *background = document_record_request(neighbor, 0, false); assert(background);
    replace_enabled = true;
    const char index[] = "latest-index";
    assert(document_save_async(index_file, NULL, 0, index, sizeof(index)));
    for (unsigned i = 0; i < 50; ++i) {
        if (document_worker_step(&service)) { fprintf(stderr, "NEAR_OPTIONAL_IO\n"); return 1; }
        document_service_process();
    }
    assert(io_reads == reads && io_opens == opens && pipe_steps == steps);
    assert(current->chapter_scan == scan && neighbor->chapter_scan == other_scan);
    assert(!background->done && !neighbor->index_done && !replace_calls);
    document_record_request_t *required = document_record_request(current, fixture_target, true); assert(required);
    assert(document_worker_step(&service) && required->done && required->result == BOOK_OK);
    document_record_cancel(required);
    assert(!document_worker_step(&service));
    if (!strcmp(argv[2], "cancel-neighbor")) {
        /* Closing the last owner cancels and collects its queued requests. */
        document_close(neighbor);
        neighbor = NULL; background = NULL;
        drain(); assert(!record_requests && !replace_calls);
    } else {
        /* Ready near frames and an abandoned optional attempt both release
         * the same gate, allowing every queued background task to resume. */
        near_pending = false; publish_flow();
        drain();
        assert(!current->chapter_restore);
        if (retained) {
            /* Required restoration reaches BODY_END; the NAV tail still waits
             * for an owner to enable its own optional input. */
            assert(neighbor->claimed && neighbor->target_ready && !neighbor->owners);
            assert(neighbor->chapter_restore && neighbor->chapter_scan == fixture_body_end);
        } else assert(!neighbor->chapter_restore);
        assert(background->done && background->result == BOOK_OK && !saves && replace_calls == 1);
        if (existing) assert(neighbor->index_done);
    }
    if (background) document_record_cancel(background);
    if (neighbor) document_close(neighbor);
    document_close(current); drain();
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    assert(!documents && !allocated_bytes && !fd_live && !record_requests && !saves);
    assert_compute_released();
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    printf("PASS near-flow %s\n", argv[2]);
    return 0;
}
