/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_BW_SCROLL_H
#define EPD_BW_SCROLL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Black/white band refresh. L8 selects band x (old bit,new bit),
 * leaving the panel raster and each transition's drive sequence intact. */
#define EPD_BW_SCROLL_BANDS 8u
#define EPD_BW_SCROLL_MAX_FRAMES 64u
#define EPD_BW_SCROLL_MAX_GAP 16u
#define EPD_BW_SCROLL_THRESHOLD 8u
#define EPD_BW_SCROLL_NEUTRAL 0xff000000u

typedef struct
{
    uint32_t frames, gap;
    bool reverse;
    uint8_t phase[EPD_BW_SCROLL_MAX_FRAMES]; /* four two-bit codes per byte */
} epd_bw_scroll_t;

static inline uint32_t epd_bw_scroll_frames(const epd_bw_scroll_t *plan)
{
    if (!plan || !plan->frames || plan->frames > EPD_BW_SCROLL_MAX_FRAMES ||
        plan->gap > EPD_BW_SCROLL_MAX_GAP) return 0;
    return plan->frames + (EPD_BW_SCROLL_BANDS - 1u) * plan->gap;
}

/* Quantize the target only. Old gray remains truthful for the priming full
 * refresh; only later updates with binary old/target can be band-encoded. */
static inline uint32_t epd_bw_scroll_quantize(uint8_t *mixed, size_t count)
{
    uint32_t changed = 0;
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t old = mixed[i] >> 4;
        uint8_t next = (mixed[i] & 15u) >= EPD_BW_SCROLL_THRESHOLD ? 15u : 0u;
        mixed[i] = (uint8_t)((old << 4) | next);
        changed += old != next;
    }
    return changed;
}

/* Validate the entire shadow first: rejection must never leave a partly
 * encoded framebuffer. Native rows become vertical strips in portrait UI. */
static inline bool epd_bw_scroll_encode(uint8_t *mixed, uint32_t width, uint32_t height)
{
    if (!mixed || !width || height < EPD_BW_SCROLL_BANDS ||
        width > SIZE_MAX / height) return false;
    size_t count = (size_t)width * height;
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t old = mixed[i] >> 4, next = mixed[i] & 15u;
        if ((old != 0 && old != 15) || (next != 0 && next != 15)) return false;
    }
    for (uint32_t row = 0; row < height; ++row)
    {
        uint8_t band = (uint8_t)(((uint64_t)row * EPD_BW_SCROLL_BANDS) / height);
        uint8_t *dst = mixed + (size_t)row * width;
        for (uint32_t column = 0; column < width; ++column)
        {
            uint8_t value = dst[column];
            dst[column] = (uint8_t)((band << 2) | ((value >> 4) == 15 ? 2u : 0u) |
                                    ((value & 15u) == 15 ? 1u : 0u));
        }
    }
    return true;
}

/* Restore the exact binary old/target shadow before releasing the SDK's
 * last draw buffer. The next begin_pixels() can then settle it normally. */
static inline void epd_bw_scroll_decode(uint8_t *mixed, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t transition = mixed[i] & 3u;
        mixed[i] = (uint8_t)((transition & 2u ? 0xf0u : 0u) |
                             (transition & 1u ? 0x0fu : 0u));
    }
}

/* Cache the four endpoint transitions before HV is enabled. No waveform
 * reader work or large automatic LUT is needed in the frame-complete ISR. */
static inline bool epd_bw_scroll_save_phase(epd_bw_scroll_t *plan, uint32_t frame,
                                           const uint32_t *lut)
{
    static const uint8_t indices[4] = {0x00, 0x0f, 0xf0, 0xff};
    if (!epd_bw_scroll_frames(plan) || !lut || frame >= plan->frames) return false;
    for (unsigned transition = 0; transition < 4; ++transition)
    {
        uint32_t code = lut[indices[transition]];
        if ((code & ~3u) != EPD_BW_SCROLL_NEUTRAL || (code & 3u) == 3u ||
            ((transition == 0 || transition == 3) && code != EPD_BW_SCROLL_NEUTRAL))
            return false;
    }
    uint8_t packed = 0;
    for (unsigned transition = 0; transition < 4; ++transition)
        packed |= (uint8_t)((lut[indices[transition]] & 3u) << (transition * 2u));
    plan->phase[frame] = packed;
    return true;
}

static inline bool epd_bw_scroll_fill_lut(uint32_t *lut, const epd_bw_scroll_t *plan,
                                         uint32_t global_frame)
{
    uint32_t total = epd_bw_scroll_frames(plan);
    if (!lut || !total) return false;
    for (unsigned i = 0; i < 256; ++i) lut[i] = EPD_BW_SCROLL_NEUTRAL;
    if (global_frame >= total) return true;
    for (uint32_t band = 0; band < EPD_BW_SCROLL_BANDS; ++band)
    {
        uint32_t order = plan->reverse ? EPD_BW_SCROLL_BANDS - 1u - band : band;
        int32_t local = (int32_t)global_frame - (int32_t)(order * plan->gap);
        if (local < 0 || (uint32_t)local >= plan->frames) continue;
        for (unsigned transition = 0; transition < 4; ++transition)
            lut[(band << 2) | transition] = EPD_BW_SCROLL_NEUTRAL |
                ((plan->phase[local] >> (transition * 2u)) & 3u);
    }
    return true;
}

#endif
