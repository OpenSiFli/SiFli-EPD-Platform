/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_H
#define BOOK_DOCUMENT_H
#include "app_service.h"
#include "../bookshelf.h"
#include "../../common/reader/reader_error.h"
#include "../formats/formats.h"
#include "platform/epd_app.h"
#include "epd_compute.h"

#define DOCUMENT_TEXT_LIMIT 1024u
#define DOCUMENT_IMAGE_WIDTH 620u
#define DOCUMENT_IMAGE_HEIGHT 1040u
#define DOCUMENT_IMAGE_CACHE_BYTES (2u * 1024u * 1024u)
#define DOCUMENT_TITLE_MAX BOOKSHELF_NAME_MAX
/* Bump whenever converter ordering, normalization, image sizing or spool
 * record construction changes. Positions refer to this content model. */
#define DOCUMENT_CONVERSION_VERSION 14u

/* Retained document buffers belong to the worker cache, across work credits. */
void *document_allocate(size_t bytes);

static inline bool document_history_version(uint32_t version)
{
    return version >= 9u && version <= DOCUMENT_CONVERSION_VERSION;
}

/* File bytes for TXT; normalized chapter UTF-8 bytes for EPUB. Markdown/MOBI
 * use SOURCE_SPOOL: offset is a record boundary and skip_utf8 is its text byte.
 * Spool coordinates require the same source identity and conversion version. */
#define DOCUMENT_SOURCE_SPOOL 3u
typedef struct { uint32_t unit, chapter, offset, skip_utf8; } document_anchor_t;

typedef book_format_t document_format_t;
#define DOCUMENT_UNKNOWN BOOK_FORMAT_UNKNOWN
#define DOCUMENT_TXT BOOK_FORMAT_TXT
#define DOCUMENT_MD BOOK_FORMAT_MARKDOWN
#define DOCUMENT_EPUB BOOK_FORMAT_EPUB
#define DOCUMENT_SPLIT_EPUB BOOK_FORMAT_SPLIT_EPUB
#define DOCUMENT_MOBI BOOK_FORMAT_MOBI
#define DOCUMENT_XHTML ((document_format_t)6)
typedef enum { DOCUMENT_TEXT = 1, DOCUMENT_PARAGRAPH, DOCUMENT_ANCHOR,
               DOCUMENT_LINK, DOCUMENT_IMAGE, DOCUMENT_METADATA } document_record_kind_t;
typedef struct {
    uint32_t kind, length, style, link;
    epd_paragraph_style_t paragraph;
} document_record_t;
typedef struct {
    uint32_t width, height;
    book_picture_layout_t layout;
} document_image_t;
typedef struct document document_t;
typedef struct document_reader document_reader_t;
typedef struct document_image_request document_image_request_t;
typedef struct document_record_request document_record_request_t;
typedef struct document_note_request document_note_request_t;
#define DOCUMENT_NOTE_BYTES 4096u
/* link_record = rendered run.link - 1. No I/O on the UI thread.
 * take: 0 pending, 1 copied, -1 failed; always cancel after use.
 * Output is NUL-terminated UTF-8; truncated includes a small caller buffer. */
document_note_request_t *document_note_request(document_t *document, uint32_t link_record);
int document_note_take(document_note_request_t *request, char *text, size_t capacity, bool *truncated);
void document_note_cancel(document_note_request_t *request);

typedef struct {
    document_record_t record;
    document_image_t image;
    char text[DOCUMENT_TEXT_LIMIT + 1];
} document_record_data_t;

/* Foreground reads use resident bytes when available; only the worker performs file I/O.
 * take: 0 pending, 1 copied, -1 failed. A consumed request can be advanced or released. */
document_record_request_t *document_record_request(document_t *document, uint32_t offset, bool foreground);
int document_record_take(document_record_request_t *request, document_record_data_t *out);
enum { DOCUMENT_NEXT_NONE, DOCUMENT_NEXT_QUEUED, DOCUMENT_NEXT_READY };
int document_record_advance(document_record_request_t *request, uint32_t limit);
/* Inspect a failed take/seek_take before cancel: BOOK_NO_MEMORY is retryable;
 * BOOK_IO_ERROR reports invalid/unreadable data. Pending/success is BOOK_OK;
 * a null, released or cancelled request reports BOOK_CANCELLED. */
int document_record_result(document_record_request_t *request);
/* Find the record containing target without loading each skipped payload. */
document_record_request_t *document_record_seek_request(document_t *document, uint32_t start,
    uint32_t target, uint32_t end, bool foreground);
int document_record_seek_take(document_record_request_t *request, uint32_t *offset, unsigned *headers);
void document_record_cancel(document_record_request_t *request);
typedef struct {
    uint32_t end, publication;
    bool complete;
    int result;
    uint32_t source_begin, source_next, source_total, chapter, chapters;
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

typedef struct {
    epd_compute_buffer_t pixels;
    unsigned width, height;
} document_picture_t;

static inline size_t document_picture_charge(size_t bytes)
{
    size_t padding = EPD_COMPUTE_CACHE_LINE - 1u;
    return ((bytes + padding) & ~padding) + padding;
}

#define DOCUMENT_COVER_WIDTH 168u
#define DOCUMENT_COVER_HEIGHT 220u
/* UI queues a package/cover probe. The document worker uses ACPU, without a
 * spool or persistent cache. take: 0 pending, 1 complete, -1 cancelled.
 * An empty title/bitmap means fallback. The caller owns transferred pixels
 * and must document_close() the request after every outcome. */
document_t *document_metadata_open(const bookshelf_item_t *file);
int document_metadata_take(document_t *document, char *title, size_t capacity,
                           document_bitmap_t *cover);

typedef struct document_source document_source_t;
struct document_source {
    const char *app_id, *cache_path, *identity, *path, *title;
    document_format_t format;
    book_resources_t resources;
    bool (*cancelled)(void *user);
    void (*release_user)(void *user);
    void *user;
    bool external;
};
/* UI queueing only; copies source strings and callbacks. Ownership of user
 * transfers only on success. Final disposal calls release_user on the UI. */
document_t *document_open_source(const document_source_t *source, char *error, size_t size);
bool document_worker_step(epd_service_t *service);
/* Observable publication/completion changes, independent of work performed.
 * Read before/after a worker step to coalesce its view notifications. */
uint32_t document_worker_revision(void);
/* Called after the last worker step. Retains the worker/module until every
 * document and cache compute client has closed; a failed CLOSE is retried. */
void document_worker_shutdown(void);
void document_worker_cancel(void); /* Signal only; worker owns cleanup. */

document_format_t document_format(const char *path, bool directory);
const char *document_format_name(document_format_t format);
/* No outer storage_lock: directory I/O locks internally; file payload hashing
 * runs on ACPU after releasing the storage lock. */
bool document_split_identity(const char *path, uint32_t *size, uint32_t *signature);
void document_worker(epd_service_t *service);
void document_service_process(void);
void document_service_shutdown(void);
document_t *document_open(const bookshelf_item_t *file, char *error, size_t size);
document_t *document_open_at(const bookshelf_item_t *file, const document_anchor_t *anchor,
                              char *error, size_t size);
document_t *document_reopen_at(document_t *document, const document_anchor_t *anchor);
bool document_source_position(document_t *document, uint32_t record, uint32_t byte,
                               document_anchor_t *anchor);
/* Explicit directory/link demand. Returns 0 while navigation is being filled,
 * 1 when complete, -1 on cancellation/failure. Existing TOC entries stay stable. */
int document_navigation_request(document_t *document);
/* Required body-anchor search, independent of optional prefetch and full TOC.
 * Upgrades a retained optional document while the target is required. Existing
 * required activity keeps its admission until the operation stops or is cancelled.
 * Returns 0 while body anchors are arriving, 1 at body EOF, -1 on failure. */
int document_target_require(document_t *document, bool required);
void document_navigation_release(document_t *document);
int document_target_cursor(document_t *document, uint32_t *record, uint32_t *byte);
/* Returned adjacent/TOC documents carry a caller reference; close after use. */
document_t *document_adjacent(document_t *document, int direction);
/* Retain a neighbor without foreground admission or conversion preemption.
 * The caller releases the returned reference with document_close(). */
document_t *document_adjacent_prefetch(document_t *document, int direction);
document_t *document_adjacent_ready(document_t *document, int direction);
bool document_adjacent_available(document_t *document, int direction);
/* Publish actual selection separately from ownership and optional flow.
 * Promotion resumes deferred input; the worker upgrades its activity before
 * executing. Repeating the same selection does not restart admission. */
void document_set_foreground(document_t *document, bool foreground);
/* One near-window request per input demand. Queue from the reader's page
 * context with its genuine INTERACTIVE parent; parent=0 withdraws the request.
 * Refusal stays latched for this demand, including after withdrawal. The
 * document worker installs/closes activities between its execution slices. */
void document_nearby_request(document_t *document, uint64_t demand, uint64_t parent);
/* Owned background reference, without promotion or preemption. Opens a whole
 * chapter from its start; nonchapter sources use chapter=0. TXT keeps original
 * byte anchors while streaming to EOF, without artificial slice page breaks. */
document_t *document_pagination_open(document_t *document, uint32_t chapter);
document_t *document_toc_open(document_t *document, unsigned index, char *error, size_t size);
/* Memory-only lookup of a normalized body link; returns an owned target
 * chapter. Resolve its fragment with document_anchor_find as records arrive. */
document_t *document_link_open(document_t *document, uint32_t record, char *error, size_t size);
/* UI-thread result of the most recent open attempt. */
reader_error_t document_open_error(void);
void document_close(document_t *document);
void document_status(document_t *document, document_status_t *status);
void document_progress(document_t *document, document_progress_t *progress);
unsigned document_source_percent(const document_t *document, const document_progress_t *progress,
                                 const document_anchor_t *anchor);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
typedef enum {
    DOC_RESTORE_ROUTE, DOC_RESTORE_STEP, DOC_RESTORE_REQUEUE,
    DOC_RESTORE_SELECTED, DOC_RESTORE_WAIT, DOC_RESTORE_ENTER, DOC_RESTORE_REJECT
} document_restore_trace_t;
void document_trace_restore(document_t *document, document_restore_trace_t event,
    const void *context, unsigned before, unsigned after, int result);
void document_conversion_diagnostic(void);
#endif
bool document_can_seek_progress(document_t *document);
document_t *document_progress_open(document_t *document, unsigned percent, unsigned *chapter_percent);
int document_progress_cursor(document_t *document, unsigned chapter_percent,
                             document_record_request_t **request, uint32_t *record, uint32_t *byte);
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
/* One coalesced memory target; 0 means unavailable. Requests only signal the
 * worker. min_available is total free application PSRAM, including headroom.
 * poll: 0 pending, 1 target reached, 2 reclaimable copies exhausted, -1 invalid.
 * released_bytes counts payload/object bytes, excluding allocator overhead.
 * Optional content growth stays paused until finish, including after poll=2.
 * Finish after the allocation attempt or cancellation; an in-flight slice may
 * complete. The free-space snapshot is not an allocation reservation. */
uint32_t document_reclaim_request(size_t min_available);
int document_reclaim_poll(uint32_t ticket, size_t *released_bytes, size_t *available_bytes);
void document_reclaim_finish(uint32_t ticket);
/* Each cursor is owned by one execution context. Destroy before document_close. */
document_reader_t *document_reader_create(document_t *document);
void document_reader_destroy(document_reader_t *reader);
bool document_reader_read(document_reader_t *reader, uint32_t offset, void *data, size_t length);
bool document_reader_record(document_reader_t *reader, uint32_t offset, document_record_t *record);
/* Shared structural validation for both layout and pixel preparation. */
bool document_reader_image_info(document_reader_t *reader, uint32_t offset, document_image_t *image);
bool document_read(document_t *document, uint32_t offset, void *data, size_t length);
bool document_record(document_t *document, uint32_t offset, document_record_t *record);
/* UI requests a row-packed Gray4 buffer. take returns
 * 0 while pending, 1 after transferring ownership, or -1 on failure.
 * Cancel every request, including a successfully transferred request. */
document_image_request_t *document_image_request(document_t *document, uint32_t offset,
                                                  size_t bytes);
int document_image_take(document_image_request_t *request, document_picture_t *picture);
void document_image_cancel(document_image_request_t *request);
void document_picture_release(document_picture_t *picture);
/* Require a successfully completed, persistent conversion; the opaque layout
 * blob may describe only a paginated prefix. Blobs contain no pointers.
 * Loads prefer queued snapshots, then disk, and return epd_app_free-owned
 * memory. Saves copy their input and may fail when memory/storage is unavailable. */
bool document_index_load(document_t *document, uint32_t layout, void **data, size_t *size);
/* Memory-only UI request/poll: 0 pending, 1 transferred, -1 absent/invalid. */
int document_index_load_async(document_t *document, uint32_t layout, void **data, size_t *size);
/* Small count metadata has a separate .count file; source/layout binding and
 * worker-only I/O are identical to page-index snapshots. */
int document_count_load_async(document_t *document, uint32_t layout, void **data, size_t *size);
bool document_count_save(document_t *document, uint32_t layout, const void *data, size_t size);
bool document_page_summary_load(document_t *document, uint32_t layout, uint32_t *ends, unsigned chapters);
bool document_page_summary_save(document_t *document, uint32_t layout, const uint32_t *ends, unsigned chapters);
unsigned document_toc_count(document_t *document);
bool document_navigation_ready(document_t *document);
bool document_toc_entry(document_t *document, unsigned index, char *title, size_t title_size,
                        char *target, size_t target_size, unsigned *level);
bool document_anchor_find(document_t *document, const char *target, uint32_t *record);
bool document_link_target(document_t *document, uint32_t record, char *target, size_t capacity);
bool document_current_title(document_t *document, uint32_t record, char *title, size_t capacity, unsigned *toc_index);
bool document_index_save(document_t *document, uint32_t layout, const void *data, size_t size);
/* Worker writes are asynchronous; true means the copied data was queued.
 * A newer save replaces the same path's pending job, preserving in-flight I/O. */
bool document_save_async(const char *path, const void *header, size_t header_size,
                         const void *data, size_t size);

/* Synchronous worker-only image conversion, with bounded source and target sizes. */
int document_image_decode(book_stream_t *stream, bool (*cancelled)(void *), void *user,
                          uint8_t **gray, unsigned *width, unsigned *height);
#endif
