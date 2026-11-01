/* SPDX-License-Identifier: Apache-2.0 */
#include "../formats/book_internal.h"
#include "book_compute_internal.h"
#include "gbk_table.h"

enum { TXT_UTF8, TXT_GBK, TXT_LE, TXT_BE };
typedef struct txt_input {
    FILE *file;
    uint64_t offset;
    int encoding, pending;
} txt_input;

static int take(txt_input *in)
{
    int c = fgetc(in->file);
    if (c != EOF) ++in->offset;
    return c;
}
static void put_back(txt_input *in, int c)
{
    if (c != EOF && ungetc(c, in->file) != EOF) --in->offset;
}
static int utf8_length(unsigned c)
{
    return c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
        c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
}
static int valid_utf8(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n)
    {
        unsigned c = p[i++], cp;
        int k = utf8_length(c), j;
        if (!k) return 0;
        if (k == 1) continue;
        if ((size_t)(k - 1) > n - i) break;
        cp = c & (0x7f >> k);
        for (j = 1; j < k; ++j)
        {
            if ((p[i] & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (p[i++] & 63);
        }
        if (cp < (k == 2 ? 0x80u : k == 3 ? 0x800u : 0x10000u) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    }
    return 1;
}
static int unit(txt_input *in)
{
    int a, b;
    if (in->pending >= 0) { a = in->pending; in->pending = -1; return a; }
    a = take(in); if (a == EOF) return EOF;
    b = take(in); if (b == EOF) return 0xfffd;
    return in->encoding == TXT_LE ? a | (b << 8) : (a << 8) | b;
}
static int codepoint(txt_input *in)
{
    int c, b, k, j;
    unsigned cp;
    if (in->encoding >= TXT_LE)
    {
        c = unit(in);
        if (c >= 0xd800 && c <= 0xdbff)
        {
            b = unit(in);
            if (b >= 0xdc00 && b <= 0xdfff) return 0x10000 + ((c - 0xd800) << 10) + b - 0xdc00;
            in->pending = b;
            return 0xfffd;
        }
        return c >= 0xdc00 && c <= 0xdfff ? 0xfffd : c;
    }
    c = take(in);
    if (c == EOF || c < 0x80) return c;
    if (in->encoding == TXT_GBK)
    {
        if (c == 0x80) return 0x20ac;
        if (c < 0x81 || c > 0xfe) return 0xfffd;
        b = take(in);
        if (b < 0x40 || b > 0xfe || b == 0x7f) { put_back(in, b); return 0xfffd; }
        cp = book_gbk_unicode[(c - 0x81) * 190 + b - 0x40 - (b > 0x7f)];
        return cp ? (int)cp : 0xfffd;
    }
    k = utf8_length((unsigned)c);
    if (!k) return 0xfffd;
    cp = (unsigned)c & (0x7f >> k);
    for (j = 1; j < k; ++j)
    {
        b = take(in);
        if (b == EOF || (b & 0xc0) != 0x80) { put_back(in, b); return 0xfffd; }
        cp = (cp << 6) | (unsigned)(b & 63);
    }
    return cp < (k == 2 ? 0x80u : k == 3 ? 0x800u : 0x10000u) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ? 0xfffd : (int)cp;
}
static size_t encode(unsigned cp, char *out)
{
    if (!cp) cp = 0xfffd;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xe0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63)); return 4;
}
static int txt_location(const book_compute_observer_t *o, const book_compute_selection_t *q,
                        uint32_t actual, unsigned flags)
{
    char id[48];
    if (!q || !o || !o->location) return BOOK_OK;
    snprintf(id, sizeof(id), "txt-byte-%u", (unsigned)actual);
    return o->location(o->user, BOOK_COMPUTE_NO_SPINE, actual, q->anchor_byte,
                       BOOK_SOURCE_FILE_BYTE | flags, id);
}
static int slice_start(txt_input *in, const book_compute_selection_t *q, uint64_t skip,
                       uint64_t target, uint64_t *start, unsigned *flags)
{
    uint64_t low = target > BOOK_COMPUTE_TXT_BACKSCAN ? target - BOOK_COMPUTE_TXT_BACKSCAN : skip;
    uint64_t wanted = target;
    size_t n, available, i;
    unsigned char *scan;
    int found = 0;
    if (low < skip) low = skip;
    if (q->flags & BOOK_COMPUTE_CONTEXT)
    {
        uint32_t context = q->context_bytes ? q->context_bytes : BOOK_COMPUTE_DEFAULT_CONTEXT;
        wanted = target > context ? target - context : skip;
        if (wanted < low) wanted = low;
    }
    if (in->encoding >= TXT_LE && ((low - skip) & 1)) ++low;
    n = (size_t)(target - low);
    scan = bf_alloc(n + 4);
    if (!scan) return BOOK_NO_MEMORY;
    if (fseek(in->file, (long)low, SEEK_SET))
    { bf_free(scan); return BOOK_IO_ERROR; }
    available = fread(scan, 1, n + 4, in->file);
    if (available < n || ferror(in->file)) { bf_free(scan); return BOOK_IO_ERROR; }
    *start = low;
    for (i = 0; i < n;)
    {
        size_t step = in->encoding >= TXT_LE ? 2 : 1;
        unsigned c;
        if (step > n - i) break;
        c = step == 1 ? scan[i] : in->encoding == TXT_LE ? scan[i] | ((unsigned)scan[i+1] << 8) : ((unsigned)scan[i] << 8) | scan[i+1];
        i += step;
        if (c == '\r' && i + step <= available)
        {
            unsigned next = step == 1 ? scan[i] : in->encoding == TXT_LE ?
                scan[i] | ((unsigned)scan[i+1] << 8) : ((unsigned)scan[i] << 8) | scan[i+1];
            if (next == '\n') i += step;
        }
        if ((c == '\n' || c == '\r') && low + i <= wanted)
        { *start = low + i; found = 1; }
    }
    if (!found && low != skip)
    {
        *flags |= BOOK_SOURCE_FRAGMENT;
        if (in->encoding == TXT_UTF8)
            while (*start - low < n && (scan[*start-low] & 0xc0) == 0x80) ++*start;
        else if (in->encoding >= TXT_LE && n >= 2)
        {
            unsigned c = in->encoding == TXT_LE ? scan[0] | ((unsigned)scan[1]<<8) : ((unsigned)scan[0]<<8) | scan[1];
            if (c >= 0xdc00 && c <= 0xdfff) *start += 2;
        }
        else if (in->encoding == TXT_GBK)
        {
            /* A GBK trail can look like a lead or printable ASCII. Synchronize
             * only at an unambiguous one-byte character inside the window. */
            for (i = 0; i < n && low + i < wanted; ++i)
                if (scan[i] < 0x40 || scan[i] == 0x7f) { *start = low + i + 1; found = 1; }
            if (!found) { bf_free(scan); return BOOK_UNSUPPORTED; }
        }
    }
    bf_free(scan);
    return BOOK_OK;
}
static uint64_t input_position(const txt_input *in)
{ return in->offset - (in->pending >= 0 ? 2u : 0u); }
static int find_target(txt_input *in, const book_sink_t *sink, const book_compute_selection_t *q,
                       uint64_t bom, uint64_t size, uint64_t *target, unsigned *flags)
{
    book_compute_selection_t scan = *q;
    uint64_t start;
    uint32_t remaining = q->skip_utf8;
    int rc;
    scan.flags = 0;
    if ((rc = slice_start(in, &scan, bom, *target, &start, flags)) < 0) return rc;
    if (fseek(in->file, (long)start, SEEK_SET)) return BOOK_IO_ERROR;
    in->offset = start; in->pending = -1;
    for (;;)
    {
        uint64_t at = input_position(in);
        char encoded[4]; size_t bytes; int c;
        if ((rc = bf_cancel(sink)) < 0) return rc;
        c = codepoint(in);
        if (c == EOF)
        {
            if (ferror(in->file)) return BOOK_IO_ERROR;
            *target = size;
            if (remaining) *flags |= BOOK_SOURCE_CLAMPED;
            return BOOK_OK;
        }
        if (input_position(in) <= *target) continue;
        bytes = c == '\r' || c == '\n' ? 0 : encode((unsigned)c, encoded);
        if (!remaining || (bytes && remaining < bytes)) { *target = at; return BOOK_OK; }
        remaining -= (uint32_t)bytes;
    }
}
static int txt_emit(const book_sink_t *sink, const book_compute_observer_t *observer,
                    const book_compute_selection_t *q, uint32_t start, const char *out, size_t used)
{
    int rc;
    if (!used) return BOOK_OK;
    if ((rc = txt_location(observer, q, start, 0)) < 0) return rc;
    return bf_text(sink, out, used, 0);
}
static int txt_convert(const char *path, const book_sink_t *sink,
                       const book_compute_selection_t *q, const book_compute_observer_t *observer)
{
    txt_input in = {0};
    unsigned char *sample;
    char *out, id[48];
    size_t n, used = 0, i, even = 0, odd = 0;
    int rc = BOOK_OK, c, last_cr = 0, line_start = 1;
    uint64_t target = 0, end = UINT64_MAX, output_start = 0;
    uint32_t text_start = 0;
    unsigned location_flags = 0;
    int target_sent = 0;
    long skip = 0;
    in.pending = -1;
    in.file = fopen(path, "rb");
    if (!in.file) return BOOK_IO_ERROR;
    sample = bf_alloc(4096);
    out = bf_alloc(BF_TEXT + 4);
    if (!sample || !out) { rc = BOOK_NO_MEMORY; goto done; }
    n = fread(sample, 1, 4096, in.file);
    if (ferror(in.file)) { rc = BOOK_IO_ERROR; goto done; }
    if (n >= 3 && !memcmp(sample, "\xef\xbb\xbf", 3)) skip = 3;
    else if (n >= 2 && sample[0] == 0xff && sample[1] == 0xfe) { in.encoding = TXT_LE; skip = 2; }
    else if (n >= 2 && sample[0] == 0xfe && sample[1] == 0xff) { in.encoding = TXT_BE; skip = 2; }
    else
    {
        for (i = 0; i + 1 < n; i += 2) { even += sample[i] == 0; odd += sample[i + 1] == 0; }
        if (n >= 4 && odd > n / 4 && even < n / 8) in.encoding = TXT_LE;
        else if (n >= 4 && even > n / 4 && odd < n / 8) in.encoding = TXT_BE;
        else in.encoding = valid_utf8(sample, n) ? TXT_UTF8 : TXT_GBK;
    }
    output_start = (uint64_t)skip;
    if (q)
    {
        struct stat st;
        if (stat(path, &st) || st.st_size < 0 || (uint64_t)st.st_size > LONG_MAX) { rc = BOOK_IO_ERROR; goto done; }
        target = q->anchor_byte;
        if (target > (uint64_t)st.st_size) { target = (uint64_t)st.st_size; location_flags |= BOOK_SOURCE_CLAMPED; }
        if (target < (uint64_t)skip) { target = (uint64_t)skip; location_flags |= BOOK_SOURCE_CLAMPED; }
        if ((rc = find_target(&in, sink, q, (uint64_t)skip, (uint64_t)st.st_size, &target, &location_flags)) < 0) goto done;
        if (!(q->flags & BOOK_COMPUTE_FULL_SOURCE))
            end = target + (q->slice_bytes ? q->slice_bytes : BOOK_COMPUTE_DEFAULT_SLICE);
        if (end > (uint64_t)st.st_size) end = (uint64_t)st.st_size;
        if ((rc = slice_start(&in, q, (uint64_t)skip, target, &output_start, &location_flags)) < 0) goto done;
        if ((rc = txt_location(observer, q, (uint32_t)output_start, BOOK_SOURCE_BEGIN | location_flags)) < 0) goto done;
    }
    if (fseek(in.file, (long)output_start, SEEK_SET)) { rc = BOOK_IO_ERROR; goto done; }
    in.offset = output_start; in.pending = -1;
    while ((rc = bf_cancel(sink)) == BOOK_OK)
    {
        uint64_t at = input_position(&in);
        if (at >= end) break;
        c = codepoint(&in);
        if (c == EOF) break;
        if (input_position(&in) > end) { in.offset = at; in.pending = -1; break; }
        if (q && !target_sent && input_position(&in) > target)
        {
            if ((rc = txt_emit(sink, observer, q, text_start, out, used)) < 0) break;
            used = 0; target_sent = 1;
            if ((rc = txt_location(observer, q, (uint32_t)at, BOOK_SOURCE_TARGET | location_flags)) < 0) break;
        }
        if (c == '\n' && last_cr) { last_cr = 0; continue; }
        if (line_start)
        {
            if ((rc = txt_location(observer, q, (uint32_t)at, 0)) < 0) break;
            snprintf(id, sizeof(id), "txt-byte-%llu", (unsigned long long)at);
            if ((rc = bf_anchor(sink, id, NULL, 0)) < 0) break;
            line_start = 0;
        }
        last_cr = c == '\r';
        if (c == '\r' || c == '\n')
        {
            if ((rc = txt_emit(sink, observer, q, text_start, out, used)) < 0 || (rc = bf_para(sink)) < 0) break;
            used = 0; line_start = 1;
        }
        else
        {
            char encoded[4]; size_t bytes = encode((unsigned)c, encoded);
            if (used + bytes > (q ? BOOK_COMPUTE_SOURCE_TEXT_MAX : BF_TEXT))
            {
                if ((rc = txt_emit(sink, observer, q, text_start, out, used)) < 0) break;
                used = 0;
            }
            if (!used) text_start = (uint32_t)at;
            memcpy(out + used, encoded, bytes); used += bytes;
        }
    }
    if (rc == BOOK_OK && ferror(in.file)) rc = BOOK_IO_ERROR;
    if (rc == BOOK_OK && (rc = txt_emit(sink, observer, q, text_start, out, used)) == BOOK_OK && !line_start)
        rc = bf_para(sink);
    if (rc == BOOK_OK && q && !target_sent)
        rc = txt_location(observer, q, (uint32_t)input_position(&in), BOOK_SOURCE_TARGET | location_flags);
    if (rc == BOOK_OK) rc = txt_location(observer, q, (uint32_t)input_position(&in), BOOK_SOURCE_END);
done:
    bf_free(sample); bf_free(out); fclose(in.file);
    return rc;
}
int book_compute_txt(const char *path, const book_sink_t *sink)
{ return txt_convert(path, sink, NULL, NULL); }
int book_compute_txt_slice(const char *path, const book_sink_t *sink,
    const book_compute_selection_t *selection, const book_compute_observer_t *observer)
{ return txt_convert(path, sink, selection, observer); }
