#include "epd_memory.h"
#include "raster.h"
#include "epd_image.h"
#include "epd_tjpgd.h"
#include <string.h>

#define JPEG_POOL_BYTES (20u * 1024u)

typedef struct {
    raster_decoder_t *decoder;
    uint8_t *prefix;
    size_t length, capacity;
    bool preparing;
} jpeg_input_t;

#ifdef EPD_IMAGE_COMPUTE
int raster_jpeg_remote(raster_decoder_t *decoder, unsigned scale);
#endif

static size_t jpeg_input(JDEC *jpeg, uint8_t *data, size_t bytes)
{
    jpeg_input_t *input = jpeg->device;
    if (!input->preparing) return raster_read_some(input->decoder, data, bytes);
    /* Retain the header for a second decoder even when the ZIP stream cannot
     * seek backwards. This includes metadata skipped by TJpgDec. */
    if (bytes > RASTER_JPEG_INPUT_LIMIT - input->length)
        input->preparing = false;
    else if (input->length + bytes > input->capacity)
    {
        size_t capacity = RT_ALIGN(input->length + bytes, 4096);
        uint8_t *prefix = epd_app_realloc(input->prefix, capacity, EPD_APP_PSRAM);
        if (prefix) { input->prefix = prefix; input->capacity = capacity; }
        else input->preparing = false;
    }
    if (!input->preparing)
    {
        epd_app_free(input->prefix);
        input->prefix = NULL;
        return raster_read_some(input->decoder, data, bytes);
    }
    size_t count = raster_read_some(input->decoder, input->prefix + input->length, bytes);
    if (data) memcpy(data, input->prefix + input->length, count);
    input->length += count;
    return count;
}

static int jpeg_output(JDEC *jpeg, void *data, JRECT *rectangle)
{
    raster_decoder_t *decoder = ((jpeg_input_t *)jpeg->device)->decoder;
    if (raster_cancelled(decoder)) return 0;
    const uint8_t *pixel = data;
    unsigned width = rectangle->right - rectangle->left + 1;
    for (unsigned y = rectangle->top; y <= rectangle->bottom; ++y)
    {
        raster_sink_row(decoder, rectangle->left, y, 1, pixel, width, RASTER_GRAY8);
        pixel += width;
    }
    if (rectangle->right + 1u == decoder->sample_width)
        raster_sink_end_band(decoder, rectangle->bottom + 1u);
    return 1;
}

/* 0: use software, 1: complete, -1: cancelled or allocation/input failed. */
static int hardware_decode(raster_decoder_t *decoder, const JDEC *jpeg)
{
    unsigned w = jpeg->width, h = jpeg->height;
    /* Match the SF32LB57 AHB decoder's YCbCr 4:2:0 path. Other baseline
     * sampling formats remain supported by the streaming software decoder. */
    if (decoder->forward_only || jpeg->ncomp != 3 || jpeg->msx != 2 || jpeg->msy != 2) return 0;
    epd_jpeg_layout_t layout;
    if (!epd_image_jpeg_layout(w, h, &layout)) return 0;
    size_t position = decoder->position;
    size_t length = decoder->file_size;
    if (length > SIZE_MAX - 63u) return 0;
    void *output_memory = epd_app_alloc(layout.buffer_size + 63, EPD_APP_PSRAM);
    void *input_memory = output_memory ? epd_app_alloc(RT_ALIGN(length, 32) + 31, EPD_APP_PSRAM) : NULL;
    if (!input_memory)
    {
        epd_app_free(output_memory);
        rt_kprintf("[image] JPEGD RAM1 allocation failed, use streaming decode\n");
        return 0;
    }
    uint8_t *input = (uint8_t *)RT_ALIGN((uintptr_t)input_memory, 32);
    uint8_t *output = (uint8_t *)RT_ALIGN((uintptr_t)output_memory, 64);
    bool ok = raster_seek(decoder, 0) && raster_read(decoder, input, length);
    if (ok && !raster_cancelled(decoder))
        ok = epd_image_jpeg_decode(input, length, &layout, output, decoder->cancel, decoder->context);
    else ok = false;
    epd_app_free(input_memory);
    int result = 0;
    if (ok)
    {
        result = raster_sink_init(decoder, w, h, true) ? 1 : 0;
        if (result == 0)
        {
            /* Releasing the full hardware frame may let streaming decode fit. */
            raster_fast_free(decoder, decoder->sum);
            raster_fast_free(decoder, decoder->x_map);
            epd_app_free(decoder->gray);
            decoder->sum = NULL;
            decoder->x_map = NULL;
            decoder->gray = NULL;
            decoder->error[0] = '\0';
        }
        const uint8_t *gray = output + layout.work_size;
        for (unsigned y = 0; result == 1 && y < h; ++y)
        {
            if (raster_cancelled(decoder)) { result = -1; break; }
            raster_sink_row(decoder, 0, y, 1, gray + y * layout.stride, w, RASTER_GRAY8);
        }
    }
    epd_app_free(output_memory);
    raster_seek(decoder, position);
    if (decoder->io_error || raster_cancelled(decoder)) result = -1;
    return result;
}

bool raster_decode_jpeg(raster_decoder_t *decoder)
{
    JDEC *jpeg = raster_fast_alloc(decoder, sizeof(*jpeg));
    void *pool = raster_fast_alloc(decoder, JPEG_POOL_BYTES);
    if (!jpeg || !pool)
    {
        raster_fast_free(decoder, jpeg);
        raster_fast_free(decoder, pool);
        strcpy(decoder->error, "图片内存不足");
        return false;
    }
    jpeg_input_t input = {.decoder = decoder, .preparing = true};
    JRESULT code = jd_prepare(jpeg, jpeg_input, pool, JPEG_POOL_BYTES, &input);
    if (code == JDR_FMT2 || code == JDR_FMT3)
    {
        raster_fast_free(decoder, jpeg);
        raster_fast_free(decoder, pool);
        return raster_jpeg_extended(decoder, input.prefix, input.length);
    }
    input.preparing = false;
    epd_app_free(input.prefix);
    input.prefix = NULL;
    bool ok = code == JDR_OK;
    if (ok)
    {
        decoder->source_width = jpeg->width;
        decoder->source_height = jpeg->height;
        int hardware = hardware_decode(decoder, jpeg);
        ok = hardware >= 0;
        if (hardware == 0)
        {
            unsigned w, h, scale = 0;
            raster_target_size(decoder, jpeg->width, jpeg->height, &w, &h);
            while (scale < 3 && (jpeg->width >> (scale + 1)) >= w && (jpeg->height >> (scale + 1)) >= h)
                ++scale;
#ifdef EPD_IMAGE_COMPUTE
            int remote = raster_jpeg_remote(decoder, scale);
            if (remote != 0)
            {
                ok = remote > 0;
                goto finished;
            }
#endif
            ok = raster_sink_init_banded(decoder, jpeg->width >> scale, jpeg->height >> scale,
                                        (jpeg->msy * 8u) >> scale);
            if (ok)
            {
                code = jd_decomp(jpeg, jpeg_output, scale);
                ok = code == JDR_OK;
            }
        }
    }
#ifdef EPD_IMAGE_COMPUTE
finished:
#endif
    if (!ok && !decoder->error[0])
        strcpy(decoder->error, code == JDR_FMT2 || code == JDR_FMT3 ?
               "不支持此 JPEG 编码" : "JPEG 解码失败");
    raster_fast_free(decoder, jpeg);
    raster_fast_free(decoder, pool);
    return ok;
}
