/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_WIRE_H
#define READER_WIRE_H
#include "epd_reader_layout.h"
#include <stddef.h>

#define READER_WIRE_VERSION 3u
#define READER_SLOTS 4u
#define READER_BACKEND_ACPU 0x55504341u
#define READER_SEED_PAGE 1u
#define READER_SEED_RECORD 2u

typedef struct {
    epd_reader_cursor_t start, cursor;
    uint32_t x, y, line_height, line_start, line_style, count, bytes, done, eof, collect;
} reader_page_header_t;

typedef struct {
    uint32_t kind, length, style, link, next_record;
    uint32_t width, height, line_space, base_height, font_height, image_width, image_height;
} reader_record_t;

typedef struct {
    /* epoch changes when a slot is seeded; sequence advances within that epoch.
     * The optional page and record seeds follow this header as packed values. */
    uint32_t version, size, slot, epoch, generation, sequence, flags, limit;
    epd_reader_cursor_t cursor;
    uint32_t page_bytes, record_bytes;
} reader_wire_request_t;

typedef struct {
    reader_record_t record;
    uint32_t data_bytes, glyph_start, glyph_count;
} reader_record_seed_t;
typedef struct { uint32_t offset; epd_reader_glyph_t glyph; } reader_wire_glyph_t;

typedef struct {
    /* The backend tag is produced by the remote implementation. Payload holds
     * run_count runs followed by text_length bytes, including changed NULs. */
    uint32_t version, size, backend, slot, epoch, generation, sequence, events;
    reader_page_header_t page;
    uint32_t run_start, run_count, text_start, text_length;
} reader_wire_reply_t;

#define READER_PAGE_MAX (sizeof(reader_page_header_t) + EPD_READER_RUNS * sizeof(epd_reader_run_t) + EPD_READER_TEXT)
#define READER_TX_MAX (sizeof(reader_wire_request_t) + READER_PAGE_MAX + sizeof(reader_record_seed_t) + EPD_READER_RECORD_TEXT * sizeof(reader_wire_glyph_t) + EPD_READER_RECORD_TEXT + 1)
#define READER_RX_MAX (sizeof(reader_wire_reply_t) + EPD_READER_RUNS * sizeof(epd_reader_run_t) + EPD_READER_TEXT)

void reader_page_header(reader_page_header_t *, const epd_reader_page_t *);
bool reader_page_header_valid(const reader_page_header_t *);
void reader_page_apply(epd_reader_page_t *, const reader_page_header_t *);
bool reader_page_equal(const epd_reader_page_t *, const epd_reader_page_t *);
bool reader_record_equal(const epd_reader_batch_t *, const epd_reader_batch_t *);
size_t reader_wire_encode(void *, size_t, const epd_reader_batch_t *, uint32_t slot,
                         uint32_t epoch, uint32_t sequence, uint32_t flags);
bool reader_wire_load(epd_reader_batch_t *, const reader_wire_request_t *);
size_t reader_wire_reply_bound(const epd_reader_batch_t *);
size_t reader_wire_reply(void *, size_t, const reader_wire_request_t *, const epd_reader_batch_t *, const epd_reader_page_t *);
bool reader_wire_apply(epd_reader_batch_t *, const reader_wire_request_t *, const void *, size_t);
#endif
