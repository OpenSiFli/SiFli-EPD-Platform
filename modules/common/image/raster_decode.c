#include "storage.h"
#include "epd_memory.h"
#include "raster.h"
#include <dfs_posix.h>
#include <string.h>
#include <stdio.h>

void raster_fit(unsigned sw, unsigned sh, unsigned max_width, unsigned max_height,
                unsigned *width, unsigned *height, bool enlarge)
{
    if (!sw || !sh || !max_width || !max_height)
    { *width = *height = 0; return; }
    uint64_t w = max_width, h = (uint64_t)sh * w / sw;
    if (h > max_height) { h = max_height; w = (uint64_t)sw * h / sh; }
    if (!enlarge && w > sw) { w = sw; h = sh; }
    *width = w ? w : 1;
    *height = h ? h : 1;
}

void raster_target_size(const raster_decoder_t *decoder, unsigned sw, unsigned sh,
                        unsigned *width, unsigned *height)
{
    if (!decoder->cover || !sw || !sh || !decoder->max_width || !decoder->max_height)
    {
        raster_fit(sw, sh, decoder->max_width, decoder->max_height, width, height, false);
        return;
    }
    uint64_t w, h;
    if ((uint64_t)sw * decoder->max_height >= (uint64_t)sh * decoder->max_width)
    {
        h = decoder->max_height;
        w = ((uint64_t)sw * h + sh - 1) / sh;
    }
    else
    {
        w = decoder->max_width;
        h = ((uint64_t)sh * w + sw - 1) / sw;
    }
    /* Upsampling belongs to the final crop, not to the decoder's area average. */
    if (w >= sw || h >= sh) { w = sw; h = sh; }
    *width = (unsigned)w;
    *height = (unsigned)h;
}

static bool sink_init(raster_decoder_t *decoder, unsigned width, unsigned height,
                      bool ordered, unsigned band_rows)
{
    if (!width || !height)
    { strcpy(decoder->error, "图片尺寸无效"); return false; }
    if (!decoder->source_width)
    {
        decoder->source_width = width;
        decoder->source_height = height;
    }
    decoder->sample_width = width;
    decoder->sample_height = height;
    decoder->ordered_rows = ordered;
    decoder->banded_rows = band_rows != 0;
    decoder->sum_first_row = 0;
    raster_target_size(decoder, decoder->source_width, decoder->source_height,
                       &decoder->width, &decoder->height);
    if (!decoder->width || !decoder->height || width < decoder->width || height < decoder->height)
    { strcpy(decoder->error, "图片尺寸无效"); return false; }
    if (decoder->width - 1u > UINT16_MAX)
    { strcpy(decoder->error, "图片尺寸计算溢出"); return false; }
    /* Keep the normal 32-bit accumulator; use 64 bits only when a downsampled
     * pixel can contain too many source pixels for a rounded 32-bit sum. */
    uint64_t columns = ((uint64_t)width + decoder->width - 1) / decoder->width;
    uint64_t rows = ((uint64_t)height + decoder->height - 1) / decoder->height;
    uint64_t samples = columns * rows;
    decoder->wide_sum = samples > UINT32_MAX / 256u;
    decoder->sum_rows = ordered ? 1 : (band_rows ? band_rows : decoder->height);
    uint64_t pixels = (uint64_t)decoder->width * decoder->height;
    uint64_t sum_bytes = (uint64_t)decoder->sum_rows * decoder->width *
                         (decoder->wide_sum ? sizeof(uint64_t) : sizeof(uint32_t));
    uint64_t map_bytes = (uint64_t)width * sizeof(*decoder->x_map);
    if (samples > UINT64_MAX / 256u ||
        pixels > SIZE_MAX || sum_bytes > SIZE_MAX || map_bytes > SIZE_MAX)
    { strcpy(decoder->error, "图片尺寸计算溢出"); return false; }
    decoder->sum = raster_fast_alloc(decoder, sum_bytes);
    decoder->x_map = raster_fast_alloc(decoder, map_bytes);
    if (ordered || band_rows) decoder->gray = epd_app_alloc(pixels, EPD_APP_PSRAM);
    if (!decoder->sum || !decoder->x_map || ((ordered || band_rows) && !decoder->gray))
    { strcpy(decoder->error, "图片内存不足"); return false; }
    memset(decoder->sum, 0, sum_bytes);
    unsigned target = 0;
    uint64_t remainder = 0;
    for (unsigned x = 0; x < width; ++x)
    {
        decoder->x_map[x] = target;
        remainder += decoder->width;
        if (remainder >= width) { remainder -= width; ++target; }
    }
    return true;
}

bool raster_sink_init(raster_decoder_t *decoder, unsigned width, unsigned height, bool ordered)
{
    return sink_init(decoder, width, height, ordered, 0);
}

bool raster_sink_init_banded(raster_decoder_t *decoder, unsigned width, unsigned height, unsigned band_rows)
{
    return sink_init(decoder, width, height, false, band_rows);
}

static unsigned luminance(const uint8_t *pixel, raster_pixel_format_t format, const uint8_t *palette)
{
    unsigned value, alpha = 255;
    if (format == RASTER_INDEX8) return palette[pixel[0]];
    if (format == RASTER_GRAY8) return pixel[0];
    if (format == RASTER_GRAY_ALPHA8)
    {
        value = pixel[0];
        alpha = pixel[1];
    }
    else
    {
        unsigned r, g, b;
        if (format == RASTER_RGB565)
        {
            unsigned packed = pixel[0] | ((unsigned)pixel[1] << 8);
            r = (packed >> 11) & 31;
            g = (packed >> 5) & 63;
            b = packed & 31;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
        }
        else { r = pixel[0]; g = pixel[1]; b = pixel[2]; }
        value = (r * 77 + g * 150 + b * 29 + 128) >> 8;
        if (format == RASTER_RGBA8888) alpha = pixel[3];
    }
    return alpha == 255 ? value : (value * alpha + 255 * (255 - alpha) + 127) / 255;
}

static unsigned source_boundary(unsigned position, unsigned source_size, unsigned target_size)
{
    return ((uint64_t)position * source_size + target_size - 1) / target_size;
}

static size_t sum_row_words(const raster_decoder_t *decoder)
{
    return (size_t)decoder->width * (decoder->wide_sum ? 2u : 1u);
}

static void finish_row(raster_decoder_t *decoder, unsigned y, const uint32_t *sum)
{
    unsigned w = decoder->width, h = decoder->height;
    unsigned sw = decoder->sample_width, sh = decoder->sample_height;
    unsigned rows = source_boundary(y + 1, sh, h) - source_boundary(y, sh, h);
    uint8_t *gray = decoder->gray + (size_t)y * w;
    unsigned left = 0;
    unsigned column_step = sw / w, column_remainder = sw % w, remainder = w - 1;
    for (unsigned x = 0; x < w; ++x)
    {
        unsigned right = left + column_step;
        remainder += column_remainder;
        if (remainder >= w) { remainder -= w; ++right; }
        if (decoder->wide_sum)
        {
            uint64_t count = (uint64_t)rows * (right - left);
            uint64_t value;
            /* The application heaps guarantee word alignment, not 8 bytes. */
            memcpy(&value, sum + (size_t)x * 2, sizeof(value));
            gray[x] = (value + count / 2) / count;
        }
        else
        {
            unsigned count = rows * (right - left);
            gray[x] = (sum[x] + count / 2) / count;
        }
        left = right;
    }
}

void raster_sink_end_band(raster_decoder_t *decoder, unsigned source_end)
{
    /* All columns before source_end are complete. Keep the unfinished row
     * across MCU bands so the area average and its rounding stay unchanged. */
    unsigned end = (uint64_t)source_end * decoder->height / decoder->sample_height;
    while (decoder->sum_first_row < end)
    {
        unsigned y = decoder->sum_first_row++;
        uint32_t *row = decoder->sum + (y % decoder->sum_rows) * sum_row_words(decoder);
        finish_row(decoder, y, row);
        memset(row, 0, sum_row_words(decoder) * sizeof(*row));
    }
}

void raster_sink_row(raster_decoder_t *decoder, unsigned x, unsigned y, unsigned step,
                      const uint8_t *pixels, unsigned count, raster_pixel_format_t format)
{
    static const uint8_t bytes[] = {1, 2, 1, 3, 4, 2};
    unsigned pixel_bytes = bytes[format];
    unsigned dy = (uint64_t)y * decoder->height / decoder->sample_height;
    uint32_t *row = decoder->sum + (dy % decoder->sum_rows) * sum_row_words(decoder);
    if (decoder->wide_sum)
    {
        while (count)
        {
            unsigned dx = decoder->x_map[x];
            uint64_t sum = 0;
            do
            {
                sum += luminance(pixels, format, decoder->palette);
                pixels += pixel_bytes;
                x += step;
                --count;
            } while (count && decoder->x_map[x] == dx);
            uint64_t value;
            uint32_t *bin = row + (size_t)dx * 2;
            memcpy(&value, bin, sizeof(value));
            value += sum;
            memcpy(bin, &value, sizeof(value));
        }
    }
    else
    {
        while (count)
        {
            unsigned dx = decoder->x_map[x];
            unsigned sum = 0;
            /* Accumulate a source run in registers before updating its output bin. */
            do
            {
                sum += luminance(pixels, format, decoder->palette);
                pixels += pixel_bytes;
                x += step;
                --count;
            } while (count && decoder->x_map[x] == dx);
            row[dx] += sum;
        }
    }
    if (decoder->ordered_rows &&
        y + 1 == source_boundary(dy + 1, decoder->sample_height, decoder->height))
    {
        finish_row(decoder, dy, row);
        memset(row, 0, sum_row_words(decoder) * sizeof(*row));
    }
}

static bool finish_gray(raster_decoder_t *decoder)
{
    unsigned w = decoder->width, h = decoder->height;
    if (!decoder->gray) decoder->gray = epd_app_alloc((size_t)w * h, EPD_APP_PSRAM);
    if (!decoder->gray) { strcpy(decoder->error, "图片内存不足"); return false; }
    for (unsigned y = 0; !decoder->ordered_rows && !decoder->banded_rows && y < h; ++y)
    {
        if (raster_cancelled(decoder)) return false;
        finish_row(decoder, y, decoder->sum + y * sum_row_words(decoder));
    }
    raster_fast_free(decoder, decoder->sum);
    decoder->sum = NULL;
    raster_fast_free(decoder, decoder->x_map);
    decoder->x_map = NULL;
    return true;
}

void raster_decoder_clear(raster_decoder_t *decoder)
{
    if (decoder->fd >= 0)
    {
        storage_lock();
        close(decoder->fd);
        storage_unlock();
    }
    epd_app_free(decoder->gray);
    raster_fast_free(decoder, decoder->sum);
    raster_fast_free(decoder, decoder->x_map);
    epd_app_free(decoder->read_memory);
    memset(decoder, 0, sizeof(*decoder));
    decoder->fd = -1;
}

bool raster_decode(raster_decoder_t *decoder)
{
    uint8_t signature[8];
    bool ok = !decoder->io_error && raster_seek(decoder, 0) &&
              raster_read(decoder, signature, sizeof(signature)) && raster_seek(decoder, 0);
    if (ok)
    {
        if (!memcmp(signature, "\x89PNG\r\n\x1a\n", 8)) ok = raster_decode_png(decoder);
        else if (signature[0] == 0xff && signature[1] == 0xd8) ok = raster_decode_jpeg(decoder);
        else { strcpy(decoder->error, "不支持的图片格式"); ok = false; }
    }
    if (ok && !raster_cancelled(decoder)) ok = finish_gray(decoder);
    else ok = false;
    if (decoder->io_error) strcpy(decoder->error, "图片读取中断或文件不完整");
    return ok;
}
