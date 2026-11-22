/* SPDX-License-Identifier: Apache-2.0 */
#include "restore_host.inc"

static const unsigned prefix_records = 33, body_records = 96;
static uint32_t fixture_target, fixture_body_end, fixture_end;
static char body[768];

static void drain(void)
{
    for (unsigned i = 0; i < 1000; ++i) {
        bool work = document_worker_step(&service);
        document_service_process();
        if (!work) return;
    }
    assert(0 && "worker must become idle");
}

static void write_record(FILE *file, unsigned kind, const void *data, unsigned length)
{
    document_record_t record = {kind, length, 0, 0};
    assert(fwrite(&record, 1, sizeof(record), file) == sizeof(record));
    if (length) assert(fwrite(data, 1, length, file) == length);
}

static book_document_event_t target_event(bool target)
{
    book_document_event_t event = {0};
    event.type = BOOK_DOCUMENT_SOURCE_ANCHOR; event.bytes = sizeof(event);
    event.b = 128; event.c = 128;
    event.d = BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_BEGIN |
              (target ? BOOK_DOCUMENT_SOURCE_TARGET : 0);
    return event;
}

static book_document_event_t end_event(void)
{
    book_document_event_t event = target_event(false);
    event.b += body_records * sizeof(body);
    event.d = BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_END;
    return event;
}

static void make_spool(bool target)
{
    FILE *file = fopen(persistent_path, "wb"); assert(file);
    for (unsigned i = 0; i < prefix_records; ++i) write_record(file, DOCUMENT_PARAGRAPH, NULL, 0);
    book_document_event_t event = target_event(target);
    write_record(file, DOCUMENT_METADATA, &event, sizeof(event));
    fixture_target = (uint32_t)ftell(file);
    for (unsigned i = 0; i < body_records; ++i) write_record(file, DOCUMENT_TEXT, body, sizeof(body));
    event = end_event(); write_record(file, DOCUMENT_METADATA, &event, sizeof(event));
    fixture_body_end = (uint32_t)ftell(file);
    /* A restored BODY boundary is required input; the following NAV tail is optional. */
    event = (book_document_event_t){0}; event.type = BOOK_DOCUMENT_CHAPTER;
    event.bytes = sizeof(event); event.b = 1;
    write_record(file, DOCUMENT_METADATA, &event, sizeof(event));
    for (unsigned i = 0; i < 96; ++i) {
        unsigned char entry[128] = {0};
        book_document_event_t toc = {0}; toc.type = BOOK_DOCUMENT_METADATA;
        toc.a = BOOK_DOCUMENT_META_TOC; toc.b = 0; toc.c = 1;
        const char title[] = "Section", path[] = "OPS/chapter.xhtml";
        toc.length1 = sizeof(title) - 1; toc.length2 = sizeof(path) - 1;
        toc.bytes = (sizeof(toc) + toc.length1 + toc.length2 + 3u) & ~3u;
        memcpy(entry, &toc, sizeof(toc)); memcpy(entry + sizeof(toc), title, toc.length1);
        memcpy(entry + sizeof(toc) + toc.length1, path, toc.length2);
        write_record(file, DOCUMENT_METADATA, entry, toc.bytes);
    }
    fixture_end = (uint32_t)ftell(file); fclose(file); committed = true;
}

static document_t *open_document(bool prefetch)
{
    bookshelf_item_t file = {0}; char error[160];
    strcpy(file.path, "/flash/restore.epub"); file.format = DOCUMENT_EPUB;
    file.size = 100000; file.modified = 1;
    document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 0, 128, 0};
    document_t *d = document_open_at(&file, &anchor, error, sizeof(error)); assert(d);
    current_document = d;
    document_prefetch_enable(d, prefetch);
    return d;
}

static void close_document(document_t *d)
{
    document_close(d); drain();
    assert(!documents && !allocated_bytes && !record_requests && !image_requests && !saves);
    assert(pipe_opens == pipe_closes && !fd_live); assert_compute_released();
}

static void reach_target(document_t *d)
{
    for (unsigned i = 0; !d->target_ready; ++i) {
        assert(i < 100 && content_pending(d) == d->claimed);
        assert(document_worker_step(&service)); document_service_process();
        assert(d->status.result == BOOK_OK);
    }
    uint32_t record, byte;
    assert(document_target_cursor(d, &record, &byte) == 1 && record == fixture_target && !byte);
    for (unsigned i = 0; !d->body_end; ++i) {
        assert(i < 100 && content_pending(d) && "RESTORE_BODY_FRONTIER");
        assert(document_worker_step(&service)); document_service_process();
        assert(d->status.result == BOOK_OK);
    }
    assert(d->body_end == fixture_body_end && d->chapter_restore &&
        d->chapter_scan == fixture_body_end && "RESTORE_GATE_PAUSE");
    document_progress_t progress; document_progress(d, &progress);
    assert(progress.end == fixture_body_end && !document_navigation_ready(d));
}

static void expect_idle(document_t *d)
{
    uint32_t scan = d->chapter_scan;
    unsigned reads = io_reads, opens = io_opens;
    for (unsigned i = 0; i < 20; ++i) {
        assert(!content_pending(d) && "RESTORE_GATE_PAUSE");
        content_work(d);
        assert(!document_worker_step(&service) && "RESTORE_GATE_PAUSE");
    }
    assert(d->chapter_scan == scan && io_reads == reads && io_opens == opens);
}

static void complete_restore(document_t *d)
{
    document_prefetch_enable(d, true);
    /* This fixture contains a single selected chapter and no adjacent source. */
    d->adjacent_pending = false;
    drain();
    assert(!d->chapter_restore && d->chapter_scan == fixture_end && d->status.result == BOOK_OK);
    assert(document_navigation_ready(d));
    document_location_t *p = d->locations;
    /* BODY_BEGIN and the explicit TARGET each publish a zero-length position. */
    for (unsigned i = 0; i < 2; ++i, p = p->next) {
        assert(p && p->record == fixture_target && !p->bytes);
        assert(p->anchor.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8 &&
            p->anchor.chapter == 0 && p->anchor.offset == 128 && !p->anchor.skip_utf8);
    }
    for (unsigned i = 0; i < body_records; ++i, p = p->next) {
        assert(p && p->record == fixture_target + i * (sizeof(document_record_t) + sizeof(body)));
        assert(p->bytes == sizeof(body) && p->anchor.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8 &&
            p->anchor.chapter == 0 && p->anchor.offset == 128 && p->anchor.skip_utf8 == i * sizeof(body));
    }
    assert(!p && d->source_total == 128 + body_records * sizeof(body));
}

static void cached_target(void)
{
    make_spool(true); unsigned opens = pipe_opens;
    document_t *d = open_document(false);
    reach_target(d); expect_idle(d);
    assert(pipe_opens == opens && d->status.complete && d->persistent);
    complete_restore(d); close_document(d);
}

static void prefetch_toggle(void)
{
    make_spool(true); document_t *d = open_document(false); reach_target(d);
    unsigned wakes = service_wakes;
    document_prefetch_enable(d, true); d->adjacent_pending = false;
    assert(service_wakes > wakes && content_pending(d));
    assert(document_worker_step(&service));
    assert(d->chapter_scan > fixture_body_end && d->chapter_scan < fixture_end);
    document_prefetch_enable(d, false); expect_idle(d);
    complete_restore(d); close_document(d);
}

static void suspend_resume(void)
{
    make_spool(true); document_t *d = open_document(false);
    assert(document_worker_step(&service) && !d->target_ready && d->chapter_restore);
    uint32_t before = d->chapter_scan;
    reader_suspended = true; document_prefetch_enable(d, false); expect_idle(d);
    reader_suspended = false; document_prefetch_enable(d, false);
    assert(content_pending(d)); reach_target(d);
    assert(d->chapter_scan > before); expect_idle(d);
    reader_suspended = true; document_prefetch_enable(d, false); expect_idle(d);
    reader_suspended = false; document_prefetch_enable(d, false); expect_idle(d);
    complete_restore(d); close_document(d);
}

static void record_priority(void)
{
    make_spool(true); document_t *d = open_document(false); reach_target(d); expect_idle(d);
    uint32_t scan = d->chapter_scan;
    unsigned reads = io_reads;
    document_record_request_t *background = document_record_request(d, fixture_target, false);
    document_record_request_t *foreground = document_record_request(d, fixture_target, true);
    document_record_data_t result;
    assert(background && foreground && document_record_take(foreground, &result) == 0 && io_reads == reads);
    document_prefetch_enable(d, true); d->adjacent_pending = false;
    assert(document_worker_step(&service));
    assert(document_record_take(foreground, &result) == 1 && document_record_take(background, &result) == 0);
    assert(result.record.kind == DOCUMENT_TEXT && result.record.length == sizeof(body) && !memcmp(result.text, body, sizeof(body)));
    assert(d->chapter_scan == scan);
    document_prefetch_enable(d, false);
    assert(document_worker_step(&service) && document_record_take(background, &result) == 1 && d->chapter_scan == scan);
    document_record_cancel(background); document_record_cancel(foreground); document_service_process();
    expect_idle(d); complete_restore(d); close_document(d);
}

static void missing_target(void)
{
    make_spool(false); document_t *d = open_document(false); drain();
    uint32_t record, byte;
    assert(!d->target_ready && !d->chapter_restore && d->chapter_scan == fixture_end);
    assert(document_target_cursor(d, &record, &byte) == -1);
    expect_idle(d); close_document(d);
}

static void target_owner(void)
{
    make_spool(true);
    document_t *target = open_document(false);
    assert(document_worker_step(&service) && target->chapter_restore);
    bookshelf_item_t file = {0}; char error[160];
    strcpy(file.path, "/flash/optional.epub"); file.format = DOCUMENT_EPUB;
    file.size = 100000; file.modified = 1;
    document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 0, 128, 0};
    document_t *neighbor = document_open_at(&file, &anchor, error, sizeof(error));
    assert(neighbor && neighbor != target);
    document_prefetch_enable(neighbor, false);
    assert(flow_owner() == neighbor && !flow_allows(target));
    assert(document_target_require(target, true) == 0);
    assert(flow_allows(target) && "TARGET_BLOCKED_BY_OPTIONAL_OWNER");
    for (unsigned i = 0; document_target_require(target, true) == 0; ++i) {
        assert(i < 100);
        assert(document_worker_step(&service));
        document_service_process();
    }
    assert(target->body_end == fixture_body_end);
    document_target_require(target, false);
    assert(!flow_allows(target));
    document_close(neighbor);
    close_document(target);
}

static void cold_conversion(void)
{
    committed = false; _unlink(persistent_path);
    book_document_event_t source = target_event(true);
    book_document_event_t text = {0}; text.type = BOOK_DOCUMENT_TEXT;
    epd_paragraph_style_t style = {0};
    text.length1 = sizeof(body);
    text.length2 = sizeof(style);
    text.bytes = RT_ALIGN(sizeof(text) + text.length1 + text.length2, 4);
    wire_bytes = 2 * sizeof(source) + text.bytes * body_records;
    wire_data = calloc(1, wire_bytes); assert(wire_data); memcpy(wire_data, &source, sizeof(source));
    size_t at = sizeof(source);
    for (unsigned i = 0; i < body_records; ++i) {
        memcpy(wire_data + at, &text, sizeof(text)); at += sizeof(text);
        memcpy(wire_data + at, body, sizeof(body)); at += sizeof(body);
        memcpy(wire_data + at, &style, sizeof(style));
        at += text.bytes - sizeof(text) - sizeof(body);
    }
    source = end_event(); memcpy(wire_data + at, &source, sizeof(source));
    unsigned opens = pipe_opens; pipe_events_per_step = 1;
    document_t *d = open_document(false); drain();
    assert(d->target_ready && d->converting && pipe_opens == opens + 1);
    unsigned steps = pipe_steps;
    for (unsigned i = 0; i < 20; ++i) assert(!document_worker_step(&service));
    assert(pipe_steps == steps);
    document_record_request_t *job = document_record_request(d, d->target_record, true); assert(job);
    drain(); document_record_data_t result;
    assert(document_record_take(job, &result) == 1 && !memcmp(result.text, body, sizeof(body)));
    document_record_cancel(job); document_service_process();
    document_prefetch_enable(d, true); d->adjacent_pending = false; drain();
    assert(d->status.complete && d->persistent && d->status.result == BOOK_OK);
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
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root);
    memset(body, 'x', sizeof(body));
    if (!strcmp(argv[2], "cached-target")) cached_target();
    else if (!strcmp(argv[2], "prefetch-toggle")) prefetch_toggle();
    else if (!strcmp(argv[2], "suspend-resume")) suspend_resume();
    else if (!strcmp(argv[2], "record-priority")) record_priority();
    else if (!strcmp(argv[2], "missing-target")) missing_target();
    else if (!strcmp(argv[2], "cold-conversion")) cold_conversion();
    else if (!strcmp(argv[2], "target-owner")) target_owner();
    else assert(false);
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    assert(!allocated_bytes && !documents && !fd_live && pipe_opens == pipe_closes);
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    printf("PASS %s reads=%u pipes=%u steps=%u\n", argv[2], io_reads, pipe_opens, pipe_steps);
    return 0;
}
