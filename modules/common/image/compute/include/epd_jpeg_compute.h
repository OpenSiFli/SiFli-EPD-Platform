/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_JPEG_COMPUTE_H
#define EPD_JPEG_COMPUTE_H
#include "epd_compute_protocol.h"

#define EPD_JPEG_CAPABILITY "epd/tjpgd"
#define EPD_JPEG_VERSION 1u
#define EPD_JPEG_POOL_BYTES (20u * 1024u)
#define EPD_JPEG_MCU_PIXELS 256u

typedef struct
{
    epd_compute_ref_t input, pool;
    uint32_t scale;
} epd_jpeg_open_t;

typedef struct
{
    int32_t code;
    uint32_t width, height, msy;
    uint32_t blocks, bytes, state;
} epd_jpeg_result_t;

typedef struct
{
    uint32_t left, top, right, bottom, bytes;
    /* Packed grayscale pixels follow. Next block is rounded to 4 bytes. */
} epd_jpeg_block_t;

#endif
