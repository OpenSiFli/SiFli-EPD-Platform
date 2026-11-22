/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_GRAY_QUANTIZE_H
#define EPD_GRAY_QUANTIZE_H

/* Distribute intermediate shades spatially while retaining exact panel levels.
 * Thresholds form an 8x8 Bayer matrix with every rank represented once. */
static inline unsigned epd_gray4_dither(unsigned gray, unsigned x, unsigned y)
{
    static const unsigned char rank[8][8] = {
        { 0, 48, 12, 60,  3, 51, 15, 63},
        {32, 16, 44, 28, 35, 19, 47, 31},
        { 8, 56,  4, 52, 11, 59,  7, 55},
        {40, 24, 36, 20, 43, 27, 39, 23},
        { 2, 50, 14, 62,  1, 49, 13, 61},
        {34, 18, 46, 30, 33, 17, 45, 29},
        {10, 58,  6, 54,  9, 57,  5, 53},
        {42, 26, 38, 22, 41, 25, 37, 21}
    };
    unsigned level = gray / 17u;
    unsigned remainder = gray - level * 17u;
    return level + (remainder * 128u > (2u * rank[y & 7u][x & 7u] + 1u) * 17u);
}
#endif
