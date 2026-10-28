/* SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0 */
#include "reader_text.h"
#include "reader_records.h"
#include <string.h>

/* Preserve the legacy TXT normalization, with source offsets independent of
 * short reads. One parser retains one bounded input window and pending record. */
typedef struct {
    epd_read_source_options_t options;
    book_stream_t stream;
    uint64_t offset, data_start;
    size_t begin, end;
    uint32_t encoding;
    bool eof, pending;
    int failure;
    uint8_t input[4096], output[EPD_READER_RECORD_TEXT_MAX];
    epd_read_record_t record;
} text_source_t;

static int cancelled(text_source_t *s)
{ return s->options.cancelled && s->options.cancelled(s->options.user) ? EPD_READ_CANCELLED : EPD_READ_OK; }
static int fill(text_source_t *s, size_t need)
{
    int result = cancelled(s);
    if (result) return result;
    if (s->failure) return s->failure;
    if (s->end - s->begin >= need || s->eof) return EPD_READ_OK;
    memmove(s->input, s->input + s->begin, s->end - s->begin);
    s->end -= s->begin; s->begin = 0;
    while (s->end < need && !s->eof)
    {
        size_t space = sizeof(s->input) - s->end;
        result = cancelled(s);
        if (result) return result;
        int n = s->stream.read(s->stream.user, s->input + s->end, space);
        if (n < 0) return s->failure = EPD_READ_IO;
        if ((size_t)n > space) return s->failure = EPD_READ_IO;
        if (!n)
        {
            s->eof = true;
            if (s->stream.size != UINT64_MAX && s->offset + s->end < s->stream.size)
                return s->failure = EPD_READ_IO;
        }
        else s->end += (size_t)n;
    }
    return EPD_READ_OK;
}
static unsigned utf_width(uint8_t c)
{ return c < 0x80 ? 1u : c >= 0xc2 && c <= 0xdf ? 2u : c >= 0xe0 && c <= 0xef ? 3u : c >= 0xf0 && c <= 0xf4 ? 4u : 0u; }
static bool utf_valid(const uint8_t *p, unsigned n)
{
    if (!n) return false;
    for (unsigned i = 1; i < n; ++i) if ((p[i] & 0xc0) != 0x80) return false;
    return !(n >= 3 && ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
                       (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)));
}
static bool utf_sample(const uint8_t *p, size_t bytes, bool eof)
{
    for (size_t i = 0; i < bytes;)
    {
        unsigned n = utf_width(p[i]);
        if (!n) return false;
        if (n > bytes - i) return !eof;
        for (unsigned k = 1; k < n; ++k) if ((p[i + k] & 0xc0) != 0x80) return false;
        i += n;
    }
    return true;
}
static unsigned emit(uint16_t c, uint8_t *out)
{
    if (c < 0x80) { out[0] = (uint8_t)c; return 1; }
    if (c < 0x800)
    { out[0] = (uint8_t)(0xc0 | (c >> 6)); out[1] = (uint8_t)(0x80 | (c & 63)); return 2; }
    out[0] = (uint8_t)(0xe0 | (c >> 12)); out[1] = (uint8_t)(0x80 | ((c >> 6) & 63));
    out[2] = (uint8_t)(0x80 | (c & 63)); return 3;
}
static int character(text_source_t *s, uint8_t out[4], unsigned *bytes, unsigned *raw)
{
    int result = fill(s, 4);
    if (result) return result;
    size_t available = s->end - s->begin;
    if (!available) return EPD_READ_END;
    const uint8_t *p = s->input + s->begin;
    *bytes = *raw = 1; out[0] = '?';
    if (p[0] < 0x80)
    {
        out[0] = p[0];
        if (p[0] == '\r') { out[0] = '\n'; if (available > 1 && p[1] == '\n') *raw = 2; }
        else if (p[0] < 32 && p[0] != '\n') out[0] = ' ';
    }
    else if (s->encoding == 2)
    {
        if (p[0] >= 0x81 && p[0] <= 0xfe && available >= 2 &&
            p[1] >= 0x40 && p[1] <= 0xfe && p[1] != 0x7f)
        {
            uint16_t code = (uint16_t)(((unsigned)p[0] << 8) | p[1]);
            uint16_t c = s->options.legacy_to_unicode(code, 936);
            if (c && !(c >= 0xd800 && c <= 0xdfff)) *bytes = emit(c, out);
            *raw = 2;
        }
    }
    else
    {
        unsigned n = utf_width(p[0]);
        if (n && n <= available && utf_valid(p, n))
        { *bytes = *raw = n; memcpy(out, p, n); }
    }
    return EPD_READ_OK;
}
static void consume(text_source_t *s, unsigned bytes)
{ s->begin += bytes; s->offset += bytes; }
static int text_open(const epd_read_source_options_t *options, void **instance)
{
    if (instance) *instance = NULL;
    if (!instance || !options || !options->path || !options->open_stream ||
        !options->memory.alloc || !options->memory.free || options->encoding > 2) return EPD_READ_INVALID;
    text_source_t *s = options->memory.alloc(sizeof(*s));
    if (!s) return EPD_READ_NO_MEMORY;
    memset(s, 0, sizeof(*s)); s->options = *options;
    int result = cancelled(s);
    if (!result) result = options->open_stream(options->user, options->path, &s->stream);
    if (!result && (!s->stream.read || !s->stream.close)) result = EPD_READ_INVALID;
    if (!result) result = fill(s, 2048);
    if (!result)
    {
        bool bom = s->end >= 3 && !memcmp(s->input, "\xef\xbb\xbf", 3);
        s->encoding = options->encoding ? options->encoding :
            bom || utf_sample(s->input, s->end, s->eof) ? 1u : 2u;
        if (s->encoding == 2 && !options->legacy_to_unicode) result = EPD_READ_UNSUPPORTED;
        if (bom) { consume(s, 3); s->data_start = 3; }
    }
    if (result)
    {
        if (s->stream.close) s->stream.close(s->stream.user);
        options->memory.free(s); return result;
    }
    *instance = s; return EPD_READ_OK;
}
static int text_next(void *instance, epd_read_batch_t *batch)
{
    text_source_t *s = instance;
    if (!s || !batch || !batch->data || batch->used > batch->capacity ||
        batch->capacity < sizeof(epd_read_record_t) + EPD_READER_RECORD_TEXT_MAX) return EPD_READ_INVALID;
    for (;;)
    {
        int result = cancelled(s);
        if (result) return result;
        if (!s->pending)
        {
            s->record = (epd_read_record_t){.kind = EPD_READ_TEXT,
                .source = {.version = 1, .offset = s->offset, .encoding = s->encoding}};
            while (s->record.bytes < EPD_READER_RECORD_TEXT_MAX)
            {
                uint8_t encoded[4]; unsigned bytes = 0, raw = 0;
                result = character(s, encoded, &bytes, &raw);
                if (result == EPD_READ_END) break;
                if (result) return result;
                if (bytes > EPD_READER_RECORD_TEXT_MAX - s->record.bytes) break;
                memcpy(s->output + s->record.bytes, encoded, bytes);
                s->record.bytes += bytes; consume(s, raw);
            }
            if (!s->record.bytes) return EPD_READ_END;
            s->record.next = (epd_read_anchor_t){.version = 1, .offset = s->offset, .encoding = s->encoding};
            s->pending = true;
        }
        result = reader_batch_append(batch, &s->record, s->output);
        if (result) return result;
        s->pending = false;
    }
}
static int text_seek(void *instance, const epd_read_anchor_t *position)
{
    text_source_t *s = instance;
    if (!s || !position || position->version != 1 || position->unit ||
        position->encoding != s->encoding || position->offset > s->stream.size ||
        position->byte > EPD_READER_RECORD_TEXT_MAX) return EPD_READ_INVALID;
    int result = cancelled(s);
    if (result) return result;
    if (!s->stream.seek) return EPD_READ_UNSUPPORTED;
    uint64_t offset = position->offset < s->data_start ? s->data_start : position->offset;
    if (s->stream.seek(s->stream.user, offset) < 0) return EPD_READ_IO;
    s->begin = s->end = 0; s->offset = offset;
    s->pending = s->eof = false; s->failure = 0;
    unsigned skip = position->byte;
    while (skip)
    {
        uint8_t encoded[4]; unsigned bytes = 0, raw = 0;
        result = character(s, encoded, &bytes, &raw);
        if (result != EPD_READ_OK) return result == EPD_READ_END ? EPD_READ_INVALID : result;
        if (bytes > skip) return EPD_READ_INVALID;
        consume(s, raw); skip -= bytes;
    }
    return EPD_READ_OK;
}
static void text_close(void *instance)
{
    text_source_t *s = instance;
    if (!s) return;
    s->stream.close(s->stream.user);
    s->options.memory.free(s);
}
const epd_read_provider_t reader_text_provider = {
    EPD_READER_SOURCE_ABI, sizeof(epd_read_provider_t), 1u, "text/plain",
    text_open, text_next, text_seek, NULL, text_close
};
