/* SPDX-License-Identifier: Apache-2.0 */
#include "hcpu_host.h"
#include <assert.h>
#undef assert
#define assert(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); exit(1); } } while (0)
#include "hcpu_fd_host.h"
#ifdef HCPU_DOCUMENT_SOURCE
#include "document_under_test.c"
#else
#include "../../modules/books/document/document.c"
#endif

static char test_root[512], persistent_path[512], index_file[512];
static unsigned pipe_opens, pipe_closes, pipe_steps, cache_commits, observers, observe_begins, observe_ends;
static unsigned pipe_navigation_opens;
static unsigned pipe_step_ticks;
static unsigned pipe_events_per_step;
static int pipe_done_error;
static int pipe_terminal_status;
static bool pipe_bad_done;
static document_save_t *borrowed_save;
static bool committed, replay_cancel, replay_nomem;
static uint8_t *wire_data;
static size_t wire_bytes;
static uint8_t *note_wire;
static size_t note_wire_bytes;
static uint8_t *next_wire_data;
static size_t next_wire_bytes;
static document_anchor_t expected_open;
static document_t *current_document;
static epd_service_t service;
static int cache_token, card_cache_token;
static bool cache_low_space;
static unsigned cache_evictions;
static bool reader_suspended;
static bool metadata_over_limit;
static unsigned cache_retired_attempts;
bool epd_reader_suspended(void) { return reader_suspended; }

struct epd_compute_pipe_host {
    epd_compute_pipe_message_t message;
    size_t at;
    unsigned steps;
    document_t *document;
    const uint8_t *wire;
    size_t wire_length;
    bool cancelled;
    uint8_t data[EPD_COMPUTE_PIPE_BYTES];
};
int epd_compute_pipe_open(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *initial, const void *data, size_t length, epd_compute_pipe_host_t *out)
{
    (void)purpose; (void)data; (void)length;
    assert(!strcmp(name, BOOK_DOCUMENT_COMPONENT) && version == BOOK_DOCUMENT_VERSION);
    current_document = worker_document;
    assert(pipe_opens == pipe_closes || (current_document->note && pipe_opens == pipe_closes + 1));
    if (current_document->note) {
        assert(initial->values[0] == BOOK_DOCUMENT_WIRE_VERSION && initial->values[6] == BOOK_DOCUMENT_NOTE);
        assert(initial->values[1] == BOOK_DOCUMENT_NOTE_BUDGET);
        assert(length == strlen(current_document->note->target) + 1 && !memcmp(data, current_document->note->target, length));
    } else if (current_document->metadata_only) {
        assert(purpose == EPD_COMPUTE_BACKGROUND);
        assert(initial->values[6] == BOOK_DOCUMENT_METADATA_ONLY);
        assert(initial->values[2] == DOCUMENT_COVER_WIDTH && initial->values[3] == DOCUMENT_COVER_HEIGHT);
    } else if (current_document->segmented) {
        uint32_t flags = BOOK_DOCUMENT_SELECT_CHAPTER | BOOK_DOCUMENT_CONTEXT;
        if (current_document->full_source) flags |= BOOK_DOCUMENT_FULL_SOURCE;
#ifdef BOOK_DOCUMENT_BODY_ONLY
        if (current_document->body_first) {
            if (current_document->nav_state == DOCUMENT_NAV_ACTIVE) {
                assert(purpose == EPD_COMPUTE_BACKGROUND);
                flags |= BOOK_DOCUMENT_NAV_ONLY;
            } else {
                flags |= BOOK_DOCUMENT_BODY_ONLY;
                if (current_document->first_body) flags |= BOOK_DOCUMENT_FIRST_BODY;
            }
        }
#endif
        if (current_document->body_first && current_document->nav_state == DOCUMENT_NAV_ACTIVE) {
            assert(initial->values[4] == current_document->chapter && !initial->values[5]);
            assert(initial->size == current_document->nav_cursor);
        } else {
            assert(initial->values[4] == current_document->requested.chapter && initial->values[5] == current_document->requested.offset);
            assert(initial->size == current_document->requested.skip_utf8);
        }
        assert(initial->values[6] == flags);
    } else assert(!initial->values[4] && !initial->values[5] && !initial->size && !initial->values[6]);
    *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->message = *initial; (*out)->document = current_document; ++pipe_opens;
    bool next = initial->values[4] == 1 && next_wire_data;
    (*out)->wire = next ? next_wire_data : wire_data;
    (*out)->wire_length = next ? next_wire_bytes : wire_bytes;
    if (current_document->note) { (*out)->wire = note_wire; (*out)->wire_length = note_wire_bytes; }
    /* This fixture has no global navigation entries. NAV_ONLY completes without
     * replaying the body stream, as the separate conversion mode requires. */
    if (initial->values[6] & BOOK_DOCUMENT_NAV_ONLY) {
        ++pipe_navigation_opens; (*out)->wire_length = 0;
    }
    return EPD_COMPUTE_OK;
}
int epd_compute_pipe_step(epd_compute_pipe_host_t p, epd_compute_pipe_service_t callback, void *user)
{
    assert(!storage_depth && !irq_depth);
    ++pipe_steps; ++p->steps;
    host_ticks += pipe_step_ticks;
    if (p->cancelled) return EPD_COMPUTE_CANCELLED;
    if (replay_cancel && p->steps == 3) { document_close(p->document); return EPD_COMPUTE_CANCELLED; }
    if (p->steps & 1) return EPD_COMPUTE_MORE;
    if (pipe_done_error) {
        p->message.operation = EPD_PIPE_DONE; p->message.status = pipe_done_error;
        p->message.values[0] = pipe_bad_done ? 0 : BOOK_DOCUMENT_WIRE_VERSION;
        const char *error = "Source I/O failed";
        p->message.length = (uint32_t)strlen(error); memcpy(p->data, error, p->message.length);
        return pipe_terminal_status ? pipe_terminal_status : EPD_COMPUTE_COMPONENT_ERROR;
    }
    if (p->at == p->wire_length) {
        p->message.operation = EPD_PIPE_DONE; p->message.status = BOOK_OK;
        p->message.values[0] = BOOK_DOCUMENT_WIRE_VERSION; p->message.length = 0;
        return EPD_COMPUTE_OK;
    }
    size_t start = p->at;
    unsigned events = 0;
    while (p->at < p->wire_length) {
        book_document_event_t e; memcpy(&e, p->wire + p->at, sizeof(e));
        assert(e.bytes && e.bytes <= p->wire_length - p->at);
        if (p->at - start + e.bytes > 2048 && p->at > start) break;
        assert(e.bytes <= sizeof(p->data));
        if (e.type == BOOK_DOCUMENT_SOURCE_ANCHOR) {
            if (e.d & BOOK_DOCUMENT_SOURCE_TARGET) p->message.values[5] = e.b;
            if (e.d & BOOK_DOCUMENT_SOURCE_END) p->message.values[7] = e.b;
        }
        p->at += e.bytes;
        if (pipe_events_per_step && ++events == pipe_events_per_step) break;
    }
    memcpy(p->data, p->wire + start, p->at - start);
    p->message.operation = EPD_PIPE_OUTPUT; p->message.length = (uint32_t)(p->at - start);
    if (replay_nomem) { allocation_failures = 1; replay_nomem = false; }
    int rc = callback(user, &p->message, p->data);
    if (rc < 0) { p->message.operation = EPD_PIPE_DONE; p->message.status = rc; p->message.length = 0; return EPD_COMPUTE_COMPONENT_ERROR; }
    return EPD_COMPUTE_MORE;
}
const epd_compute_pipe_message_t *epd_compute_pipe_result(epd_compute_pipe_host_t p) { return &p->message; }
const uint8_t *epd_compute_pipe_payload(epd_compute_pipe_host_t p) { return p->data; }
void epd_compute_pipe_cancel(epd_compute_pipe_host_t p) { p->cancelled = true; }
int epd_compute_pipe_purpose(epd_compute_pipe_host_t p, epd_compute_purpose_t purpose) { (void)p; (void)purpose; return EPD_COMPUTE_OK; }
void epd_compute_pipe_close(epd_compute_pipe_host_t p) { ++pipe_closes; free(p); }

book_format_t book_format_detect(const char *p, bool directory) { (void)p; (void)directory; return BOOK_FORMAT_UNKNOWN; }
const char *book_format_name(book_format_t f) { (void)f; return "host"; }
bool document_cache_directory(const bookshelf_item_t *f, const char *app, char *out, size_t n)
{ (void)f; (void)app; snprintf(out, n, "%s", test_root); return true; }
document_cache_t *document_cache_create(const bookshelf_item_t *f) { (void)f; return (document_cache_t *)&cache_token; }
document_cache_t *document_cache_create_source(const bookshelf_item_t *f, const char *a, const char *d, const char *i)
{ (void)a; (void)i; return d ? (document_cache_t *)&card_cache_token : document_cache_create(f); }
bool document_cache_temporary_number(const char *d, const char *e, uint32_t *n, bool (*stop)(void *), void *u)
{ (void)d; (void)e; (void)n; (void)stop; (void)u; return false; }
const char *document_cache_bucket(const document_cache_t *c)
{ return c == (document_cache_t *)&card_cache_token ? STORAGE_SD_ROOT "/.epd/cache/books" : c ? test_root : NULL; }
const char *document_cache_binary(const document_cache_t *c) { (void)c; return persistent_path; }
const char *document_cache_index(const document_cache_t *c) { (void)c; return index_file; }
uint32_t document_cache_identity(const document_cache_t *c) { (void)c; return committed ? 1234 : 0; }
bool document_cache_lookup(document_cache_t *c, char *title, size_t n, uint32_t *end)
{
    (void)c;
    if (!committed) return false;
    struct stat st; assert(!stat(persistent_path, &st));
    *end = (uint32_t)st.st_size; snprintf(title, n, "Persisted title"); return true;
}
bool document_cache_prepare(document_cache_t *c)
{ (void)c; if (metadata_over_limit) return false; _unlink(persistent_path); return true; }
bool document_cache_invalidate(document_cache_t *c) { (void)c; committed = false; return true; }
bool document_cache_observe_begin(document_cache_t *c)
{ (void)c; assert(!observers); ++observers; ++observe_begins; return true; }
void document_cache_observe_end(document_cache_t *c)
{ (void)c; assert(observers == 1); --observers; ++observe_ends; }
void document_cache_observe_resource(document_cache_t *c, const char *p, int e) { (void)c; (void)p; (void)e; }
bool document_cache_commit(document_cache_t *c, const char *title, uint32_t end)
{ (void)c; (void)title; (void)end; committed = true; ++cache_commits; return true; }
bool document_cache_touch(document_cache_t *c) { (void)c; return true; }
bool document_cache_has_space(const document_cache_t *c, size_t needed)
{ (void)needed; return c == (document_cache_t *)&card_cache_token || !cache_low_space || cache_evictions >= 2; }
bool document_cache_reclaim(document_cache_t *c)
{ (void)c; if (!cache_low_space) return false; ++cache_evictions; return true; }
void document_cache_destroy(document_cache_t *c) { (void)c; }
void document_cache_release_memory(void) { if (cache_retired_attempts) --cache_retired_attempts; }
bool document_cache_cleanup_pending(void) { return cache_retired_attempts != 0; }
void document_cache_cancel(document_cache_t *c) { (void)c; }
bool document_cache_file_signature(const char *path, uint32_t *signature)
{ (void)path; (void)signature; assert(false); return false; }
#include "hcpu_compute_stubs.h"

static void pump(void)
{
    for (unsigned i = 0; i < 10000; ++i) {
        unsigned before = pipe_steps;
        bool work = document_worker_step(&service);
        assert(pipe_steps - before <= DOCUMENT_CONVERT_STEPS);
        document_service_process();
        if (!work) return;
    }
    assert(false);
}
static void read_wire(const char *name)
{
    char path[600]; snprintf(path, sizeof(path), "%s/%s.wire", test_root, name);
    FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END);
    wire_bytes = (size_t)ftell(f); rewind(f);
    wire_data = malloc(wire_bytes); assert(wire_data);
    assert(fread(wire_data, 1, wire_bytes, f) == wire_bytes); fclose(f);
}
static void finish_navigation_restore(document_t *d)
{
    /* Resume optional navigation after the saved-position target is available. */
    document_prefetch_enable(d, true);
    /* This cache double has one selected-spine path; adjacent ownership is tested separately. */
    d->adjacent_pending = false;
    pump();
    assert(document_navigation_ready(d));
}
static void verify_records(document_t *d, bool epub)
{
    document_progress_t progress; document_progress(d, &progress);
    assert(progress.complete && progress.result == BOOK_OK && progress.end);
    uint32_t at = 0, source = 0, unit = 0, chapter = 0, target = 0;
    unsigned text_count = 0, source_count = 0, metadata_count = 0;
    for (size_t cursor = 0; cursor < wire_bytes;) {
        book_document_event_t e; memcpy(&e, wire_data + cursor, sizeof(e));
        if (e.type == BOOK_DOCUMENT_SOURCE_ANCHOR) {
            source = e.b; unit = e.d & 255u; chapter = e.a; ++source_count;
            if (e.d & BOOK_DOCUMENT_SOURCE_TARGET) target = e.b;
        }
        if (e.type == BOOK_DOCUMENT_TEXT) {
            document_record_data_t data;
            for (;;) {
                unsigned reads = io_reads, opens = io_opens;
                document_record_request_t *job = document_record_request(d, at, true); assert(job);
                assert(document_record_take(job, &data) == 0);
                assert(reads == io_reads && opens == io_opens);
                assert(document_worker_step(&service));
                assert(document_record_take(job, &data) == 1);
                document_record_cancel(job); document_service_process();
                if (data.record.kind == DOCUMENT_TEXT) break;
                if (data.record.kind == DOCUMENT_METADATA) ++metadata_count;
                at += sizeof(document_record_t) + data.record.length;
            }
            assert(data.record.length == e.length1 && !memcmp(data.text, wire_data + cursor + sizeof(e), e.length1));
            document_anchor_t saved;
            uint32_t skip = e.length1 > 3 ? 3 : 0;
            unsigned reads = io_reads, opens = io_opens;
            assert(document_source_position(d, at, skip, &saved));
            assert(reads == io_reads && opens == io_opens);
            assert(saved.unit == unit && saved.chapter == chapter);
            assert(saved.offset == source + (epub ? skip : 0) && saved.skip_utf8 == (epub ? 0 : skip));
            at += sizeof(document_record_t) + data.record.length; ++text_count;
        }
        cursor += e.bytes;
    }
    uint32_t target_record, target_byte;
    assert(document_target_cursor(d, &target_record, &target_byte) == 1 && !target_byte);
    assert(text_count >= (epub && chapter == 1 ? 1u : 2u) && source_count >= text_count && metadata_count);
    assert(d->requested.offset <= target);
    if (epub) { assert(progress.chapters == 2 && document_toc_count(d) >= 2); }
}
static void exercise(const char *name, bool epub)
{
    char error[160]; bookshelf_item_t file = {0};
    snprintf(file.path, sizeof(file.path), "/flash/%s.%s", name, epub ? "epub" : "txt");
    file.size = 100000; file.modified = 1; file.format = epub ? DOCUMENT_EPUB : DOCUMENT_TXT;
    expected_open = (document_anchor_t){epub ? 2u : 1u, epub ? 0u : UINT32_MAX, epub ? 300u : 15000u, epub ? 7u : 39u};
    committed = false; _unlink(persistent_path); read_wire(name);
    unsigned opens = io_opens, reads = io_reads, initial_pipes = pipe_opens;
    allocation_failures = 1;
    assert(!document_open_at(&file, &expected_open, error, sizeof(error)) && !allocated_bytes);
    document_t *d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d);
    current_document = d;
    assert(io_opens == opens && io_reads == reads && pipe_opens == initial_pipes);
    document_record_data_t pending;
    document_record_request_t *early = document_record_request(d, 0, true); assert(early);
    assert(document_record_take(early, &pending) == 0);
    document_record_cancel(early); document_service_process();
    pump(); assert(committed && d->persistent);
    assert(pipe_opens == initial_pipes + 1 && pipe_opens == pipe_closes);
    assert(document_navigation_ready(d)); verify_records(d, epub);
    unsigned toc_count = document_toc_count(d);
    document_close(d); pump(); assert(!documents && !allocated_bytes);
    initial_pipes = pipe_opens;
    d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d); current_document = d;
    assert(!document_navigation_ready(d)); pump();
    assert(pipe_opens == initial_pipes && d->target_ready && d->chapter_restore);
    unsigned paused_reads = io_reads, paused_opens = io_opens;
    uint32_t paused_scan = d->chapter_scan;
    for (unsigned i = 0; i < 16; ++i) assert(!document_worker_step(&service));
    assert(io_reads == paused_reads && io_opens == paused_opens && d->chapter_scan == paused_scan);
    finish_navigation_restore(d);
    assert(pipe_opens == initial_pipes && document_navigation_ready(d));
    assert(document_toc_count(d) == toc_count); verify_records(d, epub);
    document_close(d); pump(); assert(!documents && !allocated_bytes);
    committed = false;
    d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d); current_document = d;
    replay_nomem = true; pump(); assert(d->status.result == BOOK_NO_MEMORY && !committed);
    document_close(d); pump(); assert(!documents && !allocated_bytes);
    initial_pipes = pipe_opens;
    d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d);
    document_close(d); pump(); assert(pipe_opens == initial_pipes && !documents && !allocated_bytes);
    d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d);
    current_document = d; replay_cancel = true; pump(); replay_cancel = false;
    assert(!documents && !allocated_bytes && pipe_opens == pipe_closes && !committed);
    free(wire_data); wire_data = NULL;
    printf("HCPU real wire: %s lifecycle/async/source anchors/cache restore/cancel passed\n", name);
}
typedef struct memory_resource { size_t at; unsigned opened, closed; } memory_resource;
static int memory_read(void *u, void *out, size_t size)
{
    memory_resource *r = u; static const char content[] = "0123456789";
    if (size > 10 - r->at) size = 10 - r->at;
    if (size > 3) size = 3;
    memcpy(out, content + r->at, size); r->at += size; return (int)size;
}
static int memory_seek(void *u, uint64_t at)
{ if (at > 10) return BOOK_IO_ERROR; ((memory_resource *)u)->at = (size_t)at; return BOOK_OK; }
static void memory_close(void *u) { ++((memory_resource *)u)->closed; }
static int memory_open(void *u, const char *base, const char *path, book_stream_t *out)
{
    (void)base; assert(!strcmp(path, "/flash/custom.png"));
    memory_resource *r = u; ++r->opened; r->at = 0;
    *out = (book_stream_t){r, 10, memory_read, memory_seek, memory_close}; return BOOK_OK;
}
static void custom_resources(void)
{
    document_t d = {0}; document_remote_host_t h = {0}; memory_resource resource = {0};
    epd_compute_pipe_message_t m = {0}; uint8_t data[EPD_COMPUTE_PIPE_BYTES];
    strcpy(d.file.path, "/flash/chapter.xhtml"); d.source.resources = (book_resources_t){&resource, memory_open};
    h.document = &d;
    for (unsigned i = 0; i < DOCUMENT_RESOURCE_LIMIT; ++i) h.resources[i].descriptor = -1;
    strcpy(m.path, "/flash/custom.png"); m.operation = EPD_PIPE_STAT;
    assert(remote_service(&h, &m, data) == BOOK_OK && m.size == 10 && resource.closed == 1);
    m.operation = EPD_PIPE_OPEN;
    assert(remote_service(&h, &m, data) == BOOK_OK && m.handle);
    m.operation = EPD_PIPE_READ; m.offset = 4; m.length = 8;
    assert(remote_service(&h, &m, data) == BOOK_OK && m.length == 3 && !memcmp(data, "456", 3));
    m.offset = 11; m.length = 1; assert(remote_service(&h, &m, data) == BOOK_IO_ERROR);
    m.operation = EPD_PIPE_CLOSE; assert(remote_service(&h, &m, data) == BOOK_OK);
    assert(resource.opened == resource.closed && d.custom_resources);
    puts("HCPU raw resource STAT/OPEN/short READ/seek bounds/CLOSE passed");
}
#include "hcpu_lifecycle.h"
#include "hcpu_scheduler.h"
#include "hcpu_audit.h"
#include "hcpu_shutdown_source.h"
#include "hcpu_picture_position.h"
#include "hcpu_page_flow.h"
#include "hcpu_note.h"
#include "hcpu_fd_tests.h"
static void cache_space_tests(void)
{
    document_t d = {.generation = media_generation, .input = -1, .output = -1};
    strcpy(d.file.path, STORAGE_FLASH_ROOT "/test.epub");
    document_cache_t *flash = document_cache_create(&d.file);
    cache_low_space = true;
    cache_card_available = true;
    assert(!cache_select(&d, flash, "chapter-1"));
    assert(d.cached == (document_cache_t *)&card_cache_token && !cache_evictions);
    cache_card_available = false;
    assert(!cache_select(&d, flash, "chapter-1"));
    assert(d.cached == flash && cache_evictions == 2);
    cache_low_space = false;
    cache_evictions = 0;
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    assert(argc == 2 || argc == 3); snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/layout.bin", test_root);
    cache_space_tests();
    if (getenv("HCPU_FD_ONLY")) {
        descriptor_budget_tests(); descriptor_error_tests(); goto finished;
    }
    if (getenv("HCPU_PAGE_ONLY")) {
        note_lifecycle_tests();
        page_flow_tests(argc == 3 && strcmp(argv[2], "-") ? argv[2] : NULL);
        goto finished;
    }
    custom_resources();
    descriptor_budget_tests(); descriptor_error_tests();
    exercise("epub", true); exercise("utf16", false); exercise("gbk", false);
    lifecycle_tests();
    scheduler_tests();
    audit_tests();
    suspend_tests();
    optional_metadata_test();
    shutdown_ownership_test();
    spool_position_test("markdown", DOCUMENT_MD);
    spool_position_test("mobi", DOCUMENT_MOBI);
    picture_position_test(false); picture_position_test(true);
    if (argc == 3) cover_wire_test(argv[2]);
    page_flow_tests(argc == 3 ? argv[2] : NULL);
    note_lifecycle_tests();
finished:
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    assert(!allocated_bytes && pipe_opens == pipe_closes && !fd_live);
    assert_compute_released();
    DeleteCriticalSection(&irq_lock); DeleteCriticalSection(&io_lock);
    printf("HCPU document integration passed: pipes=%u steps=%u commits=%u\n", pipe_opens, pipe_steps, cache_commits);
    return 0;
}
