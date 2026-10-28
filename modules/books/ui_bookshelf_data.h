#ifndef UI_BOOKSHELF_DATA_H
#define UI_BOOKSHELF_DATA_H

#include "bookshelf.h"
#include <stddef.h>

typedef struct
{
    uint32_t offset, current_page, total_pages, layout, progress, last_read;
    uint32_t document_version, document_kind, document_byte;
    uint32_t reserved[4]; /* Preserve the persisted history record layout. */
} ui_reading_position_t;

typedef struct
{
    bookshelf_item_t file;
    ui_reading_position_t position;
    uint32_t order;
    enum { BOOK_LAST_READ_NONE, BOOK_LAST_READ_READ, BOOK_LAST_READ_TODAY, BOOK_LAST_READ_DATE } last_read_kind;
    char last_read[48];
} ui_bookshelf_book_t;

/* UI: queue without filesystem I/O; coalesce matching pending/ready scans. */
int ui_bookshelf_refresh(void);
/* UI: publish a complete result only after the reader releases its indices. */
bool ui_bookshelf_process(void);
/* Consume a scan failure from the current storage generation once. */
int ui_bookshelf_take_error(void);
/* Run and stop on the application's existing reader worker. */
struct epd_service;
bool ui_bookshelf_worker_step(struct epd_service *service);
void ui_bookshelf_worker_shutdown(void);
void ui_bookshelf_close(void);
unsigned ui_bookshelf_count(void);
const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index);
/* UI thread, after closing the previous reader. Import a full file path into
 * the active external-book slot without changing the shelf's root listing. */
bool ui_bookshelf_include_path(const char *path, unsigned *index);
bool ui_bookshelf_save(unsigned index, const ui_reading_position_t *position);
/* Drain pending history writes at lifecycle/storage boundaries. UI thread only. */
bool ui_bookshelf_flush(void);
void ui_bookshelf_cache_path(unsigned index, char *out, size_t size, const char *suffix);
bool ui_bookshelf_storage_ready(void);

#endif
