#include "image_internal.h"
#include "epd_tjpgd.h"
#include <string.h>

#define JPEG_POOL_BYTES (20u * 1024u)

typedef struct {
    ri_raster_decoder_t *decoder;
    uint8_t *prefix;
    size_t length, capacity;
    bool preparing;
    const uint8_t *memory;
    size_t remaining;
} jpeg_input_t;


static size_t jpeg_input(JDEC *jpeg, uint8_t *data, size_t bytes)
{
    jpeg_input_t *input = jpeg->device;
    if (input->memory)
    {
        if (bytes > input->remaining) bytes = input->remaining;
        if (data) memcpy(data, input->memory, bytes);
        input->memory += bytes; input->remaining -= bytes;
        return bytes;
    }
    if (!input->preparing) return ri_raster_read_some(input->decoder, data, bytes);
    /* Retain the header for a second decoder even when the ZIP stream cannot
     * seek backwards. This includes metadata skipped by TJpgDec. */
    if (bytes > RI_RASTER_JPEG_INPUT_LIMIT - input->length)
        input->preparing = false;
    else if (input->length + bytes > input->capacity)
    {
        size_t capacity = ((input->length + bytes + 4095u) & ~(size_t)4095u);
        uint8_t *prefix = ri_work_realloc(input->prefix, capacity);
        if (prefix) { input->prefix = prefix; input->capacity = capacity; }
        else input->preparing = false;
    }
    if (!input->preparing)
    {
        ri_work_free(input->prefix);
        input->prefix = NULL;
        return ri_raster_read_some(input->decoder, data, bytes);
    }
    size_t count = ri_raster_read_some(input->decoder, input->prefix + input->length, bytes);
    if (data) memcpy(data, input->prefix + input->length, count);
    input->length += count;
    return count;
}

static int jpeg_output(JDEC *jpeg, void *data, JRECT *rectangle)
{
    ri_raster_decoder_t *decoder = ((jpeg_input_t *)jpeg->device)->decoder;
    if (ri_raster_cancelled(decoder)) return 0;
    const uint8_t *pixel = data;
    unsigned width = rectangle->right - rectangle->left + 1;
    for (unsigned y = rectangle->top; y <= rectangle->bottom; ++y)
    {
        ri_raster_sink_row(decoder, rectangle->left, y, 1, pixel, width, RI_RASTER_GRAY8);
        pixel += width;
    }
    if (rectangle->right + 1u == decoder->sample_width)
        ri_raster_sink_end_band(decoder, rectangle->bottom + 1u);
    return 1;
}

static bool decode_jpeg(ri_raster_decoder_t *decoder, const uint8_t *memory, size_t size)
{
    JDEC *jpeg = ri_raster_fast_alloc(decoder, sizeof(*jpeg));
    void *pool = ri_raster_fast_alloc(decoder, JPEG_POOL_BYTES);
    if (!jpeg || !pool)
    {
        ri_raster_fast_free(decoder, jpeg);
        ri_raster_fast_free(decoder, pool);
        return false;
    }
    jpeg_input_t input = {.decoder = decoder, .preparing = true, .memory = memory, .remaining = size};
    JRESULT code = jd_prepare(jpeg, jpeg_input, pool, JPEG_POOL_BYTES, &input);
    if (!memory && (code == JDR_FMT2 || code == JDR_FMT3))
    {
        ri_raster_fast_free(decoder, jpeg);
        ri_raster_fast_free(decoder, pool);
        return ri_raster_jpeg_extended(decoder, input.prefix, input.length);
    }
    input.preparing = false;
    ri_work_free(input.prefix);
    input.prefix = NULL;
    bool ok = code == JDR_OK;
    if (ok)
    {
        if (decoder->io->max_source_pixels &&
            (uint64_t)jpeg->width * jpeg->height > decoder->io->max_source_pixels) {
            ri_raster_fast_free(decoder, jpeg); ri_raster_fast_free(decoder, pool); return false;
        }
        decoder->source_width = jpeg->width;
        decoder->source_height = jpeg->height;
        {
            unsigned w, h, scale = 0;
            ri_raster_target_size(decoder, jpeg->width, jpeg->height, &w, &h);
            while (scale < 3 && (jpeg->width >> (scale + 1)) >= w && (jpeg->height >> (scale + 1)) >= h)
                ++scale;
            ok = ri_raster_sink_init_banded(decoder, jpeg->width >> scale, jpeg->height >> scale,
                                        (jpeg->msy * 8u) >> scale);
            if (ok)
            {
                code = jd_decomp(jpeg, jpeg_output, scale);
                ok = code == JDR_OK;
            }
        }
    }
    ri_raster_fast_free(decoder, jpeg);
    ri_raster_fast_free(decoder, pool);
    return ok;
}

bool ri_raster_decode_jpeg(ri_raster_decoder_t *decoder)
{ return decode_jpeg(decoder, NULL, 0); }

bool ri_raster_jpeg_memory(ri_raster_decoder_t *decoder, const uint8_t *memory, size_t size)
{ return decode_jpeg(decoder, memory, size); }
