/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_memory.h"
#include "raster.h"
#include <string.h>

/* Use the SDK decoder for JPEG scans unsupported by the streaming decoder. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 2048
#define STBI_MALLOC(n) epd_app_alloc((n), EPD_APP_PSRAM)
#define STBI_REALLOC(p, n) epd_app_realloc((p), (n), EPD_APP_PSRAM)
#define STBI_FREE(p) epd_app_free(p)
#define STBI_ASSERT(x) RT_ASSERT(x)
#include "stb_image.h"

bool raster_jpeg_extended(raster_decoder_t *decoder, uint8_t *prefix, size_t length)
{
    uint8_t *pixels = NULL;
    bool ok = false;
    if (decoder->file_size > RASTER_JPEG_INPUT_LIMIT)
    { strcpy(decoder->error, "扩展 JPEG 压缩数据超过 2 MB"); goto done; }
    if (!prefix)
    { strcpy(decoder->error, "图片内存不足"); goto done; }
    if (length > decoder->file_size || decoder->position != length)
    { strcpy(decoder->error, "JPEG 数据不完整"); goto done; }
    uint8_t *data = epd_app_realloc(prefix, decoder->file_size, EPD_APP_PSRAM);
    if (!data) { strcpy(decoder->error, "图片内存不足"); goto done; }
    prefix = data;
    if (!raster_read(decoder, prefix + length, decoder->file_size - length) ||
        raster_cancelled(decoder)) goto done;
    /* Require an end marker; stb_image may otherwise return partial pixels
     * for a truncated scan. JPEG permits bytes following the end marker. */
    size_t end = decoder->file_size;
    while (end > 1 && !(prefix[end - 2] == 0xff && prefix[end - 1] == 0xd9)) --end;
    if (end < 2) { strcpy(decoder->error, "JPEG 数据不完整"); goto done; }
    int width, height, components;
    if (!stbi_info_from_memory(prefix, (int)end, &width, &height, &components))
    { strcpy(decoder->error, "无法读取 JPEG 图像信息"); goto done; }
    if (width <= 0 || height <= 0 || (unsigned)width > 1048576u / (unsigned)height)
    { strcpy(decoder->error, "扩展 JPEG 图像超过 1048576 像素"); goto done; }
    pixels = stbi_load_from_memory(prefix, (int)end, &width, &height, &components, 1);
    if (!pixels) { strcpy(decoder->error, "JPEG 解码失败或图片内存不足"); goto done; }
    epd_app_free(prefix);
    prefix = NULL;
    if (raster_cancelled(decoder)) goto done;
    ok = raster_sink_init(decoder, width, height, true);
    for (unsigned y = 0; ok && y < (unsigned)height; ++y)
    {
        if (raster_cancelled(decoder)) { ok = false; break; }
        raster_sink_row(decoder, 0, y, 1, pixels + (size_t)y * width, width, RASTER_GRAY8);
    }
done:
    stbi_image_free(pixels);
    epd_app_free(prefix);
    return ok;
}
