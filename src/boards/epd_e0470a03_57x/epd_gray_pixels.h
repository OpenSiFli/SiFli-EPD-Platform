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

/* Packed transpose for spans whose endpoints cover whole coverage bytes. */
#define EPD_GRAY_PACKED_TILE_H 40u
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
                for (unsigned n = 0; n < height / 2u; n += 4u)
                {
                    uint32_t value;
                    memcpy(&value, tile[x] + n, sizeof(value));
                    memcpy(out + n, &value, sizeof(value));
                }
                uint8_t *valid = prepared + EPD_GRAY_BYTES / 2u + offset / 8u;
                if (height >= 32u)
                {
                    uint32_t covered = UINT32_MAX;
                    memcpy(valid, &covered, sizeof(covered));
                    if (height == 40u) valid[4] = 0xffu;
                }
                else if (height >= 16u)
                {
                    uint16_t covered = UINT16_MAX;
                    memcpy(valid, &covered, sizeof(covered));
                    if (height == 24u) valid[2] = 0xffu;
                }
                else valid[0] = 0xffu;
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
        for (unsigned j = 0; mask && j < 8u; j += 4u, mask >>= 4)
        {
            const uint8_t *packed = prepared + (i + j) / 2u;
            if ((mask & 15u) == 15u)
                changed += epd_gray_store_word(mixed, dirty, i + j, epd_gray_expand4(packed));
            else
                for (unsigned k = 0; k < 4u; ++k)
                    if (mask & (1u << k))
                        changed += epd_gray_store(mixed, dirty, i + j + k,
                                                   (packed[k / 2u] >> ((k & 1u) * 4u)) & 15u);
        }
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
