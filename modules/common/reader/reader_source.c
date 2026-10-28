/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service.h"
#include "reader_internal.h"
#include "reader_compute_client.h"
#include "reader_resource.h"
#include "../../books/formats/book_internal.h"

static int plain_text(book_stream_t *stream, const book_sink_t *sink)
{
    char text[4096 + 4];
    size_t used = 0;
    bool first = true;
    for (;;)
    {
        int result = bf_cancel(sink);
        if (result != BOOK_OK) return result;
        int n = stream->read(stream->user, text + used, sizeof(text) - used);
        if (n < 0) return n;
        size_t bytes = used + (size_t)n, begin = 0, end = bytes;
        /* Stable record boundaries must not depend on short filesystem reads. */
        if (n && bytes < sizeof(text)) { used = bytes; continue; }
        if (first && bytes >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) begin = 3;
        first = false;
        /* Retain an incomplete UTF-8 codepoint between stream reads. */
        if (bytes > begin)
        {
            size_t lead = bytes - 1;
            while (lead > begin && ((unsigned char)text[lead] & 0xc0) == 0x80) --lead;
            unsigned char c = (unsigned char)text[lead];
            unsigned width = c < 0x80 ? 1 : c >= 0xc2 && c < 0xe0 ? 2 :
                             c < 0xf0 && c >= 0xe0 ? 3 : c <= 0xf4 && c >= 0xf0 ? 4 : 0;
            if (!width) return BOOK_UNSUPPORTED;
            if (bytes - lead < width) end = lead;
        }
        if (end > begin)
        {
            result = bf_text(sink, text + begin, end - begin, BOOK_STYLE_NORMAL);
            if (result != BOOK_OK) return result;
        }
        used = bytes - end;
        if (!n) return used ? BOOK_UNSUPPORTED : BOOK_OK;
        if (used) memmove(text, text + end, used);
    }
}

typedef struct {
    book_stream_t *source;
    const book_sink_t *sink;
} chapter_stream_t;
static int chapter_read(void *user, void *buffer, size_t length)
{
    chapter_stream_t *input = user;
    if (length > INT_MAX) length = INT_MAX;
    size_t used = 0;
    while (used < length)
    {
        int result = bf_cancel(input->sink);
        if (result != BOOK_OK) return result;
        int n = input->source->read(input->source->user, (char *)buffer + used, length - used);
        if (n < 0) return n;
        if (!n) break;
        if ((size_t)n > length - used) return BOOK_IO_ERROR;
        used += n;
    }
    return (int)used;
}

typedef struct {
    const document_source_t *source;
    const book_sink_t *sink;
    bool *cacheable;
} chapter_resources_t;
static int chapter_resource_open(void *user, const char *base, const char *reference, book_stream_t *stream)
{
    chapter_resources_t *context = user;
    const book_resources_t *resources = &context->source->resources;
    memset(stream, 0, sizeof(*stream));
    int result = bf_cancel(context->sink);
    if (result != BOOK_OK) return result;
    result = resources->open ? resources->open(resources->user, base, reference, stream) : BOOK_IO_ERROR;
    if (result != BOOK_OK)
    {
        if (stream->close) stream->close(stream->user);
        memset(stream, 0, sizeof(*stream));
    }
    if (result == BOOK_IO_ERROR || result == BOOK_ERROR || result == BOOK_UNSUPPORTED)
    {
        int cancelled = bf_cancel(context->sink);
        if (cancelled != BOOK_OK) return cancelled;
        *context->cacheable = false;
        return READER_RESOURCE_MISSING;
    }
    return result;
}
static int chapter_convert(const document_source_t *source, const book_sink_t *sink,
                           const book_allocator_t *allocator, char *error, size_t size, bool *cacheable)
{
    (void)error; (void)size;
    int result = book_markup_begin(allocator);
    if (result != BOOK_OK) return result;
    book_stream_t stream = {0};
    chapter_resources_t context = {source, sink, cacheable};
    book_resources_t resources = {&context, chapter_resource_open};
    result = bf_cancel(sink);
    if (result == BOOK_OK) result = bf_file(source->path, &stream);
    if (result == BOOK_OK)
    {
        /* SAX text boundaries remain stable across short underlying reads. */
        chapter_stream_t input = {&stream, sink};
        book_stream_t complete_reads = {.user = &input, .size = stream.size, .read = chapter_read};
        result = source->format == DOCUMENT_XHTML ?
            book_markup_convert(sink, &complete_reads, source->path, &resources) :
            plain_text(&stream, sink);
    }
    result = bf_finish(&stream, result);
    book_markup_end();
    return result;
}

bool epd_reader_open(const reader_options_t *options)
{
    epd_reader_close();
    if (!options) { epd_reader_set_error_code(READER_ERROR_INPUT); return false; }
    char error[96] = {0};
    document_t *document = NULL;
    if (options->kind == READER_SOURCE_TXT || options->kind == READER_SOURCE_XHTML)
    {
        document_source_t source = {
            .app_id = options->app_id, .cache_path = options->cache_path,
            .identity = options->content_identity, .path = options->path, .title = options->title,
            .format = options->kind == READER_SOURCE_TXT ? DOCUMENT_TXT : DOCUMENT_XHTML,
            .resources = options->resources, .cancelled = options->cancelled,
            .release_user = options->release_user, .user = options->user, .convert = chapter_convert
        };
        document = document_open_source(&source, error, sizeof(error));
    }
    if (!document)
    {
        epd_reader_set_error_code((options->kind == READER_SOURCE_TXT || options->kind == READER_SOURCE_XHTML) ?
                document_open_error() : READER_ERROR_TYPE);
        if (options->release_user) options->release_user(options->user);
        return false;
    }
    return epd_reader_attach(document, options->title, &options->restore, options->save, NULL, options->user);
}

bool epd_reader_worker_step(epd_service_t *service)
{
    if (reader_compute_worker_step(service)) return true;
    return document_worker_step(service);
}
void epd_reader_worker_shutdown(void)
{
    reader_compute_shutdown();
    document_worker_shutdown();
}
void epd_reader_worker(epd_service_t *service)
{
    while (!epd_service_cancelled(service))
        if (!epd_reader_worker_step(service)) epd_service_wait(service, RT_WAITING_FOREVER);
    epd_reader_worker_shutdown();
}
