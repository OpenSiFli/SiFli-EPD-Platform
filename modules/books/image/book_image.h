#ifndef BOOK_IMAGE_H
#define BOOK_IMAGE_H
#include "../formats/book_codec.h"

/* Output is a tightly packed, PSRAM-owned L8 image with 16 gray levels.
 * The caller frees *gray with epd_app_free and retains ownership of stream. */
int book_image_decode(book_stream_t *stream, unsigned max_width, unsigned max_height,
                      bool (*cancelled)(void *), void *cancel_user,
                      uint8_t **gray, unsigned *width, unsigned *height,
                      char *error, size_t error_size);
#endif
