/* SPDX-License-Identifier: Apache-2.0 */
#define main hcpu_standard_main
#include "body_nav_host.inc"
#undef main

typedef void view_state_t;
static document_t *selected_document;
static int selection_error;
static struct { unsigned session; } ui = {1};
enum { INTENT_TOC = 1 };
static int selected_index = -1;
static document_t *ui_document(void) { return view.document; }
static void ui_command(int kind, int index, document_cursor_t cursor)
{ (void)cursor; assert(kind == INTENT_TOC); selected_index = index; }
static void fail(int error) { selection_error = error; }
static void target_select(document_t *d, const char *target, view_state_t **current)
{ (void)target; (void)current; if (!d) { fail(READER_ERROR_LINK_MISSING); return; }
  assert(!selected_document); selected_document = d; }
#include "navigation_functions.inc"

static void load_phase(const char *directory, const char *name, uint8_t **out, size_t *size)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.wire", directory, name);
    FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); *size = ftell(f); rewind(f);
    *out = malloc(*size ? *size : 1); assert(*out);
    assert(fread(*out, 1, *size, f) == *size); fclose(f);
}
static void step(void)
{
    document_worker_step(&service); document_service_process();
}
static void body_finish(document_t *d)
{
    document_prefetch_enable(d, true);
    for (unsigned i = 0; !d->status.complete; ++i) {
        assert(i < 10000);
        if (d->body_end && d->pipe) {
            document_progress_t p; document_progress(d, &p); assert(!p.complete);
        }
        step();
    }
    assert(d->status.result == BOOK_OK && d->body_end == d->status.end);
    assert(!d->converting && !conversion_document && d->output < 0 && d->finish_stage && !d->observing);
    assert(d->nav_state == DOCUMENT_NAV_PENDING && !nav_opens);
    document_prefetch_enable(d, false);
    d->adjacent_pending = false;
}
static void at_navigation(document_t *d, unsigned count)
{
    assert(document_navigation_request(d) == 0);
    for (unsigned i = 0; d->nav_cursor < count; ++i) { assert(i < 30000); step(); }
    assert(d->nav_cursor == count && d->nav_state == DOCUMENT_NAV_ACTIVE);
}
static void navigation_finish(document_t *d)
{
    document_progress_t before, after; document_progress(d, &before);
    unsigned syncs = phased_syncs;
    document_navigation_request(d);
    for (unsigned i = 0; d->nav_state != DOCUMENT_NAV_DONE; ++i) {
        assert(i < 30000 && d->nav_state != DOCUMENT_NAV_FAILED); step();
    }
    assert(document_navigation_ready(d) && d->status.result == BOOK_OK && d->source_next == d->source_total);
    unsigned parts = 0;
    for (document_part_t *p = d->toc_context ? d->toc_context->parts : d->parts; p; p = p->next) ++parts;
    assert(parts == d->chapter_count && d->navigation_bytes <= DOCUMENT_NAVIGATION_BUDGET);
    assert(!d->toc_context || d->toc_context->bytes <= DOCUMENT_TOC_BUDGET);
    assert(!d->converting && !conversion_document && d->output < 0 && !d->observing);
    assert(document_toc_count(d) == (d->chapter_count == 3330 ? 3203 : d->chapter_count));
    document_progress(d, &after);
    assert(before.end == after.end && before.publication == after.publication);
    assert(phased_syncs == syncs);
    printf("PASS navigation parts=%u toc=%u body_nav_bytes=%u toc_bytes=%u opens=%u resumed=%u\n",
        parts, document_toc_count(d), d->navigation_bytes, d->toc_context ? d->toc_context->bytes : 0,
        nav_opens, resumed_opens);
}
static void save_spool(document_t *d)
{
    d->adjacent_pending = false;
    document_prefetch_enable(d, true); d->adjacent_pending = false;
    for (unsigned i = 0; d->finish_stage; ++i) { assert(i < 100); step(); }
    assert(d->persistent && committed);
    document_prefetch_enable(d, false);
}
static uint32_t link_record(document_t *d, bool note)
{
    for (document_navigation_t *n = d->navigation; n; n = n->next)
        if (n->kind == DOCUMENT_LINK && (strstr(n->text, "#n1") != NULL) == note) return n->record;
    assert(false); return 0;
}

/* A current-v10 spool produced before BODY-only persistence can contain an
 * optional navigation tail. Keep that read/fault contract covered explicitly. */
static void append_navigation_tail(void)
{
    FILE *f = fopen(persistent_path, "ab"); assert(f);
    for (size_t at = 0; at < phase_nav_bytes;) {
        book_document_event_t e; memcpy(&e, phase_nav + at, sizeof(e));
        assert(e.bytes && at + e.bytes <= phase_nav_bytes);
        if (e.type == BOOK_DOCUMENT_CHAPTER ||
            (e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_TOC)) {
            document_record_t record = {DOCUMENT_METADATA, e.bytes, 0, 0};
            assert(fwrite(&record, sizeof(record), 1, f) == 1);
            assert(fwrite(phase_nav + at, 1, e.bytes, f) == e.bytes);
        }
        at += e.bytes;
    }
    assert(!fclose(f));
}

#include "epub_body_nav_admission.h"

static void warm_failure(document_t *d, const char *name, uint32_t body_end)
{
    bool body_alloc = !strcmp(name, "warm-body-alloc");
    if (body_alloc) {
        step(); assert(d->chapter_restore && !d->body_end && d->chapter_scan < body_end);
        document_reader_destroy(d->chapter_reader); d->chapter_reader = NULL;
        allocation_failures = 1; pump();
        assert(!allocation_failures && d->status.result == BOOK_NO_MEMORY);
        puts("PASS body restore allocation failure remains fatal"); return;
    }
    pump();
    assert(d->body_end == body_end && d->chapter_scan == body_end && d->chapter_restore);
    document_progress_t before, after; document_progress(d, &before);
    assert(before.complete && before.result == BOOK_OK && before.end == body_end);
    uint8_t expected[16], actual[16];
    FILE *f = fopen(persistent_path, "rb"); assert(f);
    assert(!fseek(f, body_end - sizeof(expected), SEEK_SET));
    assert(fread(expected, 1, sizeof(expected), f) == sizeof(expected)); fclose(f);
    document_reader_t *reader = document_reader_create(d); assert(reader);
    if (!strcmp(name, "warm-body-read")) {
        warm_read_failure_at = body_end - 1;
        assert(!document_reader_read(reader, body_end - sizeof(actual), actual, sizeof(actual)));
        assert(d->status.result == BOOK_IO_ERROR && warm_read_failures);
        warm_read_failure_at = -1; document_reader_destroy(reader);
        puts("PASS foreground body read failure remains fatal during pending navigation restore"); return;
    }
    assert(document_navigation_request(d) == 0);
    if (!strcmp(name, "warm-cancel")) d->cancelled = true;
    else if (!strcmp(name, "warm-media")) ++media_generation;
    else if (!strcmp(name, "warm-nav-short") || !strcmp(name, "warm-whole-truncated")) {
        d->chapter_reader->window_used = 0;
        f = fopen(persistent_path, "r+b"); assert(f);
        assert(!_chsize(_fileno(f), body_end - (!strcmp(name, "warm-whole-truncated") ? 16 : 0)));
        fclose(f);
    }
    else if (!strcmp(name, "warm-nav-read")) {
        d->chapter_reader->window_used = 0; warm_read_failure_at = body_end;
    } else if (!strcmp(name, "warm-nav-record")) {
        d->chapter_reader->window_used = 0;
        f = fopen(persistent_path, "r+b"); assert(f);
        uint32_t invalid_kind = UINT32_MAX;
        assert(!fseek(f, body_end, SEEK_SET));
        assert(fwrite(&invalid_kind, sizeof(invalid_kind), 1, f) == 1); fclose(f);
    } else {
        if (!strcmp(name, "warm-nav-reader-alloc")) {
            document_reader_destroy(d->chapter_reader); d->chapter_reader = NULL;
        }
        allocation_failures = 1;
    }
    pump();
    if (!strcmp(name, "warm-whole-truncated")) {
        assert(d->status.result == BOOK_IO_ERROR && !document_navigation_ready(d));
        assert(!document_reader_read(reader, body_end - sizeof(actual), actual, sizeof(actual)));
        puts("PASS file truncated into body remains a whole-document failure");
    } else if (!strcmp(name, "warm-cancel") || !strcmp(name, "warm-media")) {
        assert(d->status.result == BOOK_CANCELLED && !document_navigation_ready(d));
        assert(!document_reader_read(reader, body_end - sizeof(actual), actual, sizeof(actual)));
        puts("PASS cancellation/media change still invalidates body and navigation");
    } else {
        document_progress(d, &after);
        assert(d->nav_state == DOCUMENT_NAV_FAILED && document_navigation_request(d) == -1);
        assert(!d->chapter_restore && !d->nav_requested && !d->finish_stage);
        assert(after.complete && after.result == BOOK_OK && before.end == after.end &&
            before.publication == after.publication && !allocation_failures);
        bool memory_failure = strstr(name, "alloc") != NULL;
        assert(d->nav_result == (memory_failure ? BOOK_NO_MEMORY : BOOK_IO_ERROR));
        assert(memory_failure ? committed : !committed);
        assert(document_reader_read(reader, body_end - sizeof(actual), actual, sizeof(actual)));
        assert(!memcmp(expected, actual, sizeof(actual)));
        document_anchor_t anchor; document_chapter_range_t range;
        assert(document_source_position(d, d->target_record, 0, &anchor));
        assert(document_chapter_range(d, d->target_record, &range) && range.end <= body_end);
        if (warm_read_failure_at >= 0) assert(warm_read_failures);
        unsigned failures = warm_read_failures, opens = pipe_opens;
        document_prefetch_enable(d, true); d->adjacent_pending = false;
        document_chapter_prefetch(d, body_end - 1); pump();
        assert(d->prefetch_done == d->prefetch_request && d->status.result == BOOK_OK);
        assert(warm_read_failures == failures && pipe_opens == opens);
        document_prefetch_enable(d, false);
        puts("PASS warm navigation failure preserves body bytes/source/boundary/publication; no retry or full-cache commit");
    }
    warm_read_failure_at = -1;
    document_reader_destroy(reader);
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root);
    load_phase(argv[2], "first", &wire_data, &wire_bytes);
    load_phase(argv[2], "body", &phase_body, &phase_body_bytes);
    load_phase(argv[2], "nav", &phase_nav, &phase_nav_bytes);
    load_phase(argv[2], "restore", &phase_restore, &phase_restore_bytes);
    if (!strcmp(argv[3], "note")) load_phase(argv[2], "note", &phase_note, &phase_note_bytes);
    bool image_restore = !strcmp(argv[3], "image-restore");
    bool restore_cache = !strcmp(argv[3], "restore-cache");
    bool restore = !strcmp(argv[3], "restore") || !strcmp(argv[3], "restore-start") || image_restore || restore_cache;
    document_anchor_t source = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 7, 4096, 0};
    if (strcmp(argv[3], "restore") && !restore_cache) source.offset = 0;
    if (image_restore) source.chapter = 1;
    flow_open_at(argv[3], restore ? &source : NULL);
    document_t *d = view.document;
    view.building->collect = true;
    for (unsigned i = 0; !view.building->done; ++i) {
        assert(i < 4096);
        document_progress(d, &view.progress);
        if (page_runnable(view.building) || view.compute_page == view.building)
            assert(page_step(view.building, false, NULL, &view.progress) >= 0);
        if (!view.building->done) flow_worker();
    }
    assert((image_restore || view.building->bytes > 40) && !d->explicit_toc_count && !nav_opens);
    assert(d->chapter == (d->chapter_count == 3330 ? 7 : 1));
    bool has_image = false;
    for (unsigned i = 0; i < view.building->count; ++i) has_image |= view.building->runs[i].image != 0;
    assert(has_image == image_restore);
    if (d->chapter_count == 3330 && !restore) {
        const char opening[] = "上午七点三十五分，石神像平常一样走出公寓。";
        bool found = false;
        for (size_t i = 0; i + sizeof(opening) - 1 <= view.building->bytes; ++i)
            if (!memcmp(view.building->text + i, opening, sizeof(opening) - 1)) found = true;
        assert(found);
    }
    if (restore) {
        document_anchor_t actual;
        assert(!d->first_body && document_source_position(d, d->target_record, 0, &actual));
        assert(actual.chapter == source.chapter && actual.offset <= source.offset &&
            actual.offset + 3 >= source.offset);
    }
    printf("PASS %s page chapter=%u bytes=%u nav=0 opens=%u\n",
        image_restore ? "image restore" : "real text", d->chapter, view.building->bytes, body_opens);
    for (unsigned i = 0; i < 3; ++i) page_reset(&flow_pages[i], (document_cursor_t){0,0});
    reader_compute_release(view.compute_spare); view.compute_spare = NULL;
    document_service_process();
    if (!strncmp(argv[3], "admission-", 10)) {
        admission_test(d, argv[3]); flow_close(); goto done;
    }
    if (!strcmp(argv[3], "target-drain")) {
        assert(!d->status.complete && !d->prefetch_enabled && !d->nav_requested);
        assert(document_target_require(d, true) == 0 && d->target_required);
        unsigned wakes = service_wakes;
        for (unsigned i = 0; i < 20; ++i) assert(document_target_require(d, true) == 0);
        assert(service_wakes == wakes && !persistence_allowed());
        for (unsigned i = 0; document_target_require(d, true) == 0; ++i) {
            assert(i < 10000 && !d->prefetch_enabled && !d->nav_requested); step();
        }
        assert(d->status.complete && !nav_opens);
        document_target_require(d, false); assert(!d->target_required);
        puts("PASS required body drain reaches EOF with optional gate closed and repeated wake delta=0");
    }
    body_finish(d);
    uint32_t body_end = d->body_end, source_end = d->source_next;
    unsigned opens = pipe_opens;
    for (unsigned i = 0; i < 20; ++i) step();
    assert(pipe_opens == opens && !nav_opens);
    if (!strcmp(argv[3], "navigation-release")) {
        assert(document_navigation_request(d) == 0);
        unsigned wakes = service_wakes;
        for (unsigned i = 0; i < 20; ++i) assert(document_navigation_request(d) == 0);
        assert(service_wakes == wakes);
        view.navigation_toc = 3200; navigation_clear();
        assert(!d->nav_requested && !view.navigation_toc && !view.navigation_link);
        for (unsigned i = 0; i < 20; ++i) step();
        assert(!nav_opens);
        assert(document_navigation_request(d) == 0); view.navigation_link = 1;
        document_target_require(d, true); link_clear();
        assert(!d->nav_requested && !d->target_required && !view.navigation_link);
        navigation_finish(d);
        puts("PASS cancelled navigation/target demand clears; repeated navigation wake delta=0");
    } else if (!strcmp(argv[3], "query")) {
        assert(!epd_reader_toc_count() && !d->nav_requested);
        assert(epd_reader_toc_prepare(true) == 0 && d->nav_requested);
        epd_reader_toc_prepare(false); assert(!d->nav_requested);
        for (unsigned i = 0; i < 20; ++i) step();
        assert(!nav_opens && pipe_opens == opens);
        assert(epd_reader_toc_seek(60) && selected_index == 60 && d->nav_requested);
        navigation_finish(d);
        assert(!epd_reader_toc_seek(4000));
        puts("PASS count query stays read-only; explicit pending TOC seek queues");
    } else if (!strcmp(argv[3], "nav-preempt-body")) {
        char error[96]; document_anchor_t next = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, d->chapter + 1, 0, 0};
        document_t *other = document_open_at_internal(&d->file, &next, error, sizeof(error), false, false);
        assert(other); document_prefetch_enable(other, true);
        for (unsigned i = 0; !other->pipe; ++i) { assert(i < 100); step(); }
        document_prefetch_enable(d, false);
        pipe_shutdown_failures = 2;
        assert(document_navigation_request(d) == 0);
        step(); step(); assert(!nav_opens && conversion_document == other);
        navigation_finish(d);
        assert(other->cancelled && d->status.result == BOOK_OK);
        document_close(other);
        puts("PASS explicit navigation closes a paused speculative body before OPEN");
    } else if (!strcmp(argv[3], "flow")) {
        document_t gate = {0};
        gate.owners = 1; gate.status.complete = gate.demand_controlled = true;
        gate.flow_order = ++flow_sequence; gate.next = documents; documents = &gate;
        assert(document_navigation_request(d) == 0 && !navigation_allowed(d) && "NAV_FLOW_GATE");
        unsigned steps = pipe_steps;
        for (unsigned i = 0; i < 20; ++i) step();
        assert(!nav_opens && pipe_steps == steps && "NAV_FLOW_GATE");
        gate.prefetch_enabled = true; at_navigation(d, 25);
        gate.prefetch_enabled = false; steps = pipe_steps;
        for (unsigned i = 0; i < 20; ++i) step();
        assert(pipe_steps == steps && d->nav_cursor == 25 && "NAV_FLOW_GATE");
        assert(epd_reader_toc_prepare(true) == 0 && flow_owner() == d && navigation_allowed(d));
        documents = gate.next;
        navigation_finish(d);
        puts("PASS pending and active navigation obey latest near-page flow owner");
    } else if (!strcmp(argv[3], "split-unchanged")) {
        bookshelf_item_t file = d->file; file.format = BOOK_FORMAT_SPLIT_EPUB;
        snprintf(file.path, sizeof(file.path), "/flash/split-book");
        char error[96]; document_t *split = document_open(&file, error, sizeof(error)); assert(split);
        assert(!split->body_first && !split->first_body && split->requested.chapter == 0);
        document_close(split);
        navigation_finish(d);
        puts("PASS directory-based split EPUB retains its existing conversion path");
    } else if (!strcmp(argv[3], "note")) {
        at_navigation(d, 25);
        pipe_shutdown_failures = 2;
        document_note_request_t *note = document_note_request(d, link_record(d, true)); assert(note);
        step(); step(); assert(!note_opens && conversion_document == d);
        for (unsigned i = 0; !note->document->status.complete; ++i) { assert(i < 1000); step(); }
        char text[128]; bool truncated;
        assert(document_note_take(note, text, sizeof(text), &truncated) == 1 && strstr(text, "Footnote payload"));
        assert(d->status.result == BOOK_OK && d->nav_cursor == 25 && !d->cancelled);
        document_note_cancel(note);
        navigation_finish(d); assert(note_opens == 1 && resumed_opens == 1);
    } else if (!strcmp(argv[3], "nav-error") || !strcmp(argv[3], "link-error") || !strcmp(argv[3], "toc-error")) {
        view_state_t *current = NULL;
        if (!strcmp(argv[3], "link-error")) follow_link(link_record(d, false) + 1, &current);
        if (!strcmp(argv[3], "toc-error")) follow_toc(60, &current);
        pipe_done_error = BOOK_IO_ERROR;
        document_navigation_request(d);
        for (unsigned i = 0; d->nav_state != DOCUMENT_NAV_FAILED; ++i) { assert(i < 1000); step(); }
        assert(d->status.result == BOOK_OK && d->nav_result == BOOK_IO_ERROR && d->finish_stage);
        document_progress(d, &view.progress); assert(view.progress.complete && view.progress.end == body_end);
        assert(document_navigation_request(d) == -1);
        navigation_resolve(&current);
        if (strcmp(argv[3], "nav-error"))
            assert(selection_error == READER_ERROR_LINK_MISSING && !view.navigation_link && !view.navigation_toc && !d->nav_requested);
        puts("PASS navigation error preserves completed body");
    } else if (!strcmp(argv[3], "link") || !strcmp(argv[3], "toc")) {
        view_state_t *current = NULL;
        if (!strcmp(argv[3], "link")) follow_link(link_record(d, false) + 1, &current);
        else follow_toc(60, &current);
        assert(!selected_document && (view.navigation_link || view.navigation_toc) && !selection_error);
        for (unsigned i = 0; !selected_document; ++i) {
            assert(i < 2000 && !selection_error); step(); navigation_resolve(&current);
        }
        assert(selected_document->requested.chapter == (!strcmp(argv[3], "link") ? 64 : 60));
        assert(!selected_document->first_body && d->nav_state != DOCUMENT_NAV_DONE && !d->cancelled && !d->nav_requested);
        document_close(selected_document); selected_document = NULL;
        navigation_finish(d);
        puts("PASS pending navigation selection resolves before full TOC");
    } else if (!strcmp(argv[3], "preempt")) {
        at_navigation(d, 25);
        bookshelf_item_t file = d->file; char error[96];
        document_anchor_t next = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, d->chapter + 1, 0, 0};
        document_t *target = document_open_at(&file, &next, error, sizeof(error)); assert(target);
        document_prefetch_enable(target, false);
        for (unsigned i = 0; !target->pipe; ++i) { assert(i < 100); step(); }
        assert(!d->cancelled && d->status.result == BOOK_OK && d->nav_state == DOCUMENT_NAV_PENDING);
        document_close(target);
        navigation_finish(d); assert(resumed_opens == 1);
    } else if (!strcmp(argv[3], "cancel") || !strcmp(argv[3], "media")) {
        at_navigation(d, 25);
        if (!strcmp(argv[3], "media")) ++media_generation;
    } else if (!strncmp(argv[3], "body-cache-", 11) || restore_cache) {
        body_cache_test(d, argv[3]);
    } else navigation_finish(d);
    assert(d->body_end == body_end && d->source_next == source_end);
    bookshelf_item_t file = d->file;
    bool body_cache = !strncmp(argv[3], "body-cache-", 11) || restore_cache;
    bool warm = !strcmp(argv[3], "warm") || !strncmp(argv[3], "warm-", 5) || body_cache;
    document_anchor_t saved_request = d->requested, saved_source;
    bool first_body = d->first_body;
    if (body_cache) assert(document_source_position(d, d->target_record, 3, &saved_source));
    if (warm) {
        if (!body_cache) save_spool(d);
        if (!strncmp(argv[3], "warm-", 5)) append_navigation_tail();
    }
    flow_close();
    if (warm) {
        opens = pipe_opens; char error[96];
        d = first_body ? document_open(&file, error, sizeof(error)) :
            document_open_at(&file, &saved_request, error, sizeof(error)); assert(d);
        document_prefetch_enable(d, false);
        if (body_cache) {
            pump(); document_anchor_t after;
            assert(d->persistent && d->status.complete && d->status.result == BOOK_OK);
            assert(!d->chapter_restore && d->body_end == body_end && pipe_opens == opens);
            assert(document_source_position(d, d->target_record, 3, &after));
            assert(saved_source.unit == after.unit && saved_source.chapter == after.chapter &&
                saved_source.offset == after.offset && saved_source.skip_utf8 == after.skip_utf8);
            printf("PASS v10 close/reopen source=%u:%u:%u:%u BODY cache hit without NAV\n",
                after.unit, after.chapter, after.offset, after.skip_utf8);
        }
        else if (strcmp(argv[3], "warm")) warm_failure(d, argv[3], body_end);
        else {
        pump();
        assert(pipe_opens == opens && d->body_end == body_end && !d->chapter_restore && !d->explicit_toc_count);
        assert(!document_toc_count(d) && !document_toc_entry(d, 0, NULL, 0, NULL, 0, NULL));
        document_progress(d, &view.progress); assert(view.progress.end == body_end && view.progress.complete);
        assert(document_navigation_request(d) == 0); pump();
        assert(document_navigation_ready(d) && document_toc_count(d) == 3203 && pipe_opens == opens + 1);
        puts("PASS warm BODY-only cache needs no BODY converter; optional navigation rebuilds in memory");
        }
        assert(pipe_opens == opens + !strcmp(argv[3], "warm"));
        document_close(d); pump();
    }
done:
    free(phase_body); free(phase_nav); free(phase_restore); free(phase_note);
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    assert(!allocated_bytes && !fd_live && !documents && !observers && !flow_jobs && pipe_opens == pipe_closes);
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    return 0;
}
