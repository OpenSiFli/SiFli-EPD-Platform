#ifndef UI_BOOKSHELF_DATA_H
#define UI_BOOKSHELF_DATA_H

#include "bookshelf.h"
#include "document/document.h"
#include <stddef.h>

/* At most one four-book shelf page owns published thumbnail pixels. */
#define BOOKSHELF_COVER_LIMIT 4u

typedef struct
{
    uint32_t offset, current_page, total_pages, layout, progress, last_read;
    uint32_t document_version, document_kind, document_byte;
    document_anchor_t source;
} ui_reading_position_t;

typedef struct
{
    bookshelf_item_t file;
    ui_reading_position_t position;
    uint32_t order;
    enum { BOOK_LAST_READ_NONE, BOOK_LAST_READ_READ, BOOK_LAST_READ_TODAY, BOOK_LAST_READ_DATE } last_read_kind;
    char last_read[48];
    document_bitmap_t cover;
    bool metadata_checked, cover_evicted;
} ui_bookshelf_book_t;

/* UI: queue without filesystem I/O; coalesce matching pending/ready scans. */
int ui_bookshelf_refresh(void);
/* UI: publish only with an idle display and after the reader releases its
 * indices. Busy displays return false without publishing or releasing covers.
 * Rebind all image widgets immediately when true, including clearing sources
 * for fallback/hidden cards. Old snapshot descriptors may no longer exist. */
bool ui_bookshelf_process(void);
/* UI, memory-only: advances only after a successful scan publication or close,
 * never for metadata/cover updates. Compare for inequality; uint32_t wraps. */
uint32_t ui_bookshelf_generation(void);
/* UI: enrich the visible range, capped at BOOKSHELF_COVER_LIMIT. Request/cancel
 * never free published covers; process retires covers outside the new range.
 * Cancel before leaving the shelf. If no more idle publications occur, at most
 * the last page stays resident until the next publication or UI teardown. */
void ui_bookshelf_metadata_request(unsigned first, unsigned count);
void ui_bookshelf_metadata_cancel(void);
/* Consume a scan failure from the current storage generation once. */
int ui_bookshelf_take_error(void);
/* Run and stop on the application's existing reader worker. */
struct epd_service;
bool ui_bookshelf_worker_step(struct epd_service *service);
void ui_bookshelf_worker_shutdown(void);
/* UI teardown: all shelf image widgets must already be detached/deleted. */
void ui_bookshelf_close(void);
unsigned ui_bookshelf_count(void);
const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index);
/* UI thread, after closing the previous reader. Import a full file path into
 * the active external-book slot without changing the shelf's root listing. */
bool ui_bookshelf_include_path(const char *path, unsigned *index);
bool ui_bookshelf_save(unsigned index, const ui_reading_position_t *position);
/* Drain pending history writes at lifecycle/storage boundaries. UI thread only. */
bool ui_bookshelf_flush(void);
/* A queued write is pending until attempted; only a failed write reports true. */
bool ui_bookshelf_save_failed(unsigned index);
void ui_bookshelf_cache_path(unsigned index, char *out, size_t size, const char *suffix);
bool ui_bookshelf_storage_ready(void);

#endif
