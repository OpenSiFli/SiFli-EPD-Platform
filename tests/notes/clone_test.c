#include "notes_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
typedef union { long double alignment; void *pointer_alignment; size_t size; } allocation_t;
static size_t live_bytes, live_count, peak_bytes, allocations, copied_bytes;
static size_t fail_after = SIZE_MAX;
static void *tracked_calloc(size_t count, size_t size)
{
    if (fail_after != SIZE_MAX) { if (!fail_after) return NULL; --fail_after; }
    size_t bytes = count * size;
    allocation_t *p = calloc(1, sizeof(*p) + bytes);
    if (!p) return NULL;
    p->size = bytes; live_bytes += bytes; ++live_count; ++allocations;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return p + 1;
}
static void tracked_free(void *p)
{
    if (!p) return;
    allocation_t *a = (allocation_t *)p - 1;
    live_bytes -= a->size; --live_count; free(a);
}
static void *tracked_memcpy(void *out, const void *in, size_t size)
{ copied_bytes += size; return memcpy(out, in, size); }

/* Compile the real model with allocation failures and traffic counters. */
#define calloc tracked_calloc
#define free tracked_free
#define memcpy tracked_memcpy
#include "../../modules/notes/notes_model.c"
#undef calloc
#undef free
#undef memcpy

/* Reference encoder before size reservation, including incremental failures. */
static bool legacy_encode(const note_t *n, note_buffer_t *b)
{
    put(b, 0x31544f4e); put(b, n->id); put(b, n->revision); put(b, n->next_block);
    put(b, n->created); put(b, n->updated); put(b, (unsigned)n->pinned | ((unsigned)n->deleted << 1));
    putstr(b, n->title); putstr(b, n->notebook); put(b, (uint32_t)n->count);
    for (size_t i = 0; i < n->count; ++i) {
        const note_block_t *p = n->blocks + i;
        put(b, p->id); put(b, p->kind); put(b, p->checked); put(b, p->source); put(b, p->milliseconds);
        putstr(b, p->text); putstr(b, p->original); putstr(b, p->attachment); put(b, (uint32_t)p->span_count);
        for (size_t j = 0; j < p->span_count; ++j) { put(b, p->spans[j].start); put(b, p->spans[j].end); put(b, p->spans[j].style); }
        put(b, (uint32_t)p->mark_count);
        for (size_t j = 0; j < p->mark_count; ++j) { put(b, p->marks[j].milliseconds); putstr(b, p->marks[j].title); }
    }
    return !b->failed;
}
static bool wire_clone(note_t *out, const note_t *source)
{
    note_buffer_t wire = {0};
    bool ok = legacy_encode(source, &wire) && note_decode(out, wire.data, wire.size);
    note_buffer_clear(&wire); return ok;
}
static void same(const note_t *a, const note_t *b)
{
    note_buffer_t x = {0}, y = {0};
    CHECK(note_encode(a, &x)); CHECK(note_encode(b, &y));
    CHECK(x.size == y.size && !memcmp(x.data, y.data, x.size));
    note_buffer_clear(&x); note_buffer_clear(&y);
}
static void fixture(note_t *n)
{
    note_init(n, 17, 123); n->updated = 456; n->pinned = n->deleted = true;
    strcpy(n->title, "标题"); strcpy(n->notebook, "笔记本");
    for (unsigned k = NOTE_TEXT; k <= NOTE_TRANSCRIPT; ++k) {
        note_block_t *b = note_insert(n, n->count, (note_kind_t)k, "中文🙂abc"); CHECK(b);
        b->source = 7; b->milliseconds = 1234; b->checked = true;
        b->original = note_string("原文", strlen("原文")); CHECK(b->original);
        CHECK(note_style(n, b->id, 0, 6, NOTE_BOLD));
        CHECK(note_style(n, b->id, 3, 10, NOTE_ITALIC));
        if (k == NOTE_AUDIO) {
            strcpy(b->attachment, "attachments/record.wav");
            CHECK(note_mark(n, b->id, 42, "重点"));
        }
    }
}
static void clone_contract(void)
{
    note_t source = {0}, copy = {0}, expected = {0}; fixture(&source);
    CHECK(wire_clone(&expected, &source)); CHECK(note_clone(&copy, &source)); same(&copy, &expected);
    CHECK(copy.blocks != source.blocks);
    for (size_t i = 0; i < source.count; ++i) {
        CHECK(copy.blocks[i].text != source.blocks[i].text);
        CHECK(copy.blocks[i].original != source.blocks[i].original);
        CHECK(copy.blocks[i].spans != source.blocks[i].spans);
        if (source.blocks[i].mark_count) CHECK(copy.blocks[i].marks != source.blocks[i].marks);
    }
    CHECK(note_clone(&source, &source)); same(&source, &expected);
    note_clear(&source); same(&copy, &expected);
    note_clear(&copy); note_clear(&expected);
    note_init(&source, 1, 0); CHECK(note_clone(&copy, &source)); same(&source, &copy);
    note_block_t *b = note_insert(&source, 0, NOTE_TEXT, ""); CHECK(b);
    note_free(b->text); b->text = NULL;
    CHECK(note_clone(&copy, &source));
    CHECK(copy.blocks[0].text && !*copy.blocks[0].text);
    CHECK(copy.blocks[0].original && !*copy.blocks[0].original);
    note_clear(&source); note_clear(&copy);
}
static void reject_unchanged(const note_t *bad)
{
    note_t target = {0}, original = {0}, legacy = {0}; fixture(&target);
    CHECK(wire_clone(&original, &target));
    note_block_t *blocks = target.blocks; char *text = blocks[0].text;
    CHECK(!wire_clone(&legacy, bad)); CHECK(!note_clone(&target, bad));
    CHECK(target.blocks == blocks && target.blocks[0].text == text); same(&target, &original);
    note_clear(&target); note_clear(&original); note_clear(&legacy);
}
static void invalid_sources(void)
{
    note_t n = {0}; fixture(&n); note_block_t *b = n.blocks;
#define REJECT(field, value) do { field = (value); reject_unchanged(&n); note_clear(&n); fixture(&n); b = n.blocks; } while (0)
    REJECT(n.id, 0); REJECT(n.revision, 0); REJECT(n.next_block, 0);
    REJECT(b->id, 0); REJECT(b->id, n.next_block); REJECT(n.blocks[1].id, b->id);
    REJECT(b->kind, (note_kind_t)(NOTE_TRANSCRIPT + 1));
    REJECT(b->text[0], '\xff'); REJECT(b->original[0], '\xff');
    REJECT(n.title[0], '\xff'); REJECT(n.notebook[0], '\xff');
    REJECT(b->attachment[0], '/');
    strcpy(b->attachment, "attachments/../x"); reject_unchanged(&n); b->attachment[0] = 0;
    REJECT(b->spans[0].start, 1); REJECT(b->spans[0].end, 1);
    REJECT(b->spans[0].end, 1000); REJECT(b->spans[0].end, b->spans[0].start);
    REJECT(b->spans[0].style, 0); REJECT(b->spans[0].style, 4);
    REJECT(b->spans[1].start, 0);
    REJECT(n.blocks[NOTE_AUDIO].marks[0].title[0], '\xff');
#undef REJECT
    note_clear(&n);

    note_init(&n, 1, 0); n.count = NOTE_BLOCK_LIMIT + 1; n.next_block = (uint32_t)n.count + 1;
    n.blocks = note_alloc(n.count * sizeof(*n.blocks)); CHECK(n.blocks);
    for (size_t i = 0; i < n.count; ++i) n.blocks[i].id = (uint32_t)i + 1;
    reject_unchanged(&n); note_clear(&n);

    note_init(&n, 1, 0); b = note_insert(&n, 0, NOTE_AUDIO, "abc"); CHECK(b);
    b->span_count = NOTE_SPAN_LIMIT + 1; b->spans = note_alloc(b->span_count * sizeof(*b->spans)); CHECK(b->spans);
    reject_unchanged(&n); b->span_count = 0;
    b->mark_count = 257; b->marks = note_alloc(b->mark_count * sizeof(*b->marks)); CHECK(b->marks);
    reject_unchanged(&n); note_clear(&n);

    note_init(&n, 1, 0); b = note_insert(&n, 0, NOTE_TEXT, ""); CHECK(b);
    note_free(b->text); b->text = note_alloc(NOTE_TEXT_LIMIT + 2); CHECK(b->text);
    memset(b->text, 'x', NOTE_TEXT_LIMIT + 1); reject_unchanged(&n);
    b->text[NOTE_TEXT_LIMIT] = 0; b->original = note_string("x", 1); CHECK(b->original);
    reject_unchanged(&n); note_clear(&n);
}
static void allocation_failures(void)
{
    note_t source = {0}, target = {0}, expected = {0}; fixture(&source); fixture(&target);
    CHECK(wire_clone(&expected, &target));
    size_t before = allocations; CHECK(note_clone(&target, &source)); size_t count = allocations - before;
    for (size_t i = 0; i < count; ++i) {
        size_t bytes = live_bytes, objects = live_count;
        note_block_t *blocks = target.blocks; char *text = blocks[0].text;
        fail_after = i; bool ok = note_clone(&target, &source); fail_after = SIZE_MAX;
        CHECK(!ok); CHECK(live_bytes == bytes && live_count == objects);
        CHECK(target.blocks == blocks && target.blocks[0].text == text); same(&target, &expected);
        fail_after = i; ok = note_clone(&target, &target); fail_after = SIZE_MAX;
        CHECK(!ok); CHECK(live_bytes == bytes && live_count == objects);
        CHECK(target.blocks == blocks && target.blocks[0].text == text); same(&target, &expected);
    }
    note_clear(&source); note_clear(&target); note_clear(&expected);
}
static void size_boundaries(void)
{
    note_t n = {0}, copy = {0}, expected = {0}; note_init(&n, 1, 0);
    char *text = note_alloc(NOTE_TEXT_LIMIT + 1); CHECK(text); memset(text, 'x', NOTE_TEXT_LIMIT);
    CHECK(note_insert(&n, 0, NOTE_TEXT, text));
    CHECK(wire_clone(&expected, &n)); CHECK(note_clone(&copy, &n)); same(&copy, &expected);
    note_clear(&n); note_clear(&copy); note_clear(&expected);

    note_init(&n, 1, 0);
    for (size_t i = 0; i < NOTE_BLOCK_LIMIT; ++i) CHECK(note_insert(&n, n.count, NOTE_TEXT, ""));
    CHECK(wire_clone(&expected, &n)); CHECK(note_clone(&copy, &n)); same(&copy, &expected);
    note_clear(&n); note_clear(&copy); note_clear(&expected);

    note_init(&n, 1, 0); note_block_t *audio = note_insert(&n, 0, NOTE_AUDIO, ""); CHECK(audio);
    for (unsigned i = 0; i < 256; ++i) CHECK(note_mark(&n, audio->id, i, "mark"));
    CHECK(wire_clone(&expected, &n)); CHECK(note_clone(&copy, &n)); same(&copy, &expected);
    note_clear(&n); note_clear(&copy); note_clear(&expected);

    /* 38 blocks leave enough room to reach exactly the 1 MiB wire limit. */
    note_init(&n, 1, 0); text[NOTE_SPAN_LIMIT] = 0;
    for (unsigned i = 0; i < 38; ++i) {
        note_block_t *b = note_insert(&n, n.count, NOTE_TEXT, text); CHECK(b);
        b->span_count = NOTE_SPAN_LIMIT; b->spans = note_alloc(b->span_count * sizeof(*b->spans)); CHECK(b->spans);
        for (uint32_t j = 0; j < NOTE_SPAN_LIMIT; ++j) b->spans[j] = (note_span_t){j, j + 1, NOTE_BOLD};
    }
    note_buffer_t wire = {0}; CHECK(note_encode(&n, &wire));
    size_t length = NOTE_SPAN_LIMIT + 4u * NOTE_TEXT_LIMIT - wire.size; note_buffer_clear(&wire);
    note_free(n.blocks[0].text); n.blocks[0].text = note_alloc(length + 2); CHECK(n.blocks[0].text);
    memset(n.blocks[0].text, 'x', length);
    CHECK(note_encode(&n, &wire)); CHECK(wire.size == 4u * NOTE_TEXT_LIMIT); note_buffer_clear(&wire);
    CHECK(wire_clone(&expected, &n)); CHECK(note_clone(&copy, &n)); same(&copy, &expected);
    n.blocks[0].text[length] = 'x'; reject_unchanged(&n);
    note_clear(&n); note_clear(&copy); note_clear(&expected); note_free(text);
}
static void differential_validation(void)
{
    note_t n = {0}; fixture(&n); uint32_t seed = 1234567;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        seed = seed * 1664525u + 1013904223u;
        note_span_t saved = n.blocks[0].spans[0];
        n.blocks[0].spans[0] = (note_span_t){seed % 16, (seed >> 8) % 16, (seed >> 16) % 6};
        note_t legacy = {0}, copy = {0};
        bool a = wire_clone(&legacy, &n), b = note_clone(&copy, &n); CHECK(a == b);
        if (a) same(&legacy, &copy);
        note_clear(&legacy); note_clear(&copy); n.blocks[0].spans[0] = saved;
    }
    note_clear(&n);
}
static void same_buffer(const note_buffer_t *a, const note_buffer_t *b)
{
    CHECK(a->failed == b->failed && a->size == b->size);
    if (a->size) CHECK(!memcmp(a->data, b->data, a->size));
    if (a->data) CHECK(a->data[a->size] == 0);
    if (b->data) CHECK(b->data[b->size] == 0);
}
static void encode_contract(void)
{
    note_t n = {0}; fixture(&n);
    note_buffer_t expected = {0}, actual = {0};
    CHECK(legacy_encode(&n, &expected));
    size_t before = allocations; CHECK(note_encode(&n, &actual));
    CHECK(allocations == before + 1 && actual.capacity == actual.size + 1);
    same_buffer(&expected, &actual); note_buffer_clear(&actual);

    unsigned char prefix[1000]; for (size_t i = 0; i < sizeof(prefix); ++i) prefix[i] = (unsigned char)i;
    note_buffer_t appended = {0}, reference = {0};
    CHECK(note_buffer_append(&appended, prefix, sizeof(prefix)));
    CHECK(note_buffer_append(&reference, prefix, sizeof(prefix)));
    before = allocations; CHECK(note_encode(&n, &appended));
    CHECK(allocations == before + 1 && appended.capacity == appended.size + 1);
    CHECK(legacy_encode(&n, &reference)); same_buffer(&appended, &reference);
    CHECK(!memcmp(appended.data, prefix, sizeof(prefix)));
    CHECK(note_encode(&n, &appended)); CHECK(legacy_encode(&n, &reference)); same_buffer(&appended, &reference);
    const unsigned char trailer[] = {0, 1, 2, 255};
    CHECK(note_buffer_append(&appended, trailer, sizeof(trailer)));
    CHECK(note_buffer_append(&reference, trailer, sizeof(trailer))); same_buffer(&appended, &reference);
    note_buffer_clear(&appended); note_buffer_clear(&reference);

    actual.capacity = expected.size + 32; actual.data = note_alloc(actual.capacity); CHECK(actual.data);
    unsigned char *data = actual.data; size_t capacity = actual.capacity;
    before = allocations; fail_after = 0; bool ok = note_encode(&n, &actual); fail_after = SIZE_MAX;
    CHECK(ok && allocations == before && actual.data == data && actual.capacity == capacity);
    same_buffer(&expected, &actual);
    actual.failed = true; size_t size = actual.size;
    CHECK(!note_encode(&n, &actual)); CHECK(actual.failed && actual.size == size && actual.data == data);
    CHECK(!note_buffer_append(&actual, trailer, sizeof(trailer))); CHECK(actual.size == size);
    note_buffer_clear(&actual); note_buffer_clear(&expected);

    /* Encoding itself does not validate IDs, UTF-8, styles or attachment paths. */
    n.id = 0; n.blocks[0].text[0] = '\xff'; n.blocks[0].spans[0].style = 99;
    strcpy(n.blocks[0].attachment, "../invalid");
    CHECK(legacy_encode(&n, &expected)); CHECK(note_encode(&n, &actual)); same_buffer(&expected, &actual);
    note_buffer_clear(&actual); note_buffer_clear(&expected); note_clear(&n);
}
static void encode_failures(void)
{
    note_t n = {0}; fixture(&n);
    unsigned char prefix[1000]; memset(prefix, 0xa5, sizeof(prefix));
    for (unsigned populated = 0; populated < 2; ++populated) {
        note_buffer_t actual = {0}, expected = {0};
        if (populated) {
            CHECK(note_buffer_append(&actual, prefix, sizeof(prefix)));
            CHECK(note_buffer_append(&expected, prefix, sizeof(prefix)));
        }
        size_t bytes = live_bytes, objects = live_count;
        fail_after = 0; bool a = note_encode(&n, &actual); fail_after = SIZE_MAX;
        fail_after = 0; bool b = legacy_encode(&n, &expected); fail_after = SIZE_MAX;
        CHECK(!a && !b && actual.failed); same_buffer(&actual, &expected);
        CHECK(actual.capacity == expected.capacity && live_bytes == bytes && live_count == objects);
        if (populated) CHECK(!memcmp(actual.data, prefix, sizeof(prefix)));
        size_t size = actual.size; CHECK(!note_buffer_append(&actual, "x", 1)); CHECK(actual.size == size);
        note_buffer_clear(&actual); note_buffer_clear(&expected);
    }
    note_buffer_t actual = {0}; fail_after = 1; bool ok = note_encode(&n, &actual); fail_after = SIZE_MAX;
    CHECK(ok && !actual.failed && actual.capacity == actual.size + 1); note_buffer_clear(&actual);

    size_t calculated = 0; CHECK(encode_size(&n, 4u * NOTE_TEXT_LIMIT, &calculated));
    CHECK(!encode_size(&n, calculated - 1, &calculated));
    size_t size = 40; CHECK(!encode_size_add(&size, SIZE_MAX, 12, 4u * NOTE_TEXT_LIMIT)); CHECK(size == 40);
    n.blocks[0].span_count = SIZE_MAX; CHECK(!encode_size(&n, 4u * NOTE_TEXT_LIMIT, &calculated));
    n.blocks[0].span_count = 3;
    n.blocks[0].mark_count = SIZE_MAX; CHECK(!encode_size(&n, 4u * NOTE_TEXT_LIMIT, &calculated));
    n.blocks[0].mark_count = 0; note_clear(&n);

    note_init(&n, 1, 0); CHECK(encode_size(&n, 40, &calculated) && calculated == 40);
    CHECK(!encode_size(&n, 39, &calculated));
    char *large = note_alloc(4u * NOTE_TEXT_LIMIT); CHECK(large); memset(large, 0xa5, 4u * NOTE_TEXT_LIMIT);
    for (unsigned excess = 0; excess < 2; ++excess) {
        note_buffer_t expected = {0}; size_t prefix_size = 4u * NOTE_TEXT_LIMIT - 40 + excess;
        CHECK(note_buffer_append(&actual, large, prefix_size)); CHECK(note_buffer_append(&expected, large, prefix_size));
        bool a = note_encode(&n, &actual), b = legacy_encode(&n, &expected);
        CHECK(a == b && a == (excess == 0)); same_buffer(&actual, &expected);
        CHECK(!memcmp(actual.data, large, prefix_size));
        if (!excess) CHECK(actual.size == 4u * NOTE_TEXT_LIMIT);
        note_buffer_clear(&actual); note_buffer_clear(&expected);
    }
    note_free(large); note_clear(&n);
}
static double seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency; QueryPerformanceCounter(&ticks); QueryPerformanceFrequency(&frequency);
    return (double)ticks.QuadPart / (double)frequency.QuadPart;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}
typedef bool (*clone_fn)(note_t *, const note_t *);
static void measure(const char *label, const note_t *n, clone_fn clone)
{
    const unsigned iterations = 200; note_t copy = {0};
    CHECK(clone(&copy, n)); note_clear(&copy);
    size_t baseline = live_bytes; allocations = copied_bytes = 0; peak_bytes = baseline;
    double start = seconds();
    for (unsigned i = 0; i < iterations; ++i) { CHECK(clone(&copy, n)); note_clear(&copy); }
    double elapsed = seconds() - start;
    printf("%s: %.3f ms/clone, %zu allocations/clone, %zu memcpy-bytes/clone, %zu peak-extra-bytes\n",
        label, elapsed * 1000 / iterations, allocations / iterations, copied_bytes / iterations, peak_bytes - baseline);
}
typedef bool (*encode_fn)(const note_t *, note_buffer_t *);
static void measure_encode(const char *label, const note_t *n, encode_fn encode)
{
    const unsigned iterations = 500; note_buffer_t wire = {0};
    CHECK(encode(n, &wire)); size_t retained = wire.capacity, encoded = wire.size; note_buffer_clear(&wire);
    size_t baseline = live_bytes; allocations = copied_bytes = 0; peak_bytes = baseline;
    double start = seconds();
    for (unsigned i = 0; i < iterations; ++i) { CHECK(encode(n, &wire)); note_buffer_clear(&wire); }
    double elapsed = seconds() - start;
    printf("%s: %.3f us/encode, %zu allocations/encode, %zu memcpy-bytes/encode, %zu peak-extra-bytes, %zu retained-bytes, %zu encoded-bytes\n",
        label, elapsed * 1000000 / iterations, allocations / iterations, copied_bytes / iterations,
        peak_bytes - baseline, retained, encoded);
}
static void benchmark(void)
{
    const size_t blocks[] = {1, 256}, sizes[] = {65536, 1024};
    for (unsigned test = 0; test < 2; ++test) {
        note_t n = {0}; note_init(&n, 1, 0); char *text = note_alloc(sizes[test] + 1); CHECK(text);
        memset(text, 'x', sizes[test]);
        for (size_t i = 0; i < blocks[test]; ++i) CHECK(note_insert(&n, n.count, NOTE_TEXT, text));
        printf("%zu blocks, %zu text bytes\n", blocks[test], blocks[test] * sizes[test]);
        measure("encode/decode", &n, wire_clone); measure("note_clone", &n, note_clone);
        note_free(text); note_clear(&n);
    }
}
static void encode_benchmark(void)
{
    const size_t blocks[] = {1, 1, 256}, sizes[] = {128, 65536, 1024};
    for (unsigned test = 0; test < 3; ++test) {
        note_t n = {0}; note_init(&n, 1, 0); char *text = note_alloc(sizes[test] + 1); CHECK(text);
        memset(text, 'x', sizes[test]);
        for (size_t i = 0; i < blocks[test]; ++i) CHECK(note_insert(&n, n.count, NOTE_TEXT, text));
        printf("%zu blocks, %zu text bytes\n", blocks[test], blocks[test] * sizes[test]);
        measure_encode("legacy_encode", &n, legacy_encode); measure_encode("note_encode", &n, note_encode);
        note_free(text); note_clear(&n);
    }
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) benchmark();
    else if (argc == 2 && !strcmp(argv[1], "--encode-benchmark")) encode_benchmark();
    else { clone_contract(); invalid_sources(); allocation_failures(); size_boundaries(); differential_validation(); encode_contract(); encode_failures(); puts("notes model: clone validation/ownership and encode append/reservation/failure checks passed"); }
    CHECK(live_bytes == 0 && live_count == 0); return 0;
}
