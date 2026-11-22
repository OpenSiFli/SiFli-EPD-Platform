/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_page_cache.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <windows.h>
#endif

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)
#define MIB ((size_t)1024 * 1024)
#define GUARD 32u
#define MAX_BLOCKS 256u
#define LAYOUT 17u

typedef struct {
    unsigned char *raw;
    void *pointer;
    size_t size;
} block_t;

static struct {
    block_t blocks[MAX_BLOCKS];
    size_t free_bytes, live_bytes, last_request;
    unsigned live, allocations, releases, queries, fail_allocations;
    unsigned call_allocations, call_releases, call_queries;
    int fixed_available;
    size_t fixed_bytes;
} heap;
static epd_reader_page_t source, expected, restored;

static void check_guards(const block_t *b)
{
    for (size_t i = 0; i < GUARD; ++i) {
        CHECK(b->raw[i] == 0xa7);
        CHECK(b->raw[GUARD + b->size + i] == 0x7a);
    }
}

static void *host_allocate(size_t size)
{
    CHECK(++heap.call_allocations <= 1);
    ++heap.allocations;
    heap.last_request = size;
    if (heap.fail_allocations) {
        --heap.fail_allocations;
        return NULL;
    }
    if (size > heap.free_bytes) return NULL;
    unsigned slot = 0;
    while (slot < MAX_BLOCKS && heap.blocks[slot].pointer) ++slot;
    CHECK(slot < MAX_BLOCKS);
    unsigned char *raw = malloc(size + 2 * GUARD);
    CHECK(raw != NULL);
    memset(raw, 0xa7, GUARD);
    memset(raw + GUARD, 0xcd, size);
    memset(raw + GUARD + size, 0x7a, GUARD);
    void *pointer = raw + GUARD;
    heap.blocks[slot] = (block_t){raw, pointer, size};
    heap.free_bytes -= size;
    heap.live_bytes += size;
    ++heap.live;
    return pointer;
}

static void host_release(void *pointer)
{
    CHECK(++heap.call_releases <= MAX_BLOCKS);
    unsigned slot = 0;
    while (slot < MAX_BLOCKS && heap.blocks[slot].pointer != pointer) ++slot;
    CHECK(pointer != NULL && slot < MAX_BLOCKS);
    block_t *b = &heap.blocks[slot];
    check_guards(b);
    heap.free_bytes += b->size;
    heap.live_bytes -= b->size;
    --heap.live;
    ++heap.releases;
    memset(b->raw, 0xdd, b->size + 2 * GUARD);
    free(b->raw);
    memset(b, 0, sizeof(*b));
}

static size_t host_available(void)
{
    /* Every failed pressure check must remove a page or return. */
    CHECK(++heap.call_queries <= MAX_BLOCKS + 1);
    ++heap.queries;
    return heap.fixed_available ? heap.fixed_bytes : heap.free_bytes;
}

static void begin_call(void)
{
    heap.call_allocations = heap.call_releases = heap.call_queries = 0;
}

static reader_page_cache_t new_cache(size_t budget, size_t reserve, size_t free_bytes)
{
    CHECK(heap.live == 0 && heap.live_bytes == 0);
    memset(&heap, 0, sizeof(heap));
    heap.free_bytes = free_bytes;
    reader_page_cache_t c = {0};
    c.budget = budget;
    c.reserve = reserve;
    c.allocate = host_allocate;
    c.release = host_release;
    c.available = host_available;
    reader_page_cache_window(&c, 100, 0, UINT32_MAX);
    return c;
}

static void audit(const reader_page_cache_t *c)
{
    size_t bytes = 0;
    unsigned count = 0;
    uint32_t previous = 0;
    for (const reader_cached_page_t *p = c->first; p; p = p->next) {
        CHECK(++count <= MAX_BLOCKS);
        CHECK(p->number >= previous);
        previous = p->number;
        unsigned slot = 0;
        while (slot < MAX_BLOCKS && heap.blocks[slot].pointer != p) ++slot;
        CHECK(slot < MAX_BLOCKS);
        CHECK(p->allocation == heap.blocks[slot].size);
        CHECK(p->allocation == sizeof(*p) + p->count * sizeof(*p->runs) + p->bytes);
        check_guards(&heap.blocks[slot]);
        bytes += p->allocation;
    }
    CHECK(count == c->count && count == heap.live);
    CHECK(bytes == c->bytes && bytes == heap.live_bytes);
    CHECK(bytes <= c->budget);
}

static void finish(reader_page_cache_t *c)
{
    begin_call();
    reader_page_cache_clear(c);
    audit(c);
    CHECK(c->first == NULL && c->count == 0 && c->bytes == 0);
    unsigned releases = heap.releases;
    begin_call();
    reader_page_cache_clear(c);
    CHECK(heap.releases == releases);
}

static void make_page(uint32_t count, uint32_t bytes, uint32_t eof)
{
    CHECK(count <= EPD_READER_RUNS && bytes <= EPD_READER_TEXT);
    memset(&source, 0xb6, sizeof(source));
    source.start = (epd_reader_cursor_t){40, 7};
    source.cursor = (epd_reader_cursor_t){43, 19};
    source.x = 29; source.y = 31; source.line_height = 33;
    source.line_start = count ? count - 1 : 0; source.line_style = EPD_READER_RIGHT;
    source.count = count; source.bytes = bytes;
    source.done = source.collect = 1; source.eof = eof;
    for (uint32_t i = 0; i < count; ++i) {
        epd_reader_run_t *r = &source.runs[i];
        memset(r, 0, sizeof(*r));
        r->x = (uint16_t)(3 + i); r->y = (uint16_t)(5 + i);
        r->width = (uint16_t)(7 + i); r->height = (uint16_t)(9 + i);
        r->text = (uint16_t)(i * bytes / count);
        r->bytes = (uint16_t)((i + 1) * bytes / count - r->text);
        r->style = i | EPD_READER_PREFORMAT; r->link = 1000 + i;
        r->image = i == 1 ? 73 : 0;
    }
    for (uint32_t i = 0; i < bytes; ++i)
        source.text[i] = (char)((i * 29 + 11) & 255u);
}

static size_t page_size(void)
{
    return sizeof(reader_cached_page_t) + source.count * sizeof(*source.runs) + source.bytes;
}

static reader_cached_page_t *put(reader_page_cache_t *c, unsigned number, uint32_t layout)
{
    begin_call();
    reader_cached_page_t *p = reader_page_cache_put(c, number, layout, &source);
    audit(c);
    return p;
}

static void check_restore(const reader_cached_page_t *p)
{
    memset(&restored, 0xe3, sizeof(restored));
    reader_page_cache_load(p, &restored);
    CHECK(restored.start.record == expected.start.record);
    CHECK(restored.start.byte == expected.start.byte);
    CHECK(restored.cursor.record == expected.cursor.record);
    CHECK(restored.cursor.byte == expected.cursor.byte);
    CHECK(restored.count == expected.count && restored.bytes == expected.bytes);
    CHECK(restored.done == 1 && restored.collect == 1 && restored.eof == expected.eof);
    CHECK(restored.x == 0 && restored.y == 0 && restored.line_height == 0);
    CHECK(restored.line_start == 0 && restored.line_style == 0);
    CHECK(memcmp(restored.runs, expected.runs, expected.count * sizeof(*expected.runs)) == 0);
    CHECK(memcmp(restored.text, expected.text, expected.bytes) == 0);
    const unsigned char *tail = (const unsigned char *)(restored.runs + expected.count);
    size_t unused = (EPD_READER_RUNS - expected.count) * sizeof(*expected.runs);
    for (size_t i = 0; i < unused; ++i) CHECK(tail[i] == 0xe3);
    for (size_t i = expected.bytes; i < EPD_READER_TEXT; ++i)
        CHECK((unsigned char)restored.text[i] == 0xe3);
}

static void used_payload_roundtrip(void)
{
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    for (uint32_t eof = 0; eof <= 1; ++eof) {
        make_page(3, 17, eof);
        memcpy(source.text, "A\0B\xe4\xb8\xad\xe6\x96\x87", 9);
        expected = source;
        size_t size = page_size();
        reader_cached_page_t *p = put(&c, 100 + eof, LAYOUT);
        CHECK(p != NULL && p->allocation == size && heap.last_request == size);
        CHECK(p->count == 3 && p->bytes == 17 && p->eof == eof);
        CHECK(p->warm_run == 0 && p->warm_byte == 0 && p->warm_failed == 0);
        CHECK(p->warm_scan == 0 && p->warm_pending == 0);
        CHECK(reader_page_cache_text(p) == (const char *)(p->runs + p->count));
        CHECK(memcmp(reader_page_cache_text(p), expected.text, expected.bytes) == 0);
        memset(&source, 0x42, sizeof(source));
        check_restore(p);
        p->warm_run = 1; p->warm_byte = 2; p->warm_failed = 1;
        p->warm_scan = 2; p->warm_pending = 1;
        check_restore(p);
        source = expected;
        CHECK(put(&c, 100 + eof, LAYOUT) == p);
        CHECK(p->warm_scan == 2 && p->warm_pending == 1);
        audit(&c);
    }
    finish(&c);
}

static void empty_and_full_payload(void)
{
    const uint32_t counts[] = {0, 0, 1, EPD_READER_RUNS};
    const uint32_t lengths[] = {0, 0, EPD_READER_TEXT, EPD_READER_TEXT};
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    for (unsigned i = 0; i < 4; ++i) {
        make_page(counts[i], lengths[i], i & 1u);
        expected = source;
        size_t size = page_size();
        reader_cached_page_t *p = put(&c, 100 + i, LAYOUT);
        CHECK(p != NULL && heap.last_request == size);
        memset(&source, 0, sizeof(source));
        check_restore(p);
    }
    finish(&c);
}

static void reject_invalid_pages(void)
{
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    make_page(3, 17, 0);
    CHECK(put(&c, 100, LAYOUT) != NULL);
    unsigned calls = heap.allocations, queries = heap.queries, releases = heap.releases;
    for (unsigned i = 0; i < 4; ++i) {
        make_page(3, 17, 0);
        if (i == 0) source.done = 0;
        if (i == 1) source.collect = 0;
        if (i == 2) source.count = EPD_READER_RUNS + 1;
        if (i == 3) source.bytes = EPD_READER_TEXT + 1;
        CHECK(put(&c, 101, LAYOUT) == NULL);
        CHECK(c.count == 1 && reader_page_cache_find(&c, 100, LAYOUT) != NULL);
        CHECK(heap.allocations == calls && heap.queries == queries && heap.releases == releases);
    }
    finish(&c);
}

static void identity_layout_and_clear(void)
{
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    make_page(3, 17, 1);
    expected = source;
    reader_cached_page_t *original = put(&c, 100, LAYOUT);
    CHECK(original != NULL);
    make_page(1, 5, 0);
    unsigned allocations = heap.allocations, queries = heap.queries;
    heap.free_bytes = 0;
    CHECK(put(&c, 100, LAYOUT) == original);
    CHECK(heap.allocations == allocations && heap.queries == queries && c.count == 1);
    check_restore(original);
    heap.free_bytes = 4 * MIB;
    CHECK(put(&c, 100, LAYOUT + 1) != NULL);
    CHECK(put(&c, 99, LAYOUT) != NULL);
    CHECK(put(&c, 101, LAYOUT) != NULL);
    CHECK(reader_page_cache_find(&c, 100, LAYOUT) == original);
    CHECK(reader_page_cache_find(&c, 100, LAYOUT + 2) == NULL);
    CHECK(reader_page_cache_find(&c, 102, LAYOUT) == NULL);
    CHECK(c.count == 4);
    finish(&c);
    CHECK(heap.releases == 4);
    CHECK(put(&c, 100, LAYOUT) != NULL);
    finish(&c);
}

static void budget_boundaries(void)
{
    make_page(3, 17, 0);
    size_t size = page_size();
    const size_t budgets[] = {0, size - 1, size};
    for (unsigned i = 0; i < 3; ++i) {
        reader_page_cache_t c = new_cache(budgets[i], MIB, 4 * MIB);
        reader_cached_page_t *p = put(&c, 100, LAYOUT);
        if (i < 2) {
            CHECK(p == NULL && heap.allocations == 0 && heap.queries == 0);
        } else {
            CHECK(p != NULL && c.bytes == size);
            CHECK(put(&c, 101, LAYOUT) == NULL);
            CHECK(heap.allocations == 1 && c.count == 1);
        }
        finish(&c);
    }
    reader_page_cache_t c = new_cache(2 * size, 0, 4 * MIB);
    c.available = NULL;
    CHECK(put(&c, 100, LAYOUT) != NULL);
    CHECK(put(&c, 101, LAYOUT) != NULL);
    CHECK(c.bytes == c.budget && heap.queries == 0);
    CHECK(put(&c, 102, LAYOUT) == NULL);
    finish(&c);
}

static void budget_evicts_farthest(void)
{
    make_page(3, 17, 0);
    size_t size = page_size();
    reader_page_cache_t c = new_cache(4 * size, MIB, 4 * MIB);
    const unsigned initial[] = {80, 10, 160, 60};
    for (unsigned i = 0; i < 4; ++i) CHECK(put(&c, initial[i], LAYOUT) != NULL);
    CHECK(put(&c, 101, LAYOUT) != NULL);
    CHECK(reader_page_cache_find(&c, 10, LAYOUT) == NULL);
    CHECK(reader_page_cache_find(&c, 160, LAYOUT) != NULL);
    CHECK(heap.call_releases == 1);
    CHECK(put(&c, 99, LAYOUT) != NULL);
    CHECK(reader_page_cache_find(&c, 160, LAYOUT) == NULL);
    CHECK(reader_page_cache_find(&c, 60, LAYOUT) != NULL);
    CHECK(put(&c, 108, LAYOUT) != NULL);
    CHECK(reader_page_cache_find(&c, 60, LAYOUT) == NULL);
    CHECK(reader_page_cache_find(&c, 80, LAYOUT) != NULL);
    CHECK(c.count == 4 && c.bytes == 4 * size && heap.releases == 3);
    finish(&c);
}

static void closer_and_equal_pages_survive(void)
{
    make_page(3, 17, 0);
    size_t size = page_size();
    reader_page_cache_t c = new_cache(size, MIB, 4 * MIB);
    CHECK(put(&c, 90, LAYOUT) != NULL);
    CHECK(put(&c, 110, LAYOUT) == NULL);
    CHECK(put(&c, 111, LAYOUT) == NULL);
    CHECK(put(&c, 89, LAYOUT) == NULL);
    CHECK(c.count == 1 && heap.releases == 0 && heap.allocations == 1);
    CHECK(put(&c, 109, LAYOUT) != NULL);
    CHECK(reader_page_cache_find(&c, 90, LAYOUT) == NULL && heap.releases == 1);
    finish(&c);
}

static void protected_neighborhood_budget(void)
{
    make_page(3, 17, 0);
    size_t size = page_size();
    reader_page_cache_t c = new_cache(8 * size, MIB, 4 * MIB);
    for (unsigned n = 97; n <= 103; ++n) CHECK(put(&c, n, LAYOUT) != NULL);
    CHECK(put(&c, 150, LAYOUT) != NULL);
    CHECK(put(&c, 104, LAYOUT) != NULL);
    CHECK(reader_page_cache_find(&c, 150, LAYOUT) == NULL && heap.releases == 1);
    CHECK(put(&c, 96, LAYOUT) == NULL);
    CHECK(put(&c, 95, LAYOUT) == NULL);
    CHECK(heap.releases == 1 && c.count == 8);
    for (unsigned n = 97; n <= 103; ++n) CHECK(reader_page_cache_find(&c, n, LAYOUT) != NULL);
    finish(&c);
    c = new_cache(6 * size, MIB, 4 * MIB);
    for (unsigned n = 97; n <= 103; ++n)
        if (n != 100) CHECK(put(&c, n, LAYOUT) != NULL);
    CHECK(put(&c, 100, LAYOUT) == NULL);
    CHECK(heap.call_releases == 0 && heap.call_allocations == 0 && c.count == 6);
    for (unsigned n = 97; n <= 103; ++n)
        if (n != 100) CHECK(reader_page_cache_find(&c, n, LAYOUT) != NULL);
    finish(&c);
}

static void variable_sizes_and_failure_after_eviction(void)
{
    make_page(3, 17, 0);
    size_t small_size = page_size();
    reader_page_cache_t c = new_cache(3 * small_size, MIB, 4 * MIB);
    CHECK(put(&c, 90, LAYOUT) != NULL);
    CHECK(put(&c, 10, LAYOUT) != NULL);
    CHECK(put(&c, 50, LAYOUT) != NULL);
    make_page(4, 41, 1);
    expected = source;
    size_t large_size = page_size();
    CHECK(large_size > small_size && large_size <= 2 * small_size);
    heap.fail_allocations = 1;
    CHECK(put(&c, 100, LAYOUT) == NULL);
    CHECK(heap.call_releases == 2 && heap.call_allocations == 1);
    CHECK(reader_page_cache_find(&c, 90, LAYOUT) != NULL);
    CHECK(reader_page_cache_find(&c, 10, LAYOUT) == NULL);
    CHECK(reader_page_cache_find(&c, 50, LAYOUT) == NULL);
    CHECK(c.count == 1 && c.bytes == small_size);
    CHECK(heap.free_bytes == 4 * MIB - small_size);
    reader_cached_page_t *p = put(&c, 100, LAYOUT);
    CHECK(p != NULL && heap.call_releases == 0 && heap.call_allocations == 1);
    CHECK(c.count == 2 && c.bytes == small_size + large_size);
    check_restore(p);
    finish(&c);
    CHECK(heap.free_bytes == 4 * MIB);
}

static void reserve_exact_and_one_byte_short(void)
{
    make_page(3, 17, 0);
    size_t size = page_size();
    reader_page_cache_t c = new_cache(2 * MIB, MIB, MIB + size - 1);
    CHECK(put(&c, 100, LAYOUT) == NULL);
    CHECK(heap.allocations == 0 && heap.call_queries == 1);
    ++heap.free_bytes;
    CHECK(put(&c, 100, LAYOUT) != NULL);
    CHECK(heap.free_bytes == MIB && c.bytes == size);
    CHECK(put(&c, 101, LAYOUT) == NULL);
    CHECK(c.count == 1 && heap.releases == 0 && heap.allocations == 1);
    finish(&c);
    CHECK(heap.free_bytes == MIB + size);
}

static void reserve_evicts_multiple_far_pages(void)
{
    make_page(3, 17, 0);
    size_t size = page_size();
    reader_page_cache_t c = new_cache(2 * MIB, MIB, MIB + 3 * size);
    CHECK(put(&c, 90, LAYOUT) != NULL);
    CHECK(put(&c, 10, LAYOUT) != NULL);
    CHECK(put(&c, 50, LAYOUT) != NULL);
    heap.free_bytes -= size; /* An unrelated allocation consumes reserve memory. */
    CHECK(put(&c, 101, LAYOUT) != NULL);
    CHECK(heap.call_releases == 2 && heap.call_queries == 3);
    CHECK(reader_page_cache_find(&c, 10, LAYOUT) == NULL);
    CHECK(reader_page_cache_find(&c, 50, LAYOUT) == NULL);
    CHECK(reader_page_cache_find(&c, 90, LAYOUT) != NULL);
    CHECK(c.count == 2 && c.bytes == 2 * size && heap.free_bytes == MIB);
    finish(&c);
}

static void protected_neighborhood_low_memory(void)
{
    make_page(3, 17, 0);
    size_t size = page_size();
    reader_page_cache_t c = new_cache(2 * MIB, MIB, MIB + 7 * size);
    for (unsigned n = 97; n <= 103; ++n) CHECK(put(&c, n, LAYOUT) != NULL);
    for (unsigned i = 0; i < 1000; ++i) {
        CHECK(put(&c, i & 1u ? 96 : 104, LAYOUT) == NULL);
        CHECK(heap.call_queries == 1 && heap.call_allocations == 0 && heap.call_releases == 0);
    }
    for (unsigned n = 97; n <= 103; ++n) CHECK(reader_page_cache_find(&c, n, LAYOUT) != NULL);
    CHECK(heap.allocations == 7 && heap.releases == 0 && heap.free_bytes == MIB);
    finish(&c);
}

static void chapter_window_pruning(void)
{
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    const struct { unsigned number; uint32_t start, end; int keep; } cases[] = {
        {1, 1, 9, 0}, {2, 9, 10, 1}, {3, 10, 15, 1}, {4, 12, 18, 1},
        {5, 19, 21, 1}, {6, 20, 22, 0}, {7, 8, 21, 1}, {8, 30, 40, 0},
        {96, 1, 9, 0}, {104, 30, 40, 0}
    };
    make_page(3, 17, 0);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        source.start = (epd_reader_cursor_t){cases[i].start, 0};
        source.cursor = (epd_reader_cursor_t){cases[i].end, 0};
        CHECK(put(&c, cases[i].number, LAYOUT) != NULL);
    }
    source.start.record = 50; source.cursor.record = 60;
    for (unsigned n = 97; n <= 103; ++n) CHECK(put(&c, n, LAYOUT) != NULL);
    begin_call();
    reader_page_cache_window(&c, 100, 10, 20);
    audit(&c);
    CHECK(c.wanted == 100 && c.begin == 10 && c.end == 20);
    CHECK(c.count == 12 && heap.call_releases == 5);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
        CHECK((reader_page_cache_find(&c, cases[i].number, LAYOUT) != NULL) == cases[i].keep);
    for (unsigned n = 97; n <= 103; ++n) CHECK(reader_page_cache_find(&c, n, LAYOUT) != NULL);
    begin_call();
    reader_page_cache_window(&c, 100, 10, 20);
    CHECK(heap.call_releases == 0);
    begin_call();
    reader_page_cache_window(&c, 200, 100, 200);
    audit(&c);
    CHECK(c.count == 0 && heap.call_releases == 12);
    finish(&c);
}

static void moving_window_and_unsigned_edges(void)
{
    make_page(3, 17, 0);
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    for (unsigned n = 96; n <= 104; ++n) CHECK(put(&c, n, LAYOUT) != NULL);
    begin_call();
    reader_page_cache_window(&c, 101, 1000, 2000);
    audit(&c);
    CHECK(c.count == 7 && reader_page_cache_find(&c, 97, LAYOUT) == NULL);
    for (unsigned n = 98; n <= 104; ++n) CHECK(reader_page_cache_find(&c, n, LAYOUT) != NULL);
    finish(&c);
    const unsigned edges[] = {0, 3, 4, UINT_MAX - 4, UINT_MAX - 3, UINT_MAX};
    for (unsigned pass = 0; pass < 2; ++pass) {
        c = new_cache(2 * MIB, MIB, 4 * MIB);
        for (unsigned i = 0; i < 6; ++i) CHECK(put(&c, edges[i], LAYOUT) != NULL);
        begin_call();
        reader_page_cache_window(&c, pass ? UINT_MAX : 0, 1000, 2000);
        audit(&c);
        CHECK(c.count == 2);
        for (unsigned i = 0; i < 6; ++i) {
            int keep = pass ? i >= 4 : i <= 1;
            CHECK((reader_page_cache_find(&c, edges[i], LAYOUT) != NULL) == keep);
        }
        finish(&c);
    }
}

static void allocation_failure_is_bounded(void)
{
    make_page(3, 17, 0);
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    CHECK(put(&c, 100, LAYOUT) != NULL);
    size_t bytes = c.bytes, free_bytes = heap.free_bytes;
    for (unsigned i = 0; i < 1000; ++i) {
        heap.fail_allocations = 1;
        CHECK(put(&c, 101, LAYOUT) == NULL);
        CHECK(heap.call_allocations == 1 && heap.call_releases == 0 && heap.call_queries == 1);
        CHECK(c.count == 1 && c.bytes == bytes && heap.free_bytes == free_bytes);
        CHECK(reader_page_cache_find(&c, 100, LAYOUT) != NULL);
    }
    CHECK(put(&c, 101, LAYOUT) != NULL);
    finish(&c);
}

static void persistent_pressure_terminates(void)
{
    make_page(3, 17, 0);
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    const unsigned pages[] = {10, 30, 60, 90, 97, 100, 103};
    for (unsigned i = 0; i < 7; ++i) CHECK(put(&c, pages[i], LAYOUT) != NULL);
    heap.fixed_available = 1;
    heap.fixed_bytes = 0;
    CHECK(put(&c, 101, LAYOUT) == NULL);
    CHECK(heap.call_releases == 4 && heap.call_queries == 5 && heap.call_allocations == 0);
    CHECK(c.count == 3);
    for (unsigned i = 4; i < 7; ++i) CHECK(reader_page_cache_find(&c, pages[i], LAYOUT) != NULL);
    for (unsigned i = 0; i < 1000; ++i) {
        CHECK(put(&c, 101, LAYOUT) == NULL);
        CHECK(heap.call_releases == 0 && heap.call_queries == 1 && heap.call_allocations == 0);
    }
    heap.fixed_available = 0;
    CHECK(put(&c, 101, LAYOUT) != NULL);
    finish(&c);
    c = new_cache(2 * MIB, MIB, 0);
    CHECK(put(&c, 100, LAYOUT) == NULL);
    CHECK(heap.call_queries == 1 && heap.call_allocations == 0 && heap.call_releases == 0);
    finish(&c);
}

static void two_mib_budget_with_one_mib_reserve(void)
{
    make_page(EPD_READER_RUNS, EPD_READER_TEXT, 0);
    size_t size = page_size();
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 3 * MIB);
    unsigned capacity = (unsigned)(c.budget / size);
    CHECK(capacity > 7 && capacity < MAX_BLOCKS);
    for (unsigned n = 0; n < capacity; ++n) {
        CHECK(put(&c, 1000 + n, LAYOUT) != NULL);
        CHECK(heap.free_bytes >= MIB);
    }
    CHECK(put(&c, 1000 + capacity, LAYOUT) == NULL);
    CHECK(c.count == capacity && heap.releases == 0);
    CHECK(put(&c, 100, LAYOUT) != NULL);
    CHECK(reader_page_cache_find(&c, 999 + capacity, LAYOUT) == NULL);
    CHECK(c.count == capacity && heap.releases == 1 && heap.free_bytes >= MIB);
    finish(&c);
    CHECK(heap.free_bytes == 3 * MIB);
}

static void contiguous_span_matches_gaps_and_layouts(void)
{
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    make_page(1, 4, 0);
    for (unsigned n = 0; n < 200; ++n) {
        if (n == 33 || n == 151) continue;
        CHECK(put(&c, n, n == 102 ? LAYOUT + 1 : LAYOUT) != NULL);
    }
    for (unsigned n = 0; n < 205; ++n) {
        unsigned ahead, back, expected_ahead = 0, expected_back = 0;
        while (reader_page_cache_find(&c, n + expected_ahead + 1, LAYOUT)) ++expected_ahead;
        while (expected_back < n && reader_page_cache_find(&c, n - expected_back - 1, LAYOUT)) ++expected_back;
        reader_page_cache_span(&c, n, LAYOUT, &ahead, &back);
        CHECK(ahead == expected_ahead && back == expected_back);
    }
    finish(&c);
    c = new_cache(2 * MIB, MIB, 4 * MIB);
    CHECK(put(&c, UINT_MAX - 2, LAYOUT) != NULL);
    CHECK(put(&c, UINT_MAX - 1, LAYOUT) != NULL);
    CHECK(put(&c, UINT_MAX, LAYOUT) != NULL);
    unsigned ahead, back;
    reader_page_cache_span(&c, UINT_MAX, LAYOUT, &ahead, &back);
    CHECK(!ahead && back == 2);
    reader_page_cache_span(&c, UINT_MAX - 2, LAYOUT, &ahead, &back);
    CHECK(ahead == 2 && !back);
    finish(&c);
}

static void unchanged_window_prunes_late_insertions(void)
{
    reader_page_cache_t c = new_cache(2 * MIB, MIB, 4 * MIB);
    make_page(1, 4, 0);
    reader_page_cache_window(&c, 100, 1000, 2000);
    CHECK(put(&c, 100, LAYOUT) != NULL);
    CHECK(put(&c, 200, LAYOUT) != NULL);
    begin_call();
    reader_page_cache_window(&c, 100, 1000, 2000);
    CHECK(c.count == 1 && heap.call_releases == 1);
    for (unsigned n = 0; n < 1000; ++n) {
        reader_page_cache_window(&c, 100, 1000, 2000);
        CHECK(c.count == 1 && reader_page_cache_find(&c, 100, LAYOUT));
    }
    begin_call();
    reader_page_cache_window(&c, 200, 1000, 2000);
    CHECK(!c.count);
    finish(&c);
}

typedef struct { const char *name; void (*run)(void); } test_t;
static const test_t tests[] = {
    {"used_payload_roundtrip", used_payload_roundtrip},
    {"empty_and_full_payload", empty_and_full_payload},
    {"reject_invalid_pages", reject_invalid_pages},
    {"identity_layout_and_clear", identity_layout_and_clear},
    {"budget_boundaries", budget_boundaries},
    {"budget_evicts_farthest", budget_evicts_farthest},
    {"closer_and_equal_pages_survive", closer_and_equal_pages_survive},
    {"protected_neighborhood_budget", protected_neighborhood_budget},
    {"variable_sizes_and_failure_after_eviction", variable_sizes_and_failure_after_eviction},
    {"reserve_exact_and_one_byte_short", reserve_exact_and_one_byte_short},
    {"reserve_evicts_multiple_far_pages", reserve_evicts_multiple_far_pages},
    {"protected_neighborhood_low_memory", protected_neighborhood_low_memory},
    {"chapter_window_pruning", chapter_window_pruning},
    {"moving_window_and_unsigned_edges", moving_window_and_unsigned_edges},
    {"allocation_failure_is_bounded", allocation_failure_is_bounded},
    {"persistent_pressure_terminates", persistent_pressure_terminates},
    {"two_mib_budget_with_one_mib_reserve", two_mib_budget_with_one_mib_reserve},
    {"contiguous_span_matches_gaps_and_layouts", contiguous_span_matches_gaps_and_layouts},
    {"unchanged_window_prunes_late_insertions", unchanged_window_prunes_late_insertions}
};

int main(int argc, char **argv)
{
#ifdef _MSC_VER
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    unsigned ran = 0;
    for (unsigned i = 0; i < sizeof(tests) / sizeof(*tests); ++i) {
        if (argc == 2 && strcmp(argv[1], "--list") == 0) {
            puts(tests[i].name);
            continue;
        }
        if (argc > 1 && strcmp(argv[1], tests[i].name) != 0) continue;
        tests[i].run();
        printf("PASS %s\n", tests[i].name);
        ++ran;
    }
    if (argc == 2 && strcmp(argv[1], "--list") == 0) return 0;
    if (!ran) { fprintf(stderr, "Unknown test case\n"); return 2; }
    return 0;
}
