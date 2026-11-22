/* SPDX-License-Identifier: Apache-2.0 */
#define TEST_TOC_CAPACITY 8192
#define main toc_standard_main
#include "toc_test.c"
#undef main

static unsigned expected_chapters, fixture_bytes;

static void verify_oracle(const char *path)
{
    FILE *f = fopen(path, "rb"); assert(f);
    uint32_t header[3];
    assert(fread(header, sizeof(header), 1, f) == 1);
    expected_chapters = header[0];
    fixture_bytes = header[2];
    assert(header[1] == expected_count && expected_count > 0);
    for (unsigned i = 0; i < expected_count; ++i) {
        uint32_t h[4]; char target[1024], title[1024];
        assert(fread(h, sizeof(h), 1, f) == 1);
        assert(h[2] < sizeof(target) && h[3] < sizeof(title));
        assert(fread(target, 1, h[2], f) == h[2]); target[h[2]] = 0;
        assert(fread(title, 1, h[3], f) == h[3]); title[h[3]] = 0;
        assert(expected[i].chapter == h[0] && expected[i].level == h[1]);
        assert(!strcmp(expected[i].target, target) && !strcmp(expected[i].title, title));
    }
    assert(fgetc(f) == EOF); fclose(f);
}

static uint32_t append_hash(uint32_t hash, const void *data, size_t length)
{
    const uint8_t *p = data;
    while (length--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static void verify_content(document_t *d)
{
    uint32_t text_hash = 2166136261u, image_hash = text_hash;
    size_t text_bytes = 0, image_bytes = 0;
    unsigned images = 0;
    for (size_t at = 0; at < wire_bytes;) {
        book_document_event_t e; memcpy(&e, wire_data + at, sizeof(e));
        if (e.type == BOOK_DOCUMENT_TEXT) {
            text_hash = append_hash(text_hash, wire_data + at + sizeof(e), e.length1);
            text_bytes += e.length1;
        } else if (e.type == BOOK_DOCUMENT_IMAGE_BEGIN) ++images;
        else if (e.type == BOOK_DOCUMENT_IMAGE_DATA) {
            image_hash = append_hash(image_hash, wire_data + at + sizeof(e), e.length1);
            image_bytes += e.length1;
        }
        at += e.bytes;
    }
    uint32_t actual_text_hash = 2166136261u, actual_image_hash = actual_text_hash;
    size_t actual_text_bytes = 0, actual_image_bytes = 0;
    unsigned actual_images = 0;
    document_reader_t *reader = document_reader_create(d); assert(reader);
    uint32_t at = 0;
    while (at < d->status.end) {
        document_record_t record; assert(document_reader_record(reader, at, &record));
        uint32_t offset = at + sizeof(record), remaining = record.length;
        if (record.kind == DOCUMENT_IMAGE) {
            document_image_t dimensions;
            assert(remaining >= sizeof(dimensions));
            assert(document_reader_read(reader, offset, &dimensions, sizeof(dimensions)));
            assert(dimensions.width && dimensions.height);
            offset += sizeof(dimensions); remaining -= sizeof(dimensions); ++actual_images;
            assert(remaining == ((dimensions.width + 1) / 2) * dimensions.height);
        }
        if (record.kind == DOCUMENT_TEXT || record.kind == DOCUMENT_IMAGE) {
            uint8_t bytes[1024];
            while (remaining) {
                size_t n = remaining > sizeof(bytes) ? sizeof(bytes) : remaining;
                assert(document_reader_read(reader, offset, bytes, n));
                if (record.kind == DOCUMENT_TEXT) {
                    actual_text_hash = append_hash(actual_text_hash, bytes, n); actual_text_bytes += n;
                } else {
                    actual_image_hash = append_hash(actual_image_hash, bytes, n); actual_image_bytes += n;
                }
                offset += (uint32_t)n; remaining -= (uint32_t)n;
            }
        }
        at += sizeof(record) + record.length;
    }
    assert(at == d->status.end && text_bytes == actual_text_bytes && text_hash == actual_text_hash);
    assert(images == actual_images && image_bytes == actual_image_bytes && image_hash == actual_image_hash);
    document_reader_destroy(reader);
    printf("PASS spool text=%zu images=%u pixels=%zu\n", text_bytes, images, image_bytes);
}

static void verify_large(document_t *d)
{
    assert(document_toc_count(d) == expected_count && d->chapter_count == expected_chapters);
    assert(document_navigation_ready(d));
    unsigned reads = io_reads, opens = io_opens;
    unsigned i = 0, titles = 0;
    for (document_navigation_t *n = d->navigation; n; n = n->next) {
        if (!navigation_is_toc(d, n)) continue;
        assert(i < expected_count);
        assert(n->chapter == expected[i].chapter && n->level == expected[i].level);
        assert(!strcmp(n->text, expected[i].target));
        assert(!strcmp(n->text + n->title_at, expected[i].title));
        if (n->title_record != UINT32_MAX) {
            uint32_t record;
            assert(document_anchor_find(d, n->text, &record) && record == n->title_record);
            char title[1024]; unsigned index;
            assert(document_current_title(d, record, title, sizeof(title), &index));
            assert(index < expected_count && !strcmp(title, expected[index].title));
            ++titles;
        }
        ++i;
    }
    assert(i == expected_count && d->target_ready && d->navigation_bytes < DOCUMENT_NAVIGATION_BUDGET);
    unsigned samples[] = {0, expected_count / 2, expected_count - 1};
    for (unsigned j = 0; j < 3; ++j) {
        unsigned index = samples[j];
        char title[1024], target[1024], error[160]; unsigned level;
        assert(document_toc_entry(d, index, title, sizeof(title), target, sizeof(target), &level));
        assert(!strcmp(title, expected[index].title) && !strcmp(target, expected[index].target));
        assert(level == expected[index].level && expected[index].chapter != UINT32_MAX);
        document_t *next = document_toc_open(d, index, error, sizeof(error)); assert(next);
        assert(next->requested.chapter == expected[index].chapter);
        document_close(next);
    }
    assert(reads == io_reads && opens == io_opens);
    unsigned positions = 0;
    for (document_location_t *p = d->locations; p; p = p->next) {
        if (p->record >= d->status.end) continue;
        document_anchor_t source;
        unsigned byte = p->bytes > 3 ? 3 : p->bytes;
        assert(document_source_position(d, p->record, byte, &source));
        assert(source.chapter == expected_open.chapter && source.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8);
        assert(source.offset == p->anchor.offset + p->anchor.skip_utf8 + byte && !source.skip_utf8);
        ++positions;
    }
    assert(positions);
    printf("PASS toc=%u chapter_targets=3 titles=%u source_positions=%u navigation=%u budget=%u\n",
           expected_count, titles, positions, d->navigation_bytes, DOCUMENT_NAVIGATION_BUDGET);
    verify_content(d);
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    assert(argc == 5);
    snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root);
    load_wire(argv[2]); verify_oracle(argv[3]);
    pipe_events_per_step = 1;
    bookshelf_item_t file = {0}; char error[160];
    strcpy(file.path, "/flash/large.epub"); file.format = DOCUMENT_EPUB;
    file.size = fixture_bytes; file.modified = 1;
    document_t *d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d);
    current_document = d; document_prefetch_enable(d, true); d->adjacent_pending = false;
    drain();
    printf("REPLAY pointer=%zu result=%d end=%u published=%u nav=%u toc=%u chapters=%u pipe_steps=%u\n",
           sizeof(void *), d->status.result, d->end, d->status.end, d->navigation_bytes,
           document_toc_count(d), d->chapter_count, pipe_steps);
    assert(d->status.complete && d->status.result == atoi(argv[4]));
    if (d->status.result == BOOK_OK) {
        verify_large(d); close_fixture(d);
        unsigned pipes = pipe_opens;
        d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d);
        current_document = d; document_prefetch_enable(d, true); d->adjacent_pending = false;
        drain();
        assert(d->status.complete && d->status.result == BOOK_OK && pipe_opens == pipes);
        verify_large(d); puts("PASS warm spool restore without converter");
    } else {
        assert(d->status.result == BOOK_NO_MEMORY && !committed);
        assert(d->navigation_bytes > DOCUMENT_NAVIGATION_BUDGET - 1024);
    }
    close_fixture(d); free(wire_data);
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    assert(!allocated_bytes && !documents && !fd_live);
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    return 0;
}
