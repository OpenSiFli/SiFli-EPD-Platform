#include "reader_page_cache.h"
#include <string.h>
#include <stdint.h>

const char *reader_page_cache_text(const reader_cached_page_t *p)
{ return (const char *)(p->runs + p->count); }

static unsigned distance(unsigned a, unsigned b) { return a > b ? a - b : b - a; }
static void remove_page(reader_page_cache_t *c, reader_cached_page_t **at)
{
    reader_cached_page_t *p = *at;
    *at = p->next; c->bytes -= p->allocation; --c->count; c->release(p);
}
void reader_page_cache_clear(reader_page_cache_t *c)
{ while (c->first) remove_page(c, &c->first); }

void reader_page_cache_window(reader_page_cache_t *c, unsigned wanted, uint32_t begin, uint32_t end)
{
    if (!c->window_dirty && c->wanted == wanted && c->begin == begin && c->end == end) return;
    c->wanted = wanted; c->begin = begin; c->end = end;
    c->window_dirty = false;
    reader_cached_page_t **at = &c->first;
    while (*at) {
        reader_cached_page_t *p = *at;
        if (distance(p->number, wanted) <= 3 || (p->end.record >= begin && p->start.record < end))
            at = &p->next;
        else remove_page(c, at);
    }
}
reader_cached_page_t *reader_page_cache_find(reader_page_cache_t *c, unsigned number, uint32_t layout)
{
    for (reader_cached_page_t *p = c->first; p && p->number <= number; p = p->next)
        if (p->number == number && p->layout == layout) return p;
    return NULL;
}
void reader_page_cache_span(const reader_page_cache_t *c, unsigned wanted, uint32_t layout,
                            unsigned *ahead, unsigned *back)
{
    unsigned run = 0, previous = 0;
    *ahead = *back = 0;
    for (const reader_cached_page_t *p = c->first; p; p = p->next) {
        if (p->layout != layout) continue;
        if (p->number < wanted) {
            run = run && p->number - previous == 1 ? run + 1 : 1;
            previous = p->number;
            if (wanted - p->number == 1) *back = run;
        } else if (p->number > wanted) {
            if (p->number - wanted != *ahead + 1) break;
            ++*ahead;
        }
    }
}
reader_cached_page_t *reader_page_cache_put(reader_page_cache_t *c, unsigned number,
                                           uint32_t layout, const epd_reader_page_t *page)
{
    if (!page->done || !page->collect || page->count > EPD_READER_RUNS || page->bytes > EPD_READER_TEXT)
        return NULL;
    reader_cached_page_t *old = reader_page_cache_find(c, number, layout);
    if (old) return old;
    size_t size = sizeof(reader_cached_page_t) + page->count * sizeof(epd_reader_run_t) + page->bytes;
    if (size > c->budget) return NULL;
    unsigned incoming = distance(number, c->wanted);
    while (c->bytes > c->budget - size || (c->available && c->available() < size + c->reserve)) {
        reader_cached_page_t **victim = NULL;
        unsigned farthest = incoming;
        for (reader_cached_page_t **at = &c->first; *at; at = &(*at)->next) {
            unsigned d = distance((*at)->number, c->wanted);
            if (d > 3 && d > farthest) { victim = at; farthest = d; }
        }
        if (!victim) return NULL;
        remove_page(c, victim);
    }
    reader_cached_page_t *p = c->allocate(size);
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->number = number; p->layout = layout; p->start = page->start; p->end = page->cursor;
    p->count = page->count; p->bytes = page->bytes; p->eof = page->eof; p->allocation = size;
    memcpy(p->runs, page->runs, page->count * sizeof(*page->runs));
    memcpy((char *)(p->runs + p->count), page->text, page->bytes);
    reader_cached_page_t **at = &c->first;
    while (*at && (*at)->number < number) at = &(*at)->next;
    p->next = *at; *at = p; c->bytes += size; ++c->count;
    if (incoming > 3 && !(p->end.record >= c->begin && p->start.record < c->end))
        c->window_dirty = true;
    return p;
}
void reader_page_cache_load(const reader_cached_page_t *saved, epd_reader_page_t *p)
{
    memset(p, 0, offsetof(epd_reader_page_t, runs));
    p->start = saved->start; p->cursor = saved->end;
    p->count = saved->count; p->bytes = saved->bytes;
    p->done = p->collect = 1; p->eof = saved->eof;
    memcpy(p->runs, saved->runs, saved->count * sizeof(*p->runs));
    memcpy(p->text, reader_page_cache_text(saved), saved->bytes);
}
