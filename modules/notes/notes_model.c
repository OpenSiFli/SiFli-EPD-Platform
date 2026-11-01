#include "notes_model.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NOTES_HOST
#include "epd_memory.h"
#endif

void *note_alloc(size_t n)
{
#ifdef NOTES_HOST
    return calloc(1, n ? n : 1);
#else
    void *p = epd_app_alloc(n ? n : 1, EPD_APP_PSRAM);
    if (p) memset(p, 0, n ? n : 1);
    return p;
#endif
}
void note_free(void *p)
{
#ifdef NOTES_HOST
    free(p);
#else
    epd_app_free(p);
#endif
}
char *note_string(const char *s, size_t n)
{
    if (!s || n > NOTE_TEXT_LIMIT) return NULL;
    char *p = note_alloc(n + 1);
    if (p) memcpy(p, s, n);
    return p;
}
size_t note_word_count(const note_t *note)
{
    size_t count=0;
    for(size_t i=0;i<note->count;++i){const note_block_t *b=note->blocks+i;
        if(b->kind==NOTE_AUDIO||b->kind==NOTE_IMAGE||b->kind==NOTE_RULE)continue;
        for(const unsigned char *p=(const unsigned char *)b->text;*p;++p)if((*p&0xc0)!=0x80)++count;
    }
    return count;
}
bool note_utf8(const char *s, size_t n)
{
    if (!s) return false;
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)s[i++], value, count, minimum;
        if (!c) return false;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { value = c & 31; count = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { value = c & 15; count = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; count = 3; minimum = 0x10000; }
        else return false;
        if (n - i < count) return false;
        while (count--) { c = (unsigned char)s[i++]; if ((c & 0xc0) != 0x80) return false; value = (value << 6) | (c & 63); }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
static bool boundary(const char *s, size_t at)
{ return ((unsigned char)s[at] & 0xc0) != 0x80; }
size_t note_byte_offset(const char *s, size_t character)
{
    size_t at = 0;
    while (s[at] && character) { ++at; while (s[at] && !boundary(s, at)) ++at; --character; }
    return at;
}
void note_init(note_t *n, uint32_t id, uint32_t now)
{ memset(n, 0, sizeof(*n)); n->id = id; n->revision = 1; n->next_block = 1; n->created = n->updated = now; }
static void block_clear(note_block_t *b)
{ note_free(b->text); note_free(b->original); note_free(b->spans); note_free(b->marks); memset(b, 0, sizeof(*b)); }
void note_clear(note_t *n)
{ for (size_t i = 0; i < n->count; ++i) block_clear(&n->blocks[i]); note_free(n->blocks); memset(n, 0, sizeof(*n)); }
size_t note_text_bytes(const note_t *n)
{
    size_t size = 0;
    for (size_t i = 0; i < n->count; ++i) {
        size += strlen(n->blocks[i].text);
        if (n->blocks[i].original) size += strlen(n->blocks[i].original);
    }
    return size;
}
note_block_t *note_find(note_t *n, uint32_t id)
{ for (size_t i = 0; i < n->count; ++i) if (n->blocks[i].id == id) return n->blocks + i; return NULL; }
note_block_t *note_insert(note_t *n, size_t at, note_kind_t kind, const char *text)
{
    if (!text || at > n->count || n->count >= NOTE_BLOCK_LIMIT || kind > NOTE_TRANSCRIPT ||
        !n->next_block || n->next_block == UINT32_MAX || !note_utf8(text, strlen(text)) ||
        strlen(text) + note_text_bytes(n) > NOTE_TEXT_LIMIT) return NULL;
    char *copy = note_string(text, strlen(text));
    note_block_t *blocks = note_alloc((n->count + 1) * sizeof(*blocks));
    if (!copy || !blocks) { note_free(copy); note_free(blocks); return NULL; }
    if (at) memcpy(blocks, n->blocks, at * sizeof(*blocks));
    if (at < n->count) memcpy(blocks + at + 1, n->blocks + at, (n->count - at) * sizeof(*blocks));
    note_free(n->blocks); n->blocks = blocks; ++n->count;
    blocks[at].id = n->next_block++; blocks[at].kind = kind; blocks[at].text = copy; ++n->revision;
    return blocks + at;
}
bool note_remove(note_t *n, uint32_t id)
{
    note_block_t *b = note_find(n, id);
    if (!b) return false;
    size_t at = (size_t)(b - n->blocks);
    block_clear(b); memmove(b, b + 1, (n->count - at - 1) * sizeof(*b)); --n->count; ++n->revision;
    return true;
}
bool note_move(note_t *n, uint32_t id, int direction)
{
    note_block_t *b = note_find(n, id);
    if (!b || (direction != -1 && direction != 1)) return false;
    size_t at = (size_t)(b - n->blocks);
    if ((!at && direction < 0) || (at + 1 == n->count && direction > 0)) return false;
    note_block_t swap = *b; *b = b[direction]; b[direction] = swap; ++n->revision; return true;
}
static unsigned style_at(const note_block_t *b, size_t at)
{
    for (size_t i = 0; i < b->span_count; ++i)
        if (at >= b->spans[i].start && at < b->spans[i].end) return b->spans[i].style;
    return 0;
}
static bool append_span(note_span_t *spans, size_t *count, size_t a, size_t z, unsigned style)
{
    if (!style || a == z) return true;
    if (*count && spans[*count - 1].end == a && spans[*count - 1].style == style) spans[*count - 1].end = (uint32_t)z;
    else {
        if (*count >= NOTE_SPAN_LIMIT) return false;
        spans[*count] = (note_span_t){(uint32_t)a, (uint32_t)z, style}; ++*count;
    }
    return true;
}
void note_edit_clear(note_text_edit_t *edit)
{ note_free(edit->text); note_free(edit->spans); memset(edit, 0, sizeof(*edit)); }
void note_edit_apply(note_block_t *b, note_text_edit_t *edit)
{
    note_free(b->text); note_free(b->spans);
    b->text = edit->text; b->spans = edit->spans; b->span_count = edit->span_count;
    edit->text = NULL; edit->spans = NULL;
}
bool note_edit_prepare(const note_block_t *b, const char *text, note_size_t total, note_text_edit_t *edit)
{
    if (!b || !text || !edit || !b->text) return false;
    size_t old = strlen(b->text), size = strlen(text);
    if (total.text < old || total.text > NOTE_TEXT_LIMIT || size > NOTE_TEXT_LIMIT - (total.text - old)) return false;
    size_t a = 0, suffix = 0;
    while (a < old && a < size && text[a] == b->text[a]) ++a;
    while (a && (!boundary(text, a) || !boundary(b->text, a))) --a;
    while (suffix < old - a && suffix < size - a && b->text[old - suffix - 1] == text[size - suffix - 1]) ++suffix;
    while (suffix && (!boundary(text, size - suffix) || !boundary(b->text, old - suffix))) --suffix;
    /* The common prefix/suffix already belong to the validated document. */
    if (!note_utf8(text + a, size - suffix - a)) return false;
    note_text_edit_t next = {0};
    next.text = note_string(text, size);
    size_t capacity = b->span_count + 2; if (capacity > NOTE_SPAN_LIMIT) capacity = NOTE_SPAN_LIMIT;
    next.spans = b->span_count ? note_alloc(capacity * sizeof(*next.spans)) : NULL;
    if (!next.text || (b->span_count && !next.spans)) { note_edit_clear(&next); return false; }
    /* Copy span intersections in three runs instead of looking up every character. */
    for (size_t i = 0; i < b->span_count; ++i) {
        note_span_t span = b->spans[i];
        if (!append_span(next.spans, &next.span_count, span.start < a ? span.start : a,
                         span.end < a ? span.end : a, span.style)) goto failed;
    }
    if (!append_span(next.spans, &next.span_count, a, size - suffix, style_at(b, a ? a - 1 : a))) goto failed;
    for (size_t i = 0; i < b->span_count; ++i) {
        note_span_t span = b->spans[i];
        size_t start = span.start > old - suffix ? span.start : old - suffix;
        if (span.end <= start) continue;
        if (!append_span(next.spans, &next.span_count, size - (old - start),
                         size - (old - span.end), span.style)) goto failed;
    }
    size_t previous = old + 12 * b->span_count;
    size_t replacement = size + 12 * next.span_count;
    if (total.encoded < previous || total.encoded > 4u * NOTE_TEXT_LIMIT ||
        replacement > 4u * NOTE_TEXT_LIMIT - (total.encoded - previous)) goto failed;
    next.size = (note_size_t){total.text - old + size, total.encoded - previous + replacement};
    *edit = next; return true;
failed:
    note_edit_clear(&next); return false;
}
bool note_edit(note_t *n, uint32_t id, const char *text)
{
    note_block_t *b = note_find(n, id); note_size_t size; note_text_edit_t edit = {0};
    if (!b || !text) return false;
    if (!strcmp(b->text, text)) return true;
    if (!note_measure(n, &size) || !note_edit_prepare(b, text, size, &edit)) return false;
    note_edit_apply(b, &edit); ++n->revision; return true;
}
bool note_style(note_t *n, uint32_t id, uint32_t begin, uint32_t end, unsigned flag)
{
    note_block_t *b = note_find(n, id);
    if (!b || begin >= end || end > strlen(b->text) || !boundary(b->text, begin) ||
        !boundary(b->text, end) || (flag != NOTE_BOLD && flag != NOTE_ITALIC)) return false;
    bool remove = true;
    size_t covered = begin;
    for(size_t i=0;i<b->span_count&&covered<end;++i) {
        if(b->spans[i].end<=covered)continue;
        if(b->spans[i].start>covered||!(b->spans[i].style&flag))break;
        covered=b->spans[i].end;
    }
    remove=covered>=end;
    size_t capacity = 2 * b->span_count + 3; if (capacity > NOTE_SPAN_LIMIT) capacity = NOTE_SPAN_LIMIT;
    note_span_t *spans = note_alloc(capacity * sizeof(*spans));
    if (!spans) return false;
    size_t count = 0, size = strlen(b->text), current = 0;
    for (size_t p = 0; p < size;) {
        size_t z = p + 1; while (z < size && !boundary(b->text, z)) ++z;
        while(current<b->span_count&&b->spans[current].end<=p)++current;
        unsigned style = current<b->span_count&&b->spans[current].start<=p?b->spans[current].style:0;
        if (p >= begin && p < end) style = remove ? style & ~flag : style | flag;
        if (!append_span(spans, &count, p, z, style)) { note_free(spans); return false; }
        p = z;
    }
    note_free(b->spans); b->spans = spans; b->span_count = count; ++n->revision; return true;
}
bool note_mark(note_t *n, uint32_t id, uint32_t ms, const char *label)
{
    note_block_t *b = note_find(n, id);
    if (!b || b->kind != NOTE_AUDIO || b->mark_count >= 256 || strlen(label) >= sizeof(b->marks[0].title)) return false;
    note_mark_t *marks = note_alloc((b->mark_count + 1) * sizeof(*marks));
    if (!marks) return false;
    if (b->mark_count) memcpy(marks, b->marks, b->mark_count * sizeof(*marks));
    marks[b->mark_count].milliseconds = ms; strcpy(marks[b->mark_count].title, label);
    note_free(b->marks); b->marks = marks; ++b->mark_count; ++n->revision; return true;
}
bool note_empty(const note_t *n)
{
    if (*n->title) return false;
    for (size_t i = 0; i < n->count; ++i) {
        const note_block_t *b = n->blocks + i;
        if (*b->attachment || b->kind == NOTE_TASK) return false;
        for (const char *p = b->text; *p; ++p) if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return false;
    }
    return true;
}
void note_display_title(const note_t *n, char *out, size_t capacity)
{
    if (!capacity) return;
    const char *s = n->title;
    if (!*s) for (size_t i = 0; i < n->count; ++i) if (*n->blocks[i].text) { s = n->blocks[i].text; break; }
    if (!*s) s = "未命名笔记";
    size_t length = 0; while(s[length] && s[length]!='\r' && s[length]!='\n')++length;
    if (length >= capacity) length = capacity - 1;
    while (length && !boundary(s, length)) --length;
    memcpy(out, s, length); out[length] = 0;
}
void note_buffer_clear(note_buffer_t *b) { note_free(b->data); memset(b, 0, sizeof(*b)); }
bool note_buffer_append(note_buffer_t *b, const void *p, size_t size)
{
    if (b->failed || size > 4u * NOTE_TEXT_LIMIT || b->size > 4u * NOTE_TEXT_LIMIT - size) { b->failed = true; return false; }
    if (b->size + size + 1 > b->capacity) {
        size_t capacity = (b->size + size + 1 + 1023) & ~(size_t)1023;
        unsigned char *next = note_alloc(capacity);
        if (!next) {
            b->failed = true;
            if (capacity > b->allocation_failed) b->allocation_failed = capacity;
            return false;
        }
        if (b->size) memcpy(next, b->data, b->size);
        note_free(b->data); b->data = next; b->capacity = capacity;
    }
    if (size) memcpy(b->data + b->size, p, size);
    b->size += size; b->data[b->size] = 0; return true;
}
static void put(note_buffer_t *b, uint32_t n)
{ unsigned char bytes[4] = {(unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16), (unsigned char)(n >> 24)}; note_buffer_append(b, bytes, 4); }
static void putstr(note_buffer_t *b, const char *s) { size_t n = s ? strlen(s) : 0; put(b, (uint32_t)n); note_buffer_append(b, s, n); }
static bool encode_size_add(size_t *size, size_t count, size_t unit, size_t limit)
{
    if (count > (limit - *size) / unit) return false;
    *size += count * unit; return true;
}
static bool encode_size(const note_t *n, size_t limit, size_t *out)
{
    size_t size = 40;
    if (size > limit || !encode_size_add(&size, strlen(n->title), 1, limit) ||
        !encode_size_add(&size, strlen(n->notebook), 1, limit)) return false;
    for (size_t i = 0; i < n->count; ++i) {
        const note_block_t *p = n->blocks + i;
        if (!encode_size_add(&size, 1, 40, limit) ||
            !encode_size_add(&size, p->text ? strlen(p->text) : 0, 1, limit) ||
            !encode_size_add(&size, p->original ? strlen(p->original) : 0, 1, limit) ||
            !encode_size_add(&size, strlen(p->attachment), 1, limit) ||
            !encode_size_add(&size, p->span_count, 12, limit) ||
            !encode_size_add(&size, p->mark_count, 8, limit)) return false;
        for (size_t j = 0; j < p->mark_count; ++j)
            if (!encode_size_add(&size, strlen(p->marks[j].title), 1, limit)) return false;
    }
    *out = size; return true;
}
bool note_encode(const note_t *n, note_buffer_t *b)
{
    if (b->failed) return false;
    size_t size;
    if (b->size <= 4u * NOTE_TEXT_LIMIT && encode_size(n, 4u * NOTE_TEXT_LIMIT - b->size, &size)) {
        size_t capacity = b->size + size + 1;
        if (capacity > b->capacity) {
            unsigned char *next = note_alloc(capacity);
            if (next) {
                if (b->size) memcpy(next, b->data, b->size);
                note_free(b->data); b->data = next; b->capacity = capacity;
            }
            else b->allocation_failed = capacity;
        }
    }
    /* If reservation cannot succeed, retain the incremental append failure behavior. */
    put(b, 0x31544f4e); put(b, n->id); put(b, n->revision); put(b, n->next_block);
    put(b, n->created); put(b, n->updated); put(b, (unsigned)n->pinned | ((unsigned)n->deleted << 1));
    putstr(b, n->title); putstr(b, n->notebook); put(b, (uint32_t)n->count);
    for (size_t i = 0; i < n->count; ++i) {
        const note_block_t *p = n->blocks + i;
        put(b, p->id); put(b, p->kind); put(b, p->checked); put(b, p->source); put(b, p->milliseconds);
        putstr(b, p->text); putstr(b, p->original); putstr(b, p->attachment); put(b, (uint32_t)p->span_count);
        for (size_t j = 0; j < p->span_count; ++j) { put(b, p->spans[j].start); put(b, p->spans[j].end); put(b, p->spans[j].style); }
        put(b, (uint32_t)p->mark_count);
        for (size_t j = 0; j < p->mark_count; ++j) { put(b, p->marks[j].milliseconds); putstr(b, p->marks[j].title); }
    }
    return !b->failed;
}
typedef struct { const unsigned char *p; size_t left; bool error; } input_t;
static uint32_t get(input_t *in)
{
    if (in->left < 4) { in->error = true; return 0; }
    const unsigned char *p = in->p; in->p += 4; in->left -= 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static char *getstr(input_t *in, size_t maximum)
{
    size_t n = get(in);
    if (in->error || n > maximum || n > in->left || !note_utf8((const char *)in->p, n)) { in->error = true; return NULL; }
    char *s = note_string((const char *)in->p, n); if (!s) in->error = true;
    in->p += n; in->left -= n; return s;
}
static void getfixed(input_t *in, char *out, size_t capacity)
{ char *s = getstr(in, capacity - 1); if (s) { strcpy(out, s); note_free(s); } }
bool note_attachment_valid(const char *s)
{
    if (!*s) return true;
    if (strncmp(s, "attachments/", 12) || !s[12]) return false;
    for (const char *p = s + 12; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-')) return false;
    return !strstr(s + 12, "..");
}
bool note_decode(note_t *out, const void *data, size_t size)
{
    if (!data || size > 4u * NOTE_TEXT_LIMIT) return false;
    input_t in = {data, size, false}; note_t n = {0};
    if (get(&in) != 0x31544f4e) return false;
    n.id = get(&in); n.revision = get(&in); n.next_block = get(&in); n.created = get(&in); n.updated = get(&in);
    uint32_t flags = get(&in); n.pinned = !!(flags & 1); n.deleted = !!(flags & 2);
    getfixed(&in, n.title, sizeof(n.title)); getfixed(&in, n.notebook, sizeof(n.notebook));
    size_t count = get(&in);
    if (in.error || flags > 3 || !n.id || !n.revision || !n.next_block || count > NOTE_BLOCK_LIMIT) return false;
    n.blocks = note_alloc(count * sizeof(*n.blocks)); if (!n.blocks) return false;
    for (size_t i = 0; i < count && !in.error; ++i) {
        note_block_t *b = n.blocks + i; ++n.count;
        b->id = get(&in); b->kind = (note_kind_t)get(&in); uint32_t checked = get(&in); b->checked = !!checked;
        b->source = get(&in); b->milliseconds = get(&in);
        if (!b->id || b->id >= n.next_block || b->kind > NOTE_TRANSCRIPT || checked > 1) { in.error = true; break; }
        for (size_t j = 0; j < i; ++j) if (n.blocks[j].id == b->id) in.error = true;
        b->text = getstr(&in, NOTE_TEXT_LIMIT); b->original = getstr(&in, NOTE_TEXT_LIMIT);
        getfixed(&in, b->attachment, sizeof(b->attachment));
        size_t spans = get(&in);
        if (in.error || spans > NOTE_SPAN_LIMIT || !note_attachment_valid(b->attachment)) { in.error = true; break; }
        b->spans = note_alloc(spans * sizeof(*b->spans));
        if (!b->spans) { in.error = true; break; }
        for (size_t j = 0; j < spans; ++j) {
            note_span_t *span = b->spans + j;
            span->start = get(&in); span->end = get(&in); span->style = get(&in); ++b->span_count;
            if (in.error || span->start >= span->end || span->end > strlen(b->text) || !span->style || span->style > 3 ||
                (j && span->start < b->spans[j - 1].end) || !boundary(b->text, span->start) || !boundary(b->text, span->end)) in.error = true;
        }
        size_t marks = get(&in);
        if (in.error || marks > 256) { in.error = true; break; }
        b->marks = note_alloc(marks * sizeof(*b->marks)); if (!b->marks) { in.error = true; break; }
        for (size_t j = 0; j < marks; ++j) { b->marks[j].milliseconds = get(&in); getfixed(&in, b->marks[j].title, sizeof(b->marks[j].title)); ++b->mark_count; }
    }
    if (in.error || in.left || note_text_bytes(&n) > NOTE_TEXT_LIMIT) { note_clear(&n); return false; }
    note_clear(out); *out = n; return true;
}
static bool clone_string_valid(const char *s, size_t maximum, size_t *length)
{
    size_t size = 0;
    if (s) while (size <= maximum && s[size]) ++size;
    if (size > maximum || (s && !note_utf8(s, size))) return false;
    *length = size; return true;
}
bool note_measure(const note_t *n, note_size_t *size)
{
    /* Validate editable content and compute the exact text and wire sizes. */
    if (!n || !n->id || !n->revision || !n->next_block || n->count > NOTE_BLOCK_LIMIT ||
        (n->count && !n->blocks)) return false;
    size_t title, notebook, text_bytes = 0, wire_size = 40;
    if (!clone_string_valid(n->title, sizeof(n->title) - 1, &title) ||
        !clone_string_valid(n->notebook, sizeof(n->notebook) - 1, &notebook)) return false;
    wire_size += title + notebook;
    for (size_t i = 0; i < n->count; ++i) {
        const note_block_t *b = n->blocks + i;
        if (!b->id || b->id >= n->next_block || (unsigned)b->kind > NOTE_TRANSCRIPT ||
            b->span_count > NOTE_SPAN_LIMIT || b->mark_count > 256 ||
            (b->span_count && !b->spans) || (b->mark_count && !b->marks)) return false;
        for (size_t j = 0; j < i; ++j) if (n->blocks[j].id == b->id) return false;
        size_t text, original, attachment;
        if (!clone_string_valid(b->text, NOTE_TEXT_LIMIT, &text) ||
            !clone_string_valid(b->original, NOTE_TEXT_LIMIT, &original) ||
            !clone_string_valid(b->attachment, sizeof(b->attachment) - 1, &attachment) ||
            !note_attachment_valid(b->attachment)) return false;
        text_bytes += text + original;
        if (text_bytes > NOTE_TEXT_LIMIT) return false;
        for (size_t j = 0; j < b->span_count; ++j) {
            const note_span_t *span = b->spans + j;
            if (span->start >= span->end || span->end > text || !span->style || span->style > 3 ||
                (j && span->start < b->spans[j - 1].end) ||
                !boundary(b->text, span->start) || !boundary(b->text, span->end)) return false;
        }
        wire_size += 40 + text + original + attachment + 12 * b->span_count + 8 * b->mark_count;
        for (size_t j = 0; j < b->mark_count; ++j) {
            size_t length;
            if (!clone_string_valid(b->marks[j].title, sizeof(b->marks[j].title) - 1, &length)) return false;
            wire_size += length;
        }
        if (wire_size > 4u * NOTE_TEXT_LIMIT) return false;
    }
    if (size) *size = (note_size_t){text_bytes, wire_size};
    return true;
}
bool note_clone(note_t *out, const note_t *source)
{
    if (!out || !note_measure(source, NULL)) return false;
    note_t copy = *source;
    copy.count = 0; copy.blocks = note_alloc(source->count * sizeof(*copy.blocks));
    if (!copy.blocks) return false;
    for (size_t i = 0; i < source->count; ++i) {
        const note_block_t *from = source->blocks + i;
        note_block_t *to = copy.blocks + i;
        *to = *from; to->text = to->original = NULL; to->spans = NULL; to->marks = NULL;
        ++copy.count;
        /* The wire format normalizes absent strings to owned empty strings. */
        to->text = note_string(from->text ? from->text : "", from->text ? strlen(from->text) : 0);
        to->original = note_string(from->original ? from->original : "", from->original ? strlen(from->original) : 0);
        to->spans = note_alloc(from->span_count * sizeof(*to->spans));
        to->marks = note_alloc(from->mark_count * sizeof(*to->marks));
        if (!to->text || !to->original || !to->spans || !to->marks) { note_clear(&copy); return false; }
        if (from->span_count) memcpy(to->spans, from->spans, from->span_count * sizeof(*to->spans));
        if (from->mark_count) memcpy(to->marks, from->marks, from->mark_count * sizeof(*to->marks));
    }
    note_clear(out); *out = copy; return true;
}
static void strout(note_buffer_t *out, const char *s) { note_buffer_append(out, s, strlen(s)); }
static void escaped(note_buffer_t *out, char c)
{ if (strchr("\\`*_{}[]<>#!|", c)) strout(out, "\\"); note_buffer_append(out, &c, 1); }
bool note_markdown(const note_t *n, note_buffer_t *out, bool plain)
{
    if (*n->title) { if (!plain) strout(out, "# "); strout(out, n->title); strout(out, "\n\n"); }
    for (size_t i = 0; i < n->count; ++i) {
        const note_block_t *b = n->blocks + i;
        if (b->kind == NOTE_RULE) { strout(out, "---\n\n"); continue; }
        if (b->kind == NOTE_TASK) strout(out, b->checked ? "- [x] " : "- [ ] ");
        else if (!plain && b->kind == NOTE_HEADING) strout(out, "## ");
        else if (!plain && b->kind == NOTE_QUOTE) strout(out, "> ");
        if (!plain && *b->attachment) {
            strout(out, b->kind == NOTE_IMAGE ? "![" : "[");
            for (const char *p = b->text; *p; ++p) escaped(out, *p);
            strout(out, "]("); strout(out, b->attachment); strout(out, ")");
        } else {
            unsigned previous = 0;
            for (size_t at = 0, length = strlen(b->text); at <= length; ++at) {
                unsigned style = plain || at == length ? 0 : style_at(b, at);
                if (style != previous) {
                    if (previous & NOTE_ITALIC) strout(out, "_");
                    if (previous & NOTE_BOLD) strout(out, "**");
                    if (style & NOTE_BOLD) strout(out, "**");
                    if (style & NOTE_ITALIC) strout(out, "_");
                    previous = style;
                }
                if (at < length) { if (plain) note_buffer_append(out, b->text + at, 1); else escaped(out, b->text[at]); }
            }
        }
        strout(out, "\n\n");
    }
    return !out->failed;
}
static bool closing_marker(const char *text, size_t at, size_t size, const char *marker, size_t length)
{
    for (size_t i = at; i + length <= size; ++i) {
        if (text[i] == '\\') { ++i; continue; }
        if (!memcmp(text + i, marker, length)) return true;
    }
    return false;
}
static bool import_inline(note_t *note, note_block_t *block, const char *line, size_t length)
{
    note_buffer_t text = {0}; note_span_t *spans = note_alloc(NOTE_SPAN_LIMIT * sizeof(*spans));
    if (!spans) return false;
    unsigned style = 0; size_t count = 0; bool ok = true;
    for (size_t pos = 0; pos < length && ok;) {
        bool escaped = line[pos] == '\\' && pos + 1 < length;
        if (escaped) ++pos;
        unsigned flag = 0; size_t marker = 0;
        if (!escaped && line[pos] == '*' && pos + 1 < length && line[pos + 1] == '*') { flag = NOTE_BOLD; marker = 2; }
        else if (!escaped && (line[pos] == '_' || line[pos] == '*')) { flag = NOTE_ITALIC; marker = 1; }
        if (flag && ((style & flag) || closing_marker(line, pos + marker, length, line + pos, marker))) {
            style ^= flag; pos += marker; continue;
        }
        size_t end = pos + 1; while (end < length && !boundary(line, end)) ++end;
        size_t begin = text.size;
        ok = note_buffer_append(&text, line + pos, end - pos) && append_span(spans, &count, begin, text.size, style);
        pos = end;
    }
    if (ok) ok = note_edit(note, block->id, text.data ? (const char *)text.data : "");
    if (ok) { note_free(block->spans); block->spans = spans; block->span_count = count; spans = NULL; }
    note_free(spans); note_buffer_clear(&text); return ok;
}
bool note_import_text(note_t *out, const char *text, size_t size, bool markdown)
{
    if (size >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) { text += 3; size -= 3; }
    if (size > NOTE_TEXT_LIMIT || !note_utf8(text, size)) return false;
    note_t n = {0}; if (!note_clone(&n, out)) return false;
    for (size_t start = 0; start < size;) {
        size_t end = start; while (end < size && text[end] != '\n') ++end;
        size_t length = end - start; if (length && text[start + length - 1] == '\r') --length;
        const char *line = text + start; note_kind_t kind = NOTE_TEXT; bool checked = false;
        if (markdown && !n.count && !*n.title && length >= 2 && !memcmp(line, "# ", 2)) {
            if (length - 2 >= sizeof(n.title)) { note_clear(&n); return false; }
            memcpy(n.title, line + 2, length - 2); n.title[length - 2] = 0; start = end + 1; continue;
        }
        if (markdown && length >= 6 && !memcmp(line, "- [", 3) && line[4] == ']' && line[5] == ' ' &&
            (line[3] == ' ' || line[3] == 'x' || line[3] == 'X')) {
            checked = line[3] != ' '; kind = NOTE_TASK; line += 6; length -= 6;
        } else if (markdown && length >= 2 && line[0] == '>' && line[1] == ' ') { kind = NOTE_QUOTE; line += 2; length -= 2; }
        else if (markdown && length >= 3 && !memcmp(line, "## ", 3)) { kind = NOTE_HEADING; line += 3; length -= 3; }
        else if (markdown && length == 3 && !memcmp(line, "---", 3)) { kind = NOTE_RULE; length = 0; }
        if (length || kind == NOTE_RULE) {
            char *copy = note_string(line, length);
            note_block_t *b = copy ? note_insert(&n, n.count, kind, copy) : NULL;
            note_free(copy);
            if (!b) { note_clear(&n); return false; }
            b->checked = checked;
            if (markdown && !import_inline(&n, b, line, length)) { note_clear(&n); return false; }
        }
        start = end + 1;
    }
    note_clear(out); *out = n; return true;
}
