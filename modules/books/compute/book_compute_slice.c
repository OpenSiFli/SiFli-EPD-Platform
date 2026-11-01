/* SPDX-License-Identifier: Apache-2.0 */
#include "../formats/book_internal.h"
#include "book_compute_internal.h"

typedef struct slice_sink {
    const book_sink_t *sink;
    const book_compute_selection_t *selection;
    const book_compute_observer_t *observer;
    const char *id, *base, *first_anchor;
    uint32_t index, position, start, wanted;
    int began, target; /* -1 defers TARGET to EOF; 0 pending; 1 emitted. */
    char link[BF_PATH];
    char heading_id[BF_PATH], heading_title[512];
    unsigned heading_level;
} slice_sink;
static int location(slice_sink *s, uint32_t at, unsigned flags)
{
    return s->observer && s->observer->location ? s->observer->location(s->observer->user,
        s->index, at, s->selection->anchor_byte, BOOK_SOURCE_BODY_UTF8 | flags, s->id) : BOOK_OK;
}
static int begin(slice_sink *s, uint32_t at)
{
    int rc;
    if (s->began) return BOOK_OK;
    s->began = 1;
    if ((rc = location(s, at, BOOK_SOURCE_BEGIN)) < 0 ||
        (rc = bf_anchor(s->sink, s->id, NULL, 0)) < 0) return rc;
    if (s->heading_id[0]) {
        if ((rc = bf_anchor(s->sink, s->heading_id, NULL, s->heading_level)) < 0) return rc;
        if (s->heading_title[0] &&
            (rc = bf_anchor(s->sink, s->heading_id, s->heading_title, s->heading_level)) < 0) return rc;
    }
    return s->link[0] ? bf_link(s->sink, s->link, true) : BOOK_OK;
}
static bool cancelled(void *u) { return bf_cancel(((slice_sink *)u)->sink) < 0; }
static int emit_text(slice_sink *s, const char *p, size_t n, unsigned style, uint32_t at)
{
    while (n)
    {
        size_t k = n > BOOK_COMPUTE_SOURCE_TEXT_MAX ? BOOK_COMPUTE_SOURCE_TEXT_MAX : n;
        int rc;
        if (k < n) while (k && ((unsigned char)p[k] & 0xc0) == 0x80) --k;
        if (!k) return BOOK_ERROR;
        if ((rc = location(s, at, 0)) < 0 || (rc = bf_text(s->sink, p, k, style)) < 0) return rc;
        p += k; n -= k; at += (uint32_t)k;
        style &= ~BOOK_STYLE_PAGE_START;
    }
    return BOOK_OK;
}
static int text(void *u, const char *p, size_t n, unsigned style)
{
    slice_sink *s = u;
    uint32_t original = s->position;
    size_t skip = 0;
    int rc;
    if (n > UINT32_MAX - original) return BOOK_UNSUPPORTED;
    s->position += (uint32_t)n;
    if (s->position <= s->start) return BOOK_OK;
    if (original < s->start) skip = s->start - original;
    /* Round down to the start of the containing Unicode character. */
    while (skip && skip < n && ((unsigned char)p[skip] & 0xc0) == 0x80) --skip;
    p += skip; n -= skip; original += (uint32_t)skip;
    if (skip) style &= ~BOOK_STYLE_PAGE_START;
    if ((rc = begin(s, original)) < 0) return rc;
    if (!s->target && !s->first_anchor && s->wanted < s->position)
    {
        size_t prefix = s->wanted > original ? s->wanted - original : 0;
        while (prefix && ((unsigned char)p[prefix] & 0xc0) == 0x80) --prefix;
        if (prefix && (rc = emit_text(s, p, prefix, style, original)) < 0) return rc;
        if (prefix) style &= ~BOOK_STYLE_PAGE_START;
        p += prefix; n -= prefix; original += (uint32_t)prefix;
        s->target = 1;
        if ((rc = location(s, original, BOOK_SOURCE_TARGET)) < 0) return rc;
    }
    return emit_text(s, p, n, style, original);
}
static int paragraph(void *u)
{
    slice_sink *s = u;
    return s->began ? bf_para(s->sink) : BOOK_OK;
}
static int anchor(void *u, const char *id, const char *title, unsigned level)
{
    slice_sink *s = u;
    if (s->first_anchor && !strcmp(s->first_anchor, id)) {
        s->first_anchor = NULL; s->wanted = s->position;
    }
    if (!s->began && (level || (title && *title) ||
        (s->sink->heading_target && s->sink->heading_target(s->sink->user, id)))) {
        int rc = bf_copy(s->heading_id, sizeof(s->heading_id), id);
        if (rc < 0) return rc;
        s->heading_title[0] = 0;
        if (title) bf_append(s->heading_title, sizeof(s->heading_title), title, strlen(title));
        s->heading_level = level;
    }
    return s->began && (title || strcmp(id, s->base)) ? bf_anchor(s->sink, id, title, level) : BOOK_OK;
}
static int link(void *u, const char *id, bool open)
{
    slice_sink *s = u;
    int rc = bf_copy(s->link, sizeof(s->link), open ? id : "");
    return rc < 0 ? rc : s->began ? bf_link(s->sink, id, open) : BOOK_OK;
}
static bool heading_target(void *u, const char *id)
{
    slice_sink *s = u;
    return s->sink->heading_target && s->sink->heading_target(s->sink->user, id);
}
static int picture_location(slice_sink *s)
{
    int rc;
    if ((rc = begin(s, s->position)) < 0) return rc;
    /* Images share the current body offset. BODY targets prefer the text at
     * that offset; a chapter without text resolves back to BEGIN at EOF. */
    if (!s->target && s->wanted == s->position && !(s->selection->flags & BOOK_COMPUTE_BODY_ONLY))
    {
        s->target = 1;
        if ((rc = location(s, s->position, BOOK_SOURCE_TARGET)) < 0) return rc;
    }
    return location(s, s->position, 0);
}
static int image(void *u, const char *id, book_stream_t *stream)
{
    slice_sink *s = u; int rc;
    if (s->position < s->start || !s->sink->image) return BOOK_OK;
    if ((rc = picture_location(s)) < 0) return rc;
    return s->sink->image(s->sink->user, id, stream);
}
static int svg(void *u, const char *xml, size_t n, const char *base, const book_resources_t *resources)
{
    slice_sink *s = u; int rc;
    if (s->position < s->start || !s->sink->svg) return BOOK_OK;
    if ((rc = picture_location(s)) < 0) return rc;
    return s->sink->svg(s->sink->user, xml, n, base, resources);
}
typedef struct body_count { const book_sink_t *sink; uint32_t bytes; } body_count;
static bool count_cancel(void *u) { return bf_cancel(((body_count *)u)->sink) < 0; }
static int count_text(void *u, const char *text, size_t n, unsigned style)
{
    body_count *c = u; (void)text; (void)style;
    if (n > UINT32_MAX - c->bytes) return BOOK_UNSUPPORTED;
    c->bytes += (uint32_t)n; return BOOK_OK;
}
static int count_svg(void *u, const char *xml, size_t n, const char *base, const book_resources_t *r)
{ (void)u; (void)xml; (void)n; (void)base; (void)r; return BOOK_OK; }
int book_compute_epub_slice(const book_sink_t *sink, book_stream_t *stream,
    const char *base, const book_resources_t *resources,
    const book_compute_selection_t *selection, const book_compute_observer_t *observer,
    uint32_t index, const char *id, const char *first_anchor)
{
    slice_sink *s = bf_alloc(sizeof(*s)); book_sink_t proxy = {0};
    uint32_t context; int rc;
    if (!s) return BOOK_NO_MEMORY;
    if (selection->skip_utf8 > UINT32_MAX - selection->anchor_byte)
    { bf_free(s); return BOOK_UNSUPPORTED; }
    s->sink = sink; s->selection = selection; s->observer = observer; s->index = index; s->id = id; s->base = base;
    s->first_anchor = selection->flags & BOOK_COMPUTE_FIRST_BODY ? first_anchor : NULL;
    context = selection->flags & BOOK_COMPUTE_CONTEXT ?
        (selection->context_bytes ? selection->context_bytes : BOOK_COMPUTE_DEFAULT_CONTEXT) : 0;
    s->wanted = selection->anchor_byte + selection->skip_utf8;
    if (s->wanted == UINT32_MAX && context)
    {
        body_count count = {sink, 0}; book_sink_t measuring = {0};
        measuring.user = &count; measuring.text = count_text;
        measuring.cancelled = count_cancel; measuring.svg = count_svg;
        rc = book_markup_convert(&measuring, stream, base, resources);
        if (rc < 0) { bf_free(s); return rc; }
        s->wanted = count.bytes;
        s->target = -1;
        if (stream->seek) rc = stream->seek(stream->user, 0);
        else
        {
            rc = bf_finish(stream, BOOK_OK);
            memset(stream, 0, sizeof(*stream));
            if (rc == BOOK_OK) rc = resources && resources->open ?
                resources->open(resources->user, base, "", stream) : BOOK_UNSUPPORTED;
        }
        if (rc < 0) { bf_free(s); return rc; }
    }
    s->start = s->wanted > context ? s->wanted - context : 0;
    proxy.user = s; proxy.cancelled = cancelled; proxy.text = text; proxy.paragraph = paragraph;
    proxy.anchor = anchor; proxy.link = link; proxy.image = image; proxy.svg = svg;
    proxy.heading_target = heading_target;
    rc = s->start ? BOOK_OK : begin(s, 0);
    if (rc == BOOK_OK) rc = book_markup_convert(&proxy, stream, base, resources);
    if (rc == BOOK_OK && !s->began) rc = begin(s, s->position);
    if (rc == BOOK_OK && !s->target && (selection->flags & BOOK_COMPUTE_BODY_ONLY) &&
        ((!s->wanted && !s->position) || (selection->flags & BOOK_COMPUTE_FIRST_BODY))) {
        rc = location(s, 0, BOOK_SOURCE_TARGET | BOOK_SOURCE_TARGET_BEGIN);
        s->target = 1;
    }
    if (rc == BOOK_OK && s->target <= 0)
        rc = location(s, s->position, BOOK_SOURCE_TARGET |
            (s->wanted > s->position ? BOOK_SOURCE_CLAMPED : 0));
    if (rc == BOOK_OK) rc = location(s, s->position, BOOK_SOURCE_END);
    bf_free(s);
    return rc;
}
