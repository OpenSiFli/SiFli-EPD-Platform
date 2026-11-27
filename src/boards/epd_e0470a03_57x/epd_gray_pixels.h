/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_GRAY_PIXELS_H
#define EPD_GRAY_PIXELS_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Keep pixel loops optimized even when the surrounding driver uses -Oz. */
#if defined(__GNUC__) && !defined(__clang__)
#define EPD_GRAY_HOT __attribute__((optimize("O2")))
#else
#define EPD_GRAY_HOT
#endif

#define EPD_GRAY_NATIVE_WIDTH 1216u
#define EPD_GRAY_NATIVE_HEIGHT 684u
#define EPD_GRAY_TILE_W 16u
#define EPD_GRAY_TILE_H 32u

typedef struct
{
    uint16_t x0, y0, x1, y1;
} epd_gray_area_t;

#define EPD_GRAY_BYTES (EPD_GRAY_NATIVE_WIDTH * EPD_GRAY_NATIVE_HEIGHT)
#define EPD_GRAY_BLOCK_BYTES 64u
#define EPD_GRAY_BLOCKS (EPD_GRAY_BYTES / EPD_GRAY_BLOCK_BYTES)
#define EPD_GRAY_DIRTY_WORDS ((EPD_GRAY_BLOCKS + 31u) / 32u)

typedef struct
{
    uint32_t blocks[EPD_GRAY_DIRTY_WORDS];
} epd_gray_dirty_t;

static inline void epd_gray_mark(epd_gray_dirty_t *dirty, uint32_t offset)
{
    uint32_t block = offset / EPD_GRAY_BLOCK_BYTES;
    dirty->blocks[block / 32u] |= 1u << (block % 32u);
}

static inline unsigned epd_gray_find_block(const epd_gray_dirty_t *dirty,
                                           unsigned start, bool marked)
{
    for (unsigned word = start / 32u; word < EPD_GRAY_DIRTY_WORDS; ++word)
    {
        uint32_t bits = marked ? dirty->blocks[word] : ~dirty->blocks[word];
        if (word == start / 32u) bits &= UINT32_MAX << (start % 32u);
        if (bits)
        {
            unsigned block = word * 32u + (unsigned)__builtin_ctz(bits);
            return block < EPD_GRAY_BLOCKS ? block : EPD_GRAY_BLOCKS;
        }
    }
    return EPD_GRAY_BLOCKS;
}

/* Consecutive cache lines can be settled or synchronized together. */
static inline bool epd_gray_next_span(const epd_gray_dirty_t *dirty, unsigned *next,
                                      unsigned *offset, unsigned *length)
{
    unsigned first = epd_gray_find_block(dirty, *next, true);
    if (first == EPD_GRAY_BLOCKS) return false;
    *next = epd_gray_find_block(dirty, first + 1u, false);
    *offset = first * EPD_GRAY_BLOCK_BYTES;
    *length = (*next - first) * EPD_GRAY_BLOCK_BYTES;
    return true;
}

static inline uint8_t epd_gray_rgb565(uint16_t pixel)
{
    return (uint8_t)(((((pixel >> 8) & 0xf8u) * 77u +
                       ((pixel >> 3) & 0xfcu) * 150u +
                       ((pixel << 3) & 0xf8u) * 29u) >> 12));
}

/* Spans start at a cache-line boundary. */
static inline void epd_gray_settle_span(uint8_t *dst, unsigned length)
{
    for (unsigned i = 0; i < length; i += 4u)
    {
        uint32_t value;
        memcpy(&value, dst + i, sizeof(value));
        value &= 0x0f0f0f0fu;
        value |= value << 4;
        memcpy(dst + i, &value, sizeof(value));
    }
}

static inline int epd_gray_store(uint8_t *mixed, epd_gray_dirty_t *dirty,
                                 unsigned offset, uint8_t gray)
{
    uint8_t old = mixed[offset];
    if ((old & 15u) == gray) return 0;
    mixed[offset] = (old & 0xf0u) | gray;
    epd_gray_mark(dirty, offset);
    return (gray != (old >> 4)) - ((old & 15u) != (old >> 4));
}

/* Two 16-bit lanes: each weighted sum is at most 64088, so neither the
 * products nor their sum carry into the adjacent pixel. */
static inline uint32_t epd_gray_rgb565_pair(uint32_t pixels)
{
    uint32_t red = (pixels >> 11) & 0x001f001fu;
    uint32_t green = (pixels >> 5) & 0x003f003fu;
    uint32_t blue = pixels & 0x001f001fu;
    uint32_t luma = red * 616u + green * 600u + blue * 232u;
    return (luma >> 12) & 0x000f000fu;
}

static inline EPD_GRAY_HOT void epd_gray_rgb565_tile(const uint8_t *source, uint32_t stride,
                                       unsigned width, unsigned height,
                                       uint8_t tile[EPD_GRAY_TILE_W][EPD_GRAY_TILE_H])
{
    for (unsigned y = 0; y < height; ++y)
    {
        unsigned x = 0;
        for (; x + 1u < width; x += 2u)
        {
            uint32_t pair;
            memcpy(&pair, source + y * stride + x * 2u, sizeof(pair));
            uint32_t gray = epd_gray_rgb565_pair(pair);
            tile[x][y] = (uint8_t)gray;
            tile[x + 1u][y] = (uint8_t)(gray >> 16);
        }
        if (x < width)
        {
            uint16_t rgb;
            memcpy(&rgb, source + y * stride + x * 2u, sizeof(rgb));
            tile[x][y] = epd_gray_rgb565(rgb);
        }
    }
}

/* Count nonzero low nibbles in four independent bytes. */
static inline int32_t epd_gray_difference_count(uint32_t difference)
{
    uint32_t bits = ((difference + 0x0f0f0f0fu) & 0x10101010u) >> 4;
    return (int32_t)((bits * 0x01010101u) >> 24);
}

static inline int32_t epd_gray_store_word(uint8_t *mixed, epd_gray_dirty_t *dirty,
                                          unsigned offset, uint32_t gray)
{
    uint32_t old;
    memcpy(&old, mixed + offset, sizeof(old));
    uint32_t target = old & 0x0f0f0f0fu;
    if (gray == target) return 0;
    uint32_t previous = (old >> 4) & 0x0f0f0f0fu;
    old = (old & 0xf0f0f0f0u) | gray;
    memcpy(mixed + offset, &old, sizeof(old));
    epd_gray_mark(dirty, offset);
    return epd_gray_difference_count(gray ^ previous) -
           epd_gray_difference_count(target ^ previous);
}

/* A prepared pixel needs four gray bits and one bit of coverage. */
#define EPD_GRAY_COVERAGE_BYTES (EPD_GRAY_BYTES / 8u)
#define EPD_GRAY_PREPARE_BYTES (EPD_GRAY_BYTES / 2u + EPD_GRAY_COVERAGE_BYTES)

static inline void epd_gray_cover_span(uint8_t *coverage, epd_gray_dirty_t *dirty,
                                        unsigned offset, unsigned length)
{
    unsigned end = offset + length;
    for (unsigned block = offset & ~(EPD_GRAY_BLOCK_BYTES - 1u); block < end;
         block += EPD_GRAY_BLOCK_BYTES)
        epd_gray_mark(dirty, block);
    while (offset < end && (offset & 7u))
    {
        coverage[offset / 8u] |= 1u << (offset & 7u);
        ++offset;
    }
    unsigned whole = (end - offset) / 8u;
    memset(coverage + offset / 8u, 0xff, whole);
    offset += whole * 8u;
    while (offset < end)
    {
        coverage[offset / 8u] |= 1u << (offset & 7u);
        ++offset;
    }
}

static inline void epd_gray_repaint_span(uint8_t *mixed, epd_gray_dirty_t *dirty,
                                         const uint8_t *coverage, const uint8_t *indices,
                                         unsigned offset, unsigned length)
{
    for (unsigned i = offset; i < offset + length; i += 8u)
    {
        unsigned byte = i / 8u;
        unsigned bits = coverage[byte];
        if (bits == 0xffu)
        {
            uint32_t first, last;
            memcpy(&first, mixed + i, sizeof(first));
            memcpy(&last, mixed + i + 4u, sizeof(last));
            if ((first & last) == UINT32_MAX) continue;
        }
        while (bits)
        {
            unsigned pixel = i + (unsigned)__builtin_ctz(bits);
            uint8_t old_index = mixed[pixel];
            uint8_t new_index = indices[old_index];
            if (new_index != old_index)
            {
                mixed[pixel] = new_index;
                epd_gray_mark(dirty, pixel);
            }
            bits &= bits - 1u;
        }
    }
}

static inline void epd_gray_prepare_pixel(uint8_t *gray, uint8_t *valid,
                                           unsigned offset, uint8_t value)
{
    unsigned shift = (offset & 1u) * 4u;
    uint8_t *packed = gray + offset / 2u;
    *packed = (uint8_t)((*packed & ~(15u << shift)) | (value << shift));
    valid[offset / 8u] |= 1u << (offset % 8u);
}

static inline EPD_GRAY_HOT void epd_gray_prepare_span(uint8_t *prepared, unsigned offset,
                                          const uint8_t *pixels, unsigned length)
{
    uint8_t *valid = prepared + EPD_GRAY_BYTES / 2u;
    while (length && (offset & 7u))
    {
        epd_gray_prepare_pixel(prepared, valid, offset++, *pixels++);
        --length;
    }
    for (; length >= 8u; length -= 8u, offset += 8u, pixels += 8u)
    {
        uint32_t first, last;
        memcpy(&first, pixels, sizeof(first));
        memcpy(&last, pixels + 4u, sizeof(last));
        first = (first | (first >> 4)) & 0x00ff00ffu;
        last = (last | (last >> 4)) & 0x00ff00ffu;
        first = (first | (first >> 8)) & 0xffffu;
        last = (last | (last >> 8)) << 16;
        first |= last;
        memcpy(prepared + offset / 2u, &first, sizeof(first));
        valid[offset / 8u] = 0xffu;
    }
    while (length--)
        epd_gray_prepare_pixel(prepared, valid, offset++, *pixels++);
}

/* Landscape LVGL coordinates already match the panel's scan order. */
static inline EPD_GRAY_HOT void epd_gray_prepare_rgb565_native(uint8_t *prepared,
    epd_gray_dirty_t *dirty, const uint8_t *source, uint32_t stride, const epd_gray_area_t *area)
{
    uint8_t *valid = prepared + EPD_GRAY_BYTES / 2u;
    for (unsigned y = area->y0; y <= area->y1; ++y)
    {
        unsigned offset = y * EPD_GRAY_NATIVE_WIDTH + area->x0;
        unsigned end = y * EPD_GRAY_NATIVE_WIDTH + area->x1 + 1u;
        for (unsigned block = offset & ~(EPD_GRAY_BLOCK_BYTES - 1u); block < end;
             block += EPD_GRAY_BLOCK_BYTES)
            epd_gray_mark(dirty, block);

        const uint8_t *pixel = source + (y - area->y0) * stride;
        while (offset < end)
        {
            if (!(offset & 7u) && end - offset >= 8u)
            {
                for (unsigned i = 0; i < 4; ++i)
                {
                    uint32_t pair;
                    memcpy(&pair, pixel + i * 4u, sizeof(pair));
                    pair = epd_gray_rgb565_pair(pair);
                    prepared[offset / 2u + i] = (uint8_t)(pair | (pair >> 12));
                }
                valid[offset / 8u] = 0xffu;
                pixel += 16u; offset += 8u;
            }
            else
            {
                uint16_t rgb;
                memcpy(&rgb, pixel, sizeof(rgb));
                epd_gray_prepare_pixel(prepared, valid, offset++, epd_gray_rgb565(rgb));
                pixel += 2u;
            }
        }
    }
}

/* Opt 1: Landscape L8 (0=black..255=white) directly matches panel scan order. */
static inline void epd_gray_prepare_l8_native(uint8_t *prepared,
    epd_gray_dirty_t *dirty, const uint8_t *source, uint32_t stride, const epd_gray_area_t *area)
{
    uint8_t *valid = prepared + EPD_GRAY_BYTES / 2u;
    for (unsigned y = area->y0; y <= area->y1; ++y)
    {
        unsigned offset = y * EPD_GRAY_NATIVE_WIDTH + area->x0;
        unsigned end = y * EPD_GRAY_NATIVE_WIDTH + area->x1 + 1u;
        for (unsigned block = offset & ~(EPD_GRAY_BLOCK_BYTES - 1u); block < end;
             block += EPD_GRAY_BLOCK_BYTES)
            epd_gray_mark(dirty, block);

        const uint8_t *pixel = source + (y - area->y0) * stride;
        while (offset < end)
        {
            if (!(offset & 7u) && offset + 8u <= end)
            {
                uint8_t *out = prepared + offset / 2u;
                for (unsigned k = 0; k < 4u; ++k)
                    out[k] = (uint8_t)((pixel[k * 2u] >> 4) | (pixel[k * 2u + 1u] & 0xf0u));
                valid[offset / 8u] = 0xffu;
                offset += 8u;
                pixel += 8u;
            }
            else
            {
                epd_gray_prepare_pixel(prepared, valid, offset++, *pixel++ >> 4);
            }
        }
    }
}

/* Opt 3: Direct L8 landscape merge into mixed[] (bypasses prepared_pixels). */
static inline EPD_GRAY_HOT int32_t epd_gray_direct_l8_native(uint8_t *mixed,
    epd_gray_dirty_t *dirty, uint8_t *coverage, epd_gray_dirty_t *cov_dirty,
    const uint8_t *source, uint32_t stride, const epd_gray_area_t *area)
{
    int32_t changed = 0;
    for (unsigned y = area->y0; y <= area->y1; ++y)
    {
        unsigned offset = y * EPD_GRAY_NATIVE_WIDTH + area->x0;
        unsigned len = area->x1 - area->x0 + 1u;
        epd_gray_cover_span(coverage, cov_dirty, offset, len);
        const uint8_t *pixel = source + (y - area->y0) * stride;
        unsigned end = offset + len;
        while (offset < end && (offset & 3u))
        {
            changed += epd_gray_store(mixed, dirty, offset++, *pixel++ >> 4);
        }
        for (; offset + 4u <= end; offset += 4u, pixel += 4u)
        {
            uint32_t w;
            memcpy(&w, pixel, 4u);
            uint32_t gray = (w >> 4) & 0x0f0f0f0fu;
            changed += epd_gray_store_word(mixed, dirty, offset, gray);
        }
        while (offset < end)
        {
            changed += epd_gray_store(mixed, dirty, offset++, *pixel++ >> 4);
        }
    }
    return changed;
}

/* Direct portrait L8 -> panel (y, 683-x) merge into mixed[] (skips prepared_pixels).
 * 32x32 cache-line-aligned tiling with tx outer / ty inner keeps source reads at 100%
 * cache-line utilization and sweeps mixed[] linearly with zero function calls. */
#define EPD_GRAY_DIRECT_W 32u
#define EPD_GRAY_DIRECT_H 32u
static inline EPD_GRAY_HOT int32_t epd_gray_direct_l8_portrait(uint8_t *mixed,
    epd_gray_dirty_t *dirty, uint8_t *coverage, epd_gray_dirty_t *cov_dirty,
    const uint8_t *source, uint32_t stride, const epd_gray_area_t *area)
{
    uint8_t tile[EPD_GRAY_DIRECT_W][EPD_GRAY_DIRECT_H];
    int32_t changed = 0;
    const uint32_t row_len = area->y1 - area->y0 + 1u;
    for (unsigned x = area->x0; x <= area->x1; ++x)
    {
        unsigned off = (EPD_GRAY_NATIVE_HEIGHT - 1u - x) * EPD_GRAY_NATIVE_WIDTH + area->y0;
        unsigned end = off + row_len;
        for (unsigned blk = off >> 6, bend = (end - 1u) >> 6; blk <= bend; ++blk)
            cov_dirty->blocks[blk >> 5] |= 1u << (blk & 31u);
        while (off < end && (off & 7u)) { coverage[off >> 3] |= (uint8_t)(1u << (off & 7u)); ++off; }
        unsigned whole = (end - off) >> 3;
        if (whole)
        {
            uint8_t *cp = coverage + (off >> 3);
            unsigned k = 0;
            while (k < whole && ((uintptr_t)(cp + k) & 3u)) cp[k++] = 0xffu;
            for (; k + 4u <= whole; k += 4u)
            {
                uint32_t ff = 0xffffffffu;
                memcpy(cp + k, &ff, 4u);
            }
            while (k < whole) cp[k++] = 0xffu;
            off += whole << 3;
        }
        while (off < end) { coverage[off >> 3] |= (uint8_t)(1u << (off & 7u)); ++off; }
    }
    for (unsigned tx = area->x0; tx <= area->x1; tx += EPD_GRAY_DIRECT_W)
    {
        unsigned width = area->x1 - tx + 1u;
        if (width > EPD_GRAY_DIRECT_W) width = EPD_GRAY_DIRECT_W;
        for (unsigned ty = area->y0; ty <= area->y1; )
        {
            unsigned step = EPD_GRAY_DIRECT_H - (ty & (EPD_GRAY_DIRECT_H - 1u));
            unsigned height = area->y1 - ty + 1u;
            if (height > step) height = step;
            const uint8_t *pixels = source + (ty - area->y0) * stride + (tx - area->x0);
            for (unsigned y = 0; y < height; ++y)
            {
                const uint8_t *row = pixels + y * stride;
                unsigned x = 0;
                for (; x + 4u <= width; x += 4u)
                {
                    uint32_t w;
                    memcpy(&w, row + x, 4u);
                    w = (w >> 4) & 0x0f0f0f0fu;
                    tile[x][y]      = (uint8_t)w;
                    tile[x + 1u][y] = (uint8_t)(w >> 8);
                    tile[x + 2u][y] = (uint8_t)(w >> 16);
                    tile[x + 3u][y] = (uint8_t)(w >> 24);
                }
                for (; x < width; ++x) tile[x][y] = row[x] >> 4;
            }
            for (unsigned x = 0; x < width; ++x)
            {
                unsigned base = (EPD_GRAY_NATIVE_HEIGHT - 1u - tx - x) *
                                EPD_GRAY_NATIVE_WIDTH + ty;
                uint8_t *dst = mixed + base;
                const uint8_t *src = tile[x];
                bool strip_dirty = false;
                unsigned y = 0;
                while (y < height && ((base + y) & 3u))
                {
                    uint8_t g = src[y], old = dst[y];
                    if ((old & 15u) != g)
                    {
                        dst[y] = (uint8_t)((old & 0xf0u) | g);
                        strip_dirty = true;
                        changed += (g != (old >> 4)) - ((old & 15u) != (old >> 4));
                    }
                    ++y;
                }
                for (; y + 4u <= height; y += 4u)
                {
                    uint32_t gray, old;
                    memcpy(&gray, src + y, 4u);
                    memcpy(&old, dst + y, 4u);
                    uint32_t target = old & 0x0f0f0f0fu;
                    if (gray == target) continue;
                    uint32_t prev = (old >> 4) & 0x0f0f0f0fu;
                    old = (old & 0xf0f0f0f0u) | gray;
                    memcpy(dst + y, &old, 4u);
                    strip_dirty = true;
                    changed += epd_gray_difference_count(gray ^ prev) -
                               epd_gray_difference_count(target ^ prev);
                }
                for (; y < height; ++y)
                {
                    uint8_t g = src[y], old = dst[y];
                    if ((old & 15u) != g)
                    {
                        dst[y] = (uint8_t)((old & 0xf0u) | g);
                        strip_dirty = true;
                        changed += (g != (old >> 4)) - ((old & 15u) != (old >> 4));
                    }
                }
                if (strip_dirty)
                {
                    unsigned blk = base >> 6;
                    dirty->blocks[blk >> 5] |= 1u << (blk & 31u);
                }
            }
            ty += height;
        }
    }
    return changed;
}

/* P1: Portrait L8 (0=black..255=white) -> packed 4-bit panel (y, 683-x) for 8-aligned y spans. */
#define EPD_GRAY_PACKED_TILE_H 40u
static inline EPD_GRAY_HOT void epd_gray_prepare_l8_packed(uint8_t *prepared,
    const uint8_t *source, uint32_t stride, const epd_gray_area_t *area)
{
    uint8_t tile[EPD_GRAY_TILE_W][EPD_GRAY_PACKED_TILE_H / 2u];
    for (unsigned ty = area->y0; ty <= area->y1; ty += EPD_GRAY_PACKED_TILE_H)
    {
        unsigned height = area->y1 - ty + 1u;
        if (height > EPD_GRAY_PACKED_TILE_H) height = EPD_GRAY_PACKED_TILE_H;
        for (unsigned tx = area->x0; tx <= area->x1; tx += EPD_GRAY_TILE_W)
        {
            unsigned width = area->x1 - tx + 1u;
            if (width > EPD_GRAY_TILE_W) width = EPD_GRAY_TILE_W;
            const uint8_t *pixels = source + (ty - area->y0) * stride + (tx - area->x0);
            for (unsigned y = 0; y < height; y += 2u)
            {
                const uint8_t *row0 = pixels + y * stride;
                const uint8_t *row1 = row0 + stride;
                unsigned x = 0;
                for (; x + 4u <= width; x += 4u)
                {
                    uint32_t a, b;
                    memcpy(&a, row0 + x, sizeof(a));
                    memcpy(&b, row1 + x, sizeof(b));
                    uint32_t packed = ((a >> 4) & 0x0f0f0f0fu) | (b & 0xf0f0f0f0u);
                    tile[x][y / 2u] = (uint8_t)packed;
                    tile[x + 1u][y / 2u] = (uint8_t)(packed >> 8);
                    tile[x + 2u][y / 2u] = (uint8_t)(packed >> 16);
                    tile[x + 3u][y / 2u] = (uint8_t)(packed >> 24);
                }
                for (; x < width; ++x)
                    tile[x][y / 2u] = (uint8_t)((row0[x] >> 4) | (row1[x] & 0xf0u));
            }
            for (unsigned x = 0; x < width; ++x)
            {
                unsigned offset = (EPD_GRAY_NATIVE_HEIGHT - 1u - tx - x) * EPD_GRAY_NATIVE_WIDTH + ty;
                uint8_t *out = prepared + offset / 2u;
                uint8_t *valid = prepared + EPD_GRAY_BYTES / 2u + offset / 8u;
                for (unsigned n = 0; n < height / 8u; ++n)
                {
                    uint32_t v;
                    memcpy(&v, tile[x] + n * 4u, 4u);
                    memcpy(out + n * 4u, &v, 4u);
                    valid[n] = 0xffu;
                }
            }
        }
    }
}

/* Packed transpose for spans whose endpoints cover whole coverage bytes. */
static inline EPD_GRAY_HOT void epd_gray_prepare_rgb565_packed(uint8_t *prepared,
    const uint8_t *source, uint32_t stride, const epd_gray_area_t *area)
{
    uint8_t tile[EPD_GRAY_TILE_W][EPD_GRAY_PACKED_TILE_H / 2u];
    for (unsigned ty = area->y0; ty <= area->y1; ty += EPD_GRAY_PACKED_TILE_H)
    {
        unsigned height = area->y1 - ty + 1u;
        if (height > EPD_GRAY_PACKED_TILE_H) height = EPD_GRAY_PACKED_TILE_H;
        for (unsigned tx = area->x0; tx <= area->x1; tx += EPD_GRAY_TILE_W)
        {
            unsigned width = area->x1 - tx + 1u;
            if (width > EPD_GRAY_TILE_W) width = EPD_GRAY_TILE_W;
            const uint8_t *pixels = source + (ty - area->y0) * stride + (tx - area->x0) * 2u;
            for (unsigned y = 0; y < height; y += 2u)
            {
                unsigned x = 0;
                for (; x + 1u < width; x += 2u)
                {
                    uint32_t a, b;
                    memcpy(&a, pixels + y * stride + x * 2u, sizeof(a));
                    memcpy(&b, pixels + (y + 1u) * stride + x * 2u, sizeof(b));
                    uint32_t packed = epd_gray_rgb565_pair(a) | (epd_gray_rgb565_pair(b) << 4);
                    tile[x][y / 2u] = (uint8_t)packed;
                    tile[x + 1u][y / 2u] = (uint8_t)(packed >> 16);
                }
                if (x < width)
                {
                    uint16_t a, b;
                    memcpy(&a, pixels + y * stride + x * 2u, sizeof(a));
                    memcpy(&b, pixels + (y + 1u) * stride + x * 2u, sizeof(b));
                    tile[x][y / 2u] = epd_gray_rgb565(a) | (epd_gray_rgb565(b) << 4);
                }
            }
            for (unsigned x = 0; x < width; ++x)
            {
                unsigned offset = (EPD_GRAY_NATIVE_HEIGHT - 1u - tx - x) * EPD_GRAY_NATIVE_WIDTH + ty;
                uint8_t *out = prepared + offset / 2u;
                uint8_t *valid = prepared + EPD_GRAY_BYTES / 2u + offset / 8u;
                for (unsigned n = 0; n < height / 8u; ++n)
                {
                    uint32_t v;
                    memcpy(&v, tile[x] + n * 4u, 4u);
                    memcpy(out + n * 4u, &v, 4u);
                    valid[n] = 0xffu;
                }
            }
        }
    }
}

static inline uint32_t epd_gray_expand4(const uint8_t *packed)
{
    uint16_t pair;
    memcpy(&pair, packed, sizeof(pair));
    uint32_t value = pair;
    value = (value | (value << 8)) & 0x00ff00ffu;
    return (value | (value << 4)) & 0x0f0f0f0fu;
}

static inline EPD_GRAY_HOT int32_t epd_gray_apply_prepared(uint8_t *mixed, epd_gray_dirty_t *dirty,
                                               const uint8_t *prepared,
                                               unsigned offset, unsigned length)
{
    const uint8_t *valid = prepared + EPD_GRAY_BYTES / 2u;
    int32_t changed = 0;
    for (unsigned i = offset; i < offset + length; i += 8u)
    {
        unsigned mask = valid[i / 8u];
        if (mask == 0xffu)
        {
            uint32_t first, last;
            memcpy(&first, mixed + i, sizeof(first));
            memcpy(&last, mixed + i + 4u, sizeof(last));
            uint32_t gray0 = epd_gray_expand4(prepared + i / 2u);
            uint32_t gray1 = epd_gray_expand4(prepared + i / 2u + 2u);
            uint32_t target0 = first & 0x0f0f0f0fu;
            uint32_t target1 = last & 0x0f0f0f0fu;
            if (gray0 == target0 && gray1 == target1) continue;
            uint32_t old0 = (first >> 4) & 0x0f0f0f0fu;
            uint32_t old1 = (last >> 4) & 0x0f0f0f0fu;
            changed += epd_gray_difference_count(gray0 ^ old0) +
                       epd_gray_difference_count(gray1 ^ old1) -
                       epd_gray_difference_count(target0 ^ old0) -
                       epd_gray_difference_count(target1 ^ old1);
            first = (first & 0xf0f0f0f0u) | gray0;
            last = (last & 0xf0f0f0f0u) | gray1;
            memcpy(mixed + i, &first, sizeof(first));
            memcpy(mixed + i + 4u, &last, sizeof(last));
            epd_gray_mark(dirty, i);
            continue;
        }
        for (unsigned k = 0; k < 8u; ++k)
            if (mask & (1u << k))
                changed += epd_gray_store(mixed, dirty, i + k,
                                          (prepared[(i + k) / 2u] >> ((k & 1u) * 4u)) & 15u);
    }
    return changed;
}

/* Portrait RGB565 -> panel (y, 683-x). Keep the displayed high nibble stable
 * across overlapping chunks. Return the change in the number of pixels that
 * differ from the displayed image; a later chunk may restore earlier pixels. */
static inline int32_t epd_gray_copy_rgb565(uint8_t *mixed, epd_gray_dirty_t *dirty,
                                      const uint8_t *source,
                                      uint32_t stride,
                                      const epd_gray_area_t *source_area,
                                      const epd_gray_area_t *clip,
                                      uint8_t tile[EPD_GRAY_TILE_W][EPD_GRAY_TILE_H])
{
    int32_t changed = 0;
    for (uint32_t ty = clip->y0; ty <= clip->y1; ty += EPD_GRAY_TILE_H)
    {
        uint32_t height = clip->y1 - ty + 1u;
        if (height > EPD_GRAY_TILE_H) height = EPD_GRAY_TILE_H;
        for (uint32_t tx = clip->x0; tx <= clip->x1; tx += EPD_GRAY_TILE_W)
        {
            uint32_t width = clip->x1 - tx + 1u;
            if (width > EPD_GRAY_TILE_W) width = EPD_GRAY_TILE_W;
            epd_gray_rgb565_tile(source + (ty - source_area->y0) * stride +
                                  (tx - source_area->x0) * 2u, stride, width, height, tile);
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t *dst = mixed + (EPD_GRAY_NATIVE_HEIGHT - 1u - tx - x) *
                                      EPD_GRAY_NATIVE_WIDTH + ty;
                uint32_t y = 0;
                while (y < height && ((uintptr_t)(dst + y) & 3u))
                {
                    changed += epd_gray_store(mixed, dirty, (unsigned)(dst + y - mixed), tile[x][y]);
                    ++y;
                }
                for (; y + 4u <= height; y += 4u)
                {
                    uint32_t gray;
                    memcpy(&gray, &tile[x][y], sizeof(gray));
                    changed += epd_gray_store_word(mixed, dirty, (unsigned)(dst + y - mixed), gray);
                }
                for (; y < height; ++y)
                {
                    changed += epd_gray_store(mixed, dirty, (unsigned)(dst + y - mixed), tile[x][y]);
                }
            }
        }
    }
    return changed;
}

#endif
