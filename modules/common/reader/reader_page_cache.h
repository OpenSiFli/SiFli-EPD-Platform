#ifndef READER_PAGE_CACHE_H
#define READER_PAGE_CACHE_H
#include "epd_reader_layout.h"
#include <stdbool.h>
#include <stddef.h>

struct reader_page_cache;

typedef struct reader_cached_page {
    struct reader_cached_page *next;
    epd_reader_cursor_t start, end;
    uint32_t number, layout, count, bytes, eof;
    uint32_t warm_run, warm_byte, warm_failed, warm_scan, warm_pending;
    size_t allocation;
    struct reader_page_cache *cache;
    uint64_t resource, reference;
    void (*dispose)(void *);
    epd_reader_run_t runs[];
} reader_cached_page_t;
typedef struct reader_page_cache {
    reader_cached_page_t *first;
    size_t bytes, budget, reserve;
    unsigned count, wanted;
    uint32_t begin, end;
    bool window_dirty;
    void *(*allocate)(size_t);
    void (*release)(void *);
    size_t (*available)(void);
    /* Optional lifecycle adapter. All cache access and disposal use the UI lane. */
    bool (*publish)(struct reader_page_cache *, reader_cached_page_t *);
    bool (*retire)(struct reader_page_cache *, reader_cached_page_t *);
    bool (*borrow)(struct reader_page_cache *, reader_cached_page_t *);
    void (*rank)(struct reader_page_cache *, reader_cached_page_t *, unsigned distance);
    void (*close)(struct reader_page_cache *);
    reader_cached_page_t *borrowed;
    uint32_t resource_scope;
    uint64_t resource_binding, resource_parent, resource_activity, resource_provider;
} reader_page_cache_t;

void reader_page_cache_clear(reader_page_cache_t *cache);
void reader_page_cache_close(reader_page_cache_t *cache);
void reader_page_cache_remove(reader_page_cache_t *cache, reader_cached_page_t *page);
/* Called by the resource destructor after it exclusively claims an unpinned page. */
void reader_page_cache_unlink(reader_page_cache_t *cache, reader_cached_page_t *page);
void reader_page_cache_window(reader_page_cache_t *cache, unsigned wanted, uint32_t begin, uint32_t end);
reader_cached_page_t *reader_page_cache_find(reader_page_cache_t *cache, unsigned number, uint32_t layout);
void reader_page_cache_span(const reader_page_cache_t *cache, unsigned wanted, uint32_t layout,
                            unsigned *ahead, unsigned *back);
reader_cached_page_t *reader_page_cache_put(reader_page_cache_t *cache, unsigned number,
                                           uint32_t layout, const epd_reader_page_t *page);
void reader_page_cache_load(const reader_cached_page_t *saved, epd_reader_page_t *page);
const char *reader_page_cache_text(const reader_cached_page_t *page);
#endif
