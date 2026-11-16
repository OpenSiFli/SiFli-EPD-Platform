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

typedef struct { const gallery_job_t *job; int fd; uint32_t session; } decode_io_t;

static bool decode_cancelled(void *context)
{
    const decode_io_t *io = context;
    return gallery_cancelled(io->job->serial) || !storage_session_valid(STORAGE_SD, io->session);
}

static int decode_read_at(void *context, size_t position, void *data, size_t size)
{
    decode_io_t *io = context;
    if (decode_cancelled(io)) return -1;
    storage_lock();
    bool positioned = storage_session_valid(STORAGE_SD, io->session) &&
                      lseek(io->fd, (off_t)position, SEEK_SET) == (off_t)position;
    storage_unlock();
    return positioned && gallery_file_io(io->fd, data, size, false, io->job->serial, io->session) ? (int)size : -1;
}

bool gallery_decode(const gallery_job_t *job, gallery_decoder_t *decoder, gallery_result_t *result)
{
    decode_io_t io = {.job = job, .session = storage_card_session(), .fd = -1};
    io.fd = gallery_file_open(job->path, O_RDONLY, io.session);
    if (io.fd < 0) { result->error_code = GALLERY_STATUS_OPEN; return false; }
    struct stat st;
    storage_lock();
    bool opened = fstat(io.fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 8;
    storage_unlock();
    decoder->context = &io;
    decoder->path = job->path;
    decoder->cancel = decode_cancelled;
    decoder->read_at = decode_read_at;
    decoder->file_size = opened ? st.st_size : 0;
    decoder->max_width = job->kind == GALLERY_THUMB ? GALLERY_THUMB_SIZE : GALLERY_WIDTH;
    decoder->max_height = job->kind == GALLERY_THUMB ? GALLERY_THUMB_SIZE : GALLERY_HEIGHT;
    decoder->background = job->kind == GALLERY_THUMB || job->speculative;
    bool ok = opened && raster_io_open(decoder);
    if (!ok) result->error_code = GALLERY_STATUS_OPEN;
    if (ok) ok = raster_decode(decoder);
    ok = gallery_file_close(io.fd) && ok;
    decoder->context = NULL;
    decoder->cancel = NULL;
    decoder->read_at = NULL;
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
    raster_fit(decoder->source_width, decoder->source_height,
               decoder->max_width, decoder->max_height, &w, &h, true);
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
