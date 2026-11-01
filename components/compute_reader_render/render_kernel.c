#include "reader_render.h"
#include <string.h>

static int frame_valid(rr_frame f, size_t capacity)
{
    uint64_t bytes = (uint64_t)f.stride * f.height;
    return f.width && f.height && f.width <= RR_MAX_DIM && f.height <= RR_MAX_DIM &&
        f.format <= RR_FORMAT_L8 && !(f.stride & 3u) && f.stride >= f.width * rr_pixel_bytes(f) &&
        bytes <= RR_MAX_BUFFER_BYTES && bytes <= capacity;
}

int rr_kernel_bind(rr_kernel *k, rr_frame f, void *pixels, size_t capacity)
{
    if (!k || !pixels || ((uintptr_t)pixels & 3u) || !frame_valid(f, capacity) ||
        capacity > UINTPTR_MAX - (uintptr_t)pixels) return RR_INVALID;
    memset(k, 0, sizeof(*k));
    k->frame = f; k->pixels = (uint8_t *)pixels; k->capacity = capacity;
    return RR_OK;
}

static int disjoint(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an <= UINTPTR_MAX - x && bn <= UINTPTR_MAX - y && (x + an <= y || y + bn <= x);
}
static void bounds(rr_kernel *k, const rr_command *c)
{
    rr_bounds b = rr_command_bounds(k->frame, c);
    k->row = 0; k->rows = b.rows; k->left = b.left; k->top = b.top; k->right = b.right;
}

static uint16_t load565(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static void store565(uint8_t *p, uint16_t color) { p[0] = (uint8_t)color; p[1] = (uint8_t)(color >> 8); }
static uint16_t gray565(unsigned gray)
{ return (uint16_t)(((gray & 0xf8u) << 8) | ((gray & 0xfcu) << 3) | (gray >> 3)); }
static uint16_t black_over(uint16_t color, unsigned alpha)
{
    unsigned keep = 255u - alpha;
    unsigned r = (((color >> 11) & 31u) * keep + 127u) / 255u;
    unsigned g = (((color >> 5) & 63u) * keep + 127u) / 255u;
    unsigned b = ((color & 31u) * keep + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static unsigned luma565(uint16_t color)
{
    unsigned r = color >> 11, g = (color >> 5) & 63u, b = color & 31u;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return (77u * r + 150u * g + 29u * b + 128u) >> 8;
}

static void render_row(rr_kernel *k)
{
    const rr_command *c = &k->command;
    uint32_t y = k->top + k->row, x;
    uint8_t *row = k->pixels + (size_t)y * k->frame.stride;
    if (c->operation == RR_INIT) { memset(row, 255, k->frame.stride); return; }
    if (c->operation == RR_BLIT_RGB565 && k->frame.format == RR_FORMAT_RGB565) {
        size_t sy = (size_t)((int64_t)y - c->y), sx = (size_t)((int64_t)k->left - c->x);
        memcpy(row + k->left * 2u, k->source + sy * c->src_stride + sx * 2u,
               (size_t)(k->right - k->left) * 2u);
        return;
    }
    for (x = k->left; x < k->right; ++x) {
        uint16_t color;
        if (c->operation == RR_FILLRECT) color = k->frame.format == RR_FORMAT_L8 ?
            (uint16_t)luma565((uint16_t)c->fill_rgb565) : (uint16_t)c->fill_rgb565;
        else {
            uint32_t sx = (uint32_t)((int64_t)x - c->x), sy = (uint32_t)((int64_t)y - c->y);
            if (c->operation == RR_BLIT_RGB565) {
                color = (uint16_t)luma565(load565(k->source + (size_t)sy * c->src_stride + sx * 2u));
            } else if (c->operation == RR_BLIT_A8) {
                unsigned alpha = k->source[(size_t)sy * c->src_stride + sx];
                if (k->frame.format == RR_FORMAT_L8) {
                    unsigned old = c->flags & RR_A8_WHITE_BG ? 255u : row[x];
                    color = (uint16_t)((old * (255u-alpha) + 127u) / 255u);
                } else color = black_over(c->flags & RR_A8_WHITE_BG ? 65535u : load565(row + x * 2u), alpha);
            } else {
                uint64_t pixel;
                unsigned nibble;
                if (c->operation == RR_SCALE_GRAY4_TO_RGB565) {
                    sx = (uint32_t)((uint64_t)sx * c->src_width / c->width);
                    sy = (uint32_t)((uint64_t)sy * c->src_height / c->height);
                }
                pixel = c->flags & RR_GRAY4_CONTIGUOUS ? (uint64_t)sy * c->src_width + sx :
                    (uint64_t)sy * c->src_stride * 2u + sx;
                nibble = (k->source[(size_t)(pixel / 2u)] >> (pixel & 1u ? 0 : 4)) & 15u;
                color = k->frame.format == RR_FORMAT_L8 ? (uint16_t)(nibble * 17u) : gray565(nibble * 17u);
            }
        }
        if (k->frame.format == RR_FORMAT_L8) row[x] = (uint8_t)color;
        else store565(row + x * 2u, color);
    }
}

static int result_status(rr_kernel *k, rr_result *r, int status)
{
    r->status = status;
    if (k) { r->rows_done = k->row; r->rows_total = k->rows; }
    return status;
}

int rr_kernel_step(rr_kernel *k, const rr_command *c, const void *source, size_t bytes,
                   rr_cancel_fn cancelled, void *user, rr_result *r)
{
    size_t expected;
    if (!r) return RR_INVALID;
    memset(r, 0, sizeof(*r)); r->version = RR_VERSION; r->bytes = sizeof(*r);
    if (c) r->id = c->id;
    if (!k || !k->pixels || rr_validate_command(c, &expected) != RR_OK ||
        bytes != expected || (bytes && (!source || !disjoint(source, bytes, k->pixels, k->capacity))))
        return result_status(NULL, r, RR_INVALID);
    if (k->active) {
        if (memcmp(c, &k->command, sizeof(*c)) || k->source != source || k->source_bytes != bytes)
            return result_status(k, r, RR_INVALID);
    } else {
        uint32_t next = k->last_id + 1u;
        if (!next) next = 1;
        if (k->last_id && c->id != next) return result_status(k, r, RR_INVALID);
        if (c->operation != RR_INIT && (!k->initialized || k->finished || k->poisoned))
            return result_status(k, r, RR_BAD_STATE);
        k->command = *c; k->source = (const uint8_t *)source; k->source_bytes = bytes;
        bounds(k, c); k->active = 1;
        if (c->operation == RR_INIT) { k->initialized = k->finished = k->poisoned = 0; }
    }
    do {
        if (cancelled && cancelled(user)) {
            k->active = 0; k->poisoned = 1; k->last_id = c->id;
            return result_status(k, r, RR_CANCELLED);
        }
        if (k->row == k->rows) break;
        render_row(k); ++k->row; ++r->rows_this_step;
    } while (r->rows_this_step < RR_ROWS_PER_STEP);
    if (k->row != k->rows) return result_status(k, r, RR_MORE);
    k->active = 0; k->last_id = c->id;
    if (c->operation == RR_INIT) k->initialized = 1;
    if (c->operation == RR_FINISH) k->finished = 1;
    k->source = NULL; k->source_bytes = 0;
    return result_status(k, r, RR_OK);
}
