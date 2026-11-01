#include "image_internal.h"
/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <stdlib.h>

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
#define STBI_MALLOC(n) ri_work_alloc((n))
#define STBI_REALLOC(p, n) ri_work_realloc((p), (n))
#define STBI_FREE(p) ri_work_free(p)
/* Keep invariant checks without linking stderr assertion formatting. */
#define STBI_ASSERT(x) do { if (!(x)) abort(); } while (0)
#include "stb_image.h"

typedef struct {
    ri_raster_decoder_t *decoder;
    uint8_t *prefix;
    size_t length, position;
    bool measuring, end_marker;
    uint8_t previous;
} jpeg_stream;

static int stream_read(void *user, char *data, int size)
{
    jpeg_stream *s = user;
    size_t n = (size_t)size;
    if (ri_raster_cancelled(s->decoder)) return 0;
    if (s->position < s->length)
    {
        if (n > s->length - s->position) n = s->length - s->position;
        memcpy(data, s->prefix + s->position, n);
    }
    else
    {
        if (s->measuring)
        {
            if (n > RI_RASTER_JPEG_INPUT_LIMIT - s->length) return 0;
            uint8_t *p = ri_work_realloc(s->prefix, s->length + n);
            if (!p) return 0;
            s->prefix = p;
        }
        n = ri_raster_read_some(s->decoder, data, n);
        if (s->measuring) { memcpy(s->prefix + s->length, data, n); s->length += n; }
    }
    /* Preserve the existing end-marker check while bytes pass through the
     * bounded callback buffer; no second file scan or input-sized allocation. */
    if (n)
    {
        const uint8_t *p = (const uint8_t *)data, *end = p + n;
        if (s->previous == 0xff && *p == 0xd9) s->end_marker = true;
        while ((p = memchr(p, 0xff, (size_t)(end - p))) != NULL)
        { if (++p < end && *p == 0xd9) s->end_marker = true; }
        s->previous = (uint8_t)data[n - 1];
    }
    s->position += n;
    return (int)n;
}
static void stream_skip(void *user, int size)
{
    char discard[256];
    while (size > 0)
    {
        int n = stream_read(user, discard, size > (int)sizeof(discard) ? (int)sizeof(discard) : size);
        if (!n) break;
        size -= n;
    }
}
static int stream_eof(void *user)
{
    jpeg_stream *s = user;
    return ri_raster_cancelled(s->decoder) ||
        (s->position >= s->length && s->decoder->position >= s->decoder->file_size);
}

static uint32_t exif_number(const uint8_t *p, unsigned bytes, bool little)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < bytes; ++i) n = (n << 8) | p[little ? bytes - 1 - i : i];
    return n;
}

/* Only the bounded JPEGInterchangeFormat in TIFF IFD1 is an image preview.
 * Do not treat arbitrary SOI bytes in metadata as a replacement image. */
static const uint8_t *exif_thumbnail(const uint8_t *p, size_t size, size_t *length)
{
    if (size < 14 || memcmp(p, "Exif\0\0", 6)) return NULL;
    p += 6; size -= 6;
    bool little = p[0] == 'I' && p[1] == 'I';
    if ((!little && (p[0] != 'M' || p[1] != 'M')) || exif_number(p + 2, 2, little) != 42) return NULL;
    size_t offset = exif_number(p + 4, 4, little);
    size_t image = 0, bytes = 0;
    for (unsigned ifd = 0; ifd < 2; ++ifd)
    {
        if (offset < 8 || offset > size - 2) return NULL;
        unsigned count = exif_number(p + offset, 2, little);
        offset += 2;
        if (count > (size - offset) / 12) return NULL;
        if (ifd)
            for (unsigned i = 0; i < count; ++i)
            {
                const uint8_t *entry = p + offset + i * 12;
                unsigned tag = exif_number(entry, 2, little);
                if ((tag == 0x201 || tag == 0x202) &&
                    exif_number(entry + 2, 2, little) == 4 && exif_number(entry + 4, 4, little) == 1)
                {
                    size_t value = exif_number(entry + 8, 4, little);
                    if (tag == 0x201) image = value; else bytes = value;
                }
            }
        offset += count * 12;
        if (!ifd) {
            if (size - offset < 4) return NULL;
            offset = exif_number(p + offset, 4, little);
        }
    }
    if (bytes < 4 || image >= size || bytes > size - image ||
        p[image] != 0xff || p[image + 1] != 0xd8 ||
        p[image + bytes - 2] != 0xff || p[image + bytes - 1] != 0xd9) return NULL;
    *length = bytes;
    return p + image;
}

static const uint8_t *budget_thumbnail(const jpeg_stream *input, unsigned w,
                                      unsigned h, unsigned components, size_t *length)
{
    const ri_io *io = input->decoder->io;
    if (!io->available) return NULL;
    /* Conservative padded planes, progressive coefficients, gray output and
     * codec/row overhead. This is selection before allocation, not an OOM retry. */
    /* The caller has checked <=2048 per axis and <=1048576 source pixels. */
    uint32_t need = ((w + 31u) & ~31u) * ((h + 31u) & ~31u) * components * 3u +
                    w * h * 2u + 64u * 1024u;
    if (need <= io->available(io->user)) return NULL;
    const uint8_t *thumbnail = NULL;
    size_t at = 2;
    while (at + 4 <= input->length && input->prefix[at] == 0xff)
    {
        unsigned marker = input->prefix[at + 1];
        if (marker == 0xff) { ++at; continue; }
        if (marker == 0xda || marker == 0xd9) break;
        size_t bytes = ((unsigned)input->prefix[at + 2] << 8) | input->prefix[at + 3];
        if (bytes < 2 || bytes > input->length - at - 2) return NULL;
        if (marker == 0xc2) return thumbnail;
        if (marker == 0xe1 && !thumbnail)
            thumbnail = exif_thumbnail(input->prefix + at + 4, bytes - 2, length);
        at += bytes + 2;
    }
    return NULL;
}

/* Require the main JPEG's end marker, not an EOI inside EXIF. Read through
 * the caller stream so truncation, cancellation and ZIP CRC failures survive. */
static bool complete_jpeg(jpeg_stream *input, const stbi_io_callbacks *callbacks)
{
    stbi__context context;
    input->measuring = false; input->position = 0;
    stbi__start_callbacks(&context, (stbi_io_callbacks *)callbacks, input);
    if (stbi__get16be(&context) != 0xffd8) return false;
    bool scan = false;
    while (!stbi__at_eof(&context))
    {
        int marker = stbi__get8(&context);
        if (marker != 0xff) { if (scan) continue; return false; }
        do {
            if (stbi__at_eof(&context)) return false;
            marker = stbi__get8(&context);
        } while (marker == 0xff);
        if (!marker || (marker >= 0xd0 && marker <= 0xd7)) {
            if (scan) continue;
            return false;
        }
        if (marker == 0xd9) return scan;
        int bytes = stbi__get16be(&context);
        if (bytes < 2) return false;
        stbi__skip(&context, bytes - 2);
        if (marker == 0xda) scan = true;
    }
    return false;
}

bool ri_raster_jpeg_extended(ri_raster_decoder_t *decoder, uint8_t *prefix, size_t length)
{
    uint8_t *pixels = NULL;
    bool ok = false;
    jpeg_stream input = {decoder, prefix, length, 0, true, false, 0};
    const stbi_io_callbacks callbacks = {stream_read, stream_skip, stream_eof};
    if (!prefix) goto done;
    if (length > decoder->file_size || decoder->position != length) goto done;
    int width, height, components;
    if (!stbi_info_from_callbacks(&callbacks, &input, &width, &height, &components)) goto done;
    if (width <= 0 || height <= 0 || (unsigned)width > 1048576u / (unsigned)height) goto done;
    if (decoder->io->max_source_pixels && (uint64_t)width * height > decoder->io->max_source_pixels) goto done;
    size_t preview_bytes = 0;
    const uint8_t *preview = budget_thumbnail(&input, (unsigned)width, (unsigned)height,
                                             (unsigned)components, &preview_bytes);
    if (preview)
    {
        if (!complete_jpeg(&input, &callbacks)) goto done;
        ok = ri_raster_jpeg_memory(decoder, preview, preview_bytes);
        /* A damaged preview cannot turn a valid but over-budget original into
         * a successful missing-image marker. Allocator/cancel errors stay live. */
        if (!ok && !decoder->status) decoder->status = RI_NOMEM;
        goto done;
    }
    input.measuring = false; input.position = 0; input.end_marker = false; input.previous = 0;
    pixels = stbi_load_from_callbacks(&callbacks, &input, &width, &height, &components, 1);
    if (!pixels) goto done;
    if (!input.end_marker) goto done;
    ri_work_free(input.prefix);
    input.prefix = NULL;
    if (ri_raster_cancelled(decoder)) goto done;
    ok = ri_raster_sink_init(decoder, width, height, true);
    for (unsigned y = 0; ok && y < (unsigned)height; ++y)
    {
        if (ri_raster_cancelled(decoder)) { ok = false; break; }
        ri_raster_sink_row(decoder, 0, y, 1, pixels + (size_t)y * width, width, RI_RASTER_GRAY8);
    }
done:
    stbi_image_free(pixels);
    ri_work_free(input.prefix);
    return ok;
}
