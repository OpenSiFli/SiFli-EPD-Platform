#ifndef BOOK_SVG_H
#define BOOK_SVG_H
#include "book_image.h"

/* Synchronous SVG decoding. Resource streams are closed before returning;
 * the caller releases the resulting L8 pixels with epd_app_free. */
int book_svg_decode(const char *xml, size_t length, const char *base,
                    const book_resources_t *resources, unsigned max_width, unsigned max_height,
                    bool (*cancelled)(void *), void *user,
                    uint8_t **gray, unsigned *width, unsigned *height);
#endif
