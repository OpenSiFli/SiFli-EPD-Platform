/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOKS_READER_MARKS_H
#define BOOKS_READER_MARKS_H

#include "document/document.h"
#include "app_service.h"

#define BOOKS_MARK_LIMIT 24u
#define BOOKS_MARK_NOTE_BYTES 288u

typedef struct {
    document_anchor_t source;
    unsigned percent;
    char excerpt[96];
    char note[BOOKS_MARK_NOTE_BYTES];
} books_mark_t;

bool books_marks_request(const bookshelf_item_t *file, epd_service_t *service);
bool books_marks_worker(epd_service_t *service);
rt_int32_t books_marks_delay(void);
bool books_marks_busy(void);
bool books_marks_process(void);
bool books_marks_ready(void);
bool books_marks_failed(void);
const books_mark_t *books_marks_list(unsigned *count);
bool books_marks_submit(epd_service_t *service, unsigned index, const books_mark_t *mark);
void books_marks_close(void);
#endif
