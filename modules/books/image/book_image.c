#include "book_image.h"
#include "../../common/image/raster.h"
#include <stdio.h>
#include <string.h>

typedef struct
{
    book_stream_t *stream;
    bool (*cancelled)(void *);
    void *cancel_user;
    uint64_t position;
} image_stream_t;

static bool cancelled(void *user)
{
    image_stream_t *input = user;
    return input->cancelled && input->cancelled(input->cancel_user);
}

static int read_at(void *user, size_t position, void *data, size_t size)
{
    image_stream_t *input = user;
    if (position != input->position)
    {
        if (!input->stream->seek || input->stream->seek(input->stream->user, position) != BOOK_OK)
            return BOOK_IO_ERROR;
        input->position = position;
    }
    size_t used = 0;
    while (used < size && !cancelled(user))
    {
        int n = input->stream->read(input->stream->user, (uint8_t *)data + used, size - used);
        if (n <= 0) return n < 0 ? n : (int)used;
        if ((size_t)n > size - used) return BOOK_IO_ERROR;
        used += n;
        input->position += n;
    }
    return cancelled(user) ? BOOK_CANCELLED : (int)used;
}

int book_image_decode(book_stream_t *stream, unsigned max_width, unsigned max_height,
                      bool (*cancel_cb)(void *), void *cancel_user,
                      uint8_t **gray, unsigned *width, unsigned *height,
                      char *error, size_t error_size)
{
    if (!stream || !stream->read || !gray || !width || !height ||
        !max_width || !max_height || max_width > 684 || max_height > 1216 ||
        stream->size > SIZE_MAX) return BOOK_UNSUPPORTED;
    *gray = NULL;
    *width = *height = 0;
    image_stream_t input = {stream, cancel_cb, cancel_user, 0};
    raster_decoder_t decoder = {.fd = -1, .context = &input, .cancel = cancelled,
        .read_at = read_at, .file_size = (size_t)stream->size,
        .forward_only = !stream->seek,
        .max_width = max_width, .max_height = max_height};
    bool ok = raster_io_open(&decoder) && raster_decode(&decoder);
    if (ok)
    {
        for (unsigned y = 0; y < decoder.height; ++y)
        {
            if (cancelled(&input)) { ok = false; break; }
            for (unsigned x = 0; x < decoder.width; ++x)
            {
                uint8_t *pixel = decoder.gray + y * decoder.width + x;
                *pixel = ((*pixel + 8u) / 17u) * 17u;
            }
        }
    }
    if (ok)
    {
        *gray = decoder.gray;
        *width = decoder.width;
        *height = decoder.height;
        decoder.gray = NULL;
    }
    else if (error && error_size)
        snprintf(error, error_size, "%s", decoder.error[0] ? decoder.error : "图片解码失败");
    bool stopped = cancelled(&input);
    raster_decoder_clear(&decoder);
    return ok ? BOOK_OK : stopped ? BOOK_CANCELLED : BOOK_ERROR;
}
