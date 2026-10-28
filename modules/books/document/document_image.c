/* SPDX-License-Identifier: Apache-2.0 */
#include "document.h"
#include "../image/book_image.h"

int document_image_decode(book_stream_t *stream, bool (*cancelled)(void *), void *user,
                          uint8_t **gray, unsigned *width, unsigned *height)
{
    return book_image_decode(stream, DOCUMENT_IMAGE_WIDTH, DOCUMENT_IMAGE_HEIGHT,
                             cancelled, user, gray, width, height, NULL, 0);
}
