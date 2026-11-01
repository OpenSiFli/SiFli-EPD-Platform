/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_reader_layout.h"
#include <stddef.h>
#include <string.h>

bool epd_reader_page_copy(epd_reader_page_t *dst, const epd_reader_page_t *src)
{
    if (!dst || !src || src->count > EPD_READER_RUNS ||
        src->bytes > EPD_READER_TEXT || src->line_start > src->count)
        return false;
    if (dst == src) return true;
    memcpy(dst, src, offsetof(epd_reader_page_t, runs));
    memcpy(dst->runs, src->runs, src->count * sizeof(src->runs[0]));
    if (src->collect) memcpy(dst->text, src->text, src->bytes);
    dst->done = src->done; dst->eof = src->eof; dst->collect = src->collect;
    return true;
}

bool epd_reader_batch_copy(epd_reader_batch_t *dst, const epd_reader_batch_t *src)
{
    if (!dst || !src || src->kind > EPD_READER_IMAGE ||
        (src->kind != EPD_READER_IMAGE && src->length > EPD_READER_RECORD_TEXT))
        return false;
    if (!epd_reader_page_copy(&dst->page, &src->page)) return false;
    if (dst == src) return true;
    memcpy(dst, src, offsetof(epd_reader_batch_t, page));
    if (src->kind == EPD_READER_TEXT_RECORD) {
        /* Zero glyph slots within the record denote unavailable measurements. */
        memcpy(dst->glyphs, src->glyphs, src->length * sizeof(src->glyphs[0]));
        memcpy(dst->data, src->data, src->length + 1u);
    }
    return true;
}
