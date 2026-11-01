#include "compute_reader_font.h"
#include "font_metadata.h"
#include <limits.h>
#include <string.h>
#include <math.h>

static void stream_seek(crf_context *c, size_t offset);
static void stream_read(crf_context *c, void *dst, size_t bytes);
static void fail(crf_context *c, crf_status error);
static int failed(crf_context *c);
static void *font_alloc(size_t bytes, void *user);
static void font_free(void *pointer, void *user);

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_GLYF_ONLY
#define STBTT_HEAP_FACTOR_SIZE_32 50
#define STBTT_HEAP_FACTOR_SIZE_128 20
#define STBTT_HEAP_FACTOR_SIZE_DEFAULT 10
#define STBTT_STREAM_TYPE crf_context *
#define STBTT_STREAM_SEEK(s, x) stream_seek((s), (x))
#define STBTT_STREAM_READ(s, x, y) stream_read((s), (x), (y))
#define STBTT_STREAM_ERROR(s) failed(s)
#define STBTT_STREAM_FAIL(s) fail((s), CRF_INVALID_FONT)
#define STBTT_USERDATA_ERROR(u) failed((crf_context *)(u))
#define STBTT_malloc(x, u) font_alloc((x), (u))
#define STBTT_free(x, u) font_free((x), (u))
/* Use the repository's SDK implementation directly, without an LVGL build. */
#include "../../SiFli-SDK/external/lvgl_v9/src/libs/tiny_ttf/stb_truetype_htcw.h"

typedef union allocation allocation;
union allocation {
    struct { allocation *prev, *next; size_t bytes; } h;
    long double alignment;
    void *pointer_alignment;
};

typedef struct {
    crf_codepoint input;
    crf_style style;
    crf_glyph glyph;
} metric_entry;
#define METRIC_SLOTS 256u

struct crf_context {
    crf_config config;
    crf_font_identity identity;
    stbtt_fontinfo info;
    crf_stats stats;
    allocation *allocations;
    uint8_t *cache;
    uint32_t tags[CRF_MAX_CACHE_BYTES / CRF_CACHE_BLOCK_BYTES];
    uint32_t ages[CRF_MAX_CACHE_BYTES / CRF_CACHE_BLOCK_BYTES], clock;
    struct { uint32_t offset, length; } metadata[7];
    metric_entry metrics[METRIC_SLOTS];
    uint32_t metric_count, metric_next;
    uint32_t slots, recent_slot, position, operations, wait_offset, wait_length;
    int ascent, descent, line_gap;
    unsigned units_per_em;
    int ready, busy;
    crf_status error;
};

static void fail(crf_context *c, crf_status error)
{
    if (c && c->error == CRF_OK) c->error = error;
}

static int failed(crf_context *c)
{
    if (!c) return 1;
    if (c->error == CRF_OK && c->config.cancelled &&
        c->config.cancelled(c->config.cancel_user)) fail(c, CRF_CANCELLED);
    return c->error != CRF_OK;
}

static int operation(crf_context *c)
{
    if (failed(c)) return 0;
    if (++c->operations > c->config.operation_limit) fail(c, CRF_LIMIT);
    return c->error == CRF_OK;
}

static void stream_seek(crf_context *c, size_t offset)
{
    if (!operation(c)) return;
    if (offset > c->identity.file_size) { fail(c, CRF_INVALID_FONT); return; }
    c->position = (uint32_t)offset;
}

/* Keep frequently reused font tables from competing with every new outline.
 * Unused table capacity is available to outlines; reserve at least eight
 * blocks for streaming outlines. Small caches retain ordinary LRU behavior.
 * Classification is needed only on a miss and uses the validated directory. */
static uint32_t cache_victim(crf_context *c, uint32_t offset, uint32_t oldest)
{
    uint32_t i, tables = 0, table = c->slots, outline = c->slots;
    if (!c->ready || !c->identity.source_id || c->slots <= 8u) return oldest;
    for (i = 0; i < c->slots; ++i) {
        if (c->tags[i] == UINT32_MAX) return i;
        if (crf_read_is_metadata(c, c->tags[i] * CRF_CACHE_BLOCK_BYTES,
                                CRF_CACHE_BLOCK_BYTES)) {
            ++tables;
            if (table == c->slots || c->ages[i] < c->ages[table]) table = i;
        } else if (outline == c->slots || c->ages[i] < c->ages[outline]) outline = i;
    }
    if (tables > c->slots - 8u || (tables >= c->slots - 8u &&
        crf_read_is_metadata(c, offset, CRF_CACHE_BLOCK_BYTES))) return table;
    return outline < c->slots ? outline : oldest;
}

static void stream_read(crf_context *c, void *dst, size_t bytes)
{
    uint8_t *out = (uint8_t *)dst;
    size_t remaining = bytes;
    if (!operation(c)) { memset(dst, 0, bytes); return; }
    if (c->position > c->identity.file_size || bytes > c->identity.file_size - c->position) {
        fail(c, CRF_INVALID_FONT);
        memset(dst, 0, bytes);
        return;
    }
    /* operation() already polled before the bounds check. Keep a poll at
     * every subsequent block, and after read_at() returns below. */
    while (remaining) {
        if (remaining != bytes && failed(c)) break;
        uint32_t block = c->position / CRF_CACHE_BLOCK_BYTES;
        uint32_t slot, oldest = 0;
        uint32_t offset = block * CRF_CACHE_BLOCK_BYTES;
        uint32_t length = c->identity.file_size - offset;
        uint32_t within = c->position - offset;
        size_t take;
        uint8_t *data;
        slot = c->recent_slot;
        if (c->tags[slot] != block) {
            for (slot = 0; slot < c->slots; ++slot) {
                if (c->tags[slot] == block) break;
                if (c->ages[slot] < c->ages[oldest]) oldest = slot;
            }
            if (slot == c->slots) slot = cache_victim(c, offset, oldest);
        }
        data = c->cache + slot * CRF_CACHE_BLOCK_BYTES;
        if (length > CRF_CACHE_BLOCK_BYTES) length = CRF_CACHE_BLOCK_BYTES;
        if (c->tags[slot] != block) {
            size_t got = 0;
            crf_status status;
            c->tags[slot] = UINT32_MAX;
            ++c->stats.read_calls;
            status = c->config.read_at(c->config.read_user, &c->identity,
                                      offset, data, length, &got);
            if (status != CRF_OK && status != CRF_NEED_DATA && status != CRF_IO_ERROR &&
                status != CRF_SHORT_READ && status != CRF_STALE_SOURCE && status != CRF_CANCELLED)
                status = CRF_IO_ERROR;
            if (status == CRF_OK && got != length) status = CRF_SHORT_READ;
            if (status == CRF_NEED_DATA) { c->wait_offset = offset; c->wait_length = length; }
            if (status != CRF_OK) { fail(c, status); break; }
            c->stats.read_bytes += got;
            if (failed(c)) break;
            c->tags[slot] = block;
        } else ++c->stats.cache_hits;
        if (++c->clock == 0) { memset(c->ages, 0, sizeof(c->ages)); c->clock = 1; }
        c->ages[slot] = c->clock;
        c->recent_slot = slot;
        take = length - within;
        if (take > remaining) take = remaining;
        memcpy(out, data + within, take);
        out += take;
        c->position += (uint32_t)take;
        remaining -= take;
    }
    if (remaining) memset(out, 0, remaining);
}

static void *font_alloc(size_t bytes, void *user)
{
    crf_context *c = (crf_context *)user;
    allocation *a;
    if (failed(c)) return NULL;
    if (bytes > SIZE_MAX - sizeof(*a) ||
        bytes + sizeof(*a) > c->config.memory_limit - c->stats.memory_current) {
        fail(c, CRF_NO_MEMORY); return NULL;
    }
    a = (allocation *)c->config.alloc(c->config.alloc_user, sizeof(*a) + bytes);
    if (!a) { fail(c, CRF_NO_MEMORY); return NULL; }
    a->h.bytes = sizeof(*a) + bytes;
    a->h.prev = NULL;
    a->h.next = c->allocations;
    if (a->h.next) a->h.next->h.prev = a;
    c->allocations = a;
    c->stats.memory_current += a->h.bytes;
    if (c->stats.memory_peak < c->stats.memory_current) c->stats.memory_peak = c->stats.memory_current;
    return a + 1;
}

static void font_free(void *pointer, void *user)
{
    crf_context *c = (crf_context *)user;
    allocation *a;
    if (!pointer) return;
    a = (allocation *)pointer - 1;
    if (a->h.prev) a->h.prev->h.next = a->h.next;
    else c->allocations = a->h.next;
    if (a->h.next) a->h.next->h.prev = a->h.prev;
    c->stats.memory_current -= a->h.bytes;
    c->config.free(c->config.alloc_user, a);
}

static void cleanup(crf_context *c)
{
    while (c->allocations) font_free(c->allocations + 1, c);
}

void crf_config_init(crf_config *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->cache_bytes = 16u * 1024u;
    config->memory_limit = 256u * 1024u;
    config->operation_limit = 1048576u;
}

size_t crf_context_bytes(void) { return sizeof(crf_context); }

crf_status crf_create(const crf_config *config, crf_context **out)
{
    crf_context *c;
    if (!out) return CRF_INVALID_ARGUMENT;
    *out = NULL;
    if (!config || !config->alloc || !config->free || !config->read_at ||
        !config->cache_bytes || config->cache_bytes % CRF_CACHE_BLOCK_BYTES ||
        config->cache_bytes > CRF_MAX_CACHE_BYTES ||
        config->memory_limit > CRF_MAX_MEMORY_BYTES ||
        config->memory_limit < sizeof(*c) + config->cache_bytes ||
        !config->operation_limit || config->operation_limit > 1048576u) return CRF_INVALID_ARGUMENT;
    c = (crf_context *)config->alloc(config->alloc_user, sizeof(*c));
    if (!c) return CRF_NO_MEMORY;
    memset(c, 0, sizeof(*c));
    c->config = *config;
    c->cache = (uint8_t *)config->alloc(config->alloc_user, config->cache_bytes);
    if (!c->cache) { config->free(config->alloc_user, c); return CRF_NO_MEMORY; }
    c->slots = config->cache_bytes / CRF_CACHE_BLOCK_BYTES;
    memset(c->tags, 0xff, sizeof(c->tags));
    memset(c->ages, 0, sizeof(c->ages));
    c->clock = 0;
    c->stats.memory_current = c->stats.memory_peak = sizeof(*c) + config->cache_bytes;
    *out = c;
    return CRF_OK;
}

void crf_destroy(crf_context *c)
{
    crf_config config;
    if (!c || c->busy) return;
    config = c->config;
    cleanup(c);
    config.free(config.alloc_user, c->cache);
    config.free(config.alloc_user, c);
}

static int identity_equal(const crf_font_identity *a, const crf_font_identity *b)
{
    return a->source_id == b->source_id && a->signature == b->signature &&
           a->generation == b->generation && a->file_size == b->file_size && a->face_index == b->face_index;
}

int crf_read_is_metadata(const crf_context *c, uint32_t offset, size_t length)
{
    unsigned i;
    if (!c || !c->ready || !c->identity.source_id || !length ||
        offset > c->identity.file_size || length > c->identity.file_size - offset) return 0;
    for (i = 0; i < 7; ++i)
        if (c->metadata[i].length && c->metadata[i].offset < offset + length &&
            offset < c->metadata[i].offset + c->metadata[i].length) return 1;
    return 0;
}

static crf_status finish(crf_context *c, crf_result *r)
{
    cleanup(c);
    r->status = c->error;
    r->wait_offset = c->wait_offset;
    r->wait_length = c->wait_length;
    if (c->error == CRF_STALE_SOURCE || c->error == CRF_IO_ERROR ||
        c->error == CRF_SHORT_READ || c->error == CRF_INVALID_FONT) {
        c->ready = 0;
        memset(c->tags, 0xff, sizeof(c->tags));
    }
    c->busy = 0;
    return r->status;
}

/* Bound table-directory traversal and verify required table extents before
 * entering stb. The source owner still validates fonts when importing them. */
static int validate_directory(crf_context *c, uint32_t offset)
{
    unsigned i, n;
    uint32_t glyf = 0, head = 0, hhea = 0, hmtx_len = 0, loca_len = 0, maxp = 0;
    unsigned glyphs, metrics, loca_format;
    uint32_t magic = ttULONG(c, offset);
    if (magic != 0x00010000u && magic != 0x74727565u) {
        fail(c, magic == 0x4f54544fu ? CRF_UNSUPPORTED_FONT : CRF_INVALID_FONT); return 0;
    }
    n = ttUSHORT(c, offset + 4);
    if (!n || n > 256u) { fail(c, CRF_INVALID_FONT); return 0; }
    for (i = 0; i < n && !failed(c); ++i) {
        uint32_t entry = offset + 12u + i * 16u;
        uint32_t tag = ttULONG(c, entry), at = ttULONG(c, entry + 8), len = ttULONG(c, entry + 12);
        if (at > c->identity.file_size || len > c->identity.file_size - at) {
            fail(c, CRF_INVALID_FONT); break;
        }
        {
            static const uint32_t tags[7] = {
                0x636d6170u, 0x6c6f6361u, 0x686d7478u, 0x68656164u,
                0x68686561u, 0x6d617870u, 0x4f532f32u
            };
            unsigned j;
            for (j = 0; j < 7; ++j) if (tag == tags[j]) {
                c->metadata[j].offset = at; c->metadata[j].length = len; break;
            }
        }
        switch (tag) {
        case 0x676c7966u: glyf = at; break;
        case 0x68656164u: if (len >= 54) head = at; break;
        case 0x68686561u: if (len >= 36) hhea = at; break;
        case 0x686d7478u: hmtx_len = len; break;
        case 0x6c6f6361u: loca_len = len; break;
        case 0x6d617870u: if (len >= 6) maxp = at; break;
        default: break;
        }
    }
    if (!glyf || !head || !hhea || !maxp) { fail(c, CRF_INVALID_FONT); return 0; }
    glyphs = ttUSHORT(c, maxp + 4);
    metrics = ttUSHORT(c, hhea + 34);
    loca_format = ttUSHORT(c, head + 50);
    c->units_per_em = ttUSHORT(c, head + 18);
    if (!glyphs || !metrics || metrics > glyphs || loca_format > 1 ||
        c->units_per_em < 16 || c->units_per_em > 16384 ||
        loca_len < (glyphs + 1u) * (loca_format ? 4u : 2u) ||
        hmtx_len < metrics * 4u + (glyphs - metrics) * 2u) fail(c, CRF_INVALID_FONT);
    return !failed(c);
}

crf_status crf_open(crf_context *c, const crf_font_identity *font, crf_result *r)
{
    int offset;
    if (!r) return CRF_INVALID_ARGUMENT;
    memset(r, 0, sizeof(*r));
    if (!c || !font || font->file_size < 12 || font->file_size > CRF_MAX_FILE_BYTES ||
        font->face_index > 65535u) return r->status = CRF_INVALID_ARGUMENT;
    if (c->busy) return r->status = CRF_BUSY;
    c->busy = 1;
    c->ready = 0;
    c->identity = *font;
    r->font = *font;
    c->error = CRF_OK;
    c->operations = c->position = c->wait_offset = c->wait_length = 0;
    memset(c->tags, 0xff, sizeof(c->tags));
    memset(&c->info, 0, sizeof(c->info));
    memset(c->metadata, 0, sizeof(c->metadata));
    c->metric_count = c->metric_next = 0;
    c->info.userdata = c;
    offset = stbtt_GetFontOffsetForIndex(c, (int)font->face_index);
    if (offset < 0 || (uint32_t)offset > font->file_size - 12) fail(c, CRF_INVALID_FONT);
    if (!failed(c) && validate_directory(c, (uint32_t)offset) &&
        !stbtt_InitFont(&c->info, c, offset)) fail(c, CRF_INVALID_FONT);
    if (!failed(c)) {
        unsigned format = ttUSHORT(c, c->info.index_map);
        if (format != 0 && format != 4 && format != 6 && format != 12 && format != 13)
            fail(c, CRF_UNSUPPORTED_FONT);
    }
    if (!failed(c)) {
        stbtt_GetFontVMetrics(&c->info, &c->ascent, &c->descent, &c->line_gap);
        c->ready = !failed(c);
    }
    return finish(c, r);
}

static int scalar(uint32_t cp) { return cp <= 0x10ffffu && !(cp >= 0xd800u && cp <= 0xdfffu); }

static int cached_metric(crf_context *c, crf_codepoint input, crf_style style, crf_glyph *g)
{
    uint32_t i;
    for (i = 0; i < c->metric_count; ++i) {
        metric_entry *m = &c->metrics[i];
        if (m->input.codepoint == input.codepoint && m->input.next_codepoint == input.next_codepoint &&
            !memcmp(&m->style, &style, sizeof(style))) { *g = m->glyph; return 1; }
    }
    return 0;
}

static void describe(crf_context *c, crf_style style, crf_codepoint input, float scale, crf_glyph *g)
{
    int x0, y0, x1, y1, advance, lsb, kern = 0;
    int glyph;
    memset(g, 0, sizeof(*g));
    g->codepoint = input.codepoint;
    g->bitmap_offset = CRF_NO_BITMAP;
    if (input.codepoint < 0x20u || input.codepoint == 0xf8ffu || input.codepoint == 0x200cu) {
        g->flags = CRF_GLYPH_CONTROL; return;
    }
    glyph = stbtt_FindGlyphIndex(&c->info, (int)input.codepoint);
    if (failed(c)) return;
    if (!glyph) { g->flags = CRF_GLYPH_MISSING; return; }
    if (glyph < 0 || glyph >= c->info.numGlyphs) { fail(c, CRF_INVALID_FONT); return; }
    g->glyph_index = (uint32_t)glyph;
    stbtt_GetGlyphBitmapBox(&c->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    if (failed(c)) return;
    if (x1 < x0 || y1 < y0 || x1 - x0 >= (int)CRF_MAX_BOX || y1 - y0 >= (int)CRF_MAX_BOX) {
        fail(c, CRF_LIMIT); return;
    }
    g->box_w = (uint32_t)(x1 - x0 + 1);
    g->box_h = (uint32_t)(y1 - y0 + 1);
    g->stride = (g->box_w + 3u) & ~3u;
    g->ofs_x = x0;
    g->ofs_y = -y1;
    stbtt_GetGlyphHMetrics(&c->info, glyph, &advance, &lsb);
    if (style.kerning && input.next_codepoint && !failed(c)) {
        int next = stbtt_FindGlyphIndex(&c->info, (int)input.next_codepoint);
        if (next < 0 || next >= c->info.numGlyphs) fail(c, CRF_INVALID_FONT);
        else if (next) kern = stbtt_GetGlyphKernAdvance(&c->info, glyph, next);
    }
    if (!failed(c)) {
        float width = scale * (advance + kern) + 0.5f;
        if (width < 0 || width >= 65536.0f) { fail(c, CRF_LIMIT); return; }
        g->advance_px = (uint32_t)width;
        if (style.weight == CRF_WEIGHT_BOLD && g->box_w > 1 && g->box_h > 1) ++g->advance_px;
        if (style.oblique) {
            uint32_t shear = (g->box_h - 1u) / 4u;
            g->box_w += shear;
            g->advance_px += shear;
            g->stride = (g->box_w + 3u) & ~3u;
            if (g->box_w > CRF_MAX_BOX) fail(c, CRF_LIMIT);
        }
    }
}

static void weight_bitmap(crf_context *c, const crf_glyph *g, uint8_t weight, uint8_t *pixels)
{
    uint32_t y, x;
    if (!weight) return;
    for (y = 0; y < g->box_h && !failed(c); ++y) {
        uint8_t *row = pixels + y * g->stride;
        uint8_t previous = 0;
        for (x = 0; x < g->box_w; ++x) {
            uint8_t current = row[x];
            if (weight == CRF_WEIGHT_BOLD) row[x] = current > previous ? current : previous;
            else {
                uint8_t next = x + 1 < g->box_w ? row[x + 1] : 0;
                uint8_t edge = previous < next ? previous : next;
                if (edge > current) edge = current;
                row[x] = (uint8_t)(((unsigned)current + edge + 1) / 2);
            }
            previous = current;
        }
    }
}

crf_status crf_batch(crf_context *c, const crf_font_identity *expected,
                     crf_style style, const crf_codepoint *input, uint32_t count,
                     int render, crf_glyph *glyphs, uint32_t glyph_capacity,
                     uint8_t *pixels, uint32_t pixel_capacity, crf_result *r)
{
    uint32_t i;
    float scale;
    if (!r) return CRF_INVALID_ARGUMENT;
    memset(r, 0, sizeof(*r));
    r->style = style;
    if (!c || !expected || (count && !input) || (glyph_capacity && !glyphs) ||
        (pixel_capacity && !pixels) || !style.size_px || style.size_px > CRF_MAX_SIZE_PX ||
        style.weight > CRF_WEIGHT_LIGHT || style.kerning > 1 || style.oblique > 1 ||
        style.reserved[0] || style.reserved[1] || style.reserved[2] || (render != 0 && render != 1) ||
        count > CRF_MAX_BATCH) return r->status = CRF_INVALID_ARGUMENT;
    r->font = c->identity;
    if (c->busy) return r->status = CRF_BUSY;
    if (!c->ready) return r->status = CRF_NOT_OPEN;
    if (!identity_equal(expected, &c->identity)) return r->status = CRF_STALE_SOURCE;
    c->busy = 1;
    c->error = CRF_OK;
    c->wait_offset = c->wait_length = c->operations = 0;
    scale = (float)style.size_px / (float)c->units_per_em;
    r->metrics.ascent = (int32_t)(scale * c->ascent);
    r->metrics.descent = (int32_t)(scale * c->descent);
    r->metrics.line_gap = (int32_t)(scale * c->line_gap);
    r->metrics.line_height = (int32_t)(scale * (c->ascent - c->descent + c->line_gap));
    r->metrics.base_line = (int32_t)(scale * (c->line_gap - c->descent));
    for (i = 0; i < count && !failed(c); ++i) {
        crf_glyph g;
        uint32_t bytes;
        c->operations = 0;
        if (!scalar(input[i].codepoint) || !scalar(input[i].next_codepoint)) {
            fail(c, CRF_INVALID_ARGUMENT); break;
        }
        if (i >= glyph_capacity) { fail(c, CRF_OUTPUT_FULL); break; }
        if (!cached_metric(c, input[i], style, &g)) {
            metric_entry *m;
            describe(c, style, input[i], scale, &g);
            if (failed(c)) break;
            m = &c->metrics[c->metric_next];
            m->input = input[i]; m->style = style; m->glyph = g;
            c->metric_next = (c->metric_next + 1u) % METRIC_SLOTS;
            if (c->metric_count < METRIC_SLOTS) ++c->metric_count;
        }
        if (failed(c)) break;
        bytes = g.stride * g.box_h;
        if (render && bytes) {
            crf_glyph base = g;
            uint32_t y;
            if (style.oblique) base.box_w -= (g.box_h - 1u) / 4u;
            r->required_bytes = r->bitmap_bytes + bytes;
            if (bytes > pixel_capacity - r->bitmap_bytes) { fail(c, CRF_OUTPUT_FULL); break; }
            memset(pixels + r->bitmap_bytes, 0, bytes);
            stbtt_MakeGlyphBitmap(&c->info, pixels + r->bitmap_bytes, (int)base.box_w, (int)g.box_h,
                                 (int)g.stride, scale, scale, (int)g.glyph_index);
            if (!failed(c)) weight_bitmap(c, &base, style.weight, pixels + r->bitmap_bytes);
            for (y = 0; style.oblique && y < g.box_h && !failed(c); ++y) {
                uint32_t shift = (g.box_h - 1u - y) / 4u;
                uint8_t *row = pixels + r->bitmap_bytes + y * g.stride;
                memmove(row + shift, row, base.box_w);
                memset(row, 0, shift);
            }
            cleanup(c);
            if (failed(c)) break;
            g.bitmap_offset = r->bitmap_bytes;
            g.bitmap_size = bytes;
        }
        if (failed(c)) break;
        glyphs[i] = g;
        if (render) r->bitmap_bytes += bytes;
        ++r->completed;
    }
    (void)failed(c);
    return finish(c, r);
}

crf_status crf_measure_pair(crf_context *c, const crf_font_identity *expected,
                            crf_style style, uint32_t cp, uint32_t next,
                            crf_glyph *glyph, crf_result *r)
{
    crf_codepoint pair = {cp, next};
    return crf_batch(c, expected, style, &pair, 1, 0, glyph, 1, NULL, 0, r);
}

void crf_get_stats(const crf_context *c, crf_stats *stats)
{
    if (stats) { if (c) *stats = c->stats; else memset(stats, 0, sizeof(*stats)); }
}

const char *crf_status_string(crf_status status)
{
    static const char *const names[] = {"ok", "invalid argument", "not open", "busy", "need data",
        "I/O error", "short read", "stale source", "invalid font", "unsupported font", "no memory",
        "output full", "cancelled", "limit"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}
