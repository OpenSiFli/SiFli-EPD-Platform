/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_reader_layout.h"
#include <stddef.h>
#include <string.h>

static int space(unsigned char c)
{ return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
static int alnum(unsigned char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

#define RUN_JUSTIFY_GAP (1u << 30)

static bool has_line(const epd_reader_page_t *p) { return p->count > p->line_start; }
static uint32_t font_em(const epd_reader_batch_t *b) { return b->font_size ? b->font_size : b->base_height; }
static uint32_t position(int32_t n, uint32_t maximum)
{ return n < 0 ? 0 : (uint32_t)n > maximum ? maximum : (uint32_t)n; }
static void margin_consume(epd_reader_batch_t *b)
{
    epd_reader_page_t *p = &b->page;
    if (p->count && (p->margin_positive || p->margin_negative))
        p->y = position((int32_t)p->y + p->margin_positive + p->margin_negative, 3u * UINT16_MAX);
    p->margin_positive = p->margin_negative = 0;
}
static void margin_add(epd_reader_page_t *p, int32_t value)
{
    if (value > p->margin_positive) p->margin_positive = value;
    if (value < p->margin_negative) p->margin_negative = value;
}
static void line_begin(epd_reader_batch_t *b)
{
    epd_reader_page_t *p = &b->page;
    bool styled = (b->paragraph.flags & EPD_PARAGRAPH_ACTIVE) != 0;
    p->line_left = styled ? position(epd_paragraph_pixels(b->paragraph.left, font_em(b), b->width), b->width - 1) : 0;
    uint32_t right = styled ? position(epd_paragraph_pixels(b->paragraph.right, font_em(b), b->width), b->width - p->line_left - 1) : 0;
    p->line_right = b->width - right;
    p->line_align = styled ? b->paragraph.align : EPD_ALIGN_DEFAULT;
    if (!p->line_align) p->line_align = b->style & EPD_READER_HEADING ? EPD_ALIGN_LEFT :
        b->style & EPD_READER_CENTER ? EPD_ALIGN_CENTER : b->style & EPD_READER_RIGHT ? EPD_ALIGN_RIGHT : EPD_ALIGN_LEFT;
    int32_t advance = styled ? epd_paragraph_pixels(b->paragraph.line_height, font_em(b), font_em(b)) : 0;
    p->line_advance = advance > 0 ? (uint32_t)advance : 0;
    p->x = p->line_left;
    if (styled && !p->cursor.byte && (b->style & EPD_READER_PARAGRAPH_START))
        p->x = position((int32_t)p->x + epd_paragraph_pixels(b->paragraph.indent, font_em(b), p->line_right - p->line_left), p->line_right - 1);
    if (b->style & EPD_READER_HEADING)
        p->x += epd_reader_heading_inset(b->font_height, p->line_right - p->x);
    p->last_letter = 0;
}
static void line_finish(epd_reader_batch_t *b, bool wrapped)
{
    epd_reader_page_t *p = &b->page;
    if (!p->line_right) {
        p->line_right = b->width;
        p->line_align = p->line_style & EPD_READER_HEADING ? EPD_ALIGN_LEFT :
            p->line_style & EPD_READER_CENTER ? EPD_ALIGN_CENTER :
            p->line_style & EPD_READER_RIGHT ? EPD_ALIGN_RIGHT : EPD_ALIGN_LEFT;
    }
    uint32_t available = p->line_right > p->x ? p->line_right - p->x : 0;
    uint32_t delta = p->line_align == EPD_ALIGN_CENTER ? available / 2 :
                     p->line_align == EPD_ALIGN_RIGHT ? available : 0;
    uint32_t gaps = 0, seen = 0;
    if (wrapped && p->line_align == EPD_ALIGN_JUSTIFY)
        for (uint32_t i = p->line_start + 1; i < p->count; ++i)
            gaps += !!(p->runs[i].style & RUN_JUSTIFY_GAP);
    for (uint32_t i = p->line_start; i < p->count; ++i) {
        if (i > p->line_start && (p->runs[i].style & RUN_JUSTIFY_GAP)) ++seen;
        p->runs[i].x += gaps ? (uint64_t)available * seen / gaps : delta;
    }
    uint32_t height = p->line_height ? p->line_height : b->base_height;
    uint32_t advance = p->line_advance ? p->line_advance : height;
    if (advance < height) advance = height;
    /* Authored line height sets the paragraph baseline; the reader's extra
     * line spacing must apply to styled and unstyled paragraphs alike. */
    p->y += advance + b->line_space;
    p->x = 0; p->line_height = 0; p->line_start = p->count; p->line_style = 0;
    p->line_advance = p->line_left = p->line_right = p->line_align = p->last_letter = 0;
}

static int append_letter(epd_reader_batch_t *b, epd_reader_glyph_t glyph)
{
    epd_reader_page_t *p = &b->page;
    if (!has_line(p)) { margin_consume(b); line_begin(b); }
    uint32_t width = glyph.width * (glyph.letter == '\t' ? 4 : 1);
    if (has_line(p) && p->x + width > p->line_right) { line_finish(b, true); line_begin(b); }
    if (width > p->line_right - p->x) width = p->line_right - p->x;
    if (!p->count && p->y + b->font_height > b->height) p->y = 0;
    if (p->y + b->font_height > b->height) {
        if (!has_line(p)) { p->x = p->line_left = p->line_right = p->line_advance = p->line_align = 0; }
        p->done = 1; return 0;
    }
    uint32_t bytes = glyph.letter == '\t' ? 4 : glyph.bytes;
    if (p->bytes + bytes + 2 >= EPD_READER_TEXT || p->count >= EPD_READER_RUNS - 1)
    { if (has_line(p)) line_finish(b, false); p->done = 1; return 0; }
    bool gap = p->line_align == EPD_ALIGN_JUSTIFY && has_line(p) &&
        (glyph.letter >= 0x2e80 || p->last_letter >= 0x2e80 || p->last_letter == ' ');
    epd_reader_run_t *run = p->count ? &p->runs[p->count - 1] : NULL;
    if (!has_line(p) || !run || gap || run->image || run->y != p->y ||
        (run->style & ~RUN_JUSTIFY_GAP) != b->style || run->link != b->link)
    {
        run = &p->runs[p->count++];
        memset(run, 0, sizeof(*run));
        run->x = p->x; run->y = p->y; run->text = p->bytes;
        run->style = b->style | (gap ? RUN_JUSTIFY_GAP : 0); run->link = b->link;
    }
    else if (p->bytes) --p->bytes;
    if (p->collect)
        memcpy(p->text + p->bytes, glyph.letter == '\t' ? "    " : b->data + p->cursor.byte, bytes);
    p->bytes += bytes; run->bytes += bytes;
    if (p->collect) p->text[p->bytes] = 0;
    ++p->bytes;
    run->width += width; run->height = b->font_height;
    p->x += width; p->last_letter = glyph.letter;
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
        b->line_space > UINT16_MAX || b->font_size > UINT16_MAX || b->paragraph.align > EPD_ALIGN_JUSTIFY || !b->limit || b->limit > EPD_READER_BATCH ||
        b->kind > EPD_READER_IMAGE || b->page.count > EPD_READER_RUNS ||
        b->page.line_start > b->page.count || b->page.bytes > EPD_READER_TEXT ||
        b->page.x > b->width || b->page.y > 3u * UINT16_MAX ||
        b->page.line_height > UINT16_MAX || b->page.line_right > b->width ||
        b->page.line_left > b->page.line_right || b->page.line_align > EPD_ALIGN_JUSTIFY ||
        b->page.line_advance > UINT16_MAX || b->page.margin_positive < 0 || b->page.margin_negative > 0 ||
        b->page.cursor.byte > b->length ||
        (b->kind != EPD_READER_IMAGE && b->length > EPD_READER_RECORD_TEXT))
        return -1;
    epd_reader_page_t *p = &b->page;
    while (!p->done && b->events < b->limit)
    {
        ++b->events;
        switch (b->kind)
        {
        case EPD_READER_EOF:
            if (has_line(p)) line_finish(b, false);
            p->done = p->eof = 1;
            return 0;
        case EPD_READER_TEXT_RECORD:
            if (p->cursor.byte < b->length)
            {
                if (!p->cursor.byte && (b->style & EPD_READER_PAGE_START))
                {
                    if (p->count)
                    {
                        if (has_line(p)) line_finish(b, false);
                        p->done = 1;
                        return 0;
                    }
                    p->x = p->y = p->line_height = p->line_style = 0;
                }
                epd_reader_glyph_t glyph = b->glyphs[p->cursor.byte];
                if (!glyph.bytes || glyph.bytes > 4 || glyph.bytes > b->length - p->cursor.byte ||
                    glyph.width > UINT16_MAX) return -1;
                if (glyph.letter == '\r') p->cursor.byte += glyph.bytes;
                else if (glyph.letter == '\n') { line_finish(b, false); p->cursor.byte += glyph.bytes; }
                else
                {
                    if (!(b->style & EPD_READER_PREFORMAT) && has_line(p) && glyph.letter < 128 &&
                        alnum((unsigned char)glyph.letter) && (!p->cursor.byte || space((unsigned char)b->data[p->cursor.byte - 1])))
                    {
                        uint32_t word_width = 0;
                        uint32_t available = p->line_right - p->line_left - (b->style & EPD_READER_HEADING ?
                            epd_reader_heading_inset(b->font_height, p->line_right - p->line_left) : 0);
                        for (uint32_t at = p->cursor.byte; at < b->length; ++at)
                        {
                            unsigned char c = b->data[at];
                            if (c >= 128 || space(c)) break;
                            if (b->glyphs[at].bytes != 1 || b->glyphs[at].width > UINT16_MAX) return -1;
                            word_width += b->glyphs[at].width;
                            if (word_width > available) break;
                        }
                        if (word_width <= available && p->x + word_width > p->line_right) line_finish(b, true);
                    }
                    if (!append_letter(b, glyph)) return 0;
                    p->cursor.byte += glyph.bytes;
                }
                if (p->cursor.byte < b->length) continue;
            }
            break;
        case EPD_READER_PARAGRAPH:
        {
            if (has_line(p)) line_finish(b, false);
            if (b->paragraph.flags & EPD_PARAGRAPH_ACTIVE) {
                int32_t margin = epd_paragraph_pixels(b->paragraph.margin, font_em(b), b->width);
                uint32_t padding = position(epd_paragraph_pixels(b->paragraph.padding, font_em(b), b->width), b->height);
                if (b->paragraph.flags & EPD_PARAGRAPH_BEGIN) margin_add(p, margin);
                if (padding) { margin_consume(b); p->y += padding; }
                if (b->paragraph.flags & EPD_PARAGRAPH_END) margin_add(p, margin);
                break;
            }
            const epd_reader_run_t *last = p->count ? &p->runs[p->count - 1] : NULL;
            uint32_t gap = b->base_height / 3;
            if (last && !last->image && (last->style & EPD_READER_HEADING) &&
                p->y == last->y + last->height + b->line_space)
            {
                uint32_t heading_gap = epd_reader_heading_space_after(last->height);
                if (heading_gap > gap) gap = heading_gap;
            }
            p->y += gap;
            break;
        }
        case EPD_READER_IMAGE:
        {
            if (!b->image_width || !b->image_height) return -1;
            if (has_line(p)) line_finish(b, false);
            margin_consume(b);
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
            run->image_bytes = b->length;
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
