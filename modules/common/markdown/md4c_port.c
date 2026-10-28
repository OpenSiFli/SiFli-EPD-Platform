#include "markdown_internal.h"
#include <stdlib.h>
#include <string.h>

/* In-place heapsort avoids a C library qsort implementation allocating memory. */
static void md_sort_swap(unsigned char *a, unsigned char *b, size_t size)
{
    size_t i;
    for(i = 0; i < size; ++i) {
        unsigned char t = a[i]; a[i] = b[i]; b[i] = t;
    }
}

static void md_sort_sift(unsigned char *base, size_t root, size_t count,
                         size_t width, int (*compare)(const void *, const void *))
{
    while(root < count / 2) {
        size_t child = root * 2 + 1;
        if(child + 1 < count && compare(base + child * width, base + (child + 1) * width) < 0)
            ++child;
        if(compare(base + root * width, base + child * width) >= 0) break;
        md_sort_swap(base + root * width, base + child * width, width);
        root = child;
    }
}

static void md_sort(void *base, size_t count, size_t width,
                    int (*compare)(const void *, const void *))
{
    size_t i;
    if(count < 2) return;
    for(i = count / 2; i > 0; --i)
        md_sort_sift(base, i - 1, count, width, compare);
    for(i = count - 1; i > 0; --i) {
        md_sort_swap(base, (unsigned char *)base + i * width, width);
        md_sort_sift(base, 0, i, width, compare);
    }
}

#define MD_ALLOC(ctx, size) md_engine_alloc((ctx)->userdata, (size))
#define MD_REALLOC(ctx, ptr, size) md_engine_resize((ctx)->userdata, (ptr), (size))
#define MD_FREE(ctx, ptr) md_engine_free((ctx)->userdata, (ptr))
#define MD_CHECK_CONTAINER_LIMIT(ctx, depth) md_engine_depth((ctx)->userdata, (unsigned)(depth))
#define MD_KEEP_TIGHT_PARAGRAPHS 1
#define md_parse md_engine_parse
#define qsort md_sort
#include "vendor/md4c/md4c.c"

#undef qsort
#undef md_parse
#define entity_lookup md_entity_lookup
#include "vendor/md4c/entity.c"
