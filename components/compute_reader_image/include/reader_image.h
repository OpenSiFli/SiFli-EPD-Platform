/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_IMAGE_H
#define READER_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { RI_OK = 0, RI_INVALID = -1, RI_CANCELLED = -2, RI_NOMEM = -3,
       RI_UNSUPPORTED = -4, RI_IO = -5, RI_BUSY = -6 };
typedef struct {
    void *user;
    /* NULL dst, capacity=0, offset=0 queries resource length in *actual.
     * Otherwise read up to capacity bytes; short reads are accepted.
     * Return zero on success. The callback may suspend the worker stack.
     * Resource is an opaque caller key; SVG href uses this same callback. */
    int (*read)(void *user, const char *resource, size_t offset,
                void *dst, size_t capacity, size_t *actual);
    void *(*alloc)(void *user, size_t bytes);
    void (*free)(void *user, void *ptr);
    bool (*cancel)(void *user);
    uint64_t max_source_pixels; /* zero: format limits only */
    /* Optional pair for final gray8 storage in shared memory. Workspace keeps
     * using alloc/free. Release outputs on ACPU; publish only pixels/size. */
    void *(*output_alloc)(void *user, size_t bytes);
    void (*output_free)(void *user, void *ptr);
    /* Optional remaining total request budget. SVG fits its temporary ARGB
     * and gray8 output into this remainder, including an algorithm reserve. */
    size_t (*available)(void *user);
} ri_io;
typedef struct { uint8_t *pixels; unsigned width, height; } ri_bitmap;
/* Synchronous, single serialized worker. A suspended call remains active.
 * Input and temporary storage use alloc/free. Output is tightly packed gray8
 * over white. Release with ri_bitmap_release before destroying allocator user.
 * The allocator must provide max_align_t alignment and return NULL on failure.
 * This API uses no LVGL, RT-Thread, display, filesystem or HCPU service. */
int ri_decode(const ri_io *io, const char *resource, unsigned max_width,
              unsigned max_height, ri_bitmap *out);
void ri_bitmap_release(ri_bitmap *bitmap);
/* Optional SVG backend. Enable once on the same serialized worker. Each decode
 * constructs and destroys its private SDK state and retains only its output. */
int ri_svg_init(const ri_io *io);
void ri_svg_shutdown(void);
int ri_svg_decode(const ri_io *io, const char *resource, unsigned max_width,
                  unsigned max_height, ri_bitmap *out);
/* Composite a clipped gray8 image or A8 glyph onto gray8; y/count bound work.
 * Source/destination must not overlap. A8 uses ink as foreground gray. */
int ri_blit(uint8_t *dst, unsigned dw, unsigned dh, size_t stride,
            const uint8_t *src, unsigned sw, unsigned sh, size_t src_stride,
            int x, int y, unsigned first_row, unsigned rows, bool alpha,
            uint8_t ink, const ri_io *io);
#ifdef __cplusplus
}
#endif
#endif
