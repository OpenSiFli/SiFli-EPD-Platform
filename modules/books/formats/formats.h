/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_FORMATS_H
#define BOOK_FORMATS_H
#include "book_codec.h"
#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum book_format
    {
        BOOK_FORMAT_UNKNOWN = -1,
        BOOK_FORMAT_TXT = 0,
        BOOK_FORMAT_MARKDOWN,
        BOOK_FORMAT_EPUB,
        BOOK_FORMAT_SPLIT_EPUB,
        /* Keep persisted reading-position format IDs stable. */
        BOOK_FORMAT_MOBI = 5
    } book_format_t;

    book_format_t book_format_detect(const char *path, bool is_directory);
    const char *book_format_name(book_format_t format);
    int book_format_convert(const char *path, const book_sink_t *sink,
                            const book_allocator_t *allocator, char *error, size_t error_capacity);

    typedef enum {
        BOOK_FILE_OPENED, BOOK_FILE_MISSING, BOOK_FILE_UNAVAILABLE
    } book_file_event_t;
    typedef void (*book_file_observer_t)(void *user, const char *path, book_file_event_t event);
    /* Worker-only, synchronous, non-nesting scope. The callback borrows path,
     * runs after the raw I/O lock is released, and must not use bf_fopen.
     * It cannot change conversion status or output. End with the same user. */
    bool book_file_observer_begin(book_file_observer_t observer, void *user);
    void book_file_observer_end(void *user);

    /* All conversions and markup scopes run serially on one document worker.
     * convert() owns its scope. Private backends using sink->markup must instead
     * call begin(), run their conversion, then end() on every return path.
     * Nested begin() fails. No allocator callback survives end(). */
    int book_markup_begin(const book_allocator_t *allocator);
    void book_markup_end(void);
    int book_markup_convert(const book_sink_t *sink, book_stream_t *stream, const char *base,
                            const book_resources_t *resources);

/* Streams and callback strings are borrowed only for each synchronous call.
 * The image callback consumes the stream but must not close it. The converter
 * drains and closes it, checking ZIP length/CRC even after partial consumption.
 * anchor(id,NULL,0) defines a location; anchor(id,title,level) adds a TOC entry.
 * TOC declarations may precede the corresponding content location definition.
 */
#ifdef __cplusplus
}
#endif
#endif
