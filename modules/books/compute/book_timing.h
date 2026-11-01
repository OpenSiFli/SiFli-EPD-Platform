/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_TIMING_H
#define BOOK_TIMING_H
#include "document_wire.h"

/* Serialized fibers retain separate timing reports. */
typedef struct book_timing {
    book_document_timing_t report;
    uint32_t (*clock)(void);
    uint32_t last;
    unsigned stage;
} book_timing_t;

void book_timing_begin(book_timing_t *timing, uint32_t (*clock)(void), uint32_t hz);
unsigned book_timing_enter(unsigned stage);
void book_timing_leave(unsigned previous);
void book_timing_end(void);
void book_timing_resume(book_timing_t *timing);
#endif
