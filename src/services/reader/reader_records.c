/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_records.h"
#include <string.h>

static bool utf8(const unsigned char *p, size_t size)
{
    for (size_t i = 0; i < size;)
    {
        uint32_t c = p[i++], low;
        unsigned remaining;
        if (c < 0x80) { if (!c) return false; continue; }
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; c &= 31; low = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; c &= 15; low = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; c &= 7; low = 0x10000; }
        else return false;
        if (remaining > size - i) return false;
        while (remaining--)
        {
            unsigned char next = p[i++];
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 63);
        }
        if (c < low || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}

bool reader_record_valid(const epd_read_record_t *r, const void *payload)
{
    if (!r || r->kind < EPD_READ_TEXT || r->kind > EPD_READ_METADATA ||
        r->source.version != EPD_READER_POSITION_VERSION ||
        r->next.version != EPD_READER_POSITION_VERSION || (r->bytes && !payload)) return false;
    if (r->kind == EPD_READ_PARAGRAPH) return !r->bytes;
    if (r->kind == EPD_READ_TEXT)
        return r->bytes && r->bytes <= EPD_READER_RECORD_TEXT_MAX && utf8(payload, r->bytes);
    if (!r->bytes || r->bytes > EPD_READER_RECORD_TEXT_MAX) return false;
    const unsigned char *p = payload;
    if (p[r->bytes - 1]) return false;
    size_t start = 0;
    for (size_t i = 0; i < r->bytes; ++i)
        if (!p[i])
        {
            if (!utf8(p + start, i - start)) return false;
            start = i + 1;
        }
    return true;
}

static bool record_size(uint32_t bytes, size_t *size)
{
    if (bytes > EPD_READER_RECORD_TEXT_MAX) return false;
    *size = (sizeof(epd_read_record_t) + (size_t)bytes + 7u) & ~(size_t)7u;
    return true;
}

void reader_batch_init(epd_read_batch_t *b, void *buffer, size_t capacity)
{
    if (!b) return;
    *b = (epd_read_batch_t){.data = buffer, .capacity = buffer ? capacity : 0};
}

int reader_batch_append(epd_read_batch_t *b, const epd_read_record_t *r, const void *payload)
{
    size_t size;
    if (!b || !b->data || b->used > b->capacity || b->count == UINT32_MAX ||
        !reader_record_valid(r, payload) || !record_size(r->bytes, &size)) return EPD_READ_INVALID;
    if (size > b->capacity - b->used) return EPD_READ_FULL;
    memcpy(b->data + b->used, r, sizeof(*r));
    if (r->bytes) memcpy(b->data + b->used + sizeof(*r), payload, r->bytes);
    memset(b->data + b->used + sizeof(*r) + r->bytes, 0, size - sizeof(*r) - r->bytes);
    b->used += size; ++b->count;
    return EPD_READ_OK;
}

int reader_batch_next(const epd_read_batch_t *b, size_t *offset,
                      epd_read_record_t *r, const void **payload)
{
    if (!b || !offset || !r || !payload || b->used > b->capacity ||
        (!b->data && b->used) || *offset > b->used || (*offset & 7u)) return EPD_READ_INVALID;
    *payload = NULL;
    if (*offset == b->used) return EPD_READ_END;
    if (b->used - *offset < sizeof(*r)) return EPD_READ_INVALID;
    memcpy(r, b->data + *offset, sizeof(*r));
    size_t size;
    if (!record_size(r->bytes, &size) || size > b->used - *offset) return EPD_READ_INVALID;
    const void *data = b->data + *offset + sizeof(*r);
    if (!reader_record_valid(r, data)) return EPD_READ_INVALID;
    *payload = data; *offset += size;
    return EPD_READ_OK;
}

bool reader_batch_valid(const epd_read_batch_t *b)
{
    size_t offset = 0;
    uint32_t count = 0;
    epd_read_record_t r;
    const void *payload;
    int result;
    while ((result = reader_batch_next(b, &offset, &r, &payload)) == EPD_READ_OK)
    {
        if (count == UINT32_MAX) return false;
        ++count;
    }
    return result == EPD_READ_END && count == b->count;
}
