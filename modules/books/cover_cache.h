/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOKS_COVER_CACHE_H
#define BOOKS_COVER_CACHE_H
#include "document/document.h"
#include "file_version.h"

/* UI: memory-only requests and ownership transfers. A miss uses the extractor. */
bool books_cover_cache_request(const bookshelf_item_t *file, const book_file_version_t *version);
int books_cover_cache_take(char *title, size_t capacity, document_bitmap_t *cover);
void books_cover_cache_save(const bookshelf_item_t *file, const book_file_version_t *version,
                           const char *title, const document_bitmap_t *cover);
void books_cover_cache_cancel(void);
bool books_cover_cache_pressure(void);
bool books_cover_cache_busy(void);
void books_cover_cache_diagnostic(void);
/* Worker: reader work preempts optional I/O; active cancellation still closes. */
struct epd_service;
bool books_cover_cache_worker_step(struct epd_service *service, bool reader_active);
void books_cover_cache_shutdown(void);
#endif
