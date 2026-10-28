/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_page.h"
#include <string.h>

int reader_page_init(reader_page_t *p, epd_reader_batch_t *b,
                     const reader_page_metrics_t *m, uint32_t generation)
{
    if (!p || !b || !m || !m->width || m->width > UINT16_MAX || !m->height || m->height > UINT16_MAX ||
        !m->base_height || m->base_height > UINT16_MAX || m->line_space > UINT16_MAX ||
        !m->step_limit || m->step_limit > EPD_READER_BATCH || !m->font_height || !m->advance || !generation)
        return EPD_READ_INVALID;
    memset(p, 0, sizeof(*p)); memset(b, 0, sizeof(*b));
    p->batch = b; p->metrics = *m; p->generation = generation;
    b->generation = generation; b->page.collect = 1;
    b->width = m->width; b->height = m->height; b->base_height = b->font_height = m->base_height;
    b->line_space = m->line_space; b->limit = m->step_limit;
    return EPD_READ_OK;
}
int reader_page_load(reader_page_t *p, const epd_read_record_t *r,
                     const void *data, uint32_t width, uint32_t height)
{
    if (!p || !p->batch || !reader_record_valid(r, data)) return EPD_READ_INVALID;
    if (p->in_flight) return EPD_READ_BUSY;
    epd_reader_batch_t *b = p->batch;
    if (p->loaded || p->eof || b->page.done) return EPD_READ_BUSY;
    if (p->error) return p->error;
    if (p->ordinal == UINT32_MAX || (r->kind == EPD_READ_IMAGE && (!width || !height)) ||
        r->bytes > UINT32_MAX - r->source.byte) return EPD_READ_INVALID;
    uint32_t font_height = p->metrics.font_height(p->metrics.user, r->style);
    if (!font_height || font_height > UINT16_MAX) return EPD_READ_INVALID;
    p->record = *r; p->position = r->source;
    if (!p->start.version) p->start = r->source;
    b->kind = r->kind == EPD_READ_METADATA ? EPD_READER_ANCHOR : r->kind;
    b->length = r->bytes; b->style = r->style; b->link = r->link;
    b->next_record = p->ordinal + 1; b->font_height = font_height;
    b->image_width = width; b->image_height = height;
    memset(b->glyphs, 0, sizeof(b->glyphs));
    if (r->bytes) memcpy(b->data, data, r->bytes);
    b->data[r->bytes] = 0;
    p->loaded = true; p->measured = 0;
    return EPD_READ_OK;
}
int reader_page_end(reader_page_t *p, const epd_read_anchor_t *end)
{
    if (!p || !p->batch || !end || end->version != EPD_READER_POSITION_VERSION) return EPD_READ_INVALID;
    if (p->in_flight) return EPD_READ_BUSY;
    if (p->loaded || p->batch->page.done) return EPD_READ_BUSY;
    p->eof = true; p->position = *end;
    if (!p->start.version) p->start = *end;
    p->batch->kind = EPD_READER_EOF; p->batch->length = 0;
    return EPD_READ_OK;
}
static uint32_t decode(const char *data, uint32_t *letter)
{
    const unsigned char *s = (const unsigned char *)data;
    uint32_t n = s[0] < 0x80 ? 1u : s[0] < 0xe0 ? 2u : s[0] < 0xf0 ? 3u : 4u;
    uint32_t value = n == 1 ? s[0] : (uint32_t)s[0] & ((1u << (7u - n)) - 1u);
    for (uint32_t i = 1; i < n; ++i) value = (value << 6) | (s[i] & 63u);
    *letter = value; return n;
}
int reader_page_prepare(reader_page_t *p, unsigned budget)
{
    if (!p || !p->batch || !budget || budget > EPD_READER_BATCH) return EPD_READ_INVALID;
    if (p->in_flight) return EPD_READ_BUSY;
    if (p->error) return p->error;
    epd_reader_batch_t *b = p->batch;
    if (b->page.done) return EPD_READ_END;
    if (!p->loaded && !p->eof) return EPD_READ_WAIT;
    if (p->loaded && p->record.kind == EPD_READ_TEXT)
    {
        while (budget-- && p->measured < b->length)
        {
            epd_reader_glyph_t *g = &b->glyphs[p->measured];
            g->bytes = decode(b->data + p->measured, &g->letter);
            g->width = g->letter == '\n' || g->letter == '\r' ? 0 :
                p->metrics.advance(p->metrics.user, b->style, g->letter == '\t' ? ' ' : g->letter);
            if (g->width > UINT16_MAX) return p->error = EPD_READ_INVALID;
            p->measured += g->bytes;
        }
        if (p->measured < b->length) return EPD_READ_WAIT;
    }
    b->generation = p->generation; b->limit = p->metrics.step_limit;
    p->in_flight = true;
    return EPD_READ_OK;
}
int reader_page_complete(reader_page_t *p, int result)
{
    if (!p || !p->batch || !p->in_flight) return EPD_READ_INVALID;
    p->in_flight = false;
    epd_reader_batch_t *b = p->batch;
    if (b->generation != p->generation) return p->error = EPD_READ_STALE;
    if (result) return p->error = EPD_READ_ERROR;
    if (b->page.cursor.record == p->ordinal)
    {
        if (p->loaded)
        {
            if (b->page.cursor.byte > p->record.bytes) return p->error = EPD_READ_INVALID;
            p->position = p->record.source;
            p->position.byte += b->page.cursor.byte;
        }
    }
    else if (p->loaded && b->page.cursor.record == p->ordinal + 1u && !b->page.cursor.byte)
    {
        p->position = p->record.next; p->loaded = false; ++p->ordinal;
    }
    else return p->error = EPD_READ_INVALID;
    return EPD_READ_OK;
}
int reader_page_turn(reader_page_t *p)
{
    if (!p || !p->batch) return EPD_READ_INVALID;
    if (p->in_flight) return EPD_READ_BUSY;
    if (p->error) return p->error;
    epd_reader_batch_t *b = p->batch;
    if (!b->page.done) return EPD_READ_WAIT;
    if (b->page.eof) return EPD_READ_END;
    epd_reader_cursor_t cursor = b->page.cursor;
    memset(&b->page, 0, sizeof(b->page)); b->page.collect = 1;
    b->page.start = b->page.cursor = cursor;
    p->start = p->position;
    return EPD_READ_OK;
}
