#ifndef EPD_MARKDOWN_INTERNAL_H
#define EPD_MARKDOWN_INTERNAL_H
#include <stddef.h>
#define md_parse md_engine_parse
#include "vendor/md4c/md4c.h"
#undef md_parse

void *md_engine_alloc(void *context, size_t size);
void *md_engine_resize(void *context, void *ptr, size_t size);
void md_engine_free(void *context, void *ptr);
int md_engine_depth(void *context, unsigned depth);
#endif
