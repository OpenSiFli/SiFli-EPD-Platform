/* SPDX-License-Identifier: Apache-2.0 */
#include "reading_data_wire.h"
#include "epd_compute_memory.h"
#include <stdbool.h>
#include <string.h>

#define RD_FNV_SEED 2166136261u
#define RD_HEADER_BYTES 36u
typedef struct {
    uint32_t magic, version, layout, end, count, toc_count, toc_scanned, complete, checksum;
} index_header_t;
typedef struct {
    uint32_t sequence, offset, total, hash;
    uint32_t magic, version, layout, end, page_max, text_limit;
    index_header_t header;
    uint8_t item[8];
    uint32_t cursors, toc, last_record, last_byte, last_toc;
    bool indexing;
    uint32_t meta[12], dep[5], source_bytes, title_bytes, cache_flags;
    uint32_t dep_used, path_left, dependency_count, segment;
    bool cache_check, string_end, dots, opaque;
} data_session_t;
typedef char rd_header_size[(sizeof(index_header_t) == RD_HEADER_BYTES) ? 1 : -1];

static uint32_t little32(const unsigned char *p)
{ return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void string_begin(data_session_t *s)
{ s->string_end = s->opaque = false; s->segment = 0; s->dots = true; }

static bool string_byte(data_session_t *s, unsigned char byte, bool path, bool explicit_id)
{
    if (s->string_end) return true;
    if (explicit_id && !s->segment && byte == '@') s->opaque = true;
    if (path && !s->opaque) {
        if (byte && (byte < 32 || byte == '\\' || byte == ':')) return false;
        if (!byte || byte == '/') {
            if (!s->segment || (s->dots && s->segment <= 2)) return false;
            s->segment = 0; s->dots = true;
        } else { ++s->segment; if (byte != '.') s->dots = false; }
    } else ++s->segment;
    if (!byte) s->string_end = true;
    return true;
}
static bool cache_byte(data_session_t *s, unsigned char byte)
{
    uint32_t pos = s->offset++;
    s->hash = (s->hash ^ byte) * 16777619u;
    if (pos < 48) {
        ((unsigned char *)s->meta)[pos] = byte;
        return s->offset != 48 || (s->meta[0] <= 1 && s->meta[1] < 2 &&
            s->meta[8] <= INT32_MAX && s->meta[10] <= 512 &&
            s->meta[10] <= (s->total - 48 - s->source_bytes - s->title_bytes) / 22);
    }
    uint32_t title = 48 + s->source_bytes, deps = title + s->title_bytes;
    if (pos < deps) {
        if (pos == 48 || pos == title) string_begin(s);
        bool source = pos < title;
        if (!string_byte(s, byte, source, pos == 48 && (s->cache_flags & 1))) return false;
        return (pos + 1 != title && pos + 1 != deps) || s->string_end;
    }
    if (s->dependency_count >= s->meta[10]) return false;
    if (s->dep_used < 20) {
        ((unsigned char *)s->dep)[s->dep_used++] = byte;
        if (s->dep_used == 20) {
            if (s->dep[0] >= 2 || s->dep[1] > 1 || s->dep[4] < 2 || s->dep[4] > 256 ||
                s->dep[4] > s->total - s->offset) return false;
            s->path_left = s->dep[4]; string_begin(s);
        }
    } else {
        if (!s->path_left || (!byte != (s->path_left == 1)) || !string_byte(s, byte, true, false)) return false;
        if (!--s->path_left) { ++s->dependency_count; s->dep_used = 0; }
    }
    return true;
}

static bool header_valid(data_session_t *s)
{
    const index_header_t *h = &s->header;
    return h->magic == s->magic && h->version == s->version && h->layout == s->layout && h->end == s->end &&
        h->count && h->count <= s->page_max && h->complete <= 1 && h->toc_scanned <= h->end &&
        (uint64_t)RD_HEADER_BYTES + ((uint64_t)h->count + 1u + h->toc_count) * 8u == s->total;
}

static bool index_byte(data_session_t *s, unsigned char byte)
{
    uint32_t pos = s->offset++;
    if (pos < 32 || pos >= RD_HEADER_BYTES) s->hash = (s->hash ^ byte) * 16777619u;
    if (pos < RD_HEADER_BYTES) {
        /* Shared wire words are little-endian on both supported cores. */
        ((unsigned char *)&s->header)[pos] = byte;
        return s->offset != RD_HEADER_BYTES || header_valid(s);
    }
    uint32_t part = (pos - RD_HEADER_BYTES) % 8u;
    s->item[part] = byte;
    if (part != 7) return true;
    uint32_t record = little32(s->item), value = little32(s->item + 4);
    if (s->cursors <= s->header.count) {
        if (record > s->end || value > s->text_limit || (record == s->end && value) ||
            (!s->cursors && (record || value))) return false;
        if (s->cursors && !(s->last_record < record || (s->last_record == record && s->last_byte < value)) &&
            !(s->header.complete && s->header.count == 1 && !s->end && !s->last_record && !s->last_byte)) return false;
        ++s->cursors; s->last_record = record; s->last_byte = value;
    } else {
        if (record >= s->header.toc_scanned || (s->toc && s->last_toc >= record)) return false;
        ++s->toc; s->last_toc = record;
    }
    return true;
}

static int32_t open_data(void **state, const epd_compute_wire_t *request)
{
    if (!state) return EPD_COMPUTE_INVALID;
    *state = NULL;
    if (!request || request->region_count || request->params.length || request->result.length || !request->step_limit)
        return EPD_COMPUTE_INVALID;
    if (epd_compute_signals(request) & EPD_COMPUTE_CANCEL) return EPD_COMPUTE_CANCELLED;
    *state = epd_compute_calloc(1, sizeof(data_session_t));
    return *state ? EPD_COMPUTE_OK : EPD_COMPUTE_NO_MEMORY;
}

static int32_t step_data(void *state, const epd_compute_wire_t *request)
{
    data_session_t *s = state;
    if (!s || !request || request->region_count != 2 || request->params.region != 0 || request->params.offset ||
        request->result.region != 1 || request->result.offset || request->params.length < sizeof(rd_request_t) ||
        request->params.length > sizeof(rd_request_t) + RD_CHUNK || request->result.length != sizeof(rd_reply_t) ||
        !request->step_limit || !request->regions) return EPD_COMPUTE_INVALID;
    const epd_compute_region_t *regions = (const void *)(uintptr_t)request->regions;
    if (epd_compute_validate_regions(regions, 2) != EPD_COMPUTE_OK ||
        regions[0].access != EPD_COMPUTE_READ || regions[1].access != EPD_COMPUTE_WRITE) return EPD_COMPUTE_INVALID;
    const rd_request_t *q = epd_compute_resolve(request, request->params, EPD_COMPUTE_READ);
    rd_reply_t *reply = epd_compute_resolve(request, request->result, EPD_COMPUTE_WRITE);
    if (!q || !reply || q->version != RD_VERSION || q->size != request->params.length ||
        q->size != sizeof(*q) + q->length || q->length > request->step_limit || q->length > RD_CHUNK)
        return EPD_COMPUTE_INVALID;
    uint32_t next = s->sequence + 1u;
    if (!next) next = 1;
    if (q->sequence != next) return EPD_COMPUTE_INVALID;
    if (epd_compute_signals(request) & EPD_COMPUTE_CANCEL) return EPD_COMPUTE_CANCELLED;
    data_session_t work = *s;
    const unsigned char *p = (const unsigned char *)(q + 1);
    uint32_t value = 0;
    int status = EPD_COMPUTE_OK;
    if (q->operation == RD_HASH) {
        if (q->offset || q->total || q->magic || q->index_version || q->layout || q->end || q->page_max || q->text_limit ||
            q->skip_offset > q->length || q->skip_size > q->length - q->skip_offset) return EPD_COMPUTE_INVALID;
        value = q->seed;
        for (uint32_t i = 0; i < q->length; ++i) {
            if (!(i % 64u) && (epd_compute_signals(request) & EPD_COMPUTE_CANCEL)) return EPD_COMPUTE_CANCELLED;
            if (i < q->skip_offset || i - q->skip_offset >= q->skip_size) value = (value ^ p[i]) * 16777619u;
        }
    } else if (q->operation == RD_CACHE_BEGIN) {
        if (q->length || q->offset || q->seed || q->skip_offset || q->skip_size || q->end || q->page_max || q->text_limit ||
            !q->magic || q->magic > 256 || !q->index_version || q->index_version > 128 || (q->layout & ~1u) ||
            q->total > 65536 || q->total < 48u + q->magic + q->index_version) return EPD_COMPUTE_INVALID;
        memset(&work, 0, sizeof(work));
        work.cache_check = true; work.total = q->total; work.hash = RD_FNV_SEED;
        work.source_bytes = q->magic; work.title_bytes = q->index_version; work.cache_flags = q->layout;
    } else if (q->operation == RD_CACHE_DATA) {
        if (!s->cache_check || !q->length || q->offset != s->offset || q->total != s->total ||
            q->length > s->total - s->offset || q->seed || q->skip_offset || q->skip_size ||
            q->magic || q->index_version || q->layout || q->end || q->page_max || q->text_limit) return EPD_COMPUTE_INVALID;
        for (uint32_t i = 0; i < q->length; ++i) {
            if (!(i % 64u) && (epd_compute_signals(request) & EPD_COMPUTE_CANCEL)) return EPD_COMPUTE_CANCELLED;
            if (!cache_byte(&work, p[i])) return EPD_COMPUTE_INVALID;
        }
        value = work.hash;
        if (work.offset == work.total) {
            if (work.dependency_count != work.meta[10] || work.dep_used || work.path_left) return EPD_COMPUTE_INVALID;
            work.cache_check = false;
        } else status = EPD_COMPUTE_MORE;
    } else if (q->operation == RD_INDEX_BEGIN) {
        if (q->length || q->offset || q->seed || q->skip_offset || q->skip_size ||
            q->total < RD_HEADER_BYTES + 16u || !q->page_max || q->page_max == UINT32_MAX)
            return EPD_COMPUTE_INVALID;
        memset(&work, 0, sizeof(work));
        work.indexing = true; work.total = q->total; work.hash = RD_FNV_SEED;
        work.magic = q->magic; work.version = q->index_version; work.layout = q->layout;
        work.end = q->end; work.page_max = q->page_max; work.text_limit = q->text_limit;
    } else if (q->operation == RD_INDEX_DATA) {
        if (!s->indexing || !q->length || q->offset != s->offset || q->total != s->total ||
            q->length > s->total - s->offset || q->seed || q->skip_offset || q->skip_size ||
            q->magic || q->index_version || q->layout || q->end || q->page_max || q->text_limit) return EPD_COMPUTE_INVALID;
        for (uint32_t i = 0; i < q->length; ++i) {
            if (!(i % 64u) && (epd_compute_signals(request) & EPD_COMPUTE_CANCEL)) return EPD_COMPUTE_CANCELLED;
            if (!index_byte(&work, p[i])) return EPD_COMPUTE_INVALID;
        }
        value = work.hash;
        if (work.offset == work.total) {
            if (work.cursors != work.header.count + 1u || work.toc != work.header.toc_count ||
                work.hash != work.header.checksum) return EPD_COMPUTE_INVALID;
            work.indexing = false;
        } else status = EPD_COMPUTE_MORE;
    } else return EPD_COMPUTE_INVALID;
    if (epd_compute_signals(request) & EPD_COMPUTE_CANCEL) return EPD_COMPUTE_CANCELLED;
    work.sequence = q->sequence;
    *s = work;
    *reply = (rd_reply_t){RD_VERSION, sizeof(*reply), q->sequence, q->operation, RD_BACKEND_ACPU, q->length, value, 0};
    return status;
}

static void close_data(void *state, const epd_compute_wire_t *request)
{ (void)request; epd_compute_free(state); }

const epd_compute_ops_t epd_reading_data_remote_ops = {open_data, step_data, NULL, close_data};
