#ifndef FILE_PAGE_INDEX_H
#define FILE_PAGE_INDEX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FILE_PAGE_ROWS 8u
#define FILE_PAGE_COUNT 16u

typedef struct {
    char name[256];
    uint64_t size;
    bool directory;
} file_page_entry_t;
typedef struct {
    unsigned first, count;
    bool valid, more;
    file_page_entry_t entries[FILE_PAGE_ROWS];
} file_page_t;
/* Allocated in PSRAM. One lookahead entry preserves exact next-page availability. */
typedef struct {
    file_page_t pages[FILE_PAGE_COUNT];
    file_page_entry_t lookahead;
    unsigned replacement, next_row;
    bool pending, eof;
} file_page_cache_t;
static file_page_t *file_page_find(file_page_cache_t *cache, unsigned first)
{
    for (unsigned i = 0; i < FILE_PAGE_COUNT; ++i)
        if (cache->pages[i].valid && cache->pages[i].first == first) return &cache->pages[i];
    return NULL;
}
static file_page_t *file_page_begin(file_page_cache_t *cache, unsigned first)
{
    file_page_t *page = &cache->pages[cache->replacement++ % FILE_PAGE_COUNT];
    page->valid = false; page->first = first; page->count = 0; page->more = false;
    return page;
}
#endif
