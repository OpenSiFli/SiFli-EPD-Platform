/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_SVG_H
#define EPD_SVG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *pixels;
    unsigned width, height;
} epd_svg_bitmap_t;

/* Return tightly packed gray8 pixels allocated with epd_app_alloc. Ownership
 * passes to the wrapper, including pixels returned together with an error.
 * The callback must fit within both limits without enlarging the source. */
typedef int (*epd_svg_load_image_t)(void *user, const char *url,
                                  unsigned max_width, unsigned max_height,
                                  epd_svg_bitmap_t *out);

/* Render after LVGL initialization, from thread context. The result is tightly
 * packed gray8 over white; release out->pixels with epd_app_free. On failure
 * out is empty. Limits: 256 KiB input, 684 x 1216 output, 384 x 608 per image.
 * SVG dimensions determine the aspect ratio; fitting never enlarges it.
 * Both callbacks receive user. Cancellation is checked between SDK stages.
 * Status: 0 success, -1 invalid, -2 cancelled, -3 no memory,
 *         -4 unsupported, -5 I/O failure. Callbacks must not reenter render. */
int epd_svg_render(const char *svg, size_t length,
                   unsigned max_width, unsigned max_height,
                   epd_svg_load_image_t load, void *user,
                   bool (*cancel)(void *), epd_svg_bitmap_t *out);

#ifdef __cplusplus
}
#endif
#endif
