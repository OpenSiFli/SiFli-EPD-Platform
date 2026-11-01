/* SPDX-License-Identifier: Apache-2.0 */
#include "../formats/book_internal.h"
#include "../codecs/mobi/mobi_port.h"
#include "mobi_include/dfs_file.h"

typedef union block block;
union block {
    max_align_t alignment;
    struct { block *prev, *next; size_t bytes; } info;
};
static const book_allocator_t *memory;
static const book_sink_t *sink;
static block *head;
static int status;
static struct dfs_fd source;
static const char *source_path;
static unsigned char *cache;
static size_t cache_start, cache_size;
enum { CACHE_BYTES = 16384 };

int mobi_port_start(const book_allocator_t *allocator, const book_sink_t *output)
{
    if (memory) return BOOK_ERROR;
    memory = allocator; sink = output; status = BOOK_OK;
    source_path = NULL; cache = NULL; cache_start = cache_size = 0;
    memset(&source, 0, sizeof(source));
    return BOOK_OK;
}
int mobi_port_status(void)
{
    if (sink && bf_cancel(sink) < 0) status = BOOK_CANCELLED;
    return status;
}
void *mobi_alloc(size_t bytes)
{
    block *b;
    if (mobi_port_status() != BOOK_OK) return NULL;
    if (!bytes) bytes = 1;
    if (bytes > SIZE_MAX - sizeof(*b)) { status = BOOK_NO_MEMORY; return NULL; }
    b = memory->alloc(sizeof(*b) + bytes);
    if (!b) { status = BOOK_NO_MEMORY; return NULL; }
    b->info.bytes = bytes; b->info.prev = NULL; b->info.next = head;
    if (head) head->info.prev = b;
    head = b;
    return b + 1;
}
void mobi_release(void *p)
{
    block *b;
    if (!p) return;
    b = (block *)p - 1;
    if (b->info.prev) b->info.prev->info.next = b->info.next; else head = b->info.next;
    if (b->info.next) b->info.next->info.prev = b->info.prev;
    memory->free(b);
}
void *mobi_calloc(size_t count, size_t bytes)
{
    void *p;
    if (bytes && count > SIZE_MAX / bytes) { status = BOOK_NO_MEMORY; return NULL; }
    p = mobi_alloc(count * bytes);
    if (p) memset(p, 0, count * bytes);
    return p;
}
void *mobi_realloc(void *p, size_t bytes)
{
    void *next; size_t old;
    if (!p) return mobi_alloc(bytes);
    if (!bytes) { mobi_release(p); return NULL; }
    old = ((block *)p - 1)->info.bytes;
    next = mobi_alloc(bytes);
    if (next) { memcpy(next, p, old < bytes ? old : bytes); mobi_release(p); }
    return next;
}
char *bk_mobi_strdup(const char *text)
{
    size_t n = strlen(text) + 1; char *p = mobi_alloc(n);
    if (p) memcpy(p, text, n);
    return p;
}
int mobi_dfs_open(struct dfs_fd *f, const char *path, int flags)
{
    struct stat st; (void)flags;
    if (mobi_port_status() < 0) return -1;
    if (stat(path, &st) || st.st_size < 0 || (uint64_t)st.st_size > LONG_MAX)
    { status = BOOK_IO_ERROR; return -1; }
    f->handle = fopen(path, "rb"); f->size = (uint64_t)st.st_size;
    if (!f->handle) { status = bf_oom() ? BOOK_NO_MEMORY : BOOK_IO_ERROR; return -1; }
    return 0;
}
int mobi_dfs_read(struct dfs_fd *f, void *out, size_t bytes)
{
    size_t n;
    if (mobi_port_status() < 0 || bytes > INT_MAX) return -1;
    n = fread(out, 1, bytes, f->handle);
    if (n != bytes) { status = BOOK_IO_ERROR; return -1; }
    return (int)n;
}
int mobi_dfs_seek(struct dfs_fd *f, long offset)
{
    if (mobi_port_status() < 0) return -1;
    if (fseek(f->handle, offset, SEEK_SET)) { status = BOOK_IO_ERROR; return -1; }
    return 0;
}
int mobi_dfs_close(struct dfs_fd *f)
{
    int rc = f->handle ? fclose(f->handle) : 0;
    f->handle = NULL;
    if (rc && status == BOOK_OK) status = BOOK_IO_ERROR;
    return rc;
}
int mobi_read_at(const char *path, size_t offset, void *buffer, size_t bytes)
{
    unsigned char *out = buffer;
    if (mobi_port_status() < 0) return status;
    if (!source_path)
    {
        if (mobi_dfs_open(&source, path, 0)) return status;
        source_path = path;
        cache = mobi_alloc(CACHE_BYTES);
        if (!cache) return status;
    }
    if (strcmp(source_path, path) || offset > source.size || bytes > source.size - offset)
        return status = BOOK_IO_ERROR;
    while (bytes)
    {
        size_t n;
        if (mobi_port_status() < 0) return status;
        if (!cache_size || offset < cache_start || offset - cache_start >= cache_size)
        {
            cache_start = offset - offset % CACHE_BYTES;
            cache_size = (size_t)source.size - cache_start;
            if (cache_size > CACHE_BYTES) cache_size = CACHE_BYTES;
            if (mobi_dfs_seek(&source, (long)cache_start) || mobi_dfs_read(&source, cache, cache_size) < 0) return status;
        }
        n = cache_size - (offset - cache_start);
        if (n > bytes) n = bytes;
        memcpy(out, cache + offset - cache_start, n);
        out += n; offset += n; bytes -= n;
    }
    return BOOK_OK;
}
void mobi_port_finish(void)
{
    mobi_dfs_close(&source);
    while (head) mobi_release(head + 1);
    memory = NULL; sink = NULL; source_path = NULL; cache = NULL;
}
