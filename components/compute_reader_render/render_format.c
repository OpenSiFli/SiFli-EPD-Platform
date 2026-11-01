#include "render_wire.h"
#include <string.h>
#include <stdbool.h>

static int64_t maximum(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t minimum(int64_t a, int64_t b) { return a < b ? a : b; }

rr_bounds rr_command_bounds(rr_frame f, const rr_command *c)
{
    rr_bounds b = {0};
    int64_t left = maximum(0, maximum(c->x, c->clip_x));
    int64_t top = maximum(0, maximum(c->y, c->clip_y));
    int64_t right = minimum(f.width, minimum((int64_t)c->x + c->width, (int64_t)c->clip_x + c->clip_w));
    int64_t bottom = minimum(f.height, minimum((int64_t)c->y + c->height, (int64_t)c->clip_y + c->clip_h));
    if (c->operation == RR_INIT) { b.rows = f.height; b.right = f.width; }
    else if (c->operation != RR_FINISH && right > left && bottom > top) {
        b.left = (uint32_t)left; b.top = (uint32_t)top;
        b.right = (uint32_t)right; b.rows = (uint32_t)(bottom - top);
    }
    return b;
}

static rr_window window(uint32_t begin, uint32_t end)
{ rr_window w = {begin & ~31u, ((end + 31u) & ~31u) - (begin & ~31u)}; return w; }

void rr_sync_windows(rr_frame f, const rr_command *c, uint32_t row, rr_window *pixels, rr_window *source)
{
    rr_bounds b = rr_command_bounds(f, c);
    *pixels = (rr_window){0, 0};
    bool has_source = c->operation == RR_BLIT_A8 || c->operation == RR_BLIT_GRAY4 ||
                      c->operation == RR_BLIT_RGB565 || c->operation == RR_SCALE_GRAY4_TO_RGB565;
    /* Even a fully clipped source command carries its base identity. */
    *source = (rr_window){0, has_source ? 32u : 0u};
    if (row >= b.rows) return;
    uint32_t rows = b.rows - row;
    if (rows > RR_ROWS_PER_STEP) rows = RR_ROWS_PER_STEP;
    uint32_t begin = (b.top + row) * f.stride + b.left * rr_pixel_bytes(f);
    uint32_t end = (b.top + row + rows - 1u) * f.stride +
                   (c->operation == RR_INIT ? f.stride : b.right * rr_pixel_bytes(f));
    *pixels = window(begin, end);
    if (has_source) {
        uint32_t first = (uint32_t)((int64_t)b.top + row - c->y), last = first + rows - 1u;
        if (c->operation == RR_SCALE_GRAY4_TO_RGB565) {
            first = (uint32_t)((uint64_t)first * c->src_height / c->height);
            last = (uint32_t)((uint64_t)last * c->src_height / c->height);
        }
        if (c->flags & RR_GRAY4_CONTIGUOUS) {
            begin = (uint32_t)((uint64_t)first * c->src_width / 2u);
            end = (uint32_t)(((uint64_t)(last + 1u) * c->src_width + 1u) / 2u);
        } else { begin = first * c->src_stride; end = (last + 1u) * c->src_stride; }
        *source = window(begin, end);
    }
}

void rr_command_init(rr_command *c, uint32_t operation, rr_frame f)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->version = RR_VERSION; c->bytes = sizeof(*c); c->operation = operation;
    c->width = c->clip_w = f.width; c->height = c->clip_h = f.height;
}

int rr_source_bytes(const rr_command *c, size_t *out)
{
    uint64_t bytes;
    uint32_t minimum;
    if (!c || !out) return RR_INVALID;
    *out = 0;
    if (c->operation == RR_INIT || c->operation == RR_FINISH || c->operation == RR_FILLRECT)
        return !c->src_width && !c->src_height && !c->src_stride ? RR_OK : RR_INVALID;
    if (c->operation != RR_BLIT_A8 && c->operation != RR_BLIT_GRAY4 &&
        c->operation != RR_SCALE_GRAY4_TO_RGB565 && c->operation != RR_BLIT_RGB565) return RR_INVALID;
    if (!c->src_width || !c->src_height || c->src_width > RR_MAX_DIM || c->src_height > RR_MAX_DIM)
        return RR_INVALID;
    if (c->flags & RR_GRAY4_CONTIGUOUS) {
        if (c->operation == RR_BLIT_A8 || c->operation == RR_BLIT_RGB565 || c->src_stride) return RR_INVALID;
        bytes = ((uint64_t)c->src_width * c->src_height + 1u) / 2u;
    } else {
        minimum = c->operation == RR_BLIT_A8 ? c->src_width :
            c->operation == RR_BLIT_RGB565 ? c->src_width * 2u : (c->src_width + 1u) / 2u;
        if (c->src_stride < minimum) return RR_INVALID;
        bytes = (uint64_t)c->src_stride * c->src_height;
    }
    if (bytes > RR_MAX_BUFFER_BYTES || (c->operation == RR_BLIT_A8 && bytes > RR_INPUT_BYTES)) return RR_INVALID;
    *out = (size_t)bytes;
    return RR_OK;
}

int rr_validate_command(const rr_command *c, size_t *source_bytes)
{
    if (!c || !source_bytes) return RR_INVALID;
    uint32_t flags = c->operation == RR_BLIT_A8 ? RR_A8_WHITE_BG :
        (c->operation == RR_BLIT_GRAY4 || c->operation == RR_SCALE_GRAY4_TO_RGB565) ? RR_GRAY4_CONTIGUOUS : 0;
    if (c->version != RR_VERSION || c->bytes != sizeof(*c) || !c->id ||
        c->operation < RR_INIT || c->operation > RR_BLIT_RGB565 || (c->flags & ~flags) ||
        c->width > RR_MAX_DIM || c->height > RR_MAX_DIM || c->clip_w > RR_MAX_DIM ||
        c->clip_h > RR_MAX_DIM || c->fill_rgb565 > 65535u) return RR_INVALID;
    if ((c->operation == RR_BLIT_A8 || c->operation == RR_BLIT_GRAY4 || c->operation == RR_BLIT_RGB565) &&
        (c->width > c->src_width || c->height > c->src_height)) return RR_INVALID;
    return rr_source_bytes(c, source_bytes);
}

int rr_a8_batch_window(rr_frame f, const void *data, uint32_t bytes,
                       uint32_t count, rr_window *out)
{
    const rr_command *commands = (const rr_command *)data;
    uint32_t begin = UINT32_MAX, end = 0, offset = RR_A8_BATCH_HEADER;
    if (!data || ((uintptr_t)data & 3u) || !count || count > RR_A8_BATCH_MAX || bytes > RR_INPUT_BYTES || bytes < offset)
        return RR_INVALID;
    for (uint32_t i = 0; i < count; ++i) {
        const rr_command *c = commands + i;
        size_t needed;
        rr_bounds b;
        uint32_t next = i ? commands[i-1].id + 1u : c->id;
        if (!next) next = 1;
        if (c->operation != RR_BLIT_A8 || c->id != next || c->height > RR_A8_BATCH_HEIGHT ||
            rr_validate_command(c, &needed) != RR_OK || needed > bytes - offset) return RR_INVALID;
        offset += (uint32_t)needed;
        b = rr_command_bounds(f, c);
        if (b.rows) {
            uint32_t lo = b.top * f.stride + b.left * rr_pixel_bytes(f);
            uint32_t hi = (b.top + b.rows - 1u) * f.stride + b.right * rr_pixel_bytes(f);
            if (lo < begin) begin = lo;
            if (hi > end) end = hi;
        }
    }
    *out = end ? window(begin, end) : (rr_window){0, 0};
    return offset == bytes ? RR_OK : RR_INVALID;
}
