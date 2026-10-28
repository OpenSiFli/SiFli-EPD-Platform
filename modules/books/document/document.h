/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_H
#define BOOK_DOCUMENT_H
#include "app_service.h"
#include "../bookshelf.h"
#include "../../common/reader/reader_error.h"
#include "../formats/formats.h"
#include "platform/epd_app.h"

#define DOCUMENT_TEXT_LIMIT 1024u
#define DOCUMENT_IMAGE_WIDTH 620u
#define DOCUMENT_IMAGE_HEIGHT 1040u
#define DOCUMENT_TITLE_MAX BOOKSHELF_NAME_MAX
/* Bump whenever converter ordering, normalization, image sizing or spool
 * record construction changes. Positions refer to this content model. */
#define DOCUMENT_CONVERSION_VERSION 3u

typedef book_format_t document_format_t;
#define DOCUMENT_UNKNOWN BOOK_FORMAT_UNKNOWN
#define DOCUMENT_TXT BOOK_FORMAT_TXT
#define DOCUMENT_MD BOOK_FORMAT_MARKDOWN
#define DOCUMENT_EPUB BOOK_FORMAT_EPUB
#define DOCUMENT_SPLIT_EPUB BOOK_FORMAT_SPLIT_EPUB
#define DOCUMENT_MOBI BOOK_FORMAT_MOBI
#define DOCUMENT_XHTML ((document_format_t)6)
typedef enum { DOCUMENT_TEXT = 1, DOCUMENT_PARAGRAPH, DOCUMENT_ANCHOR,
               DOCUMENT_LINK, DOCUMENT_IMAGE } document_record_kind_t;
typedef struct { uint32_t kind, length, style, link; } document_record_t;
typedef struct { uint32_t width, height; } document_image_t;
typedef struct document document_t;
typedef struct document_reader document_reader_t;
typedef struct document_image_request document_image_request_t;
typedef struct {
    uint32_t end, publication;
    bool complete;
    int result;
} document_progress_t;

typedef struct {
    /* Half-open spool byte ranges. previous_begin == begin at the first
     * chapter; next_end == end when no following chapter is published. */
    uint32_t begin, end, previous_begin, next_end;
    uint32_t publication;
    /* indexed: EPUB root anchors are available for this record. Other formats
     * and a pending/failed index use [0, published end). A known end is stable;
     * an unknown end must be queried again after publication/completion. */
    bool indexed, end_known, next_end_known;
} document_chapter_range_t;

/* Global payload budget, excluding slot metadata and per-reader I/O windows. */
#define DOCUMENT_CONTENT_CACHE_BUDGET (768u * 1024u)
/* Optional content/index allocations preserve this much free PSRAM. */
#define DOCUMENT_CONTENT_CACHE_RESERVE (1024u * 1024u)
typedef struct {
    uint32_t end, publication;
    bool complete, stopped;
    int result;
    reader_error_t error_code;
    char title[BOOKSHELF_NAME_MAX], error[96];
} document_status_t;
typedef struct {
    lv_image_dsc_t image;
    void *memory;
} document_bitmap_t;

typedef struct document_source document_source_t;
struct document_source {
    const char *app_id, *cache_path, *identity, *path, *title;
    document_format_t format;
    book_resources_t resources;
    bool (*cancelled)(void *user);
    void (*release_user)(void *user);
    void *user;
    int (*convert)(const document_source_t *source, const book_sink_t *sink,
                   const book_allocator_t *allocator, char *error, size_t size, bool *cacheable);
};
/* UI queueing only; copies source strings and callbacks. Ownership of user
 * transfers only on success. Final disposal calls release_user on the UI. */
document_t *document_open_source(const document_source_t *source, char *error, size_t size);
bool document_worker_step(epd_service_t *service);
void document_worker_shutdown(void);

document_format_t document_format(const char *path, bool directory);
const char *document_format_name(document_format_t format);
/* The caller holds storage_lock during directory discovery. */
bool document_split_identity(const char *path, uint32_t *size, uint32_t *signature);
void document_worker(epd_service_t *service);
void document_service_process(void);
void document_service_shutdown(void);
document_t *document_open(const bookshelf_item_t *file, char *error, size_t size);
/* UI-thread result of the most recent open attempt. */
reader_error_t document_open_error(void);
void document_close(document_t *document);
void document_status(document_t *document, document_status_t *status);
void document_progress(document_t *document, document_progress_t *progress);
/* Thread-safe, memory-only snapshot. record is a spool byte offset, not a
 * record ordinal. Returns false for an empty/unpublished offset or an error.
 * The document must remain alive throughout the call. */
bool document_chapter_range(document_t *document, uint32_t record,
                            document_chapter_range_t *range);
/* Optional content prefetch, chapter-index restoration and cache-touch work
 * start disabled. This memory-only switch preserves pending work; enabling
 * wakes the worker. An in-flight I/O slice may finish after disabling.
 * Conversion, target reads, range queries and cancellation remain available. */
void document_prefetch_enable(document_t *document, bool enabled);
/* Thread-safe queueing only. The document worker warms current/previous/next
 * chapter bytes from the spool, sharing one global content-cache budget.
 * Large/unindexed chapters use a bounded window around record; nearby hints
 * share aligned blocks. Ordinary reads refill only the selected window.
 * No decompression or synchronous whole-chapter I/O. */
void document_chapter_prefetch(document_t *document, uint32_t record);
/* Each cursor is owned by one execution context. Destroy before document_close. */
document_reader_t *document_reader_create(document_t *document);
void document_reader_destroy(document_reader_t *reader);
bool document_reader_read(document_reader_t *reader, uint32_t offset, void *data, size_t length);
bool document_reader_record(document_reader_t *reader, uint32_t offset, document_record_t *record);
/* Shared structural validation for both layout and pixel preparation. */
bool document_reader_image_info(document_reader_t *reader, uint32_t offset, document_image_t *image);
bool document_read(document_t *document, uint32_t offset, void *data, size_t length);
bool document_record(document_t *document, uint32_t offset, document_record_t *record);
/* UI-owned RGB565 output; its descriptor stays alive until the widget detaches. */
bool document_image_load(document_t *document, uint32_t offset, unsigned width,
                         unsigned height, document_bitmap_t *bitmap);
/* UI requests pixels; the existing document worker fills them. take returns
 * 0 while pending, 1 after transferring ownership, or -1 on failure.
 * Cancel every request, including a successfully transferred request. */
document_image_request_t *document_image_request(document_t *document, uint32_t offset,
                                                  unsigned width, unsigned height);
int document_image_take(document_image_request_t *request, document_bitmap_t *bitmap);
void document_image_cancel(document_image_request_t *request);
/* Require a successfully completed, persistent conversion; the opaque layout
 * blob may describe only a paginated prefix. Blobs contain no pointers.
 * Loads prefer queued snapshots, then disk, and return epd_app_free-owned
 * memory. Saves copy their input and may fail when memory/storage is unavailable. */
bool document_index_load(document_t *document, uint32_t layout, void **data, size_t *size);
bool document_index_save(document_t *document, uint32_t layout, const void *data, size_t size);
/* Worker writes are asynchronous; true means the copied data was queued.
 * A newer save replaces the same path's pending job, preserving in-flight I/O. */
bool document_save_async(const char *path, const void *header, size_t header_size,
                         const void *data, size_t size);
/* Copy the newest pending/in-flight save for path without filesystem I/O.
 * The epd_app_free-owned result includes header and payload. Callers validate
 * their own format, identity, layout and checksum. Media/session checks apply;
 * false leaves NULL/0 outputs. A snapshot does not confirm disk persistence. */
bool document_save_snapshot(const char *path, void **data, size_t *size);

/* Synchronous worker-only image conversion, with bounded source and target sizes. */
int document_image_decode(book_stream_t *stream, bool (*cancelled)(void *), void *user,
                          uint8_t **gray, unsigned *width, unsigned *height);
#endif
