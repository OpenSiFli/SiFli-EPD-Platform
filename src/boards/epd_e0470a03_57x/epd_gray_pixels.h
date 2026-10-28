/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_GRAY_PIXELS_H
#define EPD_GRAY_PIXELS_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define EPD_GRAY_NATIVE_WIDTH 1216u
#define EPD_GRAY_NATIVE_HEIGHT 684u
#define EPD_GRAY_TILE_W 16u
#define EPD_GRAY_TILE_H 32u

typedef struct
{
    uint16_t x0, y0, x1, y1;
} epd_gray_area_t;

#define EPD_GRAY_REGION_CAPACITY 32u
typedef struct
{
    epd_gray_area_t areas[EPD_GRAY_REGION_CAPACITY];
    unsigned count;
} epd_gray_regions_t;

/* Join only rectangular unions, so separated changes keep their own bounds. */
static inline void epd_gray_regions_add(epd_gray_regions_t *regions, epd_gray_area_t area)
{
    for (unsigned i = 0; i < regions->count;)
    {
        epd_gray_area_t *other = &regions->areas[i];
        if (other->x0 <= area.x0 && other->y0 <= area.y0 &&
            other->x1 >= area.x1 && other->y1 >= area.y1) return;
        bool contains = area.x0 <= other->x0 && area.y0 <= other->y0 &&
                        area.x1 >= other->x1 && area.y1 >= other->y1;
        bool vertical = area.x0 == other->x0 && area.x1 == other->x1 &&
                        area.y0 <= other->y1 + 1u && other->y0 <= area.y1 + 1u;
        bool horizontal = area.y0 == other->y0 && area.y1 == other->y1 &&
                          area.x0 <= other->x1 + 1u && other->x0 <= area.x1 + 1u;
        if (!contains && !vertical && !horizontal) { ++i; continue; }
        if (other->x0 < area.x0) area.x0 = other->x0;
        if (other->y0 < area.y0) area.y0 = other->y0;
        if (other->x1 > area.x1) area.x1 = other->x1;
        if (other->y1 > area.y1) area.y1 = other->y1;
        regions->areas[i] = regions->areas[--regions->count];
        i = 0;
    }
    if (regions->count == EPD_GRAY_REGION_CAPACITY)
    {
        regions->count = 0;
        area = (epd_gray_area_t){0, 0, EPD_GRAY_NATIVE_HEIGHT - 1, EPD_GRAY_NATIVE_WIDTH - 1};
    }
    regions->areas[regions->count++] = area;
}

static inline uint8_t epd_gray_rgb565(uint16_t pixel)
{
    return (uint8_t)(((((pixel >> 8) & 0xf8u) * 77u +
                       ((pixel >> 3) & 0xfcu) * 150u +
                       ((pixel << 3) & 0xf8u) * 29u) >> 12));
}

/* Commit the preceding displayed pixels once, before accepting a new frame. */
static inline void epd_gray_settle(uint8_t *mixed, const epd_gray_area_t *area)
{
    for (uint32_t x = area->x0; x <= area->x1; ++x)
    {
        uint8_t *dst = mixed + (EPD_GRAY_NATIVE_HEIGHT - 1u - x) *
                              EPD_GRAY_NATIVE_WIDTH + area->y0;
        uint32_t count = area->y1 - area->y0 + 1u;
        while (count && ((uintptr_t)dst & 3u))
        {
            uint8_t value = *dst & 15u;
            *dst++ = value | (value << 4);
            --count;
        }
        while (count >= 4u)
        {
            uint32_t value;
            memcpy(&value, dst, sizeof(value));
            value &= 0x0f0f0f0fu;
            value |= value << 4;
            memcpy(dst, &value, sizeof(value));
            dst += 4;
            count -= 4;
        }
        while (count--)
        {
            uint8_t value = *dst & 15u;
            *dst++ = value | (value << 4);
        }
    }
}

/* Count nonzero low nibbles in four independent bytes. */
static inline int32_t epd_gray_difference_count(uint32_t difference)
{
    uint32_t bits = ((difference + 0x0f0f0f0fu) & 0x10101010u) >> 4;
    return (int32_t)((bits * 0x01010101u) >> 24);
}

/* Portrait RGB565 -> panel (y, 683-x). Keep the displayed high nibble stable
 * across overlapping chunks. Return the change in the number of pixels that
 * differ from the displayed image; a later chunk may restore earlier pixels. */
static inline int32_t epd_gray_copy_rgb565(uint8_t *mixed, const uint8_t *source,
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
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint8_t *row = source + (ty + y - source_area->y0) * stride +
                                     (tx - source_area->x0) * 2u;
                for (uint32_t x = 0; x < width; ++x)
                {
                    uint16_t rgb;
                    memcpy(&rgb, row + x * 2u, sizeof(rgb));
                    tile[x][y] = epd_gray_rgb565(rgb);
                }
            }
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t *dst = mixed + (EPD_GRAY_NATIVE_HEIGHT - 1u - tx - x) *
                                      EPD_GRAY_NATIVE_WIDTH + ty;
                uint32_t y = 0;
                while (y < height && ((uintptr_t)(dst + y) & 3u))
                {
                    uint8_t previous = dst[y] >> 4;
                    changed += (tile[x][y] != previous) - ((dst[y] & 15u) != previous);
                    dst[y] = (dst[y] & 0xf0u) | tile[x][y];
                    ++y;
                }
                for (; y + 4u <= height; y += 4u)
                {
                    uint32_t old, gray;
                    memcpy(&old, dst + y, sizeof(old));
                    memcpy(&gray, &tile[x][y], sizeof(gray));
                    uint32_t previous = (old >> 4) & 0x0f0f0f0fu;
                    changed += epd_gray_difference_count(gray ^ previous) -
                               epd_gray_difference_count((old & 0x0f0f0f0fu) ^ previous);
                    old = (old & 0xf0f0f0f0u) | gray;
                    memcpy(dst + y, &old, sizeof(old));
                }
                for (; y < height; ++y)
                {
                    uint8_t previous = dst[y] >> 4;
                    changed += (tile[x][y] != previous) - ((dst[y] & 15u) != previous);
                    dst[y] = (dst[y] & 0xf0u) | tile[x][y];
                }
            }
        }
    }
    return changed;
}

#endif
