/* SPDX-License-Identifier: Apache-2.0 */
#define BF_RAW_STDIO
#include "../formats/book_internal.h"
#include "book_compute.h"
#include "book_compute_internal.h"
#include "document_wire.h"
#include "epd_compute_pipe.h"
#include "epd_compute_memory.h"
#include "reader_image.h"
#include "book_timing.h"
#include "document_timing_port.h"

typedef union document_memory document_memory;
typedef struct document_remote document_remote;
union document_memory {
    max_align_t alignment;
    struct { document_memory *prev, *next; size_t bytes; document_remote *owner; } h;
};
struct document_remote {
    epd_compute_pipe_remote_t pipe;
    book_timing_t timing;
    document_memory *memory;
    size_t budget, used, peak, output_size;
    unsigned width, height, image_id;
    book_compute_selection_t selection;
    uint32_t actual_target, next_offset, source_unit, selected_spine, navigation_events;
    int status, first_flush;
    int finished, result;
    document_remote *parent;
    char path[EPD_COMPUTE_PIPE_PATH];
    char note_target[BF_PATH];
    bool entry_enabled;
    union { max_align_t align; uint8_t bytes[BOOK_ENTRY_MAX]; } entry;
    uint8_t output[BOOK_DOCUMENT_BATCH_BYTES];
};
static document_remote *active;
static document_remote *body_session, *note_session;

static void *document_alloc(size_t bytes)
{
    document_remote *d = active;
    document_memory *m;
    if (!d) return NULL;
    if (bytes > SIZE_MAX - sizeof(*m)) { d->status = BOOK_NO_MEMORY; return NULL; }
    bytes += sizeof(*m);
    if (bytes > d->budget - d->used) { d->status = BOOK_NO_MEMORY; return NULL; }
    if (d->parent && d->used + bytes > d->parent->budget - d->parent->used)
        { d->status = BOOK_NO_MEMORY; return NULL; }
    m = epd_compute_malloc(bytes);
    if (!m) { d->status = BOOK_NO_MEMORY; return NULL; }
    m->h.prev = NULL; m->h.next = d->memory; m->h.bytes = bytes; m->h.owner = d;
    if (d->memory) d->memory->h.prev = m;
    d->memory = m; d->used += bytes;
    if (d->used > d->peak) d->peak = d->used;
    return m + 1;
}
static void document_free(void *pointer)
{
    document_memory *m;
    if (!pointer) return;
    m = (document_memory *)pointer - 1;
    document_remote *d = m->h.owner;
    if (m->h.prev) m->h.prev->h.next = m->h.next;
    else d->memory = m->h.next;
    if (m->h.next) m->h.next->h.prev = m->h.prev;
    d->used -= m->h.bytes;
    epd_compute_free(m);
}
static void *document_realloc(void *pointer, size_t bytes)
{
    void *next; size_t old;
    if (!pointer) return document_alloc(bytes);
    if (!bytes) { document_free(pointer); return NULL; }
    old = ((document_memory *)pointer - 1)->h.bytes - sizeof(document_memory);
    next = document_alloc(bytes);
    if (next) { memcpy(next, pointer, bytes < old ? bytes : old); document_free(pointer); }
    return next;
}
static const book_allocator_t document_allocator = {document_alloc, document_realloc, document_free};

static int exchange(document_remote *d, unsigned operation)
{
    unsigned previous = book_timing_enter(BOOK_DOCUMENT_STAGE_WAIT);
    bool ok = epd_compute_pipe_exchange(&d->pipe, operation);
    book_timing_leave(previous);
    if (!ok)
        return d->status = epd_compute_fiber_cancelled(d->pipe.fiber) ? BOOK_CANCELLED : BOOK_IO_ERROR;
    /* Positive statuses belong to transport, never to a resource service. */
    return d->pipe.message->status > 0 ? BOOK_IO_ERROR : d->pipe.message->status;
}
static int flush(document_remote *d)
{
    int rc;
    if (!d->output_size) return BOOK_OK;
    memcpy(d->pipe.data, d->output, d->output_size);
    d->pipe.message->length = (uint32_t)d->output_size;
    d->pipe.message->flags = 0;
    rc = exchange(d, EPD_PIPE_OUTPUT);
    if (rc < 0) return d->status = rc;
    d->output_size = 0; d->first_flush = 1;
    return BOOK_OK;
}
static bool cancel(void *user)
{
    document_remote *d = user;
    if (epd_compute_fiber_cancelled(d->pipe.fiber)) d->status = BOOK_CANCELLED;
    if (d->status < 0) return true;
    if (epd_compute_fiber_should_yield(d->pipe.fiber))
    {
        if (d->output_size) { if (flush(d) < 0) return true; }
        else {
            d->pipe.message->length = 0;
            int rc = exchange(d, EPD_PIPE_IDLE);
            if (rc < 0) { d->status = rc; return true; }
        }
    }
    return false;
}
static int event(document_remote *d, unsigned type, unsigned a, unsigned b, unsigned c, unsigned scalar_d,
                 const void *first, size_t n, const void *second, size_t k)
{
    book_document_event_t h;
    size_t bytes;
    int rc;
    if (cancel(d)) return d->status;
    if ((d->selection.flags & BOOK_COMPUTE_NAV_ONLY) &&
        (type == BOOK_DOCUMENT_CHAPTER || (type == BOOK_DOCUMENT_METADATA && a == BOOK_DOCUMENT_META_TOC))) {
        if (d->navigation_events == UINT32_MAX) return BOOK_UNSUPPORTED;
        if (++d->navigation_events <= d->selection.skip_utf8) return BOOK_OK;
    }
    if (n > BOOK_DOCUMENT_BATCH_BYTES - sizeof(h) || k > BOOK_DOCUMENT_BATCH_BYTES - sizeof(h) - n)
        return BOOK_UNSUPPORTED;
    bytes = (sizeof(h) + n + k + 3u) & ~(size_t)3u;
    if (bytes > sizeof(d->output)) return BOOK_UNSUPPORTED;
    if (bytes > sizeof(d->output) - d->output_size && (rc = flush(d)) < 0) return rc;
    h = (book_document_event_t){type, (uint32_t)bytes, a, b, c, scalar_d, (uint32_t)n, (uint32_t)k};
    memcpy(d->output + d->output_size, &h, sizeof(h));
    if (n) memcpy(d->output + d->output_size + sizeof(h), first, n);
    if (k) memcpy(d->output + d->output_size + sizeof(h) + n, second, k);
    memset(d->output + d->output_size + sizeof(h) + n + k, 0, bytes - sizeof(h) - n - k);
    d->output_size += bytes;
    if ((!d->first_flush && d->output_size >= BOOK_DOCUMENT_FIRST_FLUSH) || d->output_size == sizeof(d->output))
        return flush(d);
    return BOOK_OK;
}
static int text(void *u, const char *p, size_t n, unsigned style)
{
    while (n)
    {
        size_t k = n > BOOK_DOCUMENT_TEXT_BYTES ? BOOK_DOCUMENT_TEXT_BYTES : n;
        int rc;
        if (k < n) while (k && ((unsigned char)p[k] & 0xc0) == 0x80) --k;
        if (!k) return BOOK_ERROR;
        rc = event(u, BOOK_DOCUMENT_TEXT, style, 0, 0, 0, p, k, NULL, 0);
        if (rc < 0) return rc;
        p += k; n -= k;
        style &= ~BOOK_STYLE_PAGE_START;
    }
    return BOOK_OK;
}
static int paragraph(void *u)
{ return event(u, BOOK_DOCUMENT_PARAGRAPH, 0, 0, 0, 0, NULL, 0, NULL, 0); }
static int anchor(void *u, const char *id, const char *title, unsigned level)
{ return event(u, BOOK_DOCUMENT_ANCHOR, level, title != NULL, 0, 0, id, strlen(id), title, title ? strlen(title) : 0); }
static int link_event(void *u, const char *target, bool begin)
{ return event(u, BOOK_DOCUMENT_LINK, begin, 0, 0, 0, target, strlen(target), NULL, 0); }
static int metadata(void *u, const char *title, const char *author)
{
    document_remote *d = u;
    int rc = event(d, BOOK_DOCUMENT_METADATA, 0, 0, 0, 0, title, title ? strlen(title) : 0, author, author ? strlen(author) : 0);
    /* Keep the title even if a declared cover cannot be decoded. */
    return rc == BOOK_OK && (d->selection.flags & BOOK_COMPUTE_METADATA) ? flush(d) : rc;
}
static int chapter(void *u, uint32_t index, uint32_t total, book_format_t format,
                   bool selected, const char *id, const char *title)
{
    document_remote *d = u;
    if (selected) d->selected_spine = index;
    return event(d, BOOK_DOCUMENT_CHAPTER, index, total, selected, (unsigned)format,
                  id, strlen(id), title, strlen(title));
}
static int toc(void *u, uint32_t index, uint32_t total, const char *id, const char *title, unsigned level)
{ return event(u, BOOK_DOCUMENT_METADATA, BOOK_DOCUMENT_META_TOC, index, level, total,
               id, strlen(id), title, strlen(title)); }
static int source_anchor(void *u, uint32_t index, uint32_t actual, uint32_t requested,
                         unsigned flags, const char *id)
{
    document_remote *d = u;
    d->selected_spine = index; d->source_unit = flags & 0xffu;
    if (flags & BOOK_SOURCE_TARGET) d->actual_target = actual;
    if (flags & BOOK_SOURCE_END) d->next_offset = actual;
    return event(d, BOOK_DOCUMENT_SOURCE_ANCHOR, index, actual, requested, flags,
                 id, strlen(id), NULL, 0);
}

static int entry_ready(void *user, size_t bytes)
{
    document_remote *d = user;
    if (!d->entry_enabled || !book_entry_valid(d->entry.bytes, bytes)) return BOOK_OK;
    int rc = flush(d);
    if (rc < 0) return rc;
    memcpy(d->pipe.data, d->entry.bytes, bytes);
    d->pipe.message->length = (uint32_t)bytes;
    d->pipe.message->flags = BOOK_DOCUMENT_ENTRY_OUTPUT;
    rc = exchange(d, EPD_PIPE_OUTPUT);
    d->pipe.message->flags = 0;
    return rc;
}
static int path_request(document_remote *d, const char *path)
{
    size_t n = strlen(path);
    if (n >= sizeof(d->pipe.message->path)) return BOOK_UNSUPPORTED;
    memcpy(d->pipe.message->path, path, n + 1);
    d->pipe.message->length = 0;
    d->pipe.message->handle = 0;
    d->pipe.message->size = 0;
    return BOOK_OK;
}
static int resource_stat(void *u, const char *path, struct stat *st)
{
    document_remote *d = u;
    int rc = path_request(d, path);
    if (rc < 0 || (rc = exchange(d, EPD_PIPE_STAT)) < 0) return rc;
    if (d->pipe.message->size > LONG_MAX) return BOOK_UNSUPPORTED;
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)d->pipe.message->size;
    if (d->pipe.message->flags == BOOK_DOCUMENT_REGULAR) st->st_mode = S_IFREG;
    else if (d->pipe.message->flags == BOOK_DOCUMENT_DIRECTORY) st->st_mode = S_IFDIR;
    else return BOOK_IO_ERROR;
    return BOOK_OK;
}
static int resource_open(void *u, const char *path, void **handle, uint64_t *size)
{
    document_remote *d = u;
    int rc = path_request(d, path);
    if (rc < 0 || (rc = exchange(d, EPD_PIPE_OPEN)) < 0) return rc;
    if (!d->pipe.message->handle) return BOOK_IO_ERROR;
    *handle = (void *)(uintptr_t)d->pipe.message->handle;
    *size = d->pipe.message->size;
    return BOOK_OK;
}
static int resource_read(void *u, void *handle, uint64_t offset, void *buffer, size_t bytes)
{
    document_remote *d = u;
    int rc;
    if (bytes > EPD_COMPUTE_PIPE_BYTES) bytes = EPD_COMPUTE_PIPE_BYTES;
    d->pipe.message->handle = (uint32_t)(uintptr_t)handle;
    d->pipe.message->offset = offset;
    d->pipe.message->length = (uint32_t)bytes;
    /* A failed ZIP refill can return buffered output once. Keep the failure
     * visible to its next cancellation boundary before the iterator resumes. */
    if ((rc = exchange(d, EPD_PIPE_READ)) < 0) return d->status = rc;
    if (d->pipe.message->length > bytes || (bytes && !d->pipe.message->length))
        return d->status = BOOK_IO_ERROR;
    memcpy(buffer, d->pipe.data, d->pipe.message->length);
    return (int)d->pipe.message->length;
}
static int close_handle(document_remote *d, void *handle, unsigned operation)
{
    /* The host session owns and reaps all handles after transport cancellation. */
    if (epd_compute_fiber_cancelled(d->pipe.fiber)) return BOOK_OK;
    d->pipe.message->handle = (uint32_t)(uintptr_t)handle;
    d->pipe.message->length = 0;
    return exchange(d, operation);
}
static int resource_close(void *u, void *handle) { return close_handle(u, handle, EPD_PIPE_CLOSE); }
static int directory_open(void *u, const char *path, void **handle)
{
    document_remote *d = u;
    int rc = path_request(d, path);
    if (rc < 0 || (rc = exchange(d, EPD_PIPE_DIR_OPEN)) < 0) return rc;
    if (!d->pipe.message->handle) return BOOK_IO_ERROR;
    *handle = (void *)(uintptr_t)d->pipe.message->handle;
    return BOOK_OK;
}
static int directory_next(void *u, void *handle, char *name, size_t capacity)
{
    document_remote *d = u;
    size_t n; int rc;
    d->pipe.message->handle = (uint32_t)(uintptr_t)handle;
    d->pipe.message->length = capacity > EPD_COMPUTE_PIPE_BYTES ? EPD_COMPUTE_PIPE_BYTES : (uint32_t)capacity;
    if ((rc = exchange(d, EPD_PIPE_DIR_NEXT)) < 0) return rc;
    n = d->pipe.message->length;
    if (!n) return 0;
    if (n > capacity || d->pipe.data[n - 1] || memchr(d->pipe.data, 0, n - 1)) return BOOK_IO_ERROR;
    memcpy(name, d->pipe.data, n);
    return 1;
}
static int directory_close(void *u, void *handle) { return close_handle(u, handle, EPD_PIPE_DIR_CLOSE); }
static const book_compute_resource_ops_t resource_ops = {
    resource_stat, resource_open, resource_read, resource_close, directory_open, directory_next, directory_close
};

typedef struct image_context {
    document_remote *document;
    const char *root_key, *base;
    const uint8_t *root_data;
    size_t root_size;
    const book_resources_t *resources;
    book_stream_t input;
    size_t position;
    uint8_t prefix[8];
    char *key;
    int status;
} image_context;

/* Finish the original stream even when the codec rejects the image. ZIP CRC
 * errors and cancellation must not be turned into a missing-image marker. */
static int image_drain(image_context *c)
{
    uint8_t discard[512];
    if (c->status < 0) return c->status;
    while (c->input.read)
    {
        if (cancel(c->document)) return c->document->status;
        int n = c->input.read(c->input.user, discard, sizeof(discard));
        if (n < 0) return c->status = n;
        if ((size_t)n > sizeof(discard) || (uint64_t)n > c->input.size - c->position)
            return c->status = BOOK_IO_ERROR;
        c->position += (size_t)n;
        if (!n) return c->position == c->input.size ? BOOK_OK : (c->status = BOOK_IO_ERROR);
    }
    return BOOK_OK;
}
static void image_close_input(image_context *c)
{
    if (c->input.close) c->input.close(c->input.user);
    memset(&c->input, 0, sizeof(c->input));
    c->position = 0;
}
static int image_read(void *u, const char *key, size_t offset, void *dst, size_t capacity, size_t *actual)
{
    image_context *c = u;
    int rc;
    *actual = 0;
    if (c->status < 0) return c->status;
    if (cancel(c->document)) return RI_CANCELLED;
    if (c->root_data && !strcmp(key, c->root_key))
    {
        if (!dst && !capacity && !offset) { *actual = c->root_size; return RI_OK; }
        if (!dst || offset > c->root_size) return RI_IO;
        if (capacity > c->root_size - offset) capacity = c->root_size - offset;
        memcpy(dst, c->root_data + offset, capacity); *actual = capacity;
        return RI_OK;
    }
    if (c->resources && (!c->key || strcmp(c->key, key)))
    {
        rc = image_drain(c);
        image_close_input(c);
        document_free(c->key); c->key = NULL;
        if (rc < 0) return c->status = rc;
        c->key = document_alloc(strlen(key) + 1);
        if (!c->key) return RI_NOMEM;
        strcpy(c->key, key);
        if (!c->resources->open) return c->status = BOOK_IO_ERROR;
        rc = c->resources->open(c->resources->user, c->base, key, &c->input);
        if (rc < 0) return c->status = rc;
    }
    if (!c->input.read || c->input.size > SIZE_MAX) return c->status = BOOK_IO_ERROR;
    size_t size = (size_t)c->input.size;
    if (!dst && !capacity && !offset) { *actual = size; return RI_OK; }
    if (!dst || offset > size) return RI_IO;
    if (capacity > size - offset) capacity = size - offset;
    if (offset < c->position)
    {
        /* Format sniffing replays eight bytes without restarting ZIP inflate. */
        if (offset < sizeof(c->prefix) && (c->position <= sizeof(c->prefix) ||
            capacity <= sizeof(c->prefix) - offset))
        {
            size_t n = c->position - offset;
            if (n > sizeof(c->prefix) - offset) n = sizeof(c->prefix) - offset;
            if (n > capacity) n = capacity;
            memcpy(dst, c->prefix + offset, n); *actual = n; return RI_OK;
        }
        /* SVG can reuse a reference or retry a smaller canvas. Keep only one
         * open reference; reopen forward-only ZIP entries when needed. */
        if (c->input.seek) rc = c->input.seek(c->input.user, 0);
        else if (c->resources)
        {
            rc = image_drain(c); image_close_input(c);
            if (rc == BOOK_OK) rc = c->resources->open(c->resources->user, c->base, key, &c->input);
        }
        else rc = BOOK_IO_ERROR;
        if (rc < 0) return c->status = rc;
        c->position = 0;
    }
    while (c->position < offset)
    {
        uint8_t discard[256];
        size_t n = offset - c->position;
        if (n > sizeof(discard)) n = sizeof(discard);
        if (cancel(c->document)) return RI_CANCELLED;
        rc = c->input.read(c->input.user, discard, n);
        if (rc <= 0 || (size_t)rc > n) return c->status = rc < 0 ? rc : BOOK_IO_ERROR;
        if (c->position < sizeof(c->prefix))
        {
            n = sizeof(c->prefix) - c->position;
            if (n > (size_t)rc) n = (size_t)rc;
            memcpy(c->prefix + c->position, discard, n);
        }
        c->position += (size_t)rc;
    }
    if (offset != c->position) return c->status = BOOK_IO_ERROR;
    if (!capacity) return RI_OK;
    if (capacity > EPD_COMPUTE_PIPE_BYTES) capacity = EPD_COMPUTE_PIPE_BYTES;
    rc = c->input.read(c->input.user, dst, capacity);
    if (rc <= 0 || (size_t)rc > capacity) return c->status = rc < 0 ? rc : BOOK_IO_ERROR;
    if (offset < sizeof(c->prefix))
    {
        size_t n = sizeof(c->prefix) - offset;
        if (n > (size_t)rc) n = (size_t)rc;
        memcpy(c->prefix + offset, dst, n);
    }
    c->position += (size_t)rc; *actual = (size_t)rc;
    return RI_OK;
}
static void *image_alloc(void *u, size_t bytes)
{
    (void)u;
    return document_alloc(bytes);
}
static void image_free(void *u, void *p) { (void)u; document_free(p); }
static bool image_cancel(void *u) { return cancel(((image_context *)u)->document); }
static size_t image_available(void *u)
{
    document_remote *d = ((image_context *)u)->document;
    size_t remaining = d->budget - d->used;
    return remaining > sizeof(document_memory) ? remaining - sizeof(document_memory) : 0;
}
static int emit_bitmap(document_remote *d, const char *id, const ri_bitmap *bitmap)
{
    size_t stride = ((size_t)bitmap->width + 1) / 2, rows, y;
    uint8_t *packed;
    int rc;
    if (!bitmap->width || !bitmap->height || stride > BOOK_DOCUMENT_BATCH_BYTES - sizeof(book_document_event_t))
        return BOOK_UNSUPPORTED;
    rows = (BOOK_DOCUMENT_BATCH_BYTES - sizeof(book_document_event_t)) / stride;
    if (rows > bitmap->height) rows = bitmap->height;
    packed = document_alloc(rows * stride);
    if (!packed) return BOOK_NO_MEMORY;
    ++d->image_id;
    rc = event(d, BOOK_DOCUMENT_IMAGE_BEGIN, d->image_id, bitmap->width, bitmap->height,
               (unsigned)stride, id, strlen(id), NULL, 0);
    for (y = 0; rc == BOOK_OK && y < bitmap->height; y += rows)
    {
        size_t count = bitmap->height - y, r, x;
        if (count > rows) count = rows;
        memset(packed, 0xff, count * stride);
        for (r = 0; r < count; ++r)
        {
            if (cancel(d)) { rc = d->status; break; }
            for (x = 0; x < bitmap->width; ++x)
            {
                unsigned gray = ((unsigned)bitmap->pixels[(y + r) * bitmap->width + x] * 15u + 127u) / 255u;
                uint8_t *p = &packed[r * stride + x / 2];
                if (x & 1) *p = (uint8_t)((*p & 0xf0) | gray);
                else *p = (uint8_t)((gray << 4) | 15u);
            }
        }
        if (rc == BOOK_OK) rc = event(d, BOOK_DOCUMENT_IMAGE_DATA, d->image_id, (unsigned)y,
            (unsigned)count, (unsigned)stride, packed, count * stride, NULL, 0);
    }
    if (rc == BOOK_OK) rc = event(d, BOOK_DOCUMENT_IMAGE_END, d->image_id, 0, 0, 0, NULL, 0, NULL, 0);
    document_free(packed);
    return rc;
}
static int decode_image(image_context *c, const char *id, bool svg)
{
    ri_io io = {0}; ri_bitmap bitmap = {0};
    unsigned width = c->document->width, height = c->document->height;
    size_t pixels = (c->document->budget - c->document->used) / 4u;
    int rc;
    if (!pixels) return BOOK_NO_MEMORY;
    /* Reserve headroom for the decoder, gray8 and the packed output batch.
     * Every allocation still passes the hard budget; scaling is an estimate. */
    while (!svg && (uint64_t)width * height > pixels)
    {
        width = width > 1 ? (width + 1) / 2 : 1;
        height = height > 1 ? (height + 1) / 2 : 1;
    }
    io.user = c; io.read = image_read; io.alloc = image_alloc; io.free = image_free;
    io.cancel = image_cancel; io.max_source_pixels = 64u * 1024u * 1024u;
    io.available = image_available;
    rc = svg ? ri_svg_init(&io) : RI_OK;
    if (rc == RI_BUSY) rc = BOOK_COMPUTE_BUSY;
    if (rc == RI_OK)
    {
        rc = svg ? ri_svg_decode(&io, c->root_key, width, height, &bitmap) :
            ri_decode(&io, c->root_key, width, height, &bitmap);
        if (rc == RI_BUSY) rc = BOOK_COMPUTE_BUSY;
        if (svg) ri_svg_shutdown();
    }
    int finished = image_drain(c);
    if (c->resources) image_close_input(c);
    document_free(c->key); c->key = NULL;
    if (finished < 0) rc = finished;
    if (c->document->status < 0) rc = c->document->status;
    if (rc == RI_OK) rc = emit_bitmap(c->document, id, &bitmap);
    else if (finished == BOOK_OK && c->document->status == BOOK_OK &&
             !(c->document->selection.flags & BOOK_COMPUTE_METADATA) &&
             (rc == RI_INVALID || rc == RI_UNSUPPORTED || rc == RI_IO))
    {
        /* The container is intact; isolate missing or unsupported images with a
         * small, deterministic missing-image mark and continue the document. */
        ri_bitmap_release(&bitmap);
        uint8_t mark[16 * 16];
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned x = 0; x < 16; ++x)
                mark[y * 16 + x] = !x || !y || x == 15 || y == 15 || x == y || x + y == 15 ? 0 : 255;
        const ri_bitmap missing = {mark, 16, 16};
        rc = emit_bitmap(c->document, id, &missing);
    }
    ri_bitmap_release(&bitmap);
    return rc;
}
static int image(void *u, const char *id, book_stream_t *stream)
{
    image_context c = {0};
    if (!stream) return BOOK_IO_ERROR;
    c.document = u; c.root_key = id;
    c.input = *stream;
    unsigned previous = book_timing_enter(BOOK_DOCUMENT_STAGE_IMAGE);
    int rc = decode_image(&c, id, false);
    book_timing_leave(previous);
    return rc;
}
static int svg(void *u, const char *xml, size_t length, const char *base, const book_resources_t *resources)
{
    image_context c = {0};
    c.document = u; c.root_key = "document:inline-svg"; c.base = base;
    c.root_data = (const uint8_t *)xml; c.root_size = length; c.resources = resources;
    unsigned previous = book_timing_enter(BOOK_DOCUMENT_STAGE_SVG);
    int rc = decode_image(&c, base, true);
    book_timing_leave(previous);
    return rc;
}

static int32_t convert(epd_compute_fiber_t *fiber, void *user)
{
    document_remote *d = user;
    book_sink_t sink = {0}; char error[192] = {0}; int rc;
    book_compute_observer_t observer = {d, chapter, toc, source_anchor,
        d->entry_enabled ? d->entry.bytes : NULL, entry_ready};
    bool nested = d->parent && bf_active();
    d->pipe.fiber = fiber;
    book_timing_begin(&d->timing, book_document_clock, BOOK_DOCUMENT_CLOCK_HZ);
    sink.user = d; sink.cancelled = cancel; sink.text = text; sink.paragraph = paragraph;
    sink.anchor = anchor; sink.link = link_event; sink.metadata = metadata;
    sink.image = image; sink.svg = svg; sink.markup = book_markup_convert;
    rc = d->parent ? book_compute_note_interrupt(d->path, &sink, &document_allocator, &resource_ops, d,
        &d->selection, error, sizeof(error)) :
        book_compute_convert_at(d->path, &sink, &document_allocator, &resource_ops, d,
        &d->selection, &observer, error, sizeof(error));
    if ((d->selection.flags & BOOK_COMPUTE_NOTE) && (rc == BOOK_OK || rc == 1))
        rc = event(d, BOOK_DOCUMENT_NOTE_END, rc == 1, 0, 0, 0, NULL, 0, NULL, 0);
    if (d->status < 0 && d->status != rc) { rc = d->status; error[0] = 0; }
    if (rc == BOOK_OK && (d->selection.flags & BOOK_COMPUTE_NAV_ONLY) &&
        d->navigation_events < d->selection.skip_utf8) rc = BOOK_ERROR;
    if (rc == BOOK_OK) rc = flush(d);
    if (rc < 0 && !error[0]) snprintf(error, sizeof(error), "Document conversion failed (%d)", rc);
    /* XML may retain shared encoding/error tables. Their scope belongs to the
     * suspended parent; do not free those tables when the note finishes. */
    while (nested && d->memory) {
        document_memory *m = d->memory;
        d->memory = m->h.next;
        d->used -= m->h.bytes;
        m->h.owner = d->parent; m->h.prev = NULL; m->h.next = d->parent->memory;
        if (m->h.next) m->h.next->h.prev = m;
        d->parent->memory = m; d->parent->used += m->h.bytes;
        if (d->parent->used > d->parent->peak) d->parent->peak = d->parent->used;
    }
    while (d->memory) document_free(d->memory + 1);
    book_timing_end();
    book_timing_resume(d->parent && !d->parent->finished && d->parent->timing.clock ? &d->parent->timing : NULL);
    d->status = rc;
    d->pipe.message->operation = EPD_PIPE_DONE;
    d->pipe.message->status = rc;
    d->pipe.message->values[0] = BOOK_DOCUMENT_WIRE_VERSION;
    d->pipe.message->values[1] = (uint32_t)d->peak;
    d->pipe.message->values[2] = BOOK_DOCUMENT_TIMING_MAGIC;
    d->pipe.message->values[3] = sizeof(d->timing.report);
    memcpy(d->pipe.message->path, &d->timing.report, sizeof(d->timing.report));
    d->pipe.message->values[4] = d->selected_spine;
    d->pipe.message->values[5] = d->actual_target;
    d->pipe.message->values[6] = d->source_unit;
    d->pipe.message->values[7] = d->next_offset;
    d->pipe.message->length = (uint32_t)strlen(error);
    if (error[0]) memcpy(d->pipe.data, error, strlen(error));
    d->finished = 1;
    d->result = rc == BOOK_OK ? EPD_COMPUTE_OK : rc == BOOK_CANCELLED ? EPD_COMPUTE_CANCELLED : EPD_COMPUTE_COMPONENT_ERROR;
    return d->result;
}
static int32_t open_document(void **state, const epd_compute_wire_t *request)
{
    epd_compute_pipe_remote_t pipe = {0}; document_remote *d;
    size_t budget, reserve = sizeof(*d) + BOOK_DOCUMENT_STACK_BYTES + 4096u;
    *state = NULL;
    if (!epd_compute_pipe_bind(&pipe, request) || pipe.message->values[0] != BOOK_DOCUMENT_WIRE_VERSION ||
        !memchr(pipe.message->path, 0, sizeof(pipe.message->path)) || !pipe.message->path[0]) return EPD_COMPUTE_INVALID;
    uint32_t flags = pipe.message->values[6] & ~BOOK_DOCUMENT_ENTRY_CACHE;
    bool entry_enabled = (pipe.message->values[6] & BOOK_DOCUMENT_ENTRY_CACHE) != 0;
    if (entry_enabled && (!(flags & BOOK_DOCUMENT_BODY_ONLY) || pipe.message->flags)) return EPD_COMPUTE_INVALID;
    if ((flags & ~(BOOK_DOCUMENT_SELECT_CHAPTER | BOOK_DOCUMENT_CONTEXT | BOOK_DOCUMENT_FULL_SOURCE | BOOK_DOCUMENT_NOTE | BOOK_DOCUMENT_METADATA_ONLY | BOOK_DOCUMENT_BODY_ONLY | BOOK_DOCUMENT_NAV_ONLY | BOOK_DOCUMENT_FIRST_BODY)) ||
        ((flags & BOOK_DOCUMENT_METADATA_ONLY) && flags != BOOK_DOCUMENT_METADATA_ONLY) ||
        ((flags & (BOOK_DOCUMENT_BODY_ONLY | BOOK_DOCUMENT_NAV_ONLY)) &&
         (!(flags & BOOK_DOCUMENT_SELECT_CHAPTER) ||
          (flags & (BOOK_DOCUMENT_NOTE | BOOK_DOCUMENT_METADATA_ONLY | BOOK_DOCUMENT_FULL_SOURCE)) ||
          (flags & (BOOK_DOCUMENT_BODY_ONLY | BOOK_DOCUMENT_NAV_ONLY)) ==
              (BOOK_DOCUMENT_BODY_ONLY | BOOK_DOCUMENT_NAV_ONLY))) ||
        ((flags & BOOK_DOCUMENT_FIRST_BODY) &&
         (!(flags & BOOK_DOCUMENT_BODY_ONLY) || pipe.message->values[5] || pipe.message->size)) ||
        pipe.message->values[7] > BOOK_COMPUTE_DEFAULT_SLICE ||
        pipe.message->offset > BOOK_COMPUTE_TXT_BACKSCAN ||
        pipe.message->size > UINT32_MAX || (pipe.message->flags &&
        pipe.message->flags != BOOK_DOCUMENT_FORMAT_XHTML)) return EPD_COMPUTE_INVALID;
    bool note = (pipe.message->values[6] & BOOK_DOCUMENT_NOTE) != 0;
    if (note ? note_session != NULL : (body_session || note_session)) return EPD_COMPUTE_BUSY;
    budget = pipe.message->values[1] ? pipe.message->values[1] : BOOK_DOCUMENT_DEFAULT_BUDGET;
    if (note && budget > BOOK_DOCUMENT_NOTE_BUDGET) budget = BOOK_DOCUMENT_NOTE_BUDGET;
    if (budget < reserve || budget > BOOK_DOCUMENT_MAX_BUDGET ||
        pipe.message->values[2] > 4096 || pipe.message->values[3] > 4096) return EPD_COMPUTE_INVALID;
    d = epd_compute_calloc(1, sizeof(*d));
    if (!d) return EPD_COMPUTE_NO_MEMORY;
    d->pipe = pipe; d->budget = budget; d->used = d->peak = reserve;
    d->parent = note ? body_session : NULL;
    d->selection = (book_compute_selection_t){pipe.message->values[4], pipe.message->values[5],
        flags, pipe.message->values[7], (uint32_t)pipe.message->offset,
        (uint32_t)pipe.message->size, pipe.message->flags, NULL, NULL};
    d->entry_enabled = entry_enabled;
    if (entry_enabled && pipe.message->length <= BOOK_ENTRY_MAX &&
        book_entry_valid(pipe.data, pipe.message->length)) {
        memcpy(d->entry.bytes, pipe.data, pipe.message->length);
        d->selection.entry = (const void *)d->entry.bytes;
    }
    if (d->selection.flags & BOOK_COMPUTE_NOTE) {
        if (!pipe.message->length || pipe.message->length > sizeof(d->note_target) ||
            !memchr(pipe.data, 0, pipe.message->length)) { epd_compute_free(d); return EPD_COMPUTE_INVALID; }
        memcpy(d->note_target, pipe.data, pipe.message->length);
        d->selection.note_target = d->note_target;
    }
    d->selected_spine = BOOK_COMPUTE_NO_SPINE;
    d->width = pipe.message->values[2] ? pipe.message->values[2] : 684;
    d->height = pipe.message->values[3] ? pipe.message->values[3] : 1216;
    strcpy(d->path, pipe.message->path);
    d->pipe.fiber = epd_compute_fiber_create("book_doc", BOOK_DOCUMENT_STACK_BYTES, convert, d);
    if (!d->pipe.fiber) { epd_compute_free(d); return EPD_COMPUTE_NO_MEMORY; }
    if (note) note_session = d; else body_session = d;
    *state = d;
    return EPD_COMPUTE_OK;
}
static int32_t step_document(void *state, const epd_compute_wire_t *request)
{
    document_remote *d = state;
    if (!d || (d != body_session && d != note_session) || !epd_compute_pipe_validate(&d->pipe, request)) return EPD_COMPUTE_INVALID;
    if (d == body_session && note_session && !note_session->finished) return EPD_COMPUTE_BUSY;
    if (d->finished) return d->result;
    active = d;
    return epd_compute_fiber_step(d->pipe.fiber, request);
}
static void close_document(void *state, const epd_compute_wire_t *request)
{
    document_remote *d = state;
    (void)request;
    if (!d) return;
    if (d == body_session && note_session && note_session->parent == d) {
        if (!note_session->finished) {
            active = note_session;
            epd_compute_fiber_close(note_session->pipe.fiber);
            note_session->pipe.fiber = NULL;
            note_session->finished = 1; note_session->result = EPD_COMPUTE_CANCELLED;
        }
        note_session->parent = NULL;
    }
    active = d;
    if (d->pipe.fiber) epd_compute_fiber_close(d->pipe.fiber);
    while (d->memory) document_free(d->memory + 1);
    if (d == note_session) note_session = NULL; else body_session = NULL;
    active = NULL; epd_compute_free(d);
}
const epd_compute_ops_t epd_document_remote_ops = {open_document, step_document, NULL, close_document};
