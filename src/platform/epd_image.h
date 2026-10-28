#ifndef EPD_IMAGE_H
#define EPD_IMAGE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct
{
    unsigned width, height, stride;
    size_t work_size, y_size, u_size, v_size, buffer_size;
} epd_jpeg_layout_t;

/* Query the current HAL's AHB working/output sizes before allocating RAM1.
 * Planes start at buffer + work_size, followed by y_size and u_size bytes.
 * Only width x height pixels are valid; each Y row has stride bytes. */
bool epd_image_jpeg_layout(unsigned width, unsigned height, epd_jpeg_layout_t *layout);

/* Worker-thread, synchronous baseline YCbCr 4:2:0 decode under the shared GPU
 * lock. Input must be 32-byte aligned and padded; buffer must be 64-byte aligned
 * with layout->buffer_size bytes. Use an unmodified layout from the query above.
 * Returns false immediately if the GPU lock is busy. All hardware access has
 * stopped on return, including failure/cancellation. */
bool epd_image_jpeg_decode(const uint8_t *input, size_t length,
                           const epd_jpeg_layout_t *layout, void *buffer,
                           bool (*cancel)(void *context), void *context);
#endif
