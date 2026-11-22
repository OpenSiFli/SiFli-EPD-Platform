#ifndef READER_PAGE_RESOURCE_H
#define READER_PAGE_RESOURCE_H
#include "reader_page_cache.h"

/* Attach before the first page. Uses the current application's real page scope;
 * cache access, borrowed page inspection and destruction remain on the UI lane. */
void reader_page_resource_attach(reader_page_cache_t *cache);

#endif
