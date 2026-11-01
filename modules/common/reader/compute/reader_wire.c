/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_wire.h"
#include <string.h>

typedef char record_fields_are_contiguous[
    offsetof(epd_reader_batch_t, limit) - offsetof(epd_reader_batch_t, kind) == sizeof(reader_record_t) ? 1 : -1];
typedef char page_header_prefix_matches[
    offsetof(reader_page_header_t, done) == offsetof(epd_reader_page_t, runs) ? 1 : -1];

void reader_page_header(reader_page_header_t *h, const epd_reader_page_t *p)
{
    memcpy(h, p, offsetof(epd_reader_page_t, runs));
    h->done = p->done; h->eof = p->eof; h->collect = p->collect;
}
bool reader_page_header_valid(const reader_page_header_t *h)
{ return h->count <= EPD_READER_RUNS && h->bytes <= EPD_READER_TEXT && h->line_start <= h->count; }
void reader_page_apply(epd_reader_page_t *p, const reader_page_header_t *h)
{
    memcpy(p, h, offsetof(epd_reader_page_t, runs));
    p->done = h->done; p->eof = h->eof; p->collect = h->collect;
}
bool reader_page_equal(const epd_reader_page_t *a, const epd_reader_page_t *b)
{
    reader_page_header_t x, y;
    reader_page_header(&x, a); reader_page_header(&y, b);
    return reader_page_header_valid(&x) && !memcmp(&x, &y, sizeof(x)) &&
        !memcmp(a->runs, b->runs, a->count * sizeof(*a->runs)) &&
        (!a->collect || !memcmp(a->text, b->text, a->bytes));
}
static void record_get(reader_record_t *r, const epd_reader_batch_t *b)
{ memcpy(r, &b->kind, sizeof(*r)); }
static void record_set(epd_reader_batch_t *b, const reader_record_t *r)
{ memcpy(&b->kind, r, sizeof(*r)); }
bool reader_record_equal(const epd_reader_batch_t *a, const epd_reader_batch_t *b)
{
    reader_record_t x, y;
    record_get(&x, a); record_get(&y, b);
    if (memcmp(&x, &y, sizeof(x))) return false;
    if (a->kind != EPD_READER_TEXT_RECORD) return true;
    return a->length <= EPD_READER_RECORD_TEXT && !memcmp(a->data, b->data, a->length) &&
        !memcmp(a->glyphs, b->glyphs, a->length * sizeof(*a->glyphs));
}

size_t reader_wire_encode(void *buffer, size_t capacity, const epd_reader_batch_t *b,
                         uint32_t slot, uint32_t epoch, uint32_t sequence, uint32_t flags)
{
    reader_page_header_t page;
    reader_page_header(&page, &b->page);
    if (capacity < READER_TX_MAX || !reader_page_header_valid(&page) ||
        slot >= READER_SLOTS || !epoch || !sequence || flags & ~(READER_SEED_PAGE | READER_SEED_RECORD) ||
        !b->limit || b->limit > EPD_READER_BATCH ||
        (b->kind != EPD_READER_IMAGE && b->length > EPD_READER_RECORD_TEXT)) return 0;
    reader_wire_request_t *q = buffer;
    memset(q, 0, sizeof(*q));
    q->version = READER_WIRE_VERSION; q->slot = slot; q->epoch = epoch;
    q->generation = b->generation; q->sequence = sequence; q->flags = flags;
    q->limit = b->limit; q->cursor = b->page.cursor;
    uint8_t *at = (uint8_t *)(q + 1);
    if (flags & READER_SEED_PAGE) {
        memcpy(at, &page, sizeof(page)); at += sizeof(page);
        size_t runs = b->page.count * sizeof(*b->page.runs);
        memcpy(at, b->page.runs, runs); at += runs;
        if (b->page.collect) { memcpy(at, b->page.text, b->page.bytes); at += b->page.bytes; }
        q->page_bytes = (uint32_t)(at - (uint8_t *)(q + 1));
    }
    if (flags & READER_SEED_RECORD) {
        reader_record_seed_t seed = {0};
        record_get(&seed.record, b);
        uint8_t *begin = at;
        at += sizeof(seed);
        if (b->kind == EPD_READER_TEXT_RECORD) {
            if (b->page.cursor.byte > b->length) return 0;
            seed.glyph_start = b->page.cursor.byte;
            for (uint32_t i = seed.glyph_start; i < b->length;) {
                epd_reader_glyph_t g = b->glyphs[i];
                if (!g.bytes) {
                    if (seed.glyph_count < b->limit) return 0;
                    break;
                }
                if (g.bytes > 4 || g.bytes > b->length - i || g.width > UINT16_MAX) return 0;
                reader_wire_glyph_t glyph = {i, g};
                memcpy(at, &glyph, sizeof(glyph)); at += sizeof(glyph);
                ++seed.glyph_count;
                i += g.bytes;
            }
            seed.data_bytes = b->length;
            memcpy(at, b->data, b->length); at += b->length;
        }
        memcpy(begin, &seed, sizeof(seed));
        q->record_bytes = (uint32_t)(at - begin);
    }
    q->size = (uint32_t)(at - (uint8_t *)buffer);
    return q->size;
}

bool reader_wire_load(epd_reader_batch_t *b, const reader_wire_request_t *q)
{
    if (q->version != READER_WIRE_VERSION || q->size < sizeof(*q) || q->size > READER_TX_MAX ||
        q->slot >= READER_SLOTS || !q->epoch || !q->sequence || !q->limit || q->limit > EPD_READER_BATCH ||
        q->flags & ~(READER_SEED_PAGE | READER_SEED_RECORD) ||
        q->page_bytes > q->size - sizeof(*q) || q->record_bytes != q->size - sizeof(*q) - q->page_bytes ||
        (!!q->page_bytes != !!(q->flags & READER_SEED_PAGE)) ||
        (!!q->record_bytes != !!(q->flags & READER_SEED_RECORD))) return false;
    const uint8_t *at = (const uint8_t *)(q + 1);
    if (q->page_bytes) {
        reader_page_header_t h;
        if (q->page_bytes < sizeof(h)) return false;
        memcpy(&h, at, sizeof(h)); at += sizeof(h);
        if (!reader_page_header_valid(&h) || q->page_bytes != sizeof(h) + h.count * sizeof(*b->page.runs) + (h.collect ? h.bytes : 0)) return false;
        reader_page_apply(&b->page, &h);
        memcpy(b->page.runs, at, h.count * sizeof(*b->page.runs)); at += h.count * sizeof(*b->page.runs);
        if (h.collect) { memcpy(b->page.text, at, h.bytes); at += h.bytes; }
    }
    if (q->record_bytes) {
        reader_record_seed_t seed;
        if (q->record_bytes < sizeof(seed)) return false;
        memcpy(&seed, at, sizeof(seed)); at += sizeof(seed);
        if (seed.record.kind > EPD_READER_IMAGE || seed.glyph_count > EPD_READER_RECORD_TEXT ||
            seed.data_bytes > EPD_READER_RECORD_TEXT || q->record_bytes != sizeof(seed) + seed.glyph_count * sizeof(reader_wire_glyph_t) + seed.data_bytes ||
            seed.glyph_start > seed.data_bytes ||
            (seed.record.kind == EPD_READER_TEXT_RECORD ?
                seed.data_bytes != seed.record.length || seed.glyph_start != q->cursor.byte :
                seed.data_bytes || seed.glyph_count || seed.glyph_start)) return false;
        record_set(b, &seed.record);
        memset(b->glyphs, 0, sizeof(b->glyphs));
        uint32_t offset = seed.glyph_start;
        for (uint32_t i = 0; i < seed.glyph_count; ++i) {
            reader_wire_glyph_t g;
            memcpy(&g, at, sizeof(g)); at += sizeof(g);
            if (g.offset != offset || offset >= seed.data_bytes || !g.glyph.bytes || g.glyph.bytes > 4 ||
                g.glyph.bytes > seed.data_bytes - offset || g.glyph.width > UINT16_MAX) return false;
            b->glyphs[offset] = g.glyph;
            offset += g.glyph.bytes;
        }
        if (offset != seed.data_bytes && seed.glyph_count < q->limit) return false;
        memcpy(b->data, at, seed.data_bytes); b->data[seed.data_bytes] = 0;
    }
    b->generation = q->generation; b->limit = q->limit;
    return !memcmp(&b->page.cursor, &q->cursor, sizeof(q->cursor));
}

size_t reader_wire_reply_bound(const epd_reader_batch_t *b)
{
    uint32_t first = b->page.line_start;
    if (b->page.count && first > b->page.count - 1) first = b->page.count - 1;
    uint32_t runs = b->page.count + b->limit;
    if (runs > EPD_READER_RUNS) runs = EPD_READER_RUNS;
    uint32_t text = b->page.collect ? b->limit * 5 + (b->page.bytes ? 1 : 0) : 0;
    return sizeof(reader_wire_reply_t) + (runs - first) * sizeof(epd_reader_run_t) + text;
}

size_t reader_wire_reply(void *buffer, size_t capacity, const reader_wire_request_t *q,
                         const epd_reader_batch_t *b, const epd_reader_page_t *old)
{
    reader_wire_reply_t h = {0};
    h.version = READER_WIRE_VERSION; h.backend = READER_BACKEND_ACPU;
    h.slot = q->slot; h.epoch = q->epoch; h.generation = q->generation; h.sequence = q->sequence;
    h.events = b->events; reader_page_header(&h.page, &b->page);
    uint32_t first = 0, end = b->page.count;
    while (first < end && first < old->count && !memcmp(&old->runs[first], &b->page.runs[first], sizeof(*old->runs))) ++first;
    while (end > first && end <= old->count && !memcmp(&old->runs[end-1], &b->page.runs[end-1], sizeof(*old->runs))) --end;
    h.run_start = first; h.run_count = end - first;
    if (b->page.collect) {
        first = 0; end = b->page.bytes;
        while (first < end && first < old->bytes && old->text[first] == b->page.text[first]) ++first;
        while (end > first && end <= old->bytes && old->text[end-1] == b->page.text[end-1]) --end;
        h.text_start = first; h.text_length = end - first;
    }
    h.size = (uint32_t)(sizeof(h) + h.run_count * sizeof(epd_reader_run_t) + h.text_length);
    if (h.size > capacity) return 0;
    memcpy(buffer, &h, sizeof(h));
    uint8_t *at = (uint8_t *)buffer + sizeof(h);
    memcpy(at, b->page.runs + h.run_start, h.run_count * sizeof(epd_reader_run_t));
    memcpy(at + h.run_count * sizeof(epd_reader_run_t), b->page.text + h.text_start, h.text_length);
    return h.size;
}

bool reader_wire_apply(epd_reader_batch_t *b, const reader_wire_request_t *q, const void *buffer, size_t capacity)
{
    if (capacity < sizeof(reader_wire_reply_t)) return false;
    reader_wire_reply_t h;
    memcpy(&h, buffer, sizeof(h));
    if (h.version != READER_WIRE_VERSION || h.backend != READER_BACKEND_ACPU || h.slot != q->slot ||
        h.epoch != q->epoch || h.generation != b->generation || h.generation != q->generation || h.sequence != q->sequence ||
        h.events > q->limit || !reader_page_header_valid(&h.page) || h.page.collect != b->page.collect ||
        h.run_start > h.page.count || h.run_count > h.page.count - h.run_start ||
        h.text_start > h.page.bytes || h.text_length > h.page.bytes - h.text_start ||
        (!h.page.collect && h.text_length) || h.size > capacity ||
        h.size != sizeof(h) + h.run_count * sizeof(epd_reader_run_t) + h.text_length ||
        (h.page.count > b->page.count && (h.run_start > b->page.count || h.run_start + h.run_count != h.page.count)) ||
        (h.page.collect && h.page.bytes > b->page.bytes && (h.text_start > b->page.bytes || h.text_start + h.text_length != h.page.bytes))) return false;
    const uint8_t *at = (const uint8_t *)buffer + sizeof(h);
    memcpy(b->page.runs + h.run_start, at, h.run_count * sizeof(epd_reader_run_t));
    memcpy(b->page.text + h.text_start, at + h.run_count * sizeof(epd_reader_run_t), h.text_length);
    reader_page_apply(&b->page, &h.page); b->events = h.events;
    return true;
}
