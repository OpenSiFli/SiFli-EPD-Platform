/* SPDX-License-Identifier: Apache-2.0 */
#include "formats.h"
#ifdef BOOK_FORMAT_ACPU
#include "book_compute.h"
#include "book_compute_internal.h"
#include "book_fs.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static size_t allocations, live, fail_after = SIZE_MAX;
static void *tracked_alloc(size_t n)
{
    void *p;
    if (allocations++ >= fail_after) return NULL;
    p = malloc(n);
    if (p) ++live;
    return p;
}
static void tracked_free(void *p) { if (p) { assert(live); --live; free(p); } }
static void *tracked_realloc(void *p, size_t n)
{
    if (!p) return tracked_alloc(n);
    if (!n) { tracked_free(p); return NULL; }
    return realloc(p, n);
}
static const book_allocator_t allocator = {tracked_alloc, tracked_realloc, tracked_free};

typedef struct output {
    FILE *file;
    int cancel, text_calls, cancel_text, fail_text, cancel_image;
    size_t text_bytes;
} output;
static void field(output *o, const char *p, size_t n)
{
    if (!o->file) return;
    fprintf(o->file, "%zu:", n);
    if (n) assert(fwrite(p, 1, n, o->file) == n);
    fputc('\n', o->file);
}
static void string(output *o, const char *p) { field(o, p ? p : "<null>", p ? strlen(p) : 6); }
static void number(output *o, unsigned n) { if (o->file) fprintf(o->file, "%u\n", n); }
static bool cancelled(void *u) { return ((output *)u)->cancel != 0; }
static int text_out(void *u, const char *p, size_t n, unsigned style)
{
    output *o = u;
    size_t i = 0;
    assert(n > 0 && n <= 2048);
    /* Each callback must contain complete UTF-8 code points. */
    while (i < n)
    {
        unsigned char c = (unsigned char)p[i++];
        unsigned extra = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        assert(c < 0x80 || c >= 0xc2);
        while (extra--) { assert(i < n); assert(((unsigned char)p[i++] & 0xc0) == 0x80); }
    }
    ++o->text_calls;
    o->text_bytes += n;
    string(o, "text"); number(o, style); field(o, p, n);
    if (o->cancel_text) o->cancel = 1;
    return o->fail_text ? BOOK_NO_SPACE : BOOK_OK;
}
static int paragraph(void *u) { string(u, "paragraph"); return BOOK_OK; }
static int anchor(void *u, const char *id, const char *title, unsigned level)
{
    string(u, "anchor"); string(u, id); string(u, title); number(u, level); return BOOK_OK;
}
static int metadata(void *u, const char *title, const char *author)
{
    string(u, "metadata"); string(u, title); string(u, author); return BOOK_OK;
}
static int link_out(void *u, const char *target, bool begin)
{
    string(u, "link"); string(u, target); number(u, begin); return BOOK_OK;
}
static int image_out(void *u, const char *id, book_stream_t *s)
{
    output *o = u;
    char bytes[5];
    int n;
    string(o, "image"); string(o, id);
    if (!s) return BOOK_OK;
    n = s->read(s->user, bytes, sizeof(bytes));
    if (n < 0) return n;
    field(o, bytes, (size_t)n);
    if (o->cancel_image) o->cancel = 1;
    /* Converter must drain the rest and verify CRC after partial consumption. */
    return BOOK_OK;
}
static int svg_out(void *u, const char *xml, size_t n, const char *base,
                   const book_resources_t *resources)
{
    (void)resources;
    string(u, "svg"); string(u, base); field(u, xml, n); return BOOK_OK;
}
static book_sink_t make_sink(output *o)
{
    book_sink_t s = {0};
    s.user = o; s.cancelled = cancelled; s.text = text_out; s.paragraph = paragraph;
    s.anchor = anchor; s.image = image_out; s.metadata = metadata; s.link = link_out;
    s.markup = book_markup_convert; s.svg = svg_out;
    return s;
}

#ifdef BOOK_FORMAT_ACPU
typedef struct node {
    char path[1024];
    unsigned char *data;
    size_t size;
    int directory;
} node;
typedef struct directory { const node *node; size_t at; } directory;
static node nodes[64];
static size_t node_count;
typedef struct fake {
    output *output;
    int handles, opens, closes, reads, stats, dirs, dir_closes;
    int fail_read, fail_stat, fail_open, fail_close, fail_dir, bad_dir, early_eof, oversized;
    int cancel_read, cancel_stat, cancel_open, cancel_dir, nested, nested_count;
    size_t short_read;
    uint64_t read_floor;
    const char *path;
    int exercise_fs;
} fake;
static book_compute_resource_ops_t ops;
static node *lookup(const char *path)
{
    size_t i;
    for (i = 0; i < node_count; ++i) if (!strcmp(path, nodes[i].path)) return &nodes[i];
    return NULL;
}
static void load_nodes(void)
{
    char path[1024];
    FILE *index = fopen("fixtures/index.txt", "r");
    assert(index);
    while (fgets(path, sizeof(path), index))
    {
        node *n = &nodes[node_count++];
        FILE *f;
        path[strcspn(path, "\r\n")] = 0;
        n->directory = path[0] == 'D';
        strcpy(n->path, path + 2);
        if (n->directory) continue;
        f = fopen(n->path, "rb"); assert(f);
        assert(!fseek(f, 0, SEEK_END)); n->size = (size_t)ftell(f); rewind(f);
        n->data = malloc(n->size ? n->size : 1); assert(n->data);
        assert(fread(n->data, 1, n->size, f) == n->size); fclose(f);
    }
    fclose(index);
}
static void nested(fake *f)
{
    if (f->nested)
    {
        book_sink_t sink = make_sink(f->output);
        char error[64];
        int opens = f->opens, stats = f->stats;
        assert(book_compute_convert(f->path, &sink, &allocator, &ops, f, error, sizeof(error)) == BOOK_COMPUTE_BUSY);
        assert(strstr(error, "busy") && opens == f->opens && stats == f->stats);
        ++f->nested_count;
    }
}
static int fake_stat(void *u, const char *path, struct stat *st)
{
    fake *f = u;
    node *n = lookup(path);
    ++f->stats;
    nested(f);
    if (f->cancel_stat) f->output->cancel = 1;
    if (!n || f->fail_stat) return BOOK_IO_ERROR;
    memset(st, 0, sizeof(*st));
    st->st_mode = n->directory ? S_IFDIR : S_IFREG;
    st->st_size = (off_t)n->size;
    return BOOK_OK;
}
static int fake_open(void *u, const char *path, void **handle, uint64_t *size)
{
    fake *f = u;
    node *n = lookup(path);
    if (f->fail_open || !n || n->directory) return BOOK_IO_ERROR;
    *handle = n; *size = n->size; ++f->handles; ++f->opens;
    if (f->cancel_open) f->output->cancel = 1;
    return BOOK_OK;
}
static int fake_read(void *u, void *handle, uint64_t offset, void *buffer, size_t size)
{
    fake *f = u;
    node *n = handle;
    if (f->read_floor && offset >= 4096) assert(offset >= f->read_floor);
    ++f->reads;
    nested(f);
    if (f->cancel_read) return BOOK_CANCELLED;
    if (f->fail_read) return BOOK_IO_ERROR;
    if (f->early_eof) return 0;
    if (f->oversized) return (int)size + 1;
    if (offset >= n->size) return 0;
    if (size > n->size - offset) size = n->size - (size_t)offset;
    if (f->short_read && size > f->short_read) size = f->short_read;
    memcpy(buffer, n->data + (size_t)offset, size);
    return (int)size;
}
static int fake_close(void *u, void *handle)
{
    fake *f = u;
    assert(handle && f->handles > 0); --f->handles; ++f->closes;
    return f->fail_close ? BOOK_IO_ERROR : BOOK_OK;
}
static int fake_dir_open(void *u, const char *path, void **handle)
{
    fake *f = u;
    node *n = lookup(path);
    directory *d;
    if (!n || !n->directory) return BOOK_IO_ERROR;
    d = calloc(1, sizeof(*d)); assert(d); d->node = n;
    *handle = d; ++f->handles; ++f->dirs;
    if (f->cancel_dir) f->output->cancel = 1;
    return BOOK_OK;
}
static int fake_dir_next(void *u, void *handle, char *name, size_t capacity)
{
    fake *f = u;
    directory *d = handle;
    size_t length = strlen(d->node->path);
    if (f->fail_dir) return BOOK_IO_ERROR;
    if (f->bad_dir) { memset(name, 'a', capacity); return 1; }
    while (d->at < node_count)
    {
        node *n = &nodes[d->at++];
        const char *child;
        if (strncmp(n->path, d->node->path, length) || n->path[length] != '/') continue;
        child = n->path + length + 1;
        if (strchr(child, '/')) continue;
        if (strlen(child) >= capacity) return BOOK_UNSUPPORTED;
        strcpy(name, child); return 1;
    }
    return 0;
}
static int fake_dir_close(void *u, void *handle)
{
    fake *f = u;
    free(handle); --f->handles; ++f->dir_closes; return BOOK_OK;
}
static book_compute_resource_ops_t ops = {
    fake_stat, fake_open, fake_read, fake_close, fake_dir_open, fake_dir_next, fake_dir_close
};

static int run(fake *f, const char *path)
{
    book_sink_t sink = make_sink(f->output);
    char error[160];
    int rc;
    f->path = path;
    rc = book_compute_convert(path, &sink, &allocator, &ops, f, error, sizeof(error));
    assert(live == 0 && f->handles == 0);
    assert(f->opens == f->closes && f->dirs == f->dir_closes);
    if (rc < 0) assert(error[0]);
    return rc;
}
static void fs_contract(void)
{
    output o = {0}; fake f = {0}; book_sink_t sink = make_sink(&o);
    book_compute_io io = {ops, &f, &sink, BOOK_OK};
    void *memory = calloc(1, book_compute_scope_size());
    bf_FILE *file;
    char b[20];
    f.output = &o; f.short_read = 2;
    book_compute_scope_bind(memory); book_compute_fs_bind(&io);
    assert(book_markup_begin(&allocator) == BOOK_OK);
    file = bf_fopen("fixtures/bytes.bin", "rb"); assert(file);
    assert(bf_fgetc(file) == '0');
    assert(bf_ungetc('X', file) == 'X'); assert(bf_ungetc('Y', file) == EOF);
    assert(bf_fseek(file, 0, SEEK_CUR) == 0); assert(bf_fgetc(file) == '0');
    assert(bf_fseek(file, 2, SEEK_CUR) == 0); assert(bf_fgetc(file) == '3');
    assert(bf_fseek(file, -2, SEEK_END) == 0);
    assert(bf_fread(b, 2, 2, file) == 1 && !memcmp(b, "89", 2));
    assert(bf_fgetc(file) == EOF && !bf_ferror(file));
    assert(bf_fseek(file, 0, SEEK_SET) == 0);
    assert(bf_fread(b, 2, 5, file) == 5 && !memcmp(b, "0123456789", 10));
    assert(bf_fseek(file, LONG_MIN, SEEK_CUR) == -1 && bf_ferror(file));
    assert(bf_fclose(file) == 0);
    book_markup_end(); book_compute_fs_bind(NULL); book_compute_scope_bind(NULL); free(memory);
    assert(!live && !f.handles);
}
static void selftest(const char *path)
{
    output o; fake f; int rc; size_t total, i;
#define RESET() do { memset(&o, 0, sizeof(o)); memset(&f, 0, sizeof(f)); f.output = &o; allocations = 0; fail_after = SIZE_MAX; } while (0)
#define CASE(member, expected) do { RESET(); f.member = 1; assert(run(&f, path) == (expected)); } while (0)
    RESET(); f.short_read = 7; f.nested = 1; assert(run(&f, path) == BOOK_OK);
    assert(f.nested_count > 1 && o.text_bytes > 0); total = allocations;
    CASE(fail_read, BOOK_IO_ERROR); CASE(early_eof, BOOK_IO_ERROR); CASE(oversized, BOOK_IO_ERROR);
    CASE(fail_stat, BOOK_IO_ERROR); CASE(fail_open, BOOK_IO_ERROR); CASE(fail_close, BOOK_IO_ERROR);
    CASE(cancel_read, BOOK_CANCELLED); CASE(cancel_stat, BOOK_CANCELLED); CASE(cancel_open, BOOK_CANCELLED);
    RESET(); o.cancel = 1; assert(run(&f, path) == BOOK_CANCELLED); assert(!f.stats);
    RESET(); o.cancel_text = 1; assert(run(&f, path) == BOOK_CANCELLED); assert(o.text_calls == 1);
    RESET(); o.fail_text = 1; assert(run(&f, path) == BOOK_NO_SPACE);
    for (i = 0; i < total; ++i)
    {
        RESET(); fail_after = i; rc = run(&f, path); assert(rc == BOOK_NO_MEMORY);
    }
    RESET(); assert(run(&f, path) == BOOK_OK);
    if (strstr(path, "split"))
    {
        CASE(fail_dir, BOOK_IO_ERROR); CASE(bad_dir, BOOK_IO_ERROR); CASE(cancel_dir, BOOK_CANCELLED);
    }
    printf("faults/cancel/reentry/OOM: %s (%zu allocation positions)\n", path, total);
#undef RESET
#undef CASE
}
#endif

#ifdef BOOK_FORMAT_ACPU
#include "selection_cases.h"
#include "note_cases.h"
#endif
int main(int argc, char **argv)
{
    output o = {0};
    int rc;
    assert(argc >= 3);
#ifdef BOOK_FORMAT_ACPU
    fake f = {0}; size_t i;
    load_nodes(); f.output = &o; f.short_read = 7;
    if (!strcmp(argv[1], "--styles")) {
        rc = styles_run(argv[2]);
        for (i = 0; i < node_count; ++i) free(nodes[i].data);
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "--note")) {
        rc = note_run(argc, argv);
        for (i = 0; i < node_count; ++i) free(nodes[i].data);
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "--selected-faults"))
    {
        selected_faults(argv[2]);
        for (i = 0; i < node_count; ++i) free(nodes[i].data);
        return 0;
    }
    if (!strcmp(argv[1], "--selection"))
    {
        rc = selected_run(argc, argv);
        for (i = 0; i < node_count; ++i) free(nodes[i].data);
        return rc ? 1 : 0;
    }
    if (!strcmp(argv[1], "--selftest"))
    {
        selftest(argv[2]); fs_contract();
        for (i = 0; i < node_count; ++i) free(nodes[i].data);
        return 0;
    }
#endif
    o.file = fopen(argv[2], "wb"); assert(o.file);
#ifdef BOOK_FORMAT_ACPU
    rc = run(&f, argv[1]);
    for (i = 0; i < node_count; ++i) free(nodes[i].data);
#else
    {
        book_sink_t sink = make_sink(&o); char error[160];
        rc = book_format_convert(argv[1], &sink, &allocator, error, sizeof(error));
        assert(!live);
    }
#endif
    fprintf(o.file, "result:%d\n", rc); fclose(o.file);
    printf("%s: rc=%d text=%zu callbacks=%d\n", argv[1], rc, o.text_bytes, o.text_calls);
    return 0;
}
