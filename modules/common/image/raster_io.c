#include "storage.h"
#include "epd_memory.h"
#include "raster.h"
#include <dfs_posix.h>
#include <string.h>
#include <zlib.h>

#define READ_BYTES (32u * 1024u)
#define FAST_BUDGET (64u * 1024u)
#define SRAM_RESERVE (40u * 1024u)

typedef struct
{
    size_t size;
    bool sram;
} fast_header_t;

static void *fast_alloc(raster_decoder_t *decoder, size_t bytes, bool priority)
{
    if (!bytes || bytes > SIZE_MAX - sizeof(fast_header_t)) return NULL;
    size_t allocation = bytes + sizeof(fast_header_t);
    fast_header_t *memory = NULL;
    /* Background codec workspaces share PSRAM with the decoded image. */
    bool try_sram = !decoder->background && priority;
    if (!decoder->background && !priority)
    {
        rt_uint32_t total, used, peak;
        rt_memory_info(&total, &used, &peak);
        try_sram = decoder->fast_used <= FAST_BUDGET &&
                   allocation <= FAST_BUDGET - decoder->fast_used && total >= used &&
                   total - used > SRAM_RESERVE && allocation < total - used - SRAM_RESERVE;
    }
    if (try_sram)
        memory = epd_app_alloc(allocation, EPD_APP_SRAM);
    bool sram = memory != NULL;
    if (!memory) memory = epd_app_alloc(allocation, EPD_APP_PSRAM);
    if (!memory) return NULL;
    memory->size = allocation;
    memory->sram = sram;
    if (sram)
        decoder->fast_used += allocation;
    return memory + 1;
}

void *raster_fast_alloc(raster_decoder_t *decoder, size_t bytes)
{
    return fast_alloc(decoder, bytes, false);
}

void *raster_fast_alloc_priority(raster_decoder_t *decoder, size_t bytes)
{
    return fast_alloc(decoder, bytes, true);
}

void raster_fast_free(raster_decoder_t *decoder, void *pointer)
{
    if (!pointer) return;
    fast_header_t *memory = (fast_header_t *)pointer - 1;
    if (memory->sram) decoder->fast_used -= memory->size;
    epd_app_free(memory);
}

bool raster_io_open(raster_decoder_t *decoder)
{
    struct stat st;
    if (!decoder->read_at)
    {
        storage_lock();
        decoder->fd = storage_path_available(decoder->path) ? open(decoder->path, O_RDONLY) : -1;
        bool ok = decoder->fd >= 0 && fstat(decoder->fd, &st) == 0 &&
                  S_ISREG(st.st_mode) && st.st_size >= 8;
        storage_unlock();
        if (!ok) { strcpy(decoder->error, "无法打开图片"); return false; }
        decoder->file_size = st.st_size;
    }
    if (decoder->file_size < 8) { strcpy(decoder->error, "图片文件不完整"); return false; }
    decoder->read_memory = epd_app_alloc(READ_BYTES + 31, EPD_APP_PSRAM);
    if (!decoder->read_memory) { strcpy(decoder->error, "图片内存不足"); return false; }
    decoder->read_buffer = (uint8_t *)RT_ALIGN((uintptr_t)decoder->read_memory, 32);
    return true;
}

bool raster_seek(raster_decoder_t *decoder, size_t position)
{
    if (position > decoder->file_size) return false;
    /* The descriptor itself is repositioned only when refilling. */
    decoder->position = position;
    return true;
}

size_t raster_read_some(raster_decoder_t *decoder, void *data, size_t bytes)
{
    uint8_t *out = data;
    size_t copied = 0;
    while (bytes && decoder->position < decoder->file_size)
    {
        if (raster_cancelled(decoder)) break;
        size_t offset = decoder->position - decoder->read_start;
        if (decoder->position < decoder->read_start || offset >= decoder->read_count)
        {
            size_t start = decoder->position & ~(READ_BYTES - 1);
            size_t amount = decoder->file_size - start;
            if (amount > READ_BYTES) amount = READ_BYTES;
            int n = -1;
            if (decoder->read_at)
            {
                n = decoder->read_at(decoder->context, start, decoder->read_buffer, amount);
            }
            else
            {
                storage_lock();
                if (storage_path_available(decoder->path) &&
                    lseek(decoder->fd, start, SEEK_SET) == (off_t)start)
                {
                    n = 0;
                    while ((size_t)n < amount && !raster_cancelled(decoder))
                    {
                        int received = storage_path_available(decoder->path) ?
                            read(decoder->fd, decoder->read_buffer + n, amount - n) : -1;
                        if (received <= 0) { decoder->io_error = true; break; }
                        n += received;
                    }
                }
                storage_unlock();
            }
            if (raster_cancelled(decoder)) break;
            if (n <= 0 || (size_t)n != amount) { decoder->io_error = true; break; }
            decoder->read_start = start;
            decoder->read_count = n;
            /* Hash each source byte once, also when a decoder rewinds its header. */
            if (start <= decoder->crc_end && start + n > decoder->crc_end)
            {
                size_t skip = decoder->crc_end - start;
                decoder->content_crc = crc32(decoder->content_crc, decoder->read_buffer + skip, n - skip);
                decoder->crc_end = start + n;
            }
            offset = decoder->position - start;
            if (offset >= (size_t)n) { decoder->io_error = true; break; }
        }
        size_t amount = decoder->read_count - offset;
        if (amount > bytes) amount = bytes;
        if (out) { memcpy(out, decoder->read_buffer + offset, amount); out += amount; }
        decoder->position += amount;
        copied += amount;
        bytes -= amount;
    }
    return copied;
}

bool raster_read(raster_decoder_t *decoder, void *data, size_t bytes)
{
    bool ok = raster_read_some(decoder, data, bytes) == bytes;
    if (!ok && !raster_cancelled(decoder)) decoder->io_error = true;
    return ok;
}

bool raster_fingerprint(raster_decoder_t *decoder)
{
    size_t position = decoder->position;
    raster_seek(decoder, decoder->crc_end);
    size_t bytes = decoder->file_size - decoder->crc_end;
    bool ok = raster_read(decoder, NULL, bytes) && decoder->crc_end == decoder->file_size;
    raster_seek(decoder, position);
    return ok;
}

bool raster_cancelled(const raster_decoder_t *decoder)
{
    return decoder->cancel && decoder->cancel(decoder->context);
}
