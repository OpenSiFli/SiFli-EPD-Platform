#include "storage.h"
#include "epd_memory.h"
#include "gallery.h"
#include <dfs_posix.h>
#include <string.h>

const uint16_t gallery_gray16_colors[16] =
{
    0x0000, 0x1082, 0x2104, 0x3186, 0x4228, 0x52aa, 0x632c, 0x73ae,
    0x8c51, 0x9cd3, 0xad55, 0xbdd7, 0xce79, 0xdefb, 0xef7d, 0xffff
};

void gallery_fit(unsigned sw, unsigned sh, unsigned *width, unsigned *height, bool enlarge)
{
    raster_fit(sw, sh, GALLERY_WIDTH, GALLERY_HEIGHT, width, height, enlarge);
}

static bool decode_cancelled(void *context)
{
    return gallery_cancelled(((const gallery_job_t *)context)->serial);
}

bool gallery_decode(const gallery_job_t *job, gallery_decoder_t *decoder, gallery_result_t *result)
{
    decoder->context = (void *)job;
    decoder->path = job->path;
    decoder->cancel = decode_cancelled;
    decoder->max_width = GALLERY_WIDTH;
    decoder->max_height = GALLERY_HEIGHT;
    if (!raster_io_open(decoder)) { result->error_code = GALLERY_STATUS_OPEN; return false; }
    if (gallery_cache_load(decoder, result)) return true;
    bool ok = raster_decode(decoder) && gallery_fingerprint(decoder);
    storage_lock();
    close(decoder->fd);
    storage_unlock();
    decoder->fd = -1;
    return ok;
}

static bool allocation_cancelled(void *context)
{
    return gallery_cancelled(((gallery_result_t *)context)->serial);
}

bool gallery_image_alloc(gallery_result_t *result, unsigned width, unsigned height)
{
    if (!width || !height || width > GALLERY_WIDTH || height > GALLERY_HEIGHT) return false;
    unsigned stride = RT_ALIGN(width * 2, 4);
    size_t bytes = (size_t)stride * height + 31;
    result->pixels = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (!result->pixels && epd_memory_retry_wait(&result->memory_retry, bytes,
                                                allocation_cancelled, result))
        result->pixels = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (!result->pixels) return false;
    uint8_t *data = (uint8_t *)RT_ALIGN((uintptr_t)result->pixels, 32);
    memset(data, 0xff, stride * height);
    result->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    result->image.header.cf = LV_COLOR_FORMAT_RGB565;
    result->image.header.w = width;
    result->image.header.h = height;
    result->image.header.stride = stride;
    result->image.data = data;
    result->image.data_size = stride * height;
    return true;
}

void gallery_image_clean(const gallery_result_t *result)
{
    lv_draw_buf_t buffer = {.data = (uint8_t *)result->image.data, .data_size = result->image.data_size};
    epd_app_clean_draw_buffer(&buffer);
}

bool gallery_render(gallery_decoder_t *decoder, gallery_result_t *result, const gallery_job_t *job)
{
    unsigned w, h;
    gallery_fit(decoder->source_width, decoder->source_height, &w, &h, true);
    if (!gallery_image_alloc(result, w, h)) { result->error_code = GALLERY_STATUS_MEMORY; return false; }
    unsigned stride = result->image.header.stride;
    uint8_t *data = (uint8_t *)result->image.data;
    bool enlarge = w != decoder->width || h != decoder->height;
    uint32_t x_map[GALLERY_WIDTH];
    if (enlarge)
        for (unsigned x = 0; x < w; ++x)
            x_map[x] = w > 1 ? x * (decoder->width - 1) * 256 / (w - 1) : 0;
    for (unsigned y = 0; y < h; ++y)
    {
        if (gallery_cancelled(job->serial)) return false;
        uint16_t *row = (uint16_t *)(data + y * stride);
        if (!enlarge)
        {
            const uint8_t *gray = decoder->gray + y * w;
            for (unsigned x = 0; x < w; ++x) row[x] = gallery_gray16_colors[(gray[x] + 8) / 17];
        }
        else
        {
            /* Bilinear enlargement; reduction was averaged while decoding. */
            unsigned fy = h > 1 ? y * (decoder->height - 1) * 256 / (h - 1) : 0;
            unsigned sy = fy >> 8, ay = fy & 255;
            unsigned ny = sy + 1 < decoder->height ? sy + 1 : sy;
            const uint8_t *top = decoder->gray + sy * decoder->width;
            const uint8_t *bottom = decoder->gray + ny * decoder->width;
            for (unsigned x = 0; x < w; ++x)
            {
                unsigned sx = x_map[x] >> 8, ax = x_map[x] & 255;
                unsigned nx = sx + 1 < decoder->width ? sx + 1 : sx;
                unsigned a = top[sx] * (256 - ax) + top[nx] * ax;
                unsigned b = bottom[sx] * (256 - ax) + bottom[nx] * ax;
                unsigned value = (a * (256 - ay) + b * ay + 32768) >> 16;
                row[x] = gallery_gray16_colors[(value + 8) / 17];
            }
        }
    }
    result->source_width = decoder->source_width;
    result->source_height = decoder->source_height;
    gallery_image_clean(result);
    return true;
}
