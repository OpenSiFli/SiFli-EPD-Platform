/* SPDX-License-Identifier: Apache-2.0 */
#include "ai_sse.h"
#include <string.h>

static bool utf8(unsigned char c, unsigned *left, unsigned *value, unsigned *min)
{
    if (*left) {
        if ((c & 0xc0) != 0x80) return false;
        *value = (*value << 6) | (c & 63);
        if (--*left == 0 && (*value < *min || *value > 0x10ffff ||
                           (*value >= 0xd800 && *value <= 0xdfff))) return false;
    } else if (c < 0x80) { if (!c) return false; }
    else if (c >= 0xc2 && c <= 0xdf) { *left = 1; *value = c & 31; *min = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { *left = 2; *value = c & 15; *min = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { *left = 3; *value = c & 7; *min = 0x10000; }
    else return false;
    return true;
}

bool ai_utf8_valid(const char *text, size_t size)
{
    unsigned left = 0, value = 0, min = 0;
    size_t i;
    if (!text && size) return false;
    for (i = 0; i < size; ++i)
        if (!utf8((unsigned char)text[i], &left, &value, &min)) return false;
    return left == 0;
}

static epd_ai_error_t append(ai_sse_t *s, char c)
{
    if (s->size == EPD_AI_EVENT_LIMIT) return EPD_AI_LIMIT;
    if (s->size + 1 >= s->capacity) {
        size_t n = s->capacity ? s->capacity * 2 : 256;
        char *p;
        if (n > EPD_AI_EVENT_LIMIT + 1u) n = EPD_AI_EVENT_LIMIT + 1u;
        p = ai_realloc(s->data, n);
        if (!p) return EPD_AI_NO_MEMORY;
        s->data = p; s->capacity = n;
    }
    s->data[s->size++] = c; s->data[s->size] = 0;
    return EPD_AI_OK;
}

static epd_ai_error_t field_start(ai_sse_t *s)
{
    s->field[s->field_size] = 0;
    s->mode = !strcmp(s->field, "data") ? 1 : !strcmp(s->field, "event") ? 2 : 3;
    if (s->mode == 1) {
        epd_ai_error_t e = EPD_AI_OK;
        if (s->has_data) e = append(s, '\n');
        s->has_data = true;
        return e;
    }
    if (s->mode == 2) s->event_size = 0;
    return EPD_AI_OK;
}

static epd_ai_error_t line_end(ai_sse_t *s)
{
    epd_ai_error_t e = EPD_AI_OK;
    if (!s->line_started) {
        if (s->has_data)
            e = s->emit(s->user, s->event, s->data ? s->data : "", s->size);
        s->size = 0; s->has_data = false; s->event[0] = 0; s->event_size = 0;
    } else {
        if (!s->colon) e = field_start(s);
        if (s->mode == 2) s->event[s->event_size] = 0;
    }
    s->line_size = s->field_size = 0; s->mode = 0;
    s->colon = s->skip_space = s->line_started = false;
    return e;
}

static epd_ai_error_t byte(ai_sse_t *s, unsigned char c)
{
    epd_ai_error_t e;
    if (s->cr) { s->cr = false; if (c == '\n') return EPD_AI_OK; }
    if (c == '\r' || c == '\n') {
        if (s->utf_left) return EPD_AI_PROTOCOL;
        s->cr = c == '\r';
        return line_end(s);
    }
    if (!utf8(c, &s->utf_left, &s->utf_value, &s->utf_min)) return EPD_AI_PROTOCOL;
    if (++s->line_size > EPD_AI_EVENT_LIMIT + 16u) return EPD_AI_LIMIT;
    s->line_started = true;
    if (!s->colon) {
        if (c == ':') {
            s->colon = s->skip_space = true;
            return field_start(s);
        }
        if (s->field_size < sizeof(s->field) - 1) s->field[s->field_size++] = (char)c;
        else s->mode = 3;
        return EPD_AI_OK;
    }
    if (s->skip_space) { s->skip_space = false; if (c == ' ') return EPD_AI_OK; }
    if (s->mode == 1) return append(s, (char)c);
    if (s->mode == 2) {
        if (s->event_size == sizeof(s->event) - 1) return EPD_AI_LIMIT;
        s->event[s->event_size++] = (char)c;
    }
    e = EPD_AI_OK;
    return e;
}

void ai_sse_init(ai_sse_t *s, ai_sse_event_fn emit, void *user)
{ memset(s, 0, sizeof(*s)); s->emit = emit; s->user = user; }

epd_ai_error_t ai_sse_feed(ai_sse_t *s, const void *data, size_t size)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < size && !s->stopped; ++i) {
        epd_ai_error_t e;
        if (s->bom < 3) {
            if (!s->bom && p[i] != 0xef) s->bom = 3;
            else {
                unsigned char expected = s->bom == 0 ? 0xef : s->bom == 1 ? 0xbb : 0xbf;
                if (p[i] != expected) {
                    unsigned prefix = s->bom;
                    s->bom = 3;
                    e = byte(s, 0xef); if (e) return e;
                    if (prefix == 2) { e = byte(s, 0xbb); if (e) return e; }
                    e = byte(s, p[i]); if (e) return e;
                    continue;
                }
                ++s->bom; continue;
            }
        }
        e = byte(s, p[i]);
        if (e != EPD_AI_OK) return e;
    }
    return EPD_AI_OK;
}

bool ai_sse_pending(const ai_sse_t *s)
{ return s->line_started || s->has_data || s->utf_left || (s->bom && s->bom < 3); }

void ai_sse_clear(ai_sse_t *s)
{ ai_free(s->data); memset(s, 0, sizeof(*s)); }
