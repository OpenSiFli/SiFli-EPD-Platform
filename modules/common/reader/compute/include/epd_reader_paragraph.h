/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READER_PARAGRAPH_H
#define EPD_READER_PARAGRAPH_H
#include <stdint.h>

/* Affine lengths retain relative units until the reading geometry is known.
 * All three coefficients use 1/256 units; percent=256 represents 100%. */
typedef struct { int32_t px, em, percent; } epd_paragraph_length_t;
enum { EPD_PARAGRAPH_ACTIVE = 1, EPD_PARAGRAPH_BEGIN = 2, EPD_PARAGRAPH_END = 4 };
enum { EPD_ALIGN_DEFAULT, EPD_ALIGN_LEFT, EPD_ALIGN_CENTER, EPD_ALIGN_RIGHT, EPD_ALIGN_JUSTIFY };
typedef struct {
    epd_paragraph_length_t left, right, indent, line_height, margin, padding;
    uint32_t flags, align;
} epd_paragraph_style_t;

static inline int32_t epd_paragraph_pixels(epd_paragraph_length_t v, uint32_t font, uint32_t span)
{
    int64_t n = (int64_t)v.px + (int64_t)v.em * font + (int64_t)v.percent * span;
    n /= 256;
    return n > 65535 ? 65535 : n < -65535 ? -65535 : (int32_t)n;
}
#endif
