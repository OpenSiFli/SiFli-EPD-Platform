#include "notes_store.h"
#include "host_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
typedef union { long double alignment; void *pointer; size_t bytes; } allocation_t;
static size_t live_bytes, live_count, peak_bytes, attempts, fail_at = SIZE_MAX, memory_limit = SIZE_MAX;
static void *tracked_calloc(size_t count, size_t size)
{
    size_t bytes = count * size;
    if (attempts++ == fail_at || bytes > memory_limit - live_bytes) return NULL;
    allocation_t *p = calloc(1, sizeof(*p) + bytes);
    CHECK(p); p->bytes = bytes; live_bytes += bytes; ++live_count;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return p + 1;
}
static size_t allocated_bytes(const void *p) { return p ? ((const allocation_t *)p - 1)->bytes : 0; }
static void tracked_free(void *p)
{
    if (!p) return;
    allocation_t *a = (allocation_t *)p - 1;
    CHECK(live_bytes >= a->bytes && live_count); live_bytes -= a->bytes; --live_count; free(a);
}
#define calloc tracked_calloc
#define free tracked_free
#include "notes_model_under_test.inc"
#undef calloc
#undef free
#ifdef NOTES_IMPORT_LEGACY
note_import_status_t note_import_text_status(note_t *n, const char *text, size_t size, bool markdown)
{ return note_import_text(n, text, size, markdown) ? NOTE_IMPORT_OK : NOTE_IMPORT_INVALID; }
#endif

static void exact_spans(const note_t *n)
{
    for (size_t i = 0; i < n->count; ++i) {
        const note_block_t *b = n->blocks + i;
        CHECK(allocated_bytes(b->spans) == b->span_count * sizeof(*b->spans));
        CHECK(b->span_count || !b->spans);
    }
}
static void unchanged(const note_t *n, const note_buffer_t *before)
{
    note_buffer_t now = {0}; CHECK(note_encode(n, &now));
    CHECK(now.size == before->size && !memcmp(now.data, before->data, now.size)); note_buffer_clear(&now);
}
static void old_note(note_t *n)
{
    note_init(n, 17, 123); strcpy(n->title, "Saved title"); strcpy(n->notebook, "Keep category");
    CHECK(note_insert(n, 0, NOTE_TEXT, "old UTF-8 中文 data"));
    CHECK(note_style(n, n->blocks[0].id, 0, 3, NOTE_BOLD));
}
static char *formatted(size_t count, size_t *bytes)
{
    const char *part = "**中** _z_ "; size_t unit = strlen(part);
    char *text = malloc(count * unit + 1); CHECK(text);
    for (size_t i = 0; i < count; ++i) memcpy(text + i * unit, part, unit);
    text[count * unit] = 0; *bytes = count * unit; return text;
}
static void formats(void)
{
    const char *md = "\xef\xbb\xbf# Title\r\n\r\n## Section\nplain 中文\n> **bold** and _italic_\n- [x] done\n---\n\\*literal\\* and **中文 _nested_**\nunmatched * literal\n";
    note_t n = {0}; note_init(&n, 1, 1);
    CHECK(note_import_text_status(&n, md, strlen(md), true) == NOTE_IMPORT_OK);
    CHECK(!strcmp(n.title, "Title") && n.count == 7 && n.blocks[0].kind == NOTE_HEADING);
    CHECK(!strcmp(n.blocks[1].text, "plain 中文") && !n.blocks[1].span_count);
    CHECK(n.blocks[2].kind == NOTE_QUOTE && n.blocks[2].span_count == 2);
    CHECK(!strcmp(n.blocks[2].text, "bold and italic"));
    CHECK(n.blocks[2].spans[0].start == 0 && n.blocks[2].spans[0].end == 4 && n.blocks[2].spans[0].style == NOTE_BOLD);
    CHECK(n.blocks[2].spans[1].start == 9 && n.blocks[2].spans[1].end == 15 && n.blocks[2].spans[1].style == NOTE_ITALIC);
    CHECK(n.blocks[3].kind == NOTE_TASK && n.blocks[3].checked && n.blocks[4].kind == NOTE_RULE);
    CHECK(!strcmp(n.blocks[5].text, "*literal* and 中文 nested") && n.blocks[5].span_count == 2);
    CHECK(n.blocks[5].spans[0].start == 14 && n.blocks[5].spans[0].end == 21 && n.blocks[5].spans[0].style == NOTE_BOLD);
    CHECK(n.blocks[5].spans[1].start == 21 && n.blocks[5].spans[1].end == 27 && n.blocks[5].spans[1].style == (NOTE_BOLD | NOTE_ITALIC));
    CHECK(!strcmp(n.blocks[6].text, "unmatched * literal")); exact_spans(&n);
    note_buffer_t wire = {0}; note_t copy = {0}; CHECK(note_encode(&n, &wire)); CHECK(note_decode(&copy, wire.data, wire.size)); unchanged(&copy, &wire);
    note_clear(&copy); note_buffer_clear(&wire); note_clear(&n);
    note_init(&n, 1, 1); CHECK(note_import_text(&n, "**literal**\nplain", 17, false));
    CHECK(!strcmp(n.blocks[0].text, "**literal**")); exact_spans(&n); note_clear(&n);
    puts("formats: BOM/CRLF, headings, plain, quote, task, rule, UTF-8 spans, nested/escaped/unmatched markup and TXT passed");
}
static void boundaries(void)
{
    char *text = malloc(NOTE_BLOCK_LIMIT * 8u + 9u); CHECK(text);
    for (size_t i = 0; i < NOTE_BLOCK_LIMIT + 1u; ++i) memcpy(text + 8u * i, "**x** y\n", 8);
    note_t n = {0}; note_init(&n, 1, 1);
    CHECK(note_import_text_status(&n, text, NOTE_BLOCK_LIMIT * 8u, true) == NOTE_IMPORT_OK);
    CHECK(n.count == NOTE_BLOCK_LIMIT); exact_spans(&n);
    for (size_t i = 0; i < n.count; ++i) CHECK(n.blocks[i].span_count == 1 && !strcmp(n.blocks[i].text, "x y"));
    note_buffer_t before = {0}; CHECK(note_encode(&n, &before));
    CHECK(note_import_text_status(&n, "extra", 5, true) == NOTE_IMPORT_INVALID); unchanged(&n, &before);
    note_buffer_clear(&before); note_clear(&n); note_init(&n, 1, 1);
    CHECK(note_import_text_status(&n, text, (NOTE_BLOCK_LIMIT + 1u) * 8u, true) == NOTE_IMPORT_INVALID && !n.count);
    free(text); note_clear(&n);
    size_t bytes; text = formatted(NOTE_SPAN_LIMIT / 2u, &bytes); note_init(&n, 1, 1);
    CHECK(note_import_text_status(&n, text, bytes, true) == NOTE_IMPORT_OK && n.blocks[0].span_count == NOTE_SPAN_LIMIT);
    exact_spans(&n); note_clear(&n); free(text);
    text = formatted(NOTE_SPAN_LIMIT / 2u + 1u, &bytes); note_init(&n, 1, 1);
    CHECK(note_import_text_status(&n, text, bytes, true) == NOTE_IMPORT_INVALID && !n.count); free(text); note_clear(&n);
    text = malloc(NOTE_TEXT_LIMIT + 2u); CHECK(text); memset(text, 'p', NOTE_TEXT_LIMIT + 1u); note_init(&n, 1, 1);
    CHECK(note_import_text_status(&n, text, NOTE_TEXT_LIMIT, false) == NOTE_IMPORT_OK); exact_spans(&n); note_clear(&n); note_init(&n, 1, 1);
    CHECK(note_import_text_status(&n, text, NOTE_TEXT_LIMIT + 1u, false) == NOTE_IMPORT_INVALID && !n.count);
    CHECK(note_import_text_status(&n, "\xff", 1, true) == NOTE_IMPORT_INVALID); free(text); note_clear(&n);
    puts("boundaries: 1024 styled blocks, 1025 rejection, 2048 spans/overflow, 256KiB text/overflow and invalid UTF-8 passed");
}
static void model_oom(void)
{
    size_t bytes; char *md = formatted(17, &bytes);
    note_t original = {0}, probe = {0}; old_note(&original); note_buffer_t wire = {0}; CHECK(note_encode(&original, &wire));
    CHECK(note_clone(&probe, &original)); attempts = 0;
    CHECK(note_import_text_status(&probe, md, bytes, true) == NOTE_IMPORT_OK); size_t total = attempts; exact_spans(&probe); note_clear(&probe);
    for (size_t i = 0; i < total; ++i) {
        CHECK(note_clone(&probe, &original)); note_block_t *old_blocks = probe.blocks; char *old_text = probe.blocks[0].text;
        size_t baseline = live_bytes, count = live_count; attempts = 0; fail_at = i;
        note_import_status_t result = note_import_text_status(&probe, md, bytes, true); fail_at = SIZE_MAX;
        CHECK(result == NOTE_IMPORT_NOMEM && probe.blocks == old_blocks && probe.blocks[0].text == old_text);
        CHECK(live_bytes == baseline && live_count == count); unchanged(&probe, &wire); note_clear(&probe);
    }
    printf("model OOM: %zu allocation sites returned NOMEM, retained original pointers/content and leaked zero bytes\n", total);
    free(md); note_clear(&original); note_buffer_clear(&wire);
}
static void store_oom(void)
{
    size_t bytes; char *md = formatted(17, &bytes); size_t total = 0, rejected = 0, recovered = 0;
    for (size_t pass = 0; pass <= total; ++pass) {
        host_reset(); notes_store_t store = {0}; note_t n = {0}; old_note(&n);
        CHECK(notes_store_open(&store) == NOTES_OK); CHECK(notes_store_save(&store, &n) == NOTES_OK);
        CHECK(notes_store_write(&store, "/sdcard/source.md", md, bytes) == NOTES_OK);
        char path[NOTES_PATH_SIZE]; CHECK(notes_note_path(path, sizeof(path), n.id, "note.bin"));
        size_t disk_size; const unsigned char *disk = host_contents(path, &disk_size); unsigned char *saved = malloc(disk_size); CHECK(saved); memcpy(saved, disk, disk_size);
        note_buffer_t wire = {0}; CHECK(note_encode(&n, &wire)); size_t baseline = live_bytes, count = live_count;
        note_block_t *old_blocks = n.blocks; attempts = 0; fail_at = pass ? pass - 1 : SIZE_MAX;
        notes_error_t result = notes_store_import(&store, "/sdcard/source.md", &n); fail_at = SIZE_MAX;
        if (!pass) { CHECK(result == NOTES_OK); total = attempts; }
        else if (result == NOTES_NOMEM) {
            ++rejected; CHECK(n.blocks == old_blocks && live_bytes == baseline && live_count == count); unchanged(&n, &wire);
        } else { CHECK(result == NOTES_OK); ++recovered; }
        size_t now_size; disk = host_contents(path, &now_size); CHECK(disk && now_size == disk_size && !memcmp(disk, saved, now_size));
        if (result == NOTES_OK) {
            CHECK(n.count == 1 && n.blocks[0].span_count == 34); exact_spans(&n);
            note_t loaded = {0}; CHECK(notes_store_load(&store, n.id, &loaded) == NOTES_OK);
            CHECK(loaded.count == n.count && !strcmp(loaded.blocks[0].text, n.blocks[0].text)); note_clear(&loaded);
        }
        free(saved); note_buffer_clear(&wire); note_clear(&n); host_reset(); CHECK(!live_bytes && !live_count);
    }
    printf("store OOM: %zu allocation sites, NOMEM=%zu, recovered/committed=%zu; original disk note and output rollback passed\n", total, rejected, recovered);
    host_reset(); notes_store_t store = {0}; note_t n = {0}; old_note(&n); note_buffer_t wire = {0}; CHECK(note_encode(&n, &wire));
    CHECK(notes_store_open(&store) == NOTES_OK); CHECK(notes_store_write(&store, "/sdcard/bad.md", "\xff", 1) == NOTES_OK);
    CHECK(notes_store_import(&store, "/sdcard/bad.md", &n) == NOTES_INVALID); unchanged(&n, &wire);
    CHECK(notes_store_write(&store, "/sdcard/source.md", md, bytes) == NOTES_OK); host_fail_after(0);
    CHECK(notes_store_import(&store, "/sdcard/source.md", &n) == NOTES_IO); host_fail_after(-1); unchanged(&n, &wire);
    note_buffer_clear(&wire); note_clear(&n); host_reset(); free(md);
}
static void fixture(const char *path, bool plain)
{
    FILE *f = fopen(path, "rb"); CHECK(f && !fseek(f, 0, SEEK_END)); long size = ftell(f); CHECK(size > 0 && !fseek(f, 0, SEEK_SET));
    char *text = malloc((size_t)size + 1); CHECK(text); CHECK(fread(text, 1, (size_t)size, f) == (size_t)size && !fclose(f)); text[size] = 0;
    host_reset(); notes_store_t store = {0}; note_t n = {0}; CHECK(notes_store_open(&store) == NOTES_OK);
    const char *source = plain ? "/sdcard/long.txt" : "/sdcard/fixture.md";
    CHECK(notes_store_write(&store, source, text, (size_t)size) == NOTES_OK);
    memory_limit = 1024u * 1024u; peak_bytes = live_bytes; attempts = 0;
    CHECK(notes_store_import(&store, source, &n) == NOTES_OK); exact_spans(&n);
    CHECK(n.count == (plain ? 32u : 720u));
    if (plain) { CHECK(strstr(n.blocks[0].text, "MEMORY-NOTES-BEGIN-34")); CHECK(strstr(n.blocks[31].text, "MEMORY-NOTES-END-34")); }
    printf("%s: %ld input bytes, %zu blocks, retained=%zu, peak=%zu, span bytes=0, real store import/save passed\n", plain ? "TXT" : "device171 MD", size, n.count, live_bytes, peak_bytes);
    note_t loaded = {0}; CHECK(notes_store_load(&store, n.id, &loaded) == NOTES_OK);
    CHECK(loaded.count == n.count && !strcmp(loaded.blocks[n.count - 1].text, n.blocks[n.count - 1].text));
    note_clear(&loaded); note_clear(&n); memory_limit = SIZE_MAX; free(text); host_reset();
}
int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    if (!strcmp(argv[1], "formats")) formats();
    else if (!strcmp(argv[1], "boundaries")) boundaries();
    else if (!strcmp(argv[1], "model_oom")) model_oom();
    else if (!strcmp(argv[1], "store_oom")) store_oom();
    else if (!strcmp(argv[1], "fixture") || !strcmp(argv[1], "txt")) { CHECK(argc == 3); fixture(argv[2], !strcmp(argv[1], "txt")); }
    else CHECK(!"unknown case");
    CHECK(!live_bytes && !live_count); return 0;
}
