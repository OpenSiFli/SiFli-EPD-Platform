#ifndef READER_PAGE_CACHE_H
#define READER_PAGE_CACHE_H
#include "epd_reader_layout.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct reader_cached_page {
    struct reader_cached_page *next;
    epd_reader_cursor_t start, end;
    uint32_t number, layout, count, bytes, eof;
    uint32_t warm_run, warm_byte, warm_failed;
    size_t allocation;
    epd_reader_run_t runs[];
} reader_cached_page_t;
typedef struct {
    reader_cached_page_t *first;
    size_t bytes, budget, reserve;
    unsigned count, wanted;
    uint32_t begin, end;
    bool window_dirty;
    void *(*allocate)(size_t);
    void (*release)(void *);
    size_t (*available)(void);
} reader_page_cache_t;

void reader_page_cache_clear(reader_page_cache_t *cache);
void reader_page_cache_window(reader_page_cache_t *cache, unsigned wanted, uint32_t begin, uint32_t end);
reader_cached_page_t *reader_page_cache_find(reader_page_cache_t *cache, unsigned number, uint32_t layout);
void reader_page_cache_span(const reader_page_cache_t *cache, unsigned wanted, uint32_t layout,
                            unsigned *ahead, unsigned *back);
reader_cached_page_t *reader_page_cache_put(reader_page_cache_t *cache, unsigned number,
                                           uint32_t layout, const epd_reader_page_t *page);
void reader_page_cache_load(const reader_cached_page_t *saved, epd_reader_page_t *page);
const char *reader_page_cache_text(const reader_cached_page_t *page);
#endif
