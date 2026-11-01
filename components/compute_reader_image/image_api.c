/* SPDX-License-Identifier: Apache-2.0 */
#include "image_internal.h"

typedef union allocation allocation;
union allocation {
#ifdef _MSC_VER
    __declspec(align(16)) unsigned char msvc_alignment;
#else
    max_align_t native_alignment;
#endif
    long double alignment;
    void *pointer_alignment;
    uint64_t integer_alignment;
    struct { ri_io io; size_t bytes; allocation *prev, *next; bool tracked; } data;
};
static const ri_io *active;
static int allocation_status;
static allocation *domain_head;
static void (*failure_hook)(void);
void ri_domain_begin(void (*failed)(void)) { domain_head = NULL; failure_hook = failed; }
static void detach(allocation *p)
{
    if (!p->data.tracked) return;
    if (p->data.prev) p->data.prev->data.next = p->data.next;
    else domain_head = p->data.next;
    if (p->data.next) p->data.next->data.prev = p->data.prev;
    p->data.tracked = false;
}
void ri_domain_finish(void *keep)
{
    failure_hook = NULL;
    if (keep) detach((allocation *)keep - 1);
    while (domain_head) ri_work_free(domain_head + 1);
}
bool ri_scope_enter(const ri_io *io)
{
    if (active) return false;
    active = io;
    allocation_status = RI_OK;
    return true;
}
void ri_scope_leave(void) { active = NULL; }
int ri_scope_status(void) { return allocation_status; }
static void *allocate_with(const ri_io *io, size_t bytes)
{
    if (!io || bytes > SIZE_MAX - sizeof(allocation)) {
        allocation_status = RI_NOMEM; if (failure_hook) failure_hook(); return NULL;
    }
    allocation *p = io->alloc(io->user, sizeof(*p) + bytes);
    if (!p) { allocation_status = RI_NOMEM; if (failure_hook) failure_hook(); return NULL; }
    p->data.io = *io;
    p->data.bytes = bytes;
    p->data.tracked = failure_hook != NULL;
    p->data.prev = NULL; p->data.next = NULL;
    if (p->data.tracked) {
        p->data.next = domain_head;
        if (domain_head) domain_head->data.prev = p;
        domain_head = p;
    }
    return p + 1;
}
void *ri_work_alloc(size_t bytes) { return allocate_with(active, bytes); }
void *ri_output_alloc(size_t bytes)
{
    if (!active) return NULL;
    ri_io io = *active;
    if (io.output_alloc) { io.alloc = io.output_alloc; io.free = io.output_free; }
    return allocate_with(&io, bytes);
}
void ri_work_free(void *memory)
{
    if (!memory) return;
    allocation *p = (allocation *)memory - 1;
    detach(p);
    p->data.io.free(p->data.io.user, p);
}
void *ri_work_realloc(void *memory, size_t bytes)
{
    if (!memory) return ri_work_alloc(bytes);
    if (!bytes) { ri_work_free(memory); return NULL; }
    allocation *p = (allocation *)memory - 1;
    void *next = ri_work_alloc(bytes);
    if (next) {
        memcpy(next, memory, bytes < p->data.bytes ? bytes : p->data.bytes);
        ri_work_free(memory);
    }
    return next;
}
void ri_bitmap_release(ri_bitmap *bitmap)
{
    if (bitmap) { ri_work_free(bitmap->pixels); memset(bitmap, 0, sizeof(*bitmap)); }
}
bool ri_raster_cancelled(const ri_raster_decoder_t *d)
{
    return d->io->cancel && d->io->cancel(d->io->user);
}
void *ri_raster_fast_alloc(ri_raster_decoder_t *d, size_t n)
{ (void)d; return ri_work_alloc(n); }
void *ri_raster_fast_alloc_priority(ri_raster_decoder_t *d, size_t n)
{ return ri_raster_fast_alloc(d, n); }
void ri_raster_fast_free(ri_raster_decoder_t *d, void *p)
{ (void)d; ri_work_free(p); }
size_t ri_raster_read_some(ri_raster_decoder_t *d, void *data, size_t bytes)
{
    uint8_t discard[256];
    size_t done = 0;
    if (bytes > d->file_size - d->position) bytes = d->file_size - d->position;
    while (done < bytes) {
        if (ri_raster_cancelled(d)) { d->status = RI_CANCELLED; break; }
        size_t n = bytes - done, got = 0;
        if (n > 4096) n = 4096;
        if (!data && n > sizeof(discard)) n = sizeof(discard);
        if (d->io->read(d->io->user, d->resource, d->position,
                data ? (uint8_t *)data + done : discard, n, &got) || !got || got > n) {
            d->status = RI_IO; break;
        }
        done += got; d->position += got;
    }
    return done;
}
bool ri_raster_read(ri_raster_decoder_t *d, void *data, size_t n)
{
    if (ri_raster_read_some(d, data, n) == n) return true;
    if (!d->status) d->status = RI_IO;
    return false;
}
bool ri_raster_seek(ri_raster_decoder_t *d, size_t p)
{ if (p > d->file_size) return false; d->position = p; return true; }
/* Kept internal so SVG image callbacks can decode on the already active scope. */
int ri_decode_active(const ri_io *io, const char *resource, unsigned w,
                     unsigned h, ri_bitmap *out)
{
    ri_raster_decoder_t d = {0};
    uint8_t signature[8];
    d.io = io; d.resource = resource; d.max_width = w; d.max_height = h;
    int status = RI_INVALID;
    if (io->cancel && io->cancel(io->user)) return RI_CANCELLED;
    if (io->read(io->user, resource, 0, NULL, 0, &d.file_size)) return RI_IO;
    if (!ri_raster_read(&d, signature, sizeof(signature))) return d.status;
    d.position = 0;
    bool ok;
    if (!memcmp(signature, "\x89PNG\r\n\x1a\n", 8)) ok = ri_raster_decode_png(&d);
    else if (signature[0] == 0xff && signature[1] == 0xd8) ok = ri_raster_decode_jpeg(&d);
    else return RI_UNSUPPORTED;
    if (ok) ok = ri_raster_finish_gray(&d);
    if (ok) {
        out->pixels = d.gray; out->width = d.width; out->height = d.height;
        d.gray = NULL; status = RI_OK;
    }
    if (d.status) status = d.status;
    if (!ok && ri_scope_status()) status = ri_scope_status();
    if (ri_raster_cancelled(&d)) status = RI_CANCELLED;
    ri_work_free(d.gray); ri_work_free(d.sum); ri_work_free(d.x_map);
    if (status) ri_bitmap_release(out);
    return status;
}
int ri_decode(const ri_io *io, const char *resource, unsigned w,
              unsigned h, ri_bitmap *out)
{
    if (!out) return RI_INVALID;
    memset(out, 0, sizeof(*out));
    if (!io || !io->read || !io->alloc || !io->free || !resource || !w || !h ||
        w > 65535u || h > 65535u || (!!io->output_alloc != !!io->output_free)) return RI_INVALID;
    if (!ri_scope_enter(io)) return RI_BUSY;
    int status = ri_decode_active(io, resource, w, h, out);
    ri_scope_leave();
    return status;
}
int ri_blit(uint8_t *dst, unsigned dw, unsigned dh, size_t stride,
            const uint8_t *src, unsigned sw, unsigned sh, size_t ss,
            int x, int y, unsigned first, unsigned rows, bool alpha,
            uint8_t ink, const ri_io *io)
{
    if (!dst || !src || !dw || !dh || !sw || !sh || stride < dw || ss < sw ||
        dh > SIZE_MAX / stride || sh > SIZE_MAX / ss || first > sh || rows > sh-first)
        return RI_INVALID;
    for (unsigned sy = first; sy < first + rows; ++sy) {
        if (io && io->cancel && io->cancel(io->user)) return RI_CANCELLED;
        int64_t dy = (int64_t)y + sy;
        if (dy < 0 || dy >= dh) continue;
        for (unsigned sx = 0; sx < sw; ++sx) {
            int64_t dx = (int64_t)x + sx;
            if (dx < 0 || dx >= dw) continue;
            uint8_t *p = dst + (size_t)dy * stride + (size_t)dx;
            unsigned value = src[(size_t)sy * ss + sx];
            *p = alpha ? (uint8_t)((ink * value + *p * (255-value) + 127)/255) : (uint8_t)value;
        }
    }
    return RI_OK;
}
