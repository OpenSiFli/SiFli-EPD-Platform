/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READER_LAYOUT_H
#define EPD_READER_LAYOUT_H
#include <stdbool.h>
#include <stdint.h>

#define EPD_READER_CAPABILITY "epd/reader-layout"
#define EPD_READER_VERSION 1u
#define EPD_READER_TEXT 16384u
#define EPD_READER_RUNS 768u
#define EPD_READER_RECORD_TEXT 1024u
#define EPD_READER_BATCH 256u
enum { EPD_READER_EOF, EPD_READER_TEXT_RECORD, EPD_READER_PARAGRAPH,
       EPD_READER_ANCHOR, EPD_READER_LINK, EPD_READER_IMAGE };
enum { EPD_READER_PREFORMAT = 4, EPD_READER_CENTER = 8, EPD_READER_RIGHT = 16 };

typedef struct { uint32_t record, byte; } epd_reader_cursor_t;
typedef struct
{
    uint16_t x, y, width, height, text, bytes;
    uint32_t style, link, image;
} epd_reader_run_t;

#define EPD_READER_PAGE_FIELDS(cursor_type) \
    cursor_type start, cursor; \
    uint32_t x, y, line_height, line_start, line_style; \
    uint32_t count, bytes; \
    epd_reader_run_t runs[EPD_READER_RUNS]; \
    char text[EPD_READER_TEXT]; \
    uint32_t done, eof, collect

typedef struct { EPD_READER_PAGE_FIELDS(epd_reader_cursor_t); } epd_reader_page_t;
typedef struct { uint32_t letter, width, bytes; } epd_reader_glyph_t;

/* All fields are values or offsets. The batch owns both its input and output. */
typedef struct
{
    uint32_t generation, kind, length, style, link, next_record;
    uint32_t width, height, line_space, base_height, font_height;
    uint32_t image_width, image_height, limit, events;
    epd_reader_page_t page;
    epd_reader_glyph_t glyphs[EPD_READER_RECORD_TEXT];
    char data[EPD_READER_RECORD_TEXT + 1];
} epd_reader_batch_t;

/* Copy the header, count runs and (when collect is true) bytes of text, plus
 * done/eof/collect. Index pages still own count runs and track text bytes even
 * though their text is not collected. Unused destination storage is untouched.
 * Returns false for NULL or out-of-range count/bytes/line_start, without writing
 * dst. Self-copy is allowed; otherwise the page objects must not overlap. */
bool epd_reader_page_copy(epd_reader_page_t *dst, const epd_reader_page_t *src);

/* Advance at most limit character/control events in the current record. Line
 * breaks do not yield; page completion, record end or budget exhaustion do.
 * Consuming the final character also advances cursor.record in the same call.
 * Returns 0 on success, -1 for invalid input; events is reset on every call. */
int epd_reader_layout(epd_reader_batch_t *batch);
#endif
