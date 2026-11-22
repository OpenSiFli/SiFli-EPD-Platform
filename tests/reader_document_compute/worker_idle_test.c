/* SPDX-License-Identifier: Apache-2.0 */
#define main restore_fixture_main
#include "restore_gate_test.c"
#undef main

static void prepare_wire(void)
{
    committed = false; _unlink(persistent_path);
    book_document_event_t source = target_event(true);
    book_document_event_t text = {0}; text.type = BOOK_DOCUMENT_TEXT;
    epd_paragraph_style_t style = {0};
    text.length1 = sizeof(body);
    text.length2 = sizeof(style);
    text.bytes = RT_ALIGN(sizeof(text) + text.length1 + text.length2, 4);
    wire_bytes = 2 * sizeof(source) + text.bytes * body_records;
    wire_data = calloc(1, wire_bytes); assert(wire_data);
    memcpy(wire_data, &source, sizeof(source));
    size_t at = sizeof(source);
    for (unsigned i = 0; i < body_records; ++i) {
        memcpy(wire_data + at, &text, sizeof(text)); at += sizeof(text);
        memcpy(wire_data + at, body, sizeof(body)); at += sizeof(body);
        memcpy(wire_data + at, &style, sizeof(style));
        at += text.bytes - sizeof(text) - sizeof(body);
    }
    source = end_event(); memcpy(wire_data + at, &source, sizeof(source));
    pipe_events_per_step = 1;
}

static void assert_worker_idle(void)
{
    unsigned reads = io_reads, opens = io_opens, steps = pipe_steps;
    unsigned busy = 0;
    for (unsigned i = 0; i < 64; ++i) {
        busy += document_worker_step(&service) ? 1u : 0u;
        assert(io_reads == reads && io_opens == opens && pipe_steps == steps);
    }
    printf("idle probe: runnable=%u/64 read_delta=%u open_delta=%u pipe_delta=%u\n",
        busy, io_reads - reads, io_opens - opens, pipe_steps - steps);
    assert(!busy && "DOCUMENT_WORKER_IDLE");
}

static void preempted_owner(bool deferred_close)
{
    prepare_wire();
    document_t *old = open_document(false); drain();
    assert(old->target_ready && old->converting && old->owners == 1);
    document_prefetch_enable(old, true);
    assert(old->adjacent_pending && old->prefetch_enabled);
    bookshelf_item_t file = old->file;
    document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 1, 0, 0};
    char error[160];
    unsigned opened = pipe_opens;
    if (deferred_close) pipe_shutdown_failures = 3;
    document_t *next = document_open_at(&file, &anchor, error, sizeof(error)); assert(next);
    document_prefetch_enable(next, false);
    assert(old->cancelled && !old->released && old->owners == 1);
    if (deferred_close) {
        for (unsigned i = 0; i < 3; ++i) {
            assert(document_worker_step(&service));
            assert(old->pipe && !next->claimed && pipe_opens == opened);
        }
    }
    for (unsigned i = 0; !next->target_ready || !next->status.end; ++i) {
        assert(i < 100 && document_worker_step(&service)); document_service_process();
    }
    assert(old->stopped && old->status.complete && !old->pipe && !old->remote);
    assert(old->owners == 1 && old->adjacent_pending && old->prefetch_enabled);
    assert(next->converting && !conversion_runnable(next));
    printf("paused target=%lu cancelled_owner=%lu adjacent=%u prefetch=%u\n",
        (unsigned long)next->serial, (unsigned long)old->serial,
        (unsigned)old->adjacent_pending, (unsigned)old->prefetch_enabled);
    assert_worker_idle();
    /* Foreground reads must still wake and finish without restarting the parser. */
    document_record_request_t *job = document_record_request(next, next->target_record, true); assert(job);
    document_record_data_t data;
    assert(document_worker_step(&service));
    assert(document_record_take(job, &data) == 1 && data.record.length == sizeof(body));
    assert(!memcmp(data.text, body, sizeof(body)));
    document_record_cancel(job); document_service_process(); assert_worker_idle();
    document_close(old); drain(); assert(documents == next && !next->next);
    document_prefetch_enable(next, true); drain();
    assert(next->status.complete && next->status.result == BOOK_OK);
    close_document(next);
    free(wire_data); wire_data = NULL; pipe_events_per_step = 0;
}

static void cancelled_cached_owner(void)
{
    make_spool(true); document_t *d = open_document(false); reach_target(d);
    document_prefetch_enable(d, true);
    /* A storage/session cancellation can arrive while a view still owns the chapter. */
    d->cancelled = true; compute_cancel(d);
    assert(document_worker_step(&service) && d->stopped);
    assert(d->owners && !d->released && d->adjacent_pending);
    assert_worker_idle(); close_document(d);
}

static void suspend_neighbor(void)
{
    make_spool(true); document_t *d = open_document(false); reach_target(d);
    document_prefetch_enable(d, true);
    reader_suspended = true; assert_worker_idle();
    reader_suspended = false; drain();
    assert(!d->chapter_restore && !d->adjacent_pending && document_navigation_ready(d));
    close_document(d);
}

static void missing_body_end(void)
{
    prepare_wire(); wire_bytes -= sizeof(book_document_event_t);
    document_t *d = open_document(true); d->adjacent_pending = false; drain();
    assert(d->target_ready && !d->body_end && d->status.complete);
    assert(d->status.result == BOOK_ERROR && !d->persistent && !committed);
    close_document(d); free(wire_data); wire_data = NULL; pipe_events_per_step = 0;
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    assert(argc == 3); snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root); memset(body, 'x', sizeof(body));
    if (!strcmp(argv[2], "preempted-owner")) preempted_owner(false);
    else if (!strcmp(argv[2], "deferred-close")) preempted_owner(true);
    else if (!strcmp(argv[2], "cancelled-cache")) cancelled_cached_owner();
    else if (!strcmp(argv[2], "suspend-neighbor")) suspend_neighbor();
    else if (!strcmp(argv[2], "missing-body-end")) missing_body_end();
    else if (!strcmp(argv[2], "shutdown-reopen")) {
        for (unsigned i = 0; i < 8; ++i) {
            preempted_owner(false);
            document_service_shutdown(); document_worker_shutdown(); document_service_process();
            assert(!documents && !allocated_bytes && !worker_service);
        }
    } else assert(false);
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    assert(!allocated_bytes && !documents && !fd_live && pipe_opens == pipe_closes);
    assert_compute_released();
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    printf("PASS %s reads=%u pipes=%u steps=%u\n", argv[2], io_reads, pipe_opens, pipe_steps);
    return 0;
}
