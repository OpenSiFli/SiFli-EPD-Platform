/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <rtthread.h>
#include "book_codec.h"
#include "mobi.h"
#include "index.h"
#include "util.h"
#include "parse_rawml.h"
#include "mobi_port.h"

typedef struct {
    unsigned part, offset, level;
    char *title;
    size_t order;
} target_t;
typedef struct {
    MOBIData *m;
    MOBIRawml *raw;
    const book_sink_t *sink;
    book_sink_t proxy;
    target_t *targets;
    size_t target_count, target_capacity;
    MOBIPart *first_part, **parts;
    size_t part_count;
    unsigned part;
    unsigned cover_uid;
    int have_cover, cover_in_body;
    int result;
} conversion_t;
typedef struct {
    conversion_t *ctx;
    MOBIPart *part;
    const unsigned char *data;
    size_t size, pos, record_pos, record_size, record_index;
    char *record;
    size_t record_capacity;
    int error;
} source_t;
typedef struct {
    source_t source;
    size_t target;
    size_t out_pos, out_size;
    unsigned part;
    int eof;
    char tag[8192];
    char output[16384];
} markup_t;
typedef struct {
    conversion_t *ctx;
    MOBIPart *part;
    MOBIPdbRecord *record;
    unsigned char *data;
    size_t size, pos;
    int own_data;
} resource_t;

static int result_of(MOBI_RET result)
{
    int status = mobi_port_status();
    if (status != BOOK_OK) return status;
    switch (result) {
    case MOBI_SUCCESS: return BOOK_OK;
    case MOBI_MALLOC_FAILED: return BOOK_NO_MEMORY;
    case MOBI_FILE_NOT_FOUND: return BOOK_IO_ERROR;
    case MOBI_FILE_ENCRYPTED: case MOBI_FILE_UNSUPPORTED:
    case MOBI_DRM_UNSUPPORTED: return BOOK_UNSUPPORTED;
    default: return BOOK_ERROR;
    }
}
static int checked(conversion_t *ctx, int result)
{
    if (ctx->result == BOOK_OK && result != BOOK_OK) ctx->result = result;
    if (ctx->result == BOOK_OK) ctx->result = mobi_port_status();
    return ctx->result;
}
static int index_parts(conversion_t *ctx)
{
    MOBIPart *part;
    size_t i = 0;
    ctx->first_part = ctx->raw->markup ? ctx->raw->markup : ctx->raw->flow;
    for (part = ctx->first_part; part; part = part->next) {
        if (ctx->part_count == UINT_MAX || ctx->part_count == SIZE_MAX / sizeof(*ctx->parts))
            return BOOK_NO_MEMORY;
        ++ctx->part_count;
    }
    /* A single stream needs no separately allocated random-access index. */
    if (ctx->part_count <= 1) return BOOK_OK;
    ctx->parts = mobi_alloc(ctx->part_count * sizeof(*ctx->parts));
    if (!ctx->parts) return result_of(MOBI_MALLOC_FAILED);
    for (part = ctx->first_part; part; part = part->next) ctx->parts[i++] = part;
    return BOOK_OK;
}
static MOBIPart *part_at(const conversion_t *ctx, unsigned index)
{
    if (index >= ctx->part_count) return NULL;
    return ctx->parts ? ctx->parts[index] : ctx->first_part;
}
static int add_target(conversion_t *ctx, unsigned part, unsigned offset,
                      const char *title, unsigned level)
{
    target_t *target;
    MOBIPart *p = part_at(ctx, part);
    if (!p || offset > p->size) return BOOK_ERROR;
    if (ctx->target_count == ctx->target_capacity) {
        const size_t limit = SIZE_MAX / sizeof(*target);
        size_t capacity = ctx->target_capacity;
        target_t *targets;
        if (capacity == limit) return BOOK_NO_MEMORY;
        capacity = !capacity ? (limit < 32 ? limit : 32) :
            (capacity > limit / 2 ? limit : capacity * 2);
        targets = mobi_realloc(ctx->targets, capacity * sizeof(*targets));
        if (!targets) return result_of(MOBI_MALLOC_FAILED);
        ctx->targets = targets;
        ctx->target_capacity = capacity;
    }
    target = &ctx->targets[ctx->target_count];
    *target = (target_t){ part, offset, 0, NULL, ctx->target_count };
    if (title && *title) {
        target->title = bk_mobi_strdup(title);
        if (!target->title) return result_of(MOBI_MALLOC_FAILED);
        target->level = level;
    }
    ++ctx->target_count;
    return BOOK_OK;
}
static int target_after(const target_t *a, const target_t *b)
{
    if (a->part != b->part) return a->part > b->part;
    if (a->offset != b->offset) return a->offset > b->offset;
    return a->order > b->order;
}
static void target_sift(target_t *targets, size_t root, size_t count)
{
    target_t value = targets[root];
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && target_after(&targets[child + 1], &targets[child])) ++child;
        if (!target_after(&targets[child], &value)) break;
        targets[root] = targets[child];
        root = child;
    }
    targets[root] = value;
}
static int finish_targets(conversion_t *ctx)
{
    size_t i, count = ctx->target_count, used = 0;
    /* In-place heapsort bounds worst-case work to O(n log n). The original
     * collection order makes the first nonempty TOC title win after sorting. */
    for (i = count / 2; i; --i) {
        if ((i & 1023u) == 0 && mobi_port_status() != BOOK_OK) return mobi_port_status();
        target_sift(ctx->targets, i - 1, count);
    }
    for (i = count; i > 1; --i) {
        target_t value;
        if ((i & 1023u) == 0 && mobi_port_status() != BOOK_OK) return mobi_port_status();
        value = ctx->targets[0];
        ctx->targets[0] = ctx->targets[i - 1];
        ctx->targets[i - 1] = value;
        target_sift(ctx->targets, 0, i - 1);
    }
    for (i = 0; i < count; ++i) {
        target_t *target = &ctx->targets[i];
        target_t *previous = used ? &ctx->targets[used - 1] : NULL;
        if (previous && previous->part == target->part && previous->offset == target->offset) {
            if (!previous->title && target->title) {
                previous->title = target->title;
                previous->level = target->level;
            } else mobi_release(target->title);
        } else ctx->targets[used++] = *target;
    }
    ctx->target_count = used;
    return mobi_port_status();
}
static void free_targets(conversion_t *ctx)
{
    size_t i;
    for (i = 0; i < ctx->target_count; ++i) mobi_release(ctx->targets[i].title);
    mobi_release(ctx->targets);
    mobi_release(ctx->parts);
}
static void target_id(char *buffer, size_t size, unsigned part, unsigned offset)
{
    snprintf(buffer, size, "mobi:%u:%u", part, offset);
}
static const target_t *find_target(const conversion_t *ctx, const char *id)
{
    char *end, key[64];
    unsigned long part, offset;
    size_t low = 0, high = ctx->target_count;
    if (strncmp(id, "mobi:", 5)) return NULL;
    part = strtoul(id + 5, &end, 10);
    if (*end != ':' || part > UINT_MAX) return NULL;
    offset = strtoul(end + 1, &end, 10);
    if (*end || offset > UINT_MAX) return NULL;
    target_id(key, sizeof(key), (unsigned)part, (unsigned)offset);
    if (strcmp(key, id)) return NULL;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const target_t *target = &ctx->targets[middle];
        if (target->part < part || (target->part == part && target->offset < offset)) low = middle + 1;
        else high = middle;
    }
    if (low < ctx->target_count && ctx->targets[low].part == part && ctx->targets[low].offset == offset)
        return &ctx->targets[low];
    return NULL;
}
static int init_source(source_t *s, conversion_t *ctx, MOBIPart *part)
{
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    if (!part) return BOOK_ERROR;
    if (ctx->raw->skel && ctx->raw->skel->entries_count) {
        int result = result_of(mobi_reconstruct_part_data_ex(ctx->raw, part->uid, &part, false));
        if (result != BOOK_OK) return result;
        s->part = part;
        s->data = part->data;
        s->size = part->size;
    } else {
        /* KF7 and KF8 without SKEL have a single logical HTML stream. */
        s->size = ctx->raw->fdst && ctx->raw->fdst->fdst_section_count ?
            ctx->raw->fdst->fdst_section_ends[0] : ctx->m->rh->text_length;
        s->record_capacity = (ctx->m->rh->text_record_size ? ctx->m->rh->text_record_size : 4096) + 1u;
        s->record = mobi_alloc(s->record_capacity);
        if (!s->record) return result_of(MOBI_MALLOC_FAILED);
    }
    return BOOK_OK;
}
static void finish_source(source_t *s)
{
    if (s->data && s->part) { mobi_release(s->part->data); s->part->data = NULL; }
    mobi_release(s->record);
    memset(s, 0, sizeof(*s));
}
static int source_char(source_t *s)
{
    if ((s->pos & 1023u) == 0 && mobi_port_status() != BOOK_OK) {
        s->error = mobi_port_status(); return -1;
    }
    if (s->pos >= s->size) return -1;
    if (s->data) return s->data[s->pos++];
    while (s->record_pos >= s->record_size) {
        size_t count = s->record_capacity;
        MOBI_RET result = mobi_get_rawml_record(s->ctx->m, s->record_index++, s->record, &count);
        if (result != MOBI_SUCCESS) { s->error = result_of(result); return -1; }
        s->record_pos = 0;
        s->record_size = count;
    }
    ++s->pos;
    return (unsigned char)s->record[s->record_pos++];
}
/* Borrow a bounded contiguous span without expanding the whole document. */
static int source_span(source_t *s, const unsigned char **data, size_t *size)
{
    int result = mobi_port_status();
    if (result != BOOK_OK) { s->error = result; return result; }
    if (s->pos >= s->size) { *size = 0; return BOOK_OK; }
    if (s->data) {
        *data = s->data + s->pos;
        *size = s->size - s->pos;
    } else {
        while (s->record_pos >= s->record_size) {
            size_t count = s->record_capacity;
            MOBI_RET ret = mobi_get_rawml_record(s->ctx->m, s->record_index++, s->record, &count);
            if (ret != MOBI_SUCCESS) { s->error = result_of(ret); return s->error; }
            s->record_pos = 0; s->record_size = count;
        }
        *data = (const unsigned char *)s->record + s->record_pos;
        *size = s->record_size - s->record_pos;
        if (*size > s->size - s->pos) *size = s->size - s->pos;
    }
    if (*size > 4096) *size = 4096;
    return BOOK_OK;
}
static void source_consume(source_t *s, size_t size)
{
    s->pos += size;
    if (!s->data) s->record_pos += size;
}
static int read_tag(source_t *source, char *tag, size_t size)
{
    size_t n = 0;
    int quote = 0, ch;
    tag[n++] = '<';
    while ((ch = source_char(source)) >= 0) {
        if (n + 1 >= size) return BOOK_UNSUPPORTED;
        tag[n++] = (char)ch;
        if (n == 4 && !memcmp(tag, "<!--", 4)) {
            int previous = 0, before = 0;
            while ((ch = source_char(source)) >= 0) {
                if (before == '-' && previous == '-' && ch == '>') {
                    strcpy(tag, "<!-- -->"); return BOOK_OK;
                }
                before = previous; previous = ch;
            }
            return source->error ? source->error : BOOK_ERROR;
        }
        if (quote) { if (ch == quote) quote = 0; }
        else if (ch == '\'' || ch == '"') quote = ch;
        else if (ch == '>') { tag[n] = 0; return BOOK_OK; }
    }
    return source->error ? source->error : BOOK_ERROR;
}
static int same(const char *a, size_t n, const char *b)
{
    size_t i;
    if (strlen(b) != n) return 0;
    for (i = 0; i < n; ++i) if (tolower((unsigned char)a[i]) != (unsigned char)b[i]) return 0;
    return 1;
}
static int attribute(const char *tag, const char *name, char *value, size_t size,
                     size_t *begin, size_t *end)
{
    const char *p = tag + 1;
    while (*p && !isspace((unsigned char)*p) && *p != '>') ++p;
    while (*p) {
        const char *key, *start, *finish;
        int quote;
        while (isspace((unsigned char)*p) || *p == '/') ++p;
        if (!*p || *p == '>') break;
        key = p;
        while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '>') ++p;
        finish = p;
        while (isspace((unsigned char)*p)) ++p;
        if (*p != '=') continue;
        ++p;
        while (isspace((unsigned char)*p)) ++p;
        quote = (*p == '\'' || *p == '"') ? *p++ : 0;
        start = p;
        while (*p && (quote ? *p != quote : (!isspace((unsigned char)*p) && *p != '>'))) ++p;
        if (same(key, (size_t)(finish - key), name)) {
            size_t n = (size_t)(p - start);
            if (n >= size) return -1;
            memcpy(value, start, n); value[n] = 0;
            if (begin) *begin = (size_t)(key - tag);
            if (end) *end = (size_t)(p - tag) + (quote && *p ? 1 : 0);
            return 1;
        }
        if (quote && *p) ++p;
    }
    return 0;
}
static int numeric_target(conversion_t *ctx, const char *href, unsigned *part, unsigned *offset)
{
    if (!strncmp(href, "kindle:pos:fid:", 15)) {
        char *end;
        unsigned long fid = strtoul(href + 15, &end, 32), off;
        size_t raw_offset;
        uint32_t number;
        if (strncmp(end, ":off:", 5)) return 0;
        off = strtoul(end + 5, &end, 32);
        if (*end && *end != '?') return 0;
        if (mobi_get_offset_by_posoff(&number, &raw_offset, ctx->raw, fid, off) != MOBI_SUCCESS || raw_offset > UINT_MAX)
            return 0;
        *part = number; *offset = (unsigned)raw_offset; return 1;
    }
    return 0;
}
static int tag_target(conversion_t *ctx, const char *tag, unsigned *part, unsigned *offset,
                      size_t *begin, size_t *end)
{
    char value[1024], *tail;
    unsigned long number;
    int found = attribute(tag, "filepos", value, sizeof(value), begin, end);
    if (found == 1) {
        number = strtoul(value, &tail, 10);
        if (*tail || number > UINT_MAX) return 0;
        *part = 0; *offset = (unsigned)number; return 1;
    }
    found = attribute(tag, "href", value, sizeof(value), begin, end);
    return found == 1 && numeric_target(ctx, value, part, offset);
}
static int read_toc(conversion_t *ctx)
{
    size_t i;
    MOBIIndx *ncx = ctx->raw->ncx;
    if (!ncx) return BOOK_OK;
    for (i = 0; i < ncx->entries_count; ++i) {
        MOBIIndexEntry *entry = &ncx->entries[i];
        uint32_t part = 0, off = 0, fid, cncx, level = 0;
        size_t position;
        char *title = NULL;
        int result;
        if (mobi_is_rawml_kf8(ctx->raw)) {
            if (mobi_get_indxentry_tagvalue(&fid, entry, INDX_TAG_NCX_POSFID) != MOBI_SUCCESS ||
                mobi_get_indxentry_tagvalue(&off, entry, INDX_TAG_NCX_POSOFF) != MOBI_SUCCESS ||
                mobi_get_offset_by_posoff(&part, &position, ctx->raw, fid, off) != MOBI_SUCCESS || position > UINT_MAX) continue;
            off = (uint32_t)position;
        } else if (mobi_get_indxentry_tagvalue(&off, entry, INDX_TAG_NCX_FILEPOS) != MOBI_SUCCESS) continue;
        mobi_get_indxentry_tagvalue(&level, entry, INDX_TAG_NCX_LEVEL);
        if (mobi_get_indxentry_tagvalue(&cncx, entry, INDX_TAG_NCX_TEXT_CNCX) == MOBI_SUCCESS)
            title = mobi_get_cncx_string_utf8(ncx->cncx_record, cncx, ncx->encoding);
        result = add_target(ctx, part, off, title, level + 1);
        mobi_release(title);
        if (result != BOOK_OK) return result;
    }
    return BOOK_OK;
}
static int scan_links(markup_t *stream)
{
    for (;;) {
        const unsigned char *data, *tag;
        size_t size;
        unsigned part, offset;
        size_t begin, end;
        int result = source_span(&stream->source, &data, &size);
        if (result != BOOK_OK) return result;
        if (!size) break;
        tag = memchr(data, '<', size);
        source_consume(&stream->source, tag ? (size_t)(tag - data) + 1 : size);
        if (!tag) continue;
        result = read_tag(&stream->source, stream->tag, sizeof(stream->tag));
        if (result != BOOK_OK) return result;
        if (stream->source.ctx->have_cover) {
            char ref[1024];
            unsigned long uid = ULONG_MAX;
            if (attribute(stream->tag, "recindex", ref, sizeof(ref), NULL, NULL) == 1)
                uid = strtoul(ref, NULL, 10) - 1;
            else if (attribute(stream->tag, "src", ref, sizeof(ref), NULL, NULL) == 1 &&
                     !strncmp(ref, "kindle:embed:", 13)) uid = strtoul(ref + 13, NULL, 32) - 1;
            if (uid == stream->source.ctx->cover_uid) stream->source.ctx->cover_in_body = 1;
        }
        if (tag_target(stream->source.ctx, stream->tag, &part, &offset, &begin, &end)) {
            result = add_target(stream->source.ctx, part, offset, NULL, 0);
            if (result != BOOK_OK) return result;
        }
    }
    return stream->source.error;
}
static int transform_tag(markup_t *s, size_t *length)
{
    unsigned part, offset;
    size_t begin, end;
    char value[1024], replacement[120];
    int found = tag_target(s->source.ctx, s->tag, &part, &offset, &begin, &end);
    if (found) snprintf(replacement, sizeof(replacement), "href=\"#mobi:%u:%u\"", part, offset);
    else {
        found = attribute(s->tag, "recindex", value, sizeof(value), &begin, &end);
        if (found == 1) {
            unsigned long record = strtoul(value, NULL, 10);
            if (!record || record > UINT_MAX) return BOOK_ERROR;
            snprintf(replacement, sizeof(replacement), "src=\"mobi-resource-%u\"", (unsigned)record - 1);
        }
    }
    if (!found) {
        const char *names[] = { "src", "href", "xlink:href" };
        unsigned i;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (attribute(s->tag, names[i], value, sizeof(value), &begin, &end) != 1) continue;
            if (!strncmp(value, "kindle:embed:", 13)) {
                unsigned long uid = strtoul(value + 13, NULL, 32);
                if (!uid || uid > UINT_MAX) return BOOK_ERROR;
                snprintf(replacement, sizeof(replacement), "%s=\"mobi-resource-%u\"", names[i], (unsigned)uid - 1);
                found = 1; break;
            }
            if (!strncmp(value, "kindle:flow:", 12)) {
                unsigned long uid = strtoul(value + 12, NULL, 32);
                if (uid > UINT_MAX) return BOOK_ERROR;
                snprintf(replacement, sizeof(replacement), "%s=\"mobi-flow-%u\"", names[i], (unsigned)uid);
                found = 1; break;
            }
        }
    }
    if (found == 1) {
        size_t prefix = begin, suffix = strlen(s->tag + end), replace = strlen(replacement);
        if (*length + prefix + suffix + replace >= sizeof(s->output)) return BOOK_UNSUPPORTED;
        memcpy(s->output + *length, s->tag, prefix); *length += prefix;
        memcpy(s->output + *length, replacement, replace); *length += replace;
        memcpy(s->output + *length, s->tag + end, suffix); *length += suffix;
    } else {
        size_t n = strlen(s->tag);
        if (*length + n >= sizeof(s->output)) return BOOK_UNSUPPORTED;
        memcpy(s->output + *length, s->tag, n); *length += n;
    }
    if (mobi_is_cp1252(s->source.ctx->m)) {
        size_t in = *length, expanded = in, out;
        char converted[8];
        size_t i;
        for (i = 0; i < in; ++i) if ((unsigned char)s->output[i] >= 128) {
            size_t n = sizeof(converted);
            if (mobi_cp1252_to_utf8(converted, s->output + i, &n, 1) != MOBI_SUCCESS) return BOOK_ERROR;
            expanded += n - 1;
        }
        if (expanded > sizeof(s->output)) return BOOK_UNSUPPORTED;
        out = expanded;
        while (in) {
            unsigned char c = (unsigned char)s->output[--in];
            if (c < 128) s->output[--out] = (char)c;
            else {
                size_t n = sizeof(converted);
                if (mobi_cp1252_to_utf8(converted, (const char *)&c, &n, 1) != MOBI_SUCCESS) return BOOK_ERROR;
                out -= n; memcpy(s->output + out, converted, n);
            }
        }
        *length = expanded;
    }
    return BOOK_OK;
}
static int markup_read(void *user, void *buffer, size_t length)
{
    markup_t *s = user;
    size_t copied = 0;
    int result = mobi_port_status();
    if (result != BOOK_OK) return result;
    while (copied < length) {
        if (s->out_pos < s->out_size) {
            size_t n = s->out_size - s->out_pos;
            if (n > length - copied) n = length - copied;
            memcpy((char *)buffer + copied, s->output + s->out_pos, n);
            copied += n; s->out_pos += n; continue;
        }
        if (s->eof) break;
        s->out_size = s->out_pos = 0;
        {
            conversion_t *ctx = s->source.ctx;
            const target_t *target;
            size_t start = s->source.pos;
            int ch = source_char(&s->source);
            if (ch < 0) {
                if (s->source.error) return s->source.error;
                s->eof = 1;
            } else if (ch == '<') {
                result = read_tag(&s->source, s->tag, sizeof(s->tag));
                if (result != BOOK_OK) return result;
            }
            while (s->target < ctx->target_count &&
                   (target = &ctx->targets[s->target])->part == s->part &&
                   (target->offset <= start || (ch == '<' && target->offset < s->source.pos))) {
                int n = snprintf(s->output + s->out_size, sizeof(s->output) - s->out_size,
                    "<a id=\"mobi:%u:%u\"></a>", s->part, target->offset);
                if (n < 0 || (size_t)n >= sizeof(s->output) - s->out_size) return BOOK_UNSUPPORTED;
                s->out_size += (size_t)n;
                ++s->target;
            }
            if (ch == '<') {
                result = transform_tag(s, &s->out_size);
                if (result != BOOK_OK) return result;
            } else if (ch >= 0) {
                if (ch >= 128 && mobi_is_cp1252(s->source.ctx->m)) {
                    char input = (char)ch, out[8]; size_t count = sizeof(out);
                    if (mobi_cp1252_to_utf8(out, &input, &count, 1) != MOBI_SUCCESS) return BOOK_ERROR;
                    memcpy(s->output + s->out_size, out, count); s->out_size += count;
                } else {
                    s->output[s->out_size++] = (char)ch;
                    if (!mobi_is_cp1252(s->source.ctx->m)) {
                        const unsigned char *data, *tag;
                        size_t size;
                        result = source_span(&s->source, &data, &size);
                        if (result != BOOK_OK) return result;
                        if (size) {
                            tag = memchr(data, '<', size);
                            if (tag) size = (size_t)(tag - data);
                            if (s->target < ctx->target_count &&
                                (target = &ctx->targets[s->target])->part == s->part) {
                                size_t distance = target->offset > s->source.pos ?
                                    target->offset - s->source.pos : 0;
                                if (size > distance) size = distance;
                            }
                            if (size > sizeof(s->output) - s->out_size) size = sizeof(s->output) - s->out_size;
                            memcpy(s->output + s->out_size, data, size);
                            s->out_size += size;
                            source_consume(&s->source, size);
                        }
                    }
                }
            }
        }
    }
    return (int)copied;
}
static int resource_read(void *user, void *buffer, size_t length)
{
    resource_t *r = user;
    int result = mobi_port_status();
    if (result != BOOK_OK) return result;
    if (length > r->size - r->pos) length = r->size - r->pos;
    if (length > INT_MAX) length = INT_MAX;
    memcpy(buffer, r->data + r->pos, length); r->pos += length;
    return (int)length;
}
static int resource_seek(void *user, uint64_t offset)
{
    resource_t *r = user;
    if (offset > r->size) return BOOK_IO_ERROR;
    r->pos = (size_t)offset; return mobi_port_status();
}
static void resource_close(void *user)
{
    resource_t *r = user;
    if (r->part) r->part->data = NULL;
    if (r->record) { mobi_release(r->record->data); r->record->data = NULL; }
    else if (r->own_data) mobi_release(r->data);
    mobi_release(r);
}
static int open_resource(void *user, const char *base, const char *reference, book_stream_t *stream)
{
    conversion_t *ctx = user;
    MOBIPart *part = NULL;
    resource_t *r;
    const char *p;
    unsigned long uid;
    (void)base;
    if (mobi_port_status() != BOOK_OK) return mobi_port_status();
    if ((p = strstr(reference, "kindle:embed:")) != NULL) {
        uid = strtoul(p + 13, NULL, 32);
        if (!uid) return BOOK_ERROR;
        part = mobi_get_resource_by_uid(ctx->raw, uid - 1);
    } else if ((p = strstr(reference, "mobi-resource-")) != NULL) {
        part = mobi_get_resource_by_uid(ctx->raw, strtoul(p + 14, NULL, 10));
    } else if ((p = strstr(reference, "resource")) != NULL) {
        part = mobi_get_resource_by_uid(ctx->raw, strtoul(p + 8, NULL, 10));
    }
    r = mobi_calloc(1, sizeof(*r));
    if (!r) return result_of(MOBI_MALLOC_FAILED);
    r->ctx = ctx;
    if (part) {
        int result;
        if (part->type != T_JPG && part->type != T_PNG && part->type != T_GIF && part->type != T_BMP) {
            mobi_release(r); return BOOK_UNSUPPORTED;
        }
        result = result_of(mobi_load_resource_data(ctx->raw, part));
        if (result != BOOK_OK) { mobi_release(r); return result; }
        r->part = part;
        r->record = mobi_get_record_by_seqnumber(ctx->m, part->record_index);
        r->data = part->data; r->size = part->size;
    } else if (((p = strstr(reference, "mobi-flow-")) != NULL ||
                (p = strstr(reference, "kindle:flow:")) != NULL) && ctx->raw->fdst) {
        MOBIPart *range = NULL;
        int result;
        uid = !strncmp(p, "mobi-flow-", 10) ? strtoul(p + 10, NULL, 10) : strtoul(p + 12, NULL, 32);
        if (uid >= ctx->raw->fdst->fdst_section_count) { mobi_release(r); return BOOK_ERROR; }
        result = result_of(mobi_reconstruct_rawml_range(ctx->raw,
            ctx->raw->fdst->fdst_section_starts[uid], ctx->raw->fdst->fdst_section_ends[uid], &range));
        if (result != BOOK_OK) { mobi_release(r); return result; }
        r->data = range->data; r->size = range->size; r->own_data = 1;
        mobi_release(range);
    } else { mobi_release(r); return BOOK_UNSUPPORTED; }
    *stream = (book_stream_t){ r, r->size, resource_read, resource_seek, resource_close };
    return BOOK_OK;
}
static bool proxy_cancelled(void *user)
{
    conversion_t *ctx = user;
    return ctx->result == BOOK_CANCELLED || mobi_port_status() == BOOK_CANCELLED;
}
static int proxy_text(void *user, const char *text, size_t length, unsigned style)
{
    conversion_t *ctx = user;
    if (checked(ctx, BOOK_OK)) return ctx->result;
    return checked(ctx, ctx->sink->text(ctx->sink->user, text, length, style));
}
static int proxy_paragraph(void *user)
{
    conversion_t *ctx = user;
    if (checked(ctx, BOOK_OK)) return ctx->result;
    return checked(ctx, ctx->sink->paragraph ? ctx->sink->paragraph(ctx->sink->user) : BOOK_OK);
}
static int proxy_anchor(void *user, const char *id, const char *title, unsigned level)
{
    conversion_t *ctx = user;
    const char *canonical = strstr(id, "mobi:");
    char buffer[1200];
    const target_t *target;
    if (!canonical) {
        const char *fragment = strrchr(id, '#');
        snprintf(buffer, sizeof(buffer), "mobi-id:%u:%s", ctx->part, fragment ? fragment + 1 : id);
        canonical = buffer;
    } else {
        target = find_target(ctx, canonical);
        if (target && target->title) { title = target->title; level = target->level; }
    }
    if (checked(ctx, BOOK_OK)) return ctx->result;
    return checked(ctx, ctx->sink->anchor ? ctx->sink->anchor(ctx->sink->user, canonical, title, level) : BOOK_OK);
}
static int proxy_link(void *user, const char *target, bool begin)
{
    conversion_t *ctx = user;
    const char *canonical = target ? strstr(target, "mobi:") : NULL;
    char buffer[1200];
    if (!canonical && target && strchr(target, '#')) {
        const char *fragment = strrchr(target, '#');
        unsigned part = ctx->part;
        const char *p = strstr(target, "part");
        if (p) part = (unsigned)strtoul(p + 4, NULL, 10);
        snprintf(buffer, sizeof(buffer), "mobi-id:%u:%s", part, fragment + 1);
        canonical = buffer;
    }
    if (checked(ctx, BOOK_OK)) return ctx->result;
    return checked(ctx, ctx->sink->link ? ctx->sink->link(ctx->sink->user, canonical ? canonical : target, begin) : BOOK_OK);
}
static int proxy_image(void *user, const char *id, book_stream_t *stream)
{
    conversion_t *ctx = user;
    if (checked(ctx, BOOK_OK)) return ctx->result;
    return checked(ctx, ctx->sink->image ? ctx->sink->image(ctx->sink->user, id, stream) : BOOK_OK);
}
static int proxy_svg(void *user, const char *xml, size_t length, const char *base,
                     const book_resources_t *resources)
{
    conversion_t *ctx = user;
    if (checked(ctx, BOOK_OK)) return ctx->result;
    return checked(ctx, ctx->sink->svg
        ? ctx->sink->svg(ctx->sink->user, xml, length, base, resources) : BOOK_UNSUPPORTED);
}
static int convert(const char *path, const book_sink_t *sink,
                   const book_allocator_t *allocator, char *error, size_t error_size)
{
    conversion_t ctx;
    markup_t *html = NULL;
    book_resources_t resources;
    char *title, *author;
    unsigned pass, index;
    size_t next_target = 0;
    int result;
    if (error && error_size) *error = 0;
    if (!path || !sink || !sink->text || !sink->markup || !allocator || !allocator->alloc || !allocator->free)
        return BOOK_ERROR;
    result = mobi_port_start(allocator, sink);
    if (result != BOOK_OK) return result;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sink = sink;
    ctx.proxy = (book_sink_t){ &ctx, proxy_cancelled, proxy_text, proxy_paragraph, proxy_anchor,
                             proxy_image, NULL, proxy_link, NULL, proxy_svg, NULL };
    resources = (book_resources_t){ &ctx, open_resource };
    ctx.m = mobi_init();
    if (!ctx.m) { result = result_of(MOBI_MALLOC_FAILED); goto done; }
    result = result_of(mobi_load_filename(ctx.m, path));
    if (result != BOOK_OK) goto done;
    if (mobi_is_encrypted(ctx.m) || mobi_is_replica(ctx.m)) { result = BOOK_UNSUPPORTED; goto done; }
    result = mobi_port_status();
    if (result != BOOK_OK) goto done;
    ctx.raw = mobi_init_rawml(ctx.m);
    if (!ctx.raw) { result = result_of(MOBI_MALLOC_FAILED); goto done; }
    result = result_of(mobi_parse_rawml_opt(ctx.raw, ctx.m, true, false, false));
    if (result != BOOK_OK) goto done;
    result = index_parts(&ctx);
    if (result != BOOK_OK) goto done;
    title = mobi_meta_get_title(ctx.m); author = mobi_meta_get_author(ctx.m);
    result = sink->metadata ? sink->metadata(sink->user, title, author) : BOOK_OK;
    mobi_release(title); mobi_release(author);
    if (result != BOOK_OK) goto done;
    result = read_toc(&ctx);
    if (result != BOOK_OK) goto done;
    {
        MOBIExthHeader *cover = mobi_get_exthrecord_by_tag(ctx.m, EXTH_COVEROFFSET);
        if (cover && cover->data && cover->size && cover->size <= 4) {
            MOBIPart *part;
            ctx.cover_uid = mobi_decode_exthvalue(cover->data, cover->size);
            part = mobi_get_resource_by_uid(ctx.raw, ctx.cover_uid);
            ctx.have_cover = part && (part->type == T_JPG || part->type == T_PNG ||
                                     part->type == T_GIF || part->type == T_BMP);
        }
    }
    html = mobi_calloc(1, sizeof(*html));
    if (!html) { result = result_of(MOBI_MALLOC_FAILED); goto done; }
    /* A bounded first pass finds backward and cross-part targets before any
     * markup is emitted. Only offsets and TOC labels survive each part. */
    for (pass = 0; pass < 2; ++pass) {
        MOBIPart *part;
        if (pass && ctx.have_cover && !ctx.cover_in_body && sink->image) {
            book_stream_t cover;
            char id[64];
            snprintf(id, sizeof(id), "mobi-resource-%u", ctx.cover_uid);
            result = open_resource(&ctx, "", id, &cover);
            if (result != BOOK_OK) goto done;
            result = proxy_image(&ctx, id, &cover);
            cover.close(cover.user);
            if (result != BOOK_OK) goto done;
        }
        for (index = 0, part = ctx.first_part; part; ++index, part = part->next) {
            book_stream_t stream;
            char base[64];
            memset(html, 0, sizeof(*html));
            result = init_source(&html->source, &ctx, part);
            if (result != BOOK_OK) goto done;
            ctx.part = html->part = index;
            if (!pass) {
                result = add_target(&ctx, index, 0, NULL, 0);
                if (result == BOOK_OK) result = scan_links(html);
            } else {
                while (next_target < ctx.target_count && ctx.targets[next_target].part < index) ++next_target;
                html->target = next_target;
                snprintf(base, sizeof(base), "part%u.html", index);
                stream = (book_stream_t){ html, 0, markup_read, NULL, NULL };
                result = sink->markup(&ctx.proxy, &stream, base, &resources);
                if (result == BOOK_OK) result = checked(&ctx, BOOK_OK);
                next_target = html->target;
            }
            finish_source(&html->source);
            if (result != BOOK_OK) goto done;
        }
        if (!pass) {
            result = finish_targets(&ctx);
            if (result != BOOK_OK) goto done;
        }
    }
done:
    if (html) finish_source(&html->source);
    free_targets(&ctx);
    if (result == BOOK_OK) result = mobi_port_status();
    if (error && error_size && result != BOOK_OK)
        snprintf(error, error_size, "%s", result == BOOK_CANCELLED ? "MOBI conversion cancelled" :
            result == BOOK_NO_MEMORY ? "MOBI memory allocation failed" :
            result == BOOK_NO_SPACE ? "Document cache storage full" : result == BOOK_IO_ERROR ? "MOBI input/output failed" :
            result == BOOK_UNSUPPORTED ? "Unsupported or encrypted MOBI content" : "Invalid MOBI document");
    /* Also covers allocations orphaned by third-party early error returns. */
    mobi_port_finish();
    return result;
}
__attribute__((visibility("default"))) const book_convert_api_t book_converter = {
    BOOK_CODEC_ABI, sizeof(book_convert_api_t), convert
};
