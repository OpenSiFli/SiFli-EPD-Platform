/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epd_bw_scroll.h"

static void make_phase(uint32_t *lut, unsigned frame)
{
    for (unsigned i = 0; i < 256; ++i) lut[i] = EPD_BW_SCROLL_NEUTRAL;
    if (frame < 30) lut[0x0f] |= 2u;
    if (frame < 18) lut[0xf0] |= 1u;
}

static void quantization(void)
{
    uint8_t pixels[256];
    unsigned expected = 0;
    for (unsigned i = 0; i < 256; ++i)
    {
        pixels[i] = (uint8_t)i;
        expected += (i >> 4) != ((i % 16) < 8 ? 0u : 15u);
    }
    assert(epd_bw_scroll_quantize(pixels, 256) == expected);
    for (unsigned i = 0; i < 256; ++i)
    {
        assert((pixels[i] >> 4) == (i >> 4));
        assert((pixels[i] & 15u) == ((i % 16) < 8 ? 0u : 15u));
    }
}

static void geometry(unsigned width, unsigned height)
{
    size_t count = (size_t)width * height;
    uint8_t *pixels = malloc(count + 2), *original = malloc(count);
    assert(pixels && original);
    pixels[0] = 0x5a;
    pixels[count + 1] = 0xa5;
    static const uint8_t states[4] = {0x00, 0x0f, 0xf0, 0xff};
    for (size_t i = 0; i < count; ++i) original[i] = pixels[i + 1] = states[i % 4];
    assert(epd_bw_scroll_encode(pixels + 1, width, height));
    assert(pixels[0] == 0x5a && pixels[count + 1] == 0xa5);
    unsigned rows_per_band[8] = {0};
    unsigned previous_band = 0;
    for (unsigned row = 0; row < height; ++row)
    {
        unsigned band = pixels[1 + (size_t)row * width] >> 2;
        assert(band < 8 && (band == previous_band || band == previous_band + 1));
        previous_band = band;
        ++rows_per_band[band];
        for (unsigned column = 0; column < width; ++column)
        {
            size_t i = (size_t)row * width + column;
            assert((pixels[i + 1] >> 2) == band);
            assert((pixels[i + 1] & 3) == i % 4);
        }
    }
    assert((pixels[1] >> 2) == 0 && (pixels[count] >> 2) == 7);
    for (unsigned band = 0; band < 8; ++band)
        assert(rows_per_band[band] == height / 8 || rows_per_band[band] == (height + 7) / 8);
    epd_bw_scroll_decode(pixels + 1, count);
    assert(memcmp(pixels + 1, original, count) == 0);
    assert(pixels[0] == 0x5a && pixels[count + 1] == 0xa5);

    /* A gray pixel late in the buffer cannot cause a partial mutation. */
    pixels[count] = 0x7f;
    memcpy(original, pixels + 1, count);
    assert(!epd_bw_scroll_encode(pixels + 1, width, height));
    assert(memcmp(pixels + 1, original, count) == 0);
    pixels[count] = 0xf7;
    memcpy(original, pixels + 1, count);
    assert(!epd_bw_scroll_encode(pixels + 1, width, height));
    assert(memcmp(pixels + 1, original, count) == 0);
    assert(!epd_bw_scroll_encode(pixels + 1, 0, height));
    assert(!epd_bw_scroll_encode(pixels + 1, width, 7));
    assert(!epd_bw_scroll_encode(NULL, width, height));
    free(original);
    free(pixels);
}

static void schedule(void)
{
    const unsigned gaps[] = {0, 1, 4, 16};
    const unsigned lengths[] = {44, 64};
    uint32_t base[256], guarded[258];
    for (unsigned length_index = 0; length_index < 2; ++length_index)
        for (unsigned gap_index = 0; gap_index < 4; ++gap_index)
            for (unsigned reverse = 0; reverse < 2; ++reverse)
            {
                epd_bw_scroll_t plan = {0};
                plan.frames = lengths[length_index];
                plan.gap = gaps[gap_index];
                plan.reverse = reverse != 0;
                for (unsigned frame = 0; frame < plan.frames; ++frame)
                {
                    make_phase(base, frame);
                    assert(epd_bw_scroll_save_phase(&plan, frame, base));
                }
                unsigned counts[8][4][3] = {{{0}}};
                unsigned total = epd_bw_scroll_frames(&plan);
                assert(total == plan.frames + 7 * plan.gap);
                for (unsigned frame = 0; frame < total + 2; ++frame)
                {
                    guarded[0] = 0x12345678;
                    guarded[257] = 0x89abcdef;
                    assert(epd_bw_scroll_fill_lut(guarded + 1, &plan, frame));
                    assert(guarded[0] == 0x12345678 && guarded[257] == 0x89abcdef);
                    for (unsigned index = 32; index < 256; ++index)
                        assert(guarded[index + 1] == EPD_BW_SCROLL_NEUTRAL);
                    for (unsigned band = 0; band < 8; ++band)
                    {
                        int local = (int)frame - (int)((reverse ? 7 - band : band) * plan.gap);
                        bool active = local >= 0 && (unsigned)local < plan.frames && frame < total;
                        for (unsigned transition = 0; transition < 4; ++transition)
                        {
                            unsigned expected = active && transition == 1 && local < 30 ? 2 :
                                                active && transition == 2 && local < 18 ? 1 : 0;
                            unsigned code = guarded[1 + band * 4 + transition] & 3;
                            assert(code == expected);
                            if (frame < total) ++counts[band][transition][code];
                        }
                    }
                }
                for (unsigned band = 0; band < 8; ++band)
                {
                    assert(counts[band][0][0] == total && counts[band][3][0] == total);
                    assert(counts[band][1][2] == 30 && counts[band][1][1] == 0);
                    assert(counts[band][2][1] == 18 && counts[band][2][2] == 0);
                }
                make_phase(base, 0);
                base[0] |= 1u;
                assert(!epd_bw_scroll_save_phase(&plan, 0, base));
                make_phase(base, 0);
                base[0x0f] = 0xff000003u;
                assert(!epd_bw_scroll_save_phase(&plan, 0, base));
                make_phase(base, 0);
                assert(!epd_bw_scroll_save_phase(&plan, plan.frames, base));
            }
    epd_bw_scroll_t invalid = {0};
    assert(epd_bw_scroll_frames(&invalid) == 0);
    invalid.frames = 65;
    assert(epd_bw_scroll_frames(&invalid) == 0);
    invalid.frames = 44;
    invalid.gap = 17;
    assert(epd_bw_scroll_frames(&invalid) == 0);
    assert(!epd_bw_scroll_fill_lut(guarded + 1, &invalid, 0));
}

static void zero_gap_equivalence(void)
{
    const size_t count = 1216u * 684u;
    uint8_t *encoded = malloc(count), *original = malloc(count);
    assert(encoded && original);
    static const uint8_t states[4] = {0x00, 0x0f, 0xf0, 0xff};
    for (size_t i = 0; i < count; ++i) original[i] = encoded[i] = states[(i ^ (i / 1216)) & 3];
    assert(epd_bw_scroll_encode(encoded, 1216, 684));
    epd_bw_scroll_t plan = {0};
    plan.frames = 44;
    uint32_t base[256], lut[256];
    for (unsigned frame = 0; frame < 44; ++frame)
    {
        make_phase(base, frame);
        assert(epd_bw_scroll_save_phase(&plan, frame, base));
    }
    for (unsigned frame = 0; frame < 44; ++frame)
    {
        make_phase(base, frame);
        assert(epd_bw_scroll_fill_lut(lut, &plan, frame));
        for (size_t pixel = 0; pixel < count; ++pixel)
            assert(lut[encoded[pixel]] == base[original[pixel]]);
    }
    epd_bw_scroll_decode(encoded, count);
    assert(memcmp(encoded, original, count) == 0);
    free(original);
    free(encoded);
}

int main(void)
{
    assert(sizeof(epd_bw_scroll_t) <= 80); /* target's static SRAM is tight */
    quantization();
    geometry(1216, 684);
    geometry(13, 17);
    schedule();
    zero_gap_equivalence();
    puts("PASS: quantization, native/uneven strip geometry, exact shadow restore, rejection without mutation");
    puts("PASS: 16 timing plans, unchanged/idle neutral codes, pulse preservation, LUT bounds");
    puts("PASS: zero-gap output equals baseline for all 831744 pixels across 44 frames");
    return 0;
}
