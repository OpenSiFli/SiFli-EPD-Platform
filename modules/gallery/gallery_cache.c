#include "storage_file.h"
#include "storage.h"
#include "epd_memory.h"
#include "gallery.h"
#include <dfs_posix.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

#define CACHE_MAGIC 0x34434745u
#define CACHE_VERSION 1u
#define CACHE_SLOTS 2u

typedef struct
{
    uint32_t magic, version;
    uint32_t file_size, source_crc;
    uint32_t source_width, source_height, width, height;
    uint32_t pixels_crc;
    char source[STORAGE_PATH_MAX];
} cache_header_t;

static bool cache_path(char *path, size_t size, const char *source)
{
    char name[24];
    unsigned slot = crc32(0, (const Bytef *)source, strlen(source)) % CACHE_SLOTS;
    snprintf(name, sizeof(name), "image%u.g4", slot);
    return storage_app_path(path, size, "gallery", STORAGE_APP_CACHE, name);
}

static bool cache_read(int fd, const gallery_job_t *job, void *data, size_t size)
{
    uint8_t *out = data;
    while (size)
    {
        if (gallery_cancelled(job->serial)) return false;
        size_t amount = size > 32768 ? 32768 : size;
        storage_lock();
        int n = read(fd, out, amount);
        storage_unlock();
        if (n <= 0) return false;
        size -= n;
        out += n;
    }
    return true;
}

bool gallery_cache_load(gallery_decoder_t *decoder, gallery_result_t *result)
{
    char path[STORAGE_PATH_MAX];
    if (!cache_path(path, sizeof(path), gallery_decoder_job(decoder)->path)) return false;
    storage_lock();
    int fd = storage_file_recover(path) ? open(path, O_RDONLY) : -1;
    storage_unlock();
    if (fd < 0) return false;
    cache_header_t header;
    uint8_t *packed = NULL;
    bool ok = cache_read(fd, gallery_decoder_job(decoder), &header, sizeof(header));
    unsigned w = 0, h = 0;
    if (ok)
    {
        ok = header.magic == CACHE_MAGIC && header.version == CACHE_VERSION &&
             header.file_size == decoder->file_size &&
             memchr(header.source, 0, sizeof(header.source)) && !strcmp(header.source, gallery_decoder_job(decoder)->path) &&
             header.source_width && header.source_width <= GALLERY_MAX_SIDE &&
             header.source_height && header.source_height <= GALLERY_MAX_SIDE &&
             (uint64_t)header.source_width * header.source_height <= GALLERY_MAX_PIXELS;
    }
    if (ok)
    {
        gallery_fit(header.source_width, header.source_height, &w, &h, true);
        ok = header.width == w && header.height == h;
    }
    size_t bytes = (w * h + 1u) / 2u;
    if (ok)
    {
        struct stat st;
        storage_lock();
        ok = fstat(fd, &st) == 0 && (size_t)st.st_size == sizeof(header) + bytes;
        storage_unlock();
    }
    /* A path/size match is insufficient on LittleFS or after changing TF cards. */
    if (ok) ok = gallery_fingerprint(decoder) && header.source_crc == decoder->content_crc;
    if (ok)
    {
        packed = epd_app_alloc(bytes, EPD_APP_PSRAM);
        ok = packed && cache_read(fd, gallery_decoder_job(decoder), packed, bytes) &&
             crc32(0, packed, bytes) == header.pixels_crc;
    }
    storage_lock();
    close(fd);
    storage_unlock();
    if (ok) ok = gallery_image_alloc(result, w, h);
    if (ok)
    {
        for (unsigned y = 0; y < h; ++y)
        {
            if (gallery_cancelled(gallery_decoder_job(decoder)->serial)) { ok = false; break; }
            uint16_t *row = (uint16_t *)(result->image.data + y * result->image.header.stride);
            for (unsigned x = 0; x < w; ++x)
            {
                unsigned i = y * w + x;
                row[x] = gallery_gray16_colors[(packed[i / 2] >> ((i & 1) ? 0 : 4)) & 15];
            }
        }
    }
    epd_app_free(packed);
    if (!ok)
    {
        epd_app_free(result->pixels);
        result->pixels = NULL;
        memset(&result->image, 0, sizeof(result->image));
        return false;
    }
    decoder->source_width = header.source_width;
    decoder->source_height = header.source_height;
    decoder->width = w;
    decoder->height = h;
    decoder->cache_hit = true;
    result->source_width = header.source_width;
    result->source_height = header.source_height;
    gallery_image_clean(result);
    return true;
}

void *gallery_cache_pack(const gallery_decoder_t *decoder, const gallery_result_t *result, size_t *size)
{
    if (decoder->cache_hit || decoder->io_error || decoder->crc_end != decoder->file_size) return NULL;
    unsigned w = result->image.header.w, h = result->image.header.h;
    size_t bytes = (w * h + 1u) / 2u;
    cache_header_t *header = epd_app_alloc(sizeof(*header) + bytes, EPD_APP_PSRAM);
    if (!header) return NULL;
    memset(header, 0, sizeof(*header) + bytes);
    header->magic = CACHE_MAGIC;
    header->version = CACHE_VERSION;
    header->file_size = decoder->file_size;
    header->source_crc = decoder->content_crc;
    header->source_width = decoder->source_width;
    header->source_height = decoder->source_height;
    header->width = w;
    header->height = h;
    snprintf(header->source, sizeof(header->source), "%s", gallery_decoder_job(decoder)->path);
    uint8_t *packed = (uint8_t *)(header + 1);
    for (unsigned y = 0; y < h; ++y)
    {
        if (gallery_cancelled(gallery_decoder_job(decoder)->serial)) { epd_app_free(header); return NULL; }
        const uint16_t *row = (const uint16_t *)(result->image.data + y * result->image.header.stride);
        for (unsigned x = 0; x < w; ++x)
        {
            unsigned i = y * w + x;
            packed[i / 2] |= (row[x] >> 12) << ((i & 1) ? 0 : 4);
        }
    }
    header->pixels_crc = crc32(0, packed, bytes);
    *size = sizeof(*header) + bytes;
    return header;
}

void gallery_cache_save(const gallery_job_t *job, const void *data, size_t size)
{
    char path[STORAGE_PATH_MAX];
    if (!data || gallery_cancelled(job->serial) || !cache_path(path, sizeof(path), job->path)) return;
    /* This is optional and runs after publication; failure must not affect the image. */
    (void)storage_file_replace(path, data, size);
}
