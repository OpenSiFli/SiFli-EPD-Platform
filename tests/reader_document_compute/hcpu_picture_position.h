/* SPDX-License-Identifier: Apache-2.0 */
static void picture_event(unsigned type, unsigned a, unsigned b, unsigned c, unsigned d,
                          const void *payload, unsigned length)
{
    book_document_event_t e = {0};
    e.bytes = (sizeof(e) + length + 3u) & ~3u;
    e.type = type; e.a = a; e.b = b; e.c = c; e.d = d; e.length1 = length;
    wire_data = realloc(wire_data, wire_bytes + e.bytes); assert(wire_data);
    memset(wire_data + wire_bytes, 0, e.bytes);
    memcpy(wire_data + wire_bytes, &e, sizeof(e));
    if (length) memcpy(wire_data + wire_bytes + sizeof(e), payload, length);
    wire_bytes += e.bytes;
}
static void picture_pixels(unsigned id)
{
    const uint8_t pixels[8] = {0};
    picture_event(BOOK_DOCUMENT_IMAGE_BEGIN, id, 4, 4, 2, NULL, 0);
    picture_event(BOOK_DOCUMENT_IMAGE_DATA, id, 0, 4, 2, pixels, sizeof(pixels));
    picture_event(BOOK_DOCUMENT_IMAGE_END, id, 0, 0, 0, NULL, 0);
}
static void picture_position_test(bool with_text)
{
    wire_data = NULL; wire_bytes = 0;
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR, 0, 0, 0,
                  BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_BEGIN, NULL, 0);
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR, 0, 0, 0,
                  BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_TARGET, NULL, 0);
    picture_event(BOOK_DOCUMENT_PARAGRAPH, 0, 0, 0, 0, NULL, 0);
    picture_pixels(1);
    if (with_text) {
        picture_event(BOOK_DOCUMENT_TEXT, 0, 0, 0, 0, "abcdef", 6);
        picture_pixels(2);
    }
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR, 0, with_text ? 6 : 0, 0,
                  BOOK_DOCUMENT_SOURCE_BODY_UTF8 | BOOK_DOCUMENT_SOURCE_END, NULL, 0);
    bookshelf_item_t file = {0}; char error[96];
    strcpy(file.path, "/flash/picture.epub"); file.format = DOCUMENT_EPUB;
    file.size = 10000; file.modified = 1;
    committed = false; _unlink(persistent_path);
    for (unsigned cached = 0; cached < 2; ++cached) {
        unsigned pipes = pipe_opens;
        document_t *d = document_open(&file, error, sizeof(error)); assert(d);
        pump(); assert_terminal(d, BOOK_OK); assert(d->persistent);
        assert(pipe_opens == pipes + (cached ? 0u : 1u));
        uint32_t target, byte;
        assert(document_target_cursor(d, &target, &byte) == 1 && byte == 0);
        document_anchor_t source;
        unsigned reads = io_reads, opens = io_opens;
        assert(document_source_position(d, target, byte, &source));
        assert(source.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8 && !source.offset && !source.skip_utf8);
        assert(io_reads == reads && io_opens == opens);
        if (cached) finish_navigation_restore(d);
        unsigned images = 0, texts = 0;
        for (uint32_t at = target; at < d->status.end;) {
            document_record_data_t data = read_one(d, at);
            if (data.record.kind == DOCUMENT_IMAGE || data.record.kind == DOCUMENT_TEXT) {
                reads = io_reads; opens = io_opens;
                assert(document_source_position(d, at, data.record.kind == DOCUMENT_TEXT ? 2 : 0, &source));
                assert(source.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8 && !source.skip_utf8);
                assert(source.offset == (data.record.kind == DOCUMENT_TEXT ? 2u : texts ? 6u : 0u));
                assert(io_reads == reads && io_opens == opens);
                if (data.record.kind == DOCUMENT_IMAGE) ++images; else ++texts;
            }
            at += sizeof(document_record_t) + data.record.length;
        }
        assert(images == (with_text ? 2u : 1u) && texts == (with_text ? 1u : 0u));
        lifecycle_close(d);
    }
    free(wire_data); wire_data = NULL;
    printf("HCPU %s source positions: target/pictures/text/cache reopen, no save-time I/O passed\n",
           with_text ? "mixed picture/text" : "picture-only cover");
}
static void cover_wire_test(const char *path)
{
    FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END);
    wire_bytes = (size_t)ftell(f); rewind(f);
    wire_data = malloc(wire_bytes); assert(wire_data);
    assert(fread(wire_data, 1, wire_bytes, f) == wire_bytes); fclose(f);
    bookshelf_item_t file = {0}; char error[96];
    strcpy(file.path, "/flash/real-cover.epub"); file.format = DOCUMENT_EPUB;
    file.size = 10000; file.modified = 1;
    committed = false; _unlink(persistent_path);
    for (unsigned cached = 0; cached < 2; ++cached) {
        unsigned pipes = pipe_opens;
        document_t *d = document_open(&file, error, sizeof(error)); assert(d);
        pump(); assert_terminal(d, BOOK_OK);
        assert(pipe_opens == pipes + (cached ? 0u : 1u));
        uint32_t record, byte;
        assert(document_target_cursor(d, &record, &byte) == 1 && byte == 0);
        document_anchor_t source;
        assert(document_source_position(d, record, byte, &source));
        assert(source.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8 && !source.chapter && !source.offset);
        bool image = false;
        for (uint32_t at = record; at < d->status.end;) {
            document_record_data_t data = read_one(d, at);
            assert(data.record.kind != DOCUMENT_TEXT);
            if (data.record.kind == DOCUMENT_IMAGE) {
                assert(data.record.length > sizeof(document_image_t));
                assert(document_source_position(d, at, 0, &source));
                assert(source.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8 && !source.offset);
                image = true; break;
            }
            at += sizeof(document_record_t) + data.record.length;
        }
        assert(image);
        lifecycle_close(d);
    }
    free(wire_data); wire_data = NULL;
    puts("HCPU real SVG cover wire: cold/cache TARGET precedes image; both page and image have saveable source positions");
}
