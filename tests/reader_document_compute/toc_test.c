/* SPDX-License-Identifier: Apache-2.0 */
#include "toc_host.inc"
#ifdef TEST_LEGACY_NAVIGATION
#define navigation_append(d, r, k, l, data, n, explicit_toc) (navigation_append)(d, r, k, l, data, n)
#define navigation_is_toc(d, n) ((n)->kind == DOCUMENT_ANCHOR && (n)->text[(n)->title_at])
#endif

typedef struct {
    char target[1024], title[1024];
    uint32_t chapter, level;
} expected_toc_t;
#ifndef TEST_TOC_CAPACITY
#define TEST_TOC_CAPACITY 512
#endif
static expected_toc_t expected[TEST_TOC_CAPACITY];
static unsigned expected_count, legacy_count;

static void drain(void)
{
    for (unsigned i = 0; i < 10000; ++i) {
        bool work = document_worker_step(&service);
        document_service_process();
        if (!work) return;
    }
    assert(false);
}

static void add(document_t *d, uint32_t record, const char *target, const char *title, unsigned level)
{
    char data[2048]; size_t at = strlen(target) + 1;
    memcpy(data, target, at); strcpy(data + at, title);
    assert(navigation_append(d, record, DOCUMENT_ANCHOR, level, data, at + strlen(title) + 1, false));
}

static void meta(document_t *d, uint32_t record, const char *target, const char *title,
                 unsigned chapter, unsigned level)
{
    uint8_t data[2048];
    book_document_event_t e = {0};
    e.type = BOOK_DOCUMENT_METADATA; e.a = BOOK_DOCUMENT_META_TOC;
    e.b = chapter; e.c = level; e.length1 = (uint32_t)strlen(target); e.length2 = (uint32_t)strlen(title);
    e.bytes = sizeof(e) + e.length1 + e.length2;
    memcpy(data, &e, sizeof(e)); memcpy(data + sizeof(e), target, e.length1);
    memcpy(data + sizeof(e) + e.length1, title, e.length2);
    assert(metadata_apply(d, data, e.bytes, record) == BOOK_OK);
}

static void title_at(document_t *d, uint32_t record, const char *title, unsigned index)
{
    char text[1024]; unsigned actual = UINT32_MAX;
    assert(document_current_title(d, record, text, sizeof(text), &actual));
    assert(!strcmp(text, title) && actual == index);
}

static void free_navigation(document_t *d)
{
    while (d->navigation) {
        document_navigation_t *n = d->navigation; d->navigation = n->next; epd_app_free(n);
    }
    assert(!allocated_bytes);
}

static void identity_case(void)
{
    document_t d = {0}; d.status.end = 1000;
    meta(&d, 0, "a.xhtml#one", "Same", 2, 1);
    add(&d, 40, "a.xhtml#one", "Same", 1);
    assert(document_toc_count(&d) == 1 && "TOC_DUPLICATE");
    assert(d.navigation->chapter == 2 && d.navigation->record == 0);
    add(&d, 80, "b.xhtml#one", "Same", 1);
    add(&d, 100, "a.xhtml#two", "Same", 1);
    add(&d, 120, "a.xhtml#one", "Alias", 1);
    add(&d, 140, "a.xhtml#one", "Same", 2);
    assert(document_toc_count(&d) == 1);
    unsigned bytes = d.navigation_bytes, revision = d.navigation_revision;
    LONG allocations_before = allocations;
    /* Repeated declarations of the same origin need no new allocation. */
    d.navigation_bytes = DOCUMENT_NAVIGATION_BUDGET;
    allocation_failures = 1;
    add(&d, 160, "a.xhtml#one", "Same", 1);
    assert(allocation_failures == 1 && document_toc_count(&d) == 1); allocation_failures = 0;
    d.navigation_bytes = bytes;
    assert(d.navigation_revision == revision && allocations == allocations_before);
    assert(!navigation_append(&d, 0, DOCUMENT_ANCHOR, 1, "bad", 3, false));
    assert(!navigation_append(&d, 0, DOCUMENT_ANCHOR, 1, "bad\0x", 5, false));
    free_navigation(&d);
}

static void association_case(void)
{
    document_t d = {0}; d.status.end = 1000;
    meta(&d, 0, "a.xhtml", "Chapter", 2, 1);
    add(&d, 40, "a.xhtml", "Chapter", 1);
    add(&d, 100, "a.xhtml", "", 0);
    meta(&d, 140, "a.xhtml#one", "First", 2, 2);
    add(&d, 180, "a.xhtml#one", "First", 2);
    title_at(&d, 300, "Chapter", 0);
    add(&d, 200, "a.xhtml#one", "", 0);
    title_at(&d, 300, "First", 1);
    /* A body location can precede both kinds of title declaration. */
    add(&d, 400, "a.xhtml#two", "", 0);
    add(&d, 500, "a.xhtml#two", "Second", 2);
    document_navigation_t *second = d.navigation_tail;
    add(&d, 550, "b.xhtml", "Other", 1);
    document_navigation_t *tail = d.navigation_tail;
    meta(&d, 600, "a.xhtml#two", "Second", 7, 2);
    document_navigation_t *declared_second = d.navigation_tail;
    assert(document_toc_count(&d) == 3 && "TOC_DUPLICATE");
    assert(declared_second->chapter == 7 && declared_second->title_record == 400);
    assert(second->chapter == UINT32_MAX && tail->chapter == UINT32_MAX);
    meta(&d, 650, "a.xhtml#two", "Second", UINT32_MAX, 2);
    assert(declared_second->chapter == 7);
    meta(&d, 675, "b.xhtml", "Other", 8, 1);
    unsigned reads = io_reads, opens = io_opens;
    title_at(&d, 450, "Second", 2); title_at(&d, 250, "First", 1);
    uint32_t record;
    assert(document_anchor_find(&d, "a.xhtml#two", &record) && record == 400);
    d.status.end = 400; title_at(&d, 450, "First", 1);
    d.status.end = 1000; title_at(&d, 450, "Second", 2);
    add(&d, 700, "b.xhtml", "", 0); title_at(&d, 800, "Other", 3);
    assert(navigation_append(&d, 850, DOCUMENT_LINK, 0, "a.xhtml#two", sizeof("a.xhtml#two"), false));
    assert(navigation_append(&d, 900, DOCUMENT_LINK, 0, "a.xhtml#two", sizeof("a.xhtml#two"), false));
    char target[100];
    assert(document_link_target(&d, 850, target, sizeof(target)) && !strcmp(target, "a.xhtml#two"));
    assert(document_link_target(&d, 900, target, sizeof(target)));
    assert(reads == io_reads && opens == io_opens);
    free_navigation(&d);
}

static void load_wire(const char *path)
{
    FILE *file = fopen(path, "rb"); assert(file); fseek(file, 0, SEEK_END);
    wire_bytes = (size_t)ftell(file); rewind(file);
    wire_data = malloc(wire_bytes); assert(wire_data);
    assert(fread(wire_data, 1, wire_bytes, file) == wire_bytes); fclose(file);
    expected_open.unit = BOOK_DOCUMENT_SOURCE_BODY_UTF8;
    for (size_t at = 0; at < wire_bytes;) {
        book_document_event_t e; memcpy(&e, wire_data + at, sizeof(e));
        assert(e.bytes >= sizeof(e) && e.bytes <= wire_bytes - at);
        if ((e.type == BOOK_DOCUMENT_ANCHOR && e.b && e.length2) ||
            (e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_TOC && e.length2)) ++legacy_count;
        if (e.type == BOOK_DOCUMENT_SOURCE_ANCHOR && (e.d & BOOK_DOCUMENT_SOURCE_TARGET)) {
            expected_open.chapter = e.a; expected_open.offset = e.c;
        }
        if (e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_TOC) {
            expected_toc_t entry = {0}; assert(e.length1 < sizeof(entry.target) && e.length2 < sizeof(entry.title));
            memcpy(entry.target, wire_data + at + sizeof(e), e.length1);
            memcpy(entry.title, wire_data + at + sizeof(e) + e.length1, e.length2);
            entry.chapter = e.b; entry.level = e.c;
            unsigned i;
            for (i = 0; i < expected_count; ++i)
                if (!strcmp(entry.target, expected[i].target) && !strcmp(entry.title, expected[i].title) &&
                    entry.level == expected[i].level) break;
            if (i == expected_count) { assert(expected_count < TEST_TOC_CAPACITY); expected[expected_count++] = entry; }
        }
        at += e.bytes;
    }
    assert(expected_count);
}

static document_t *open_fixture(void)
{
    bookshelf_item_t file = {0}; char error[160];
    strcpy(file.path, "/flash/toc.epub"); file.format = DOCUMENT_EPUB; file.size = 100000; file.modified = 1;
    document_t *d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d);
    current_document = d; document_prefetch_enable(d, true); d->adjacent_pending = false;
    drain(); assert(d->status.complete && d->status.result == BOOK_OK && d->persistent);
    assert(document_navigation_ready(d)); return d;
}

static void close_fixture(document_t *d)
{
    document_close(d); drain();
    assert(!documents && !allocated_bytes && !fd_live); assert_compute_released();
}

static void verify(document_t *d)
{
    printf("TOC expected=%u actual=%u\n", expected_count, document_toc_count(d));
    assert(document_toc_count(d) == expected_count && "TOC_DUPLICATE");
    unsigned reads = io_reads, opens = io_opens, hashes = hash_calls;
    for (unsigned i = 0; i < expected_count; ++i) {
        char title[1024], target[1024]; unsigned level;
        assert(document_toc_entry(d, i, title, sizeof(title), target, sizeof(target), &level));
        assert(!strcmp(title, expected[i].title) && !strcmp(target, expected[i].target) && level == expected[i].level);
    }
    assert(!document_toc_entry(d, expected_count, NULL, 0, NULL, 0, NULL));
    unsigned index = 0, resolved = 0;
    for (document_navigation_t *n = d->navigation; n; n = n->next) {
        if (!navigation_is_toc(d, n)) continue;
        assert(n->chapter == expected[index++].chapter);
        if (n->title_record != UINT32_MAX) {
            uint32_t record; assert(document_anchor_find(d, n->text, &record) && record == n->title_record);
            title_at(d, record, n->text + n->title_at, index - 1);
            ++resolved;
        }
    }
    for (document_location_t *p = d->locations; p; p = p->next) {
        if (p->record >= d->status.end) continue;
        document_anchor_t source;
        unsigned byte = p->bytes > 3 ? 3 : p->bytes;
        assert(document_source_position(d, p->record, byte, &source));
        assert(source.chapter == p->anchor.chapter && source.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8);
        assert(source.offset == p->anchor.offset + p->anchor.skip_utf8 + byte && !source.skip_utf8);
    }
    /* The selected chapter must carry the chapter number stored in META_TOC. */
    for (unsigned i = 0; i < expected_count; ++i) if (expected[i].chapter != UINT32_MAX) {
        char error[160]; document_t *target = document_toc_open(d, i, error, sizeof(error)); assert(target);
        assert(target->requested.chapter == expected[i].chapter && target->requested.offset == 0);
        document_close(target);
    }
    assert(reads == io_reads && opens == io_opens && hashes == hash_calls);
    printf("PASS entries, chapter targets, %u resolved titles, source positions and memory-only queries\n", resolved);
}

static void verify_spool_positions(document_t *d)
{
    document_reader_t *reader = document_reader_create(d); assert(reader);
    document_anchor_t expected_source = {0}; unsigned texts = 0, declarations = 0;
    uint32_t at = 0;
    while (at < d->status.end) {
        document_record_t record; assert(document_reader_record(reader, at, &record));
        if (record.kind == DOCUMENT_METADATA) {
            book_document_event_t e;
            assert(record.length >= sizeof(e) && document_reader_read(reader, at + sizeof(record), &e, sizeof(e)));
            if (e.type == BOOK_DOCUMENT_SOURCE_ANCHOR)
                expected_source = (document_anchor_t){e.d & 255, e.a, e.b, 0};
            if (e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_TOC && e.length2) ++declarations;
        } else if (record.kind == DOCUMENT_ANCHOR) {
            char data[DOCUMENT_TEXT_LIMIT + 1];
            assert(record.length <= DOCUMENT_TEXT_LIMIT && document_reader_read(reader, at + sizeof(record), data, record.length));
            const char *title = memchr(data, 0, record.length); assert(title);
            if (title[1]) ++declarations;
        } else if (record.kind == DOCUMENT_TEXT) {
            unsigned reads = io_reads, opens = io_opens;
            document_anchor_t actual;
            unsigned byte = record.length > 3 ? 3 : record.length;
            assert(document_source_position(d, at, byte, &actual));
            assert(actual.unit == expected_source.unit && actual.chapter == expected_source.chapter);
            assert(actual.offset == expected_source.offset + expected_source.skip_utf8 + byte && !actual.skip_utf8);
            assert(reads == io_reads && opens == io_opens);
            expected_source.skip_utf8 += record.length; ++texts;
        }
        at += sizeof(record) + record.length;
    }
    assert(at == d->status.end && declarations == legacy_count);
    document_reader_destroy(reader);
    printf("PASS unchanged raw declarations=%u, independent source positions for %u text records\n", declarations, texts);
}

static void replay_case(const char *mode)
{
    bool warm = !strcmp(mode, "warm"), legacy = !strcmp(mode, "legacy-create");
    committed = warm; unsigned opened = pipe_opens;
    document_t *d = open_fixture();
    assert(pipe_opens == opened + (warm ? 0 : 1));
    if (legacy) {
        assert(document_toc_count(d) == legacy_count && legacy_count > expected_count);
        printf("PASS baseline reproduced %u duplicated entries and saved legacy spool\n", document_toc_count(d));
        close_fixture(d); return;
    }
    verify(d); verify_spool_positions(d); uint32_t end = d->end; unsigned bytes = d->navigation_bytes;
    close_fixture(d);
    d = open_fixture(); verify(d); verify_spool_positions(d);
    assert(d->end == end && d->navigation_bytes == bytes && pipe_opens == opened + (warm ? 0 : 1));
    close_fixture(d);
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    assert(argc >= 3); snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root);
    if (!strcmp(argv[2], "identity")) identity_case();
    else if (!strcmp(argv[2], "association")) association_case();
    else { assert(argc == 4); load_wire(argv[3]); replay_case(argv[2]); free(wire_data); }
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    assert(!allocated_bytes && !documents && !fd_live && !hash_calls);
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    printf("PASS %s reads=%u pipes=%u steps=%u\n", argv[2], io_reads, pipe_opens, pipe_steps);
    return 0;
}
