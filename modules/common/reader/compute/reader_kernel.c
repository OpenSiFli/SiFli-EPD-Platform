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

static int space(unsigned char c)
{ return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
static int alnum(unsigned char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static void line_finish(epd_reader_batch_t *b)
{
    epd_reader_page_t *p = &b->page;
    uint32_t delta = p->line_style & EPD_READER_CENTER ? (b->width - p->x) / 2 :
                     p->line_style & EPD_READER_RIGHT ? b->width - p->x : 0;
    for (uint32_t i = p->line_start; i < p->count; ++i) p->runs[i].x += delta;
    p->y += (p->line_height ? p->line_height : b->base_height) + b->line_space;
    p->x = 0; p->line_height = 0; p->line_start = p->count; p->line_style = 0;
}

static int append_letter(epd_reader_batch_t *b, epd_reader_glyph_t glyph)
{
    epd_reader_page_t *p = &b->page;
    uint32_t width = glyph.width * (glyph.letter == '\t' ? 4 : 1);
    if (width > b->width) width = b->width;
    if (p->x && p->x + width > b->width) line_finish(b);
    if (p->y + b->font_height > b->height) { p->done = 1; return 0; }
    uint32_t bytes = glyph.letter == '\t' ? 4 : glyph.bytes;
    if (p->bytes + bytes + 2 >= EPD_READER_TEXT || p->count >= EPD_READER_RUNS - 1)
    { if (p->x) line_finish(b); p->done = 1; return 0; }
    epd_reader_run_t *run = p->count ? &p->runs[p->count - 1] : NULL;
    if (!run || run->image || run->y != p->y || run->style != b->style || run->link != b->link)
    {
        run = &p->runs[p->count++];
        memset(run, 0, sizeof(*run));
        run->x = p->x; run->y = p->y; run->text = p->bytes;
        run->style = b->style; run->link = b->link;
    }
    else if (p->bytes) --p->bytes;
    if (p->collect)
        memcpy(p->text + p->bytes, glyph.letter == '\t' ? "    " : b->data + p->cursor.byte, bytes);
    p->bytes += bytes; run->bytes += bytes;
    if (p->collect) p->text[p->bytes] = 0;
    ++p->bytes;
    run->width += width; run->height = b->font_height;
    p->x += width;
    if (b->font_height > p->line_height) p->line_height = b->font_height;
    p->line_style |= b->style;
    return 1;
}

int epd_reader_layout(epd_reader_batch_t *b)
{
    if (!b) return -1;
    b->events = 0;
    if (!b->width || b->width > UINT16_MAX || !b->height || b->height > UINT16_MAX ||
        !b->base_height || b->base_height > UINT16_MAX || !b->font_height || b->font_height > UINT16_MAX ||
        b->line_space > UINT16_MAX || !b->limit || b->limit > EPD_READER_BATCH ||
        b->kind > EPD_READER_IMAGE || b->page.count > EPD_READER_RUNS ||
        b->page.line_start > b->page.count || b->page.bytes > EPD_READER_TEXT ||
        b->page.x > b->width || b->page.y > 3u * UINT16_MAX ||
        b->page.line_height > UINT16_MAX || b->page.cursor.byte > b->length ||
        (b->kind != EPD_READER_IMAGE && b->length > EPD_READER_RECORD_TEXT))
        return -1;
    epd_reader_page_t *p = &b->page;
    while (!p->done && b->events < b->limit)
    {
        ++b->events;
        switch (b->kind)
        {
        case EPD_READER_EOF:
            if (p->x) line_finish(b);
            p->done = p->eof = 1;
            return 0;
        case EPD_READER_TEXT_RECORD:
            if (p->cursor.byte < b->length)
            {
                epd_reader_glyph_t glyph = b->glyphs[p->cursor.byte];
                if (!glyph.bytes || glyph.bytes > 4 || glyph.bytes > b->length - p->cursor.byte ||
                    glyph.width > UINT16_MAX) return -1;
                if (glyph.letter == '\r') p->cursor.byte += glyph.bytes;
                else if (glyph.letter == '\n') { line_finish(b); p->cursor.byte += glyph.bytes; }
                else
                {
                    if (!(b->style & EPD_READER_PREFORMAT) && p->x && glyph.letter < 128 &&
                        alnum((unsigned char)glyph.letter) && (!p->cursor.byte || space((unsigned char)b->data[p->cursor.byte - 1])))
                    {
                        uint32_t word_width = 0;
                        for (uint32_t at = p->cursor.byte; at < b->length; ++at)
                        {
                            unsigned char c = b->data[at];
                            if (c >= 128 || space(c)) break;
                            if (b->glyphs[at].bytes != 1 || b->glyphs[at].width > UINT16_MAX) return -1;
                            word_width += b->glyphs[at].width;
                            if (word_width > b->width) break;
                        }
                        if (word_width <= b->width && p->x + word_width > b->width) line_finish(b);
                    }
                    if (!append_letter(b, glyph)) return 0;
                    p->cursor.byte += glyph.bytes;
                }
                if (p->cursor.byte < b->length) continue;
            }
            break;
        case EPD_READER_PARAGRAPH:
            if (p->x) line_finish(b);
            p->y += b->base_height / 3;
            break;
        case EPD_READER_IMAGE:
        {
            if (!b->image_width || !b->image_height) return -1;
            if (p->x) line_finish(b);
            uint32_t width = b->image_width, height = b->image_height;
            if (width > b->width)
            {
                height = (uint64_t)height * b->width / width;
                if (!height) height = 1;
                width = b->width;
            }
            if (height > b->height)
            {
                width = (uint64_t)width * b->height / height;
                if (!width) width = 1;
                height = b->height;
            }
            if ((p->y + height > b->height && p->count) || p->count == EPD_READER_RUNS)
            { p->done = 1; return 0; }
            epd_reader_run_t *run = &p->runs[p->count++];
            memset(run, 0, sizeof(*run));
            run->image = p->cursor.record + 1; run->link = b->link;
            run->width = width; run->height = height;
            run->x = (b->width - width) / 2; run->y = p->y;
            p->y += height + b->line_space; p->line_start = p->count;
            break;
        }
        default: break;
        }
        p->cursor.record = b->next_record; p->cursor.byte = 0;
        return 0;
    }
    return 0;
}
