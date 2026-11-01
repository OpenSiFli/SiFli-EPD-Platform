/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"
#ifdef BOOK_FORMAT_ACPU
#include "../compute/book_compute_internal.h"
#endif

typedef struct epub_item
{
    char *id, *href;
    int html, nav, ncx, cover;
    uint32_t next_id;
} epub_item;
#define EPUB_ITEMS_PER_BLOCK 64u
#define EPUB_ID_BUCKETS 256u
/* Keep item addresses stable and avoid an old/new full-manifest allocation.
 * Short IDs and paths share blocks instead of one allocator header each. */
typedef struct epub_strings {
    struct epub_strings *next;
    size_t used;
    char data[4096];
} epub_strings;
typedef struct epub_part
{
    char *zip, *href;
    int used;
} epub_part;
typedef struct epub
{
    const book_sink_t *sink;
    book_sink_t mapped;
    bf_zip base, current;
    bf_package package;
    book_resources_t resources;
    char opf[BF_PATH], title[512], author[512], toc[256];
    char cover_id[256], cover_path[BF_PATH];
    epub_item **items;
    epub_strings *strings;
    uint32_t ids[EPUB_ID_BUCKETS];
    size_t count;
    uint32_t *spine;
    size_t spine_count;
    epub_part *parts;
    size_t part_count;
    const epub_part *active_part;
    int capture, split;
    char *heading_targets;
    size_t heading_bytes, heading_capacity;
    bool headings_incomplete, heading_borrowed;
#ifdef BOOK_FORMAT_ACPU
    const book_compute_selection_t *selection;
    const book_compute_observer_t *observer;
    uint32_t selected_index, chapter_total;
    char reading_entry[BF_PATH];
    unsigned reading_rank;
    const char *body_href;
#endif
} epub;
static epub_item *item_at(const epub *e, size_t index)
{ return &e->items[index / EPUB_ITEMS_PER_BLOCK][index % EPUB_ITEMS_PER_BLOCK]; }
static unsigned id_bucket(const char *id)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p) hash = (hash ^ *p) * 16777619u;
    return hash % EPUB_ID_BUCKETS;
}
static size_t item_index(const epub *e, const char *id)
{
    size_t found = SIZE_MAX;
    /* Preserve the first manifest match when a malformed package repeats IDs. */
    for (uint32_t n = e->ids[id_bucket(id)]; n; n = item_at(e, n - 1)->next_id)
        if (!strcmp(item_at(e, n - 1)->id, id)) found = n - 1;
    return found;
}
static char *epub_string(epub *e, const char *text)
{
    size_t n = strlen(text) + 1;
    epub_strings *block = e->strings;
    if (n > sizeof(block->data)) return NULL;
    if (!block || n > sizeof(block->data) - block->used) {
        block = bf_alloc(sizeof(*block));
        if (!block) return NULL;
        block->next = e->strings; e->strings = block;
    }
    char *result = block->data + block->used;
    memcpy(result, text, n); block->used += n;
    return result;
}
static void clear_package(epub *e)
{
    for (size_t i = 0; i < e->count; i += EPUB_ITEMS_PER_BLOCK)
        bf_free(e->items[i / EPUB_ITEMS_PER_BLOCK]);
    bf_free(e->items); e->items = NULL; e->count = 0;
    while (e->strings) {
        epub_strings *block = e->strings;
        e->strings = block->next; bf_free(block);
    }
    bf_free(e->spine); e->spine = NULL; e->spine_count = 0;
    memset(e->ids, 0, sizeof(e->ids));
}
static int property(const char *list, const char *word)
{
    size_t n = strlen(word);
    while (*list) {
        while (isspace((unsigned char)*list)) ++list;
        const char *end = list;
        while (*end && !isspace((unsigned char)*end)) ++end;
        if ((size_t)(end - list) == n && !memcmp(list, word, n)) return 1;
        list = end;
    }
    return 0;
}
static int opf_event(void *u, int event, const char *name, const char *ns, const bf_attrs *a,
                     const char *s, size_t n)
{
    epub *e = u;
    char id[256], href[BF_PATH], media[128], props[512], path[BF_PATH];
    int r;
    if (event == 2)
    {
        if (e->capture == 1)
            bf_append(e->title, sizeof(e->title), s, n);
        if (e->capture == 2)
            bf_append(e->author, sizeof(e->author), s, n);
        return BOOK_OK;
    }
    if (event == 0)
    {
        if (!strcmp(ns, BF_NS_DC))
            e->capture = 0;
        return BOOK_OK;
    }
    if (!strcmp(ns, BF_NS_DC))
    {
        if (!strcmp(name, "title") && !*e->title)
            e->capture = 1;
        else if (!strcmp(name, "creator"))
        {
            if (*e->author)
                bf_append(e->author, sizeof(e->author), ", ", 2);
            e->capture = 2;
        }
        return BOOK_OK;
    }
    if (strcmp(ns, BF_NS_OPF))
        return BOOK_OK;
#ifdef BOOK_FORMAT_ACPU
    if (!strcmp(name, "reference") && bf_attr(a, "type", "", props, sizeof(props)) > 0 &&
        (property(props, "text") || property(props, "bodymatter")) &&
        bf_attr(a, "href", "", href, sizeof(href)) > 0 &&
        bf_resolve(e->opf, href, e->reading_entry, sizeof(e->reading_entry)) == BOOK_OK)
        e->reading_rank = 3;
#endif
    if (!strcmp(name, "meta") && bf_attr(a, "name", "", props, sizeof(props)) > 0 &&
        !strcmp(props, "cover"))
    {
        if (bf_attr(a, "content", "", e->cover_id, sizeof(e->cover_id)) < 0) e->cover_id[0] = 0;
        return BOOK_OK;
    }
    if (!strcmp(name, "reference") && bf_attr(a, "type", "", props, sizeof(props)) > 0 &&
        property(props, "cover") && bf_attr(a, "href", "", href, sizeof(href)) > 0)
    {
        if (bf_resolve(e->opf, href, e->cover_path, sizeof(e->cover_path)) < 0) e->cover_path[0] = 0;
        return BOOK_OK;
    }
    if (!strcmp(name, "spine"))
    {
        if (bf_attr(a, "toc", "", e->toc, sizeof(e->toc)) < 0)
            return BOOK_UNSUPPORTED;
    }
    if (!strcmp(name, "item"))
    {
        epub_item *p;
        if (bf_attr(a, "id", "", id, sizeof(id)) <= 0 ||
            bf_attr(a, "href", "", href, sizeof(href)) <= 0)
            return BOOK_ERROR;
        if (bf_attr(a, "media-type", "", media, sizeof(media)) < 0 ||
            bf_attr(a, "properties", "", props, sizeof(props)) < 0)
            return BOOK_UNSUPPORTED;
        if (e->count >= UINT32_MAX)
            return BOOK_UNSUPPORTED;
        if ((r = bf_resolve(e->opf, href, path, sizeof(path))) < 0)
            return r;
        if (bf_external(path))
            return BOOK_UNSUPPORTED;
        if (e->count % EPUB_ITEMS_PER_BLOCK == 0) {
            size_t blocks = e->count / EPUB_ITEMS_PER_BLOCK;
            if (blocks >= SIZE_MAX / sizeof(*e->items)) return BOOK_UNSUPPORTED;
            epub_item **items = bf_realloc(e->items, (blocks + 1) * sizeof(*items));
            if (!items) return BOOK_NO_MEMORY;
            e->items = items;
            items[blocks] = bf_alloc(EPUB_ITEMS_PER_BLOCK * sizeof(**items));
            if (!items[blocks]) return BOOK_NO_MEMORY;
        }
        p = item_at(e, e->count++);
        p->id = epub_string(e, id);
        p->href = epub_string(e, path);
        if (!p->id || !p->href)
            return BOOK_NO_MEMORY;
        p->html = !strcmp(media, "application/xhtml+xml") || !strcmp(media, "text/html");
        p->nav = strstr(props, "nav") != NULL;
        p->ncx = !strcmp(media, "application/x-dtbncx+xml");
        p->cover = property(props, "cover-image");
        unsigned bucket = id_bucket(id);
        p->next_id = e->ids[bucket]; e->ids[bucket] = (uint32_t)e->count;
    }
    if (!strcmp(name, "itemref"))
    {
        uint32_t *p;
        if (bf_attr(a, "linear", "", props, sizeof(props)) < 0)
            return BOOK_UNSUPPORTED;
        if (!strcmp(props, "no"))
            return BOOK_OK;
        if (bf_attr(a, "idref", "", id, sizeof(id)) <= 0)
            return BOOK_ERROR;
        size_t index = item_index(e, id);
        if (index == SIZE_MAX) return BOOK_ERROR;
        if (e->spine_count >= SIZE_MAX / sizeof(*p) - EPUB_ITEMS_PER_BLOCK ||
            e->spine_count >= UINT32_MAX) return BOOK_UNSUPPORTED;
        if (e->spine_count % EPUB_ITEMS_PER_BLOCK == 0) {
            p = bf_realloc(e->spine, (e->spine_count + EPUB_ITEMS_PER_BLOCK) * sizeof(*p));
            if (!p) return BOOK_NO_MEMORY;
            e->spine = p;
        }
        e->spine[e->spine_count++] = (uint32_t)index;
    }
    return BOOK_OK;
}
static int container_event(void *u, int event, const char *name, const char *ns, const bf_attrs *a,
                           const char *s, size_t n)
{
    epub *e = u;
    char p[BF_PATH], type[128];
    (void)s;
    (void)n;
    if (event == 1 && !strcmp(name, "rootfile") &&
        !strcmp(ns, "urn:oasis:names:tc:opendocument:xmlns:container"))
    {
        if (bf_attr(a, "full-path", "", p, sizeof(p)) <= 0 ||
            bf_attr(a, "media-type", "", type, sizeof(type)) < 0)
            return BOOK_ERROR;
        if (!*e->opf && (!*type || !strcmp(type, "application/oebps-package+xml")))
            return bf_resolve("", p, e->opf, sizeof(e->opf));
    }
    return BOOK_OK;
}
static int natural(const char *a, const char *b)
{
    const char *aa = a, *bb = b;
    while (*a && *b)
    {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b))
        {
            const char *x = a, *y = b;
            size_t nx, ny;
            while (*x == '0')
                ++x;
            while (*y == '0')
                ++y;
            a = x;
            b = y;
            while (isdigit((unsigned char)*a))
                ++a;
            while (isdigit((unsigned char)*b))
                ++b;
            nx = (size_t)(a - x);
            ny = (size_t)(b - y);
            if (nx != ny)
                return nx < ny ? -1 : 1;
            if (nx)
            {
                int c = memcmp(x, y, nx);
                if (c)
                    return c;
            }
        }
        else
        {
            int c = tolower((unsigned char)*a) - tolower((unsigned char)*b);
            if (c)
                return c;
            ++a;
            ++b;
        }
    }
    if (*a || *b)
        return *a ? 1 : -1;
    return strcmp(aa, bb);
}
static int compare_part(const void *a, const void *b)
{
    const epub_part *x = a, *y = b;
    int r = natural(x->zip, y->zip);
    return r ? r : natural(x->href, y->href);
}
static int scan_split(epub *e, const char *dir)
{
    bf_directory d;
    struct dirent *entry;
    int r = BOOK_OK;
    char path[BF_PATH], name[BF_PATH];
    bf_zip z;
    if (bf_dir_open(&d, dir) < 0)
        return BOOK_IO_ERROR;
    while ((entry = bf_dir_next(&d)) != NULL)
    {
        unsigned i;
        if ((r = bf_cancel(e->sink)) < 0)
            break;
        if (!bf_chapter_zip(entry->d_name))
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path))
        {
            r = BOOK_UNSUPPORTED;
            break;
        }
        if ((r = bf_zip_open(&z, path, e->sink)) < 0)
            break;
        for (i = 0; i < mz_zip_reader_get_num_files(&z.zip); ++i)
        {
            epub_part *p;
            if ((r = bf_zip_name(&z, i, name, sizeof(name))) < 0)
                break;
            if (!bf_suffix(name, ".xhtml") && !bf_suffix(name, ".html") && !bf_suffix(name, ".htm"))
                continue;
            if (e->part_count >= SIZE_MAX / sizeof(*p))
            {
                r = BOOK_UNSUPPORTED;
                break;
            }
            p = bf_realloc(e->parts, (e->part_count + 1) * sizeof(*p));
            if (!p)
            {
                r = BOOK_NO_MEMORY;
                break;
            }
            e->parts = p;
            p = &e->parts[e->part_count++];
            memset(p, 0, sizeof(*p));
            p->zip = bf_strdup(entry->d_name);
            p->href = bf_strdup(name);
            if (!p->zip || !p->href)
            {
                r = BOOK_NO_MEMORY;
                break;
            }
        }
        bf_zip_close(&z);
        if (r < 0)
            break;
    }
    if (r == BOOK_OK && d.status < 0)
        r = d.status;
    bf_dir_close(&d);
    if (r < 0)
        return r;
    if (!e->part_count)
        return BOOK_ERROR;
    qsort(e->parts, e->part_count, sizeof(*e->parts), compare_part);
    return BOOK_OK;
}
static int map_id(epub *e, const char *id, char *out, size_t cap)
{
    const char *hash = strchr(id, '#');
    size_t n = hash ? (size_t)(hash - id) : strlen(id), i;
    const epub_part *p = NULL;
    if (!e->split || bf_external(id) || strchr(id, '!'))
        return bf_copy(out, cap, id);
    if (e->active_part && strlen(e->active_part->href) == n &&
        !strncmp(id, e->active_part->href, n))
        p = e->active_part;
    else
        for (i = 0; i < e->part_count; ++i)
            if (strlen(e->parts[i].href) == n && !strncmp(id, e->parts[i].href, n))
            {
                p = &e->parts[i];
                break;
            }
    if (!p)
        return bf_copy(out, cap, id);
    return snprintf(out, cap, "%s!%s", p->zip, id) >= (int)cap ? BOOK_UNSUPPORTED : BOOK_OK;
}
static bool mapped_cancel(void *u)
{
    epub *e = u;
    return bf_cancel(e->sink) < 0;
}
static int mapped_text(void *u, const char *p, size_t n, unsigned st)
{
    return bf_text(((epub *)u)->sink, p, n, st);
}
static bool mapped_heading_target(void *u, const char *id)
{
    epub *e = u;
    for (size_t at = 0; at < e->heading_bytes; at += strlen(e->heading_targets + at) + 1)
        if (!strcmp(e->heading_targets + at, id)) return true;
    return false;
}
static int mapped_para(void *u)
{
    return bf_para(((epub *)u)->sink);
}
static int mapped_anchor(void *u, const char *id, const char *title, unsigned level)
{
    epub *e = u;
    char target[BF_PATH];
    int r = map_id(e, id, target, sizeof(target));
    return r < 0 ? r : bf_anchor(e->sink, target, title, level);
}
static int mapped_link(void *u, const char *id, bool begin)
{
    epub *e = u;
    char target[BF_PATH];
    int r = map_id(e, id, target, sizeof(target));
    return r < 0 ? r : bf_link(e->sink, target, begin);
}
static int mapped_image(void *u, const char *id, book_stream_t *s)
{
    epub *e = u;
    char target[BF_PATH];
    int r = map_id(e, id, target, sizeof(target));
    return r < 0 ? r : e->sink->image ? e->sink->image(e->sink->user, target, s) : BOOK_OK;
}
static int mapped_svg(void *u, const char *xml, size_t length, const char *base,
                      const book_resources_t *resources)
{
    epub *e = u;
    int r = bf_cancel(e->sink);
    return r < 0 ? r : e->sink->svg
        ? e->sink->svg(e->sink->user, xml, length, base, resources) : BOOK_UNSUPPORTED;
}
typedef struct nav_frame
{
    char title[512], href[BF_PATH];
    int sent;
} nav_frame;
typedef struct nav_state
{
    epub *e;
    const char *base;
    nav_frame f[32];
    unsigned depth, ol;
    int ncx, in_text, in_a, in_toc, nav_depth;
} nav_state;
#ifdef BOOK_FORMAT_ACPU
static void reading_entry(epub *e, const char *path, const char *title, unsigned rank)
{
    if (!e->selection || !(e->selection->flags & BOOK_COMPUTE_FIRST_BODY)) return;
    if (!rank) {
        char folded[128]; size_t n = strlen(title);
        if (n >= sizeof(folded)) n = sizeof(folded) - 1;
        for (size_t i = 0; i < n; ++i) folded[i] = (char)tolower((unsigned char)title[i]);
        folded[n] = 0;
        const char *front[] = {"cover", "title page", "copyright", "contents", "about the author",
            "封面", "书名页", "版权", "目录", "制作说明", "作者简介", "内容简介", "出版说明"};
        for (size_t i = 0; i < sizeof(front) / sizeof(front[0]); ++i)
            if (!strncmp(folded, front[i], strlen(front[i]))) return;
        bool chapter = !strncmp(folded, "chapter ", 8) || !strcmp(folded, "正文") ||
            (!strncmp(folded, "第", strlen("第")) && strstr(folded, "章"));
        rank = chapter ? 2 : 1;
    }
    if (rank > e->reading_rank && strlen(path) < sizeof(e->reading_entry)) {
        strcpy(e->reading_entry, path); e->reading_rank = rank;
    }
}
#endif
static int heading_target(epub *e, const char *p)
{
    size_t bytes = strlen(p) + 1;
    if (!strchr(p, '#') || mapped_heading_target(e, p)) return BOOK_OK;
    if (bytes > 65536u - e->heading_bytes) {
        e->headings_incomplete = true;
        return BOOK_OK;
    }
    size_t need = e->heading_bytes + bytes;
    if (need > e->heading_capacity) {
        size_t capacity = e->heading_capacity ? e->heading_capacity : 1024u;
        while (capacity < need) capacity *= 2;
        char *targets = bf_realloc(e->heading_targets, capacity);
        if (!targets) return BOOK_NO_MEMORY;
        e->heading_targets = targets; e->heading_capacity = capacity;
    }
    memcpy(e->heading_targets + e->heading_bytes, p, bytes);
    e->heading_bytes += bytes;
    return BOOK_OK;
}
static int nav_emit(nav_state *v, nav_frame *f, unsigned level)
{
    char p[BF_PATH];
    int r;
    if (f->sent || !*f->href)
        return BOOK_OK;
    f->sent = 1;
    r = bf_resolve(v->base, f->href, p, sizeof(p));
    if (r < 0) return r;
#ifdef BOOK_FORMAT_ACPU
    reading_entry(v->e, p, f->title, 0);
#endif
    if ((r = heading_target(v->e, p)) < 0) return r;
#ifdef BOOK_FORMAT_ACPU
    if (v->e->selection && (v->e->selection->flags & BOOK_COMPUTE_BODY_ONLY)) return BOOK_OK;
    if (v->e->observer && v->e->observer->toc)
    {
        epub *e = v->e;
        uint32_t index = BOOK_COMPUTE_NO_SPINE;
        size_t i, length = strcspn(p, "#");
        char stable[BF_PATH];
        for (i = 0; i < e->spine_count; ++i) {
            const char *href = item_at(e, e->spine[i])->href;
            if (strlen(href) == length && !strncmp(p, href, length)) index = (uint32_t)i;
        }
        if ((r = map_id(e, p, stable, sizeof(stable))) < 0) return r;
        r = e->observer->toc(e->observer->user, index, e->chapter_total, stable,
                            *f->title ? f->title : f->href, level ? level : 1);
        /* The explicit TOC already carries this title and target. Body
         * anchors are emitted separately when their content is rendered. */
        return r;
    }
#endif
    return mapped_anchor(v->e, p, *f->title ? f->title : f->href, level ? level : 1);
}
static int nav_event(void *u, int event, const char *name, const char *ns, const bf_attrs *a,
                     const char *s, size_t n)
{
    nav_state *v = u;
    nav_frame *f;
    char type[128];
    int r;
    (void)ns;
#ifdef BOOK_FORMAT_ACPU
    if (!v->ncx && event == 1 && !strcmp(name, "a") &&
        bf_attr(a, "type", "http://www.idpf.org/2007/ops", type, sizeof(type)) > 0 && property(type, "bodymatter")) {
        char href[BF_PATH], path[BF_PATH];
        if (bf_attr(a, "href", "", href, sizeof(href)) > 0 &&
            bf_resolve(v->base, href, path, sizeof(path)) == BOOK_OK) reading_entry(v->e, path, "", 3);
    }
#endif
    if (v->ncx)
    {
        if (event == 1 && !strcmp(name, "navPoint"))
        {
            if (v->depth >= 32)
                return BOOK_UNSUPPORTED;
            memset(&v->f[v->depth++], 0, sizeof(v->f[0]));
        }
        if (!v->depth)
            return BOOK_OK;
        f = &v->f[v->depth - 1];
        if (event == 1 && !strcmp(name, "text"))
            v->in_text = 1;
        if (event == 2 && v->in_text)
            bf_append(f->title, sizeof(f->title), s, n);
        if (event == 0 && !strcmp(name, "text"))
            v->in_text = 0;
        if (event == 1 && !strcmp(name, "content"))
        {
            if (bf_attr(a, "src", "", f->href, sizeof(f->href)) < 0)
                return BOOK_UNSUPPORTED;
            return nav_emit(v, f, v->depth);
        }
        if (event == 0 && !strcmp(name, "navPoint"))
        {
            r = nav_emit(v, f, v->depth);
            --v->depth;
            return r;
        }
    }
    else
    {
        if (event == 1 && !strcmp(name, "nav"))
        {
            ++v->nav_depth;
            if (bf_attr(a, "type", "http://www.idpf.org/2007/ops", type, sizeof(type)) < 0)
                return BOOK_UNSUPPORTED;
            if (strstr(type, "toc"))
                v->in_toc = v->nav_depth;
        }
        if (!v->in_toc)
            return BOOK_OK;
        if (event == 1 && !strcmp(name, "ol"))
            ++v->ol;
        if (event == 0 && !strcmp(name, "ol") && v->ol)
            --v->ol;
        f = &v->f[0];
        if (event == 1 && !strcmp(name, "a"))
        {
            memset(f, 0, sizeof(*f));
            v->in_a = 1;
            if (bf_attr(a, "href", "", f->href, sizeof(f->href)) < 0)
                return BOOK_UNSUPPORTED;
        }
        if (event == 2 && v->in_a)
            bf_append(f->title, sizeof(f->title), s, n);
        if (event == 0 && !strcmp(name, "a"))
        {
            v->in_a = 0;
            return nav_emit(v, f, v->ol);
        }
        if (event == 0 && !strcmp(name, "nav"))
        {
            if (v->in_toc == v->nav_depth)
                v->in_toc = 0;
            --v->nav_depth;
        }
    }
    return BOOK_OK;
}
#ifdef BOOK_FORMAT_ACPU
#include "book_epub_nav.inc"
#endif
static int read_navigation(epub *e)
{
    size_t i;
    epub_item *item = NULL;
    nav_state *n;
    int r;
    for (i = 0; i < e->count; ++i)
        if (item_at(e, i)->nav)
        {
            item = item_at(e, i);
            break;
        }
    if (!item)
        for (i = 0; i < e->count; ++i)
            if (item_at(e, i)->ncx && (!*e->toc || !strcmp(e->toc, item_at(e, i)->id)))
            {
                item = item_at(e, i);
                break;
            }
    if (!item)
        return BOOK_OK;
#ifdef BOOK_FORMAT_ACPU
    if (e->body_href) return read_body_navigation(e, item);
#endif
    n = bf_alloc(sizeof(*n));
    if (!n)
        return BOOK_NO_MEMORY;
    n->e = e;
    n->base = item->href;
    n->ncx = item->ncx;
    r = bf_xml_entry(&e->base, item->href, e->sink, nav_event, n);
    bf_free(n);
    return r;
}
static int open_part(epub *e, const char *dir, const char *href, epub_part *part, book_stream_t *s)
{
    char path[BF_PATH];
    int r;
    e->active_part = part;
    if (part)
    {
        bf_zip_close(&e->current);
        if (snprintf(path, sizeof(path), "%s/%s", dir, part->zip) >= (int)sizeof(path))
            return BOOK_UNSUPPORTED;
        if ((r = bf_zip_open(&e->current, path, e->sink)) < 0)
            return r;
        e->package.current = &e->current;
        part->used = 1;
        r = bf_zip_entry(&e->current, href, s);
    }
    else
    {
        e->package.current = &e->base;
        r = bf_zip_entry(&e->base, href, s);
    }
    return r;
}
static int render_part(epub *e, const char *dir, const char *href, epub_part *part)
{
    book_stream_t s;
    int r = open_part(e, dir, href, part, &s);
    if (r < 0)
        return r;
    /* markup emits anchor(href, NULL, 0) before content. Keep this root
     * location distinct from titled navigation and fragment anchors: the
     * document index uses it as the spine/part boundary without a sink ABI change. */
#ifdef BOOK_FORMAT_ACPU
    if (e->selection)
    {
        char stable[BF_PATH];
        r = map_id(e, href, stable, sizeof(stable));
        if (r == BOOK_OK) r = book_compute_epub_slice(&e->mapped, &s, href, &e->resources,
            e->selection, e->observer, e->selected_index, stable,
            e->reading_rank == 3 && strchr(e->reading_entry, '#') ? e->reading_entry : NULL);
    }
    else
#endif
        r = book_markup_convert(&e->mapped, &s, href, &e->resources);
    return bf_finish(&s, r);
}
#ifdef BOOK_FORMAT_ACPU
typedef struct chapter_plan { const char *href; epub_part *part; } chapter_plan;
typedef struct reading_probe {
    book_stream_t source;
    uint32_t remaining;
    unsigned depth, skip;
    bool body, found, exhausted;
} reading_probe;
static int reading_probe_read(void *user, void *data, size_t size)
{
    reading_probe *p = user;
    if (!p->remaining) { p->exhausted = true; return BOOK_CANCELLED; }
    if (size > p->remaining) size = p->remaining;
    int n = p->source.read(p->source.user, data, size);
    if (n > 0) p->remaining -= (uint32_t)n;
    return n;
}
static int reading_probe_event(void *user, int event, const char *name, const char *ns,
    const bf_attrs *attrs, const char *text, size_t length)
{
    reading_probe *p = user; (void)ns; (void)attrs;
    if (event == 1) {
        ++p->depth;
        if (!strcmp(name, "body")) p->body = true;
        if (!p->skip && (!strcmp(name, "head") || !strcmp(name, "svg") ||
            !strcmp(name, "script") || !strcmp(name, "style"))) p->skip = p->depth;
    } else if (event == 0) {
        if (p->skip == p->depth) p->skip = 0;
        if (!strcmp(name, "body")) p->body = false;
        if (p->depth) --p->depth;
    } else if (p->body && !p->skip) {
        for (size_t i = 0; i < length; ++i) if (!isspace((unsigned char)text[i])) {
            p->found = true; return BOOK_CANCELLED;
        }
    }
    return BOOK_OK;
}
/* Skip image-only entries with a bounded text probe. Image resources are not
 * opened, and no cover page is emitted while choosing the reading entry. */
static int reading_fallback(epub *e, const char *path, chapter_plan *plan, size_t count, uint32_t *spine)
{
    size_t first = *spine == UINT32_MAX ? 0 : *spine;
    for (size_t i = first; i < count && i - first < 32; ++i) {
        if (e->cover_path[0] && !strcmp(e->cover_path, plan[i].href)) continue;
        reading_probe p = {0}; p.remaining = 64u * 1024u;
        int rc = open_part(e, path, plan[i].href, plan[i].part, &p.source);
        if (rc < 0) return rc;
        book_stream_t limited = {0}; limited.user = &p; limited.read = reading_probe_read;
        rc = bf_xml(&limited, e->sink, reading_probe_event, &p, 0);
        rc = bf_finish(&p.source, rc);
        if (p.found) { *spine = (uint32_t)i; return 1; }
        /* A probe does not decide whether legacy HTML is readable. Let the
         * normal markup reader validate syntax after an inconclusive probe. */
        if (p.exhausted || rc == BOOK_ERROR) break;
        if (rc < 0) return rc;
    }
    *spine = (uint32_t)first;
    return BOOK_OK;
}
#include "book_epub_entry.inc"

static int render_selected(epub *e, const char *path)
{
    size_t i, j, count = 0, capacity = e->spine_count + e->part_count;
    chapter_plan *plan;
    int rc = BOOK_OK;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(*plan)) return BOOK_UNSUPPORTED;
    plan = bf_alloc(capacity * sizeof(*plan));
    if (!plan) return BOOK_NO_MEMORY;
    for (i = 0; i < e->spine_count; ++i)
    {
        epub_item *item = item_at(e, e->spine[i]);
        if (!item || !item->html) { rc = item ? BOOK_UNSUPPORTED : BOOK_ERROR; goto done; }
        plan[count].href = item->href;
        for (j = 0; j < e->part_count; ++j)
            if (!e->parts[j].used && !strcmp(e->parts[j].href, item->href))
            { plan[count].part = &e->parts[j]; e->parts[j].used = 1; break; }
        ++count;
    }
    if (e->split)
        for (i = 0; i < e->part_count; ++i)
            if (!e->parts[i].used)
            { plan[count].href = e->parts[i].href; plan[count++].part = &e->parts[i]; }
    e->chapter_total = (uint32_t)count;
    bool body_only = (e->selection->flags & BOOK_COMPUTE_BODY_ONLY) != 0;
    bool nav_only = (e->selection->flags & BOOK_COMPUTE_NAV_ONLY) != 0;
    book_compute_selection_t selected = *e->selection;
    bool first_requested = (selected.flags & BOOK_COMPUTE_FIRST_BODY) != 0;
    if (body_only && !first_requested && selected.spine < count)
        e->body_href = plan[selected.spine].href;
    if (body_only && (rc = read_navigation(e)) < 0) goto done;
    e->body_href = NULL;
    if (selected.flags & BOOK_COMPUTE_FIRST_BODY) {
        selected.spine = UINT32_MAX;
        size_t length = strcspn(e->reading_entry, "#");
        for (i = 0; length && i < count; ++i)
            if (strlen(plan[i].href) == length && !strncmp(plan[i].href, e->reading_entry, length)) {
                selected.spine = (uint32_t)i; break;
            }
        uint32_t hinted = selected.spine;
        if ((rc = reading_fallback(e, path, plan, count, &selected.spine)) < 0) goto done;
        if (!rc) selected.flags &= ~BOOK_COMPUTE_FIRST_BODY;
        rc = BOOK_OK;
        if (hinted != selected.spine) e->reading_entry[0] = 0;
        e->selection = &selected;
    }
    if (!count || selected.spine >= count) { rc = BOOK_ERROR; goto done; }
    if ((rc = bf_metadata(e->sink, e->title, e->author)) < 0) goto done;
    if (e->observer && e->observer->chapter)
        for (i = 0; i < count; ++i)
        {
            if ((body_only && i != selected.spine) || (nav_only && i == selected.spine)) continue;
            char stable[BF_PATH]; e->active_part = plan[i].part;
            if ((rc = bf_cancel(e->sink)) < 0 || (rc = map_id(e, plan[i].href, stable, sizeof(stable))) < 0) goto done;
            rc = e->observer->chapter(e->observer->user, (uint32_t)i, (uint32_t)count,
                e->split ? BOOK_FORMAT_SPLIT_EPUB : BOOK_FORMAT_EPUB,
                i == e->selection->spine, stable, plan[i].href);
            if (rc < 0) goto done;
        }
    e->active_part = NULL;
    if (!body_only && (rc = read_navigation(e)) < 0) goto done;
    if (nav_only) goto done;
    e->selected_index = e->selection->spine;
    if ((rc = entry_export(e, plan[e->selected_index].href, &selected, first_requested)) < 0) goto done;
    char *href = bf_strdup(plan[e->selected_index].href);
    if (!href) { rc = BOOK_NO_MEMORY; goto done; }
    epub_part *part = plan[e->selected_index].part;
    /* Chapter/TOC callbacks have consumed the package. Only the selected path
     * and split-part mapping are needed while decoding its text and images. */
    bf_free(plan); plan = NULL;
    clear_package(e);
    rc = render_part(e, path, href, part);
    bf_free(href);
done:
    e->body_href = NULL;
    bf_free(plan);
    return rc;
}
#endif

/* Bounds apply to shelf probes; ordinary reading retains its existing limits. */
static int package_xml(epub *e, const char *path, bf_event event)
{
#ifdef BOOK_FORMAT_ACPU
    if (e->selection && (e->selection->flags & BOOK_COMPUTE_METADATA)) {
        book_stream_t s = {0};
        int r = bf_zip_entry(&e->base, path, &s);
        if (r < 0) return r;
        r = s.size <= 512u * 1024u ? bf_xml(&s, e->sink, event, e, 0) : BOOK_UNSUPPORTED;
        return bf_finish(&s, r);
    }
#endif
    return bf_xml_entry(&e->base, path, e->sink, event, e);
}
#ifdef BOOK_FORMAT_ACPU
typedef struct { char base[BF_PATH], image[BF_PATH]; } cover_page;
static int cover_event(void *user, int event, const char *name, const char *ns,
                       const bf_attrs *attrs, const char *text, size_t length)
{
    cover_page *page = user;
    char ref[BF_PATH];
    (void)text; (void)length; (void)ns;
    if (event != 1 || page->image[0]) return BOOK_OK;
    int n = !strcmp(name, "img") ? bf_attr(attrs, "src", "", ref, sizeof(ref)) :
        !strcmp(name, "image") ? bf_attr(attrs, "href", "", ref, sizeof(ref)) : 0;
    if (!n && !strcmp(name, "image"))
        n = bf_attr(attrs, "href", "http://www.w3.org/1999/xlink", ref, sizeof(ref));
    return n > 0 ? bf_resolve(page->base, ref, page->image, sizeof(page->image)) : BOOK_OK;
}
/* Each pass consumes a bounded prefix, regardless of the declared OPF size.
 * Only title/author and two cover candidates survive SAX callbacks. */
#define EPUB_METADATA_SCAN_BYTES (512u * 1024u)
typedef struct {
    epub *book;
    book_stream_t input;
    size_t consumed;
    char image[BF_PATH], legacy[BF_PATH];
    bool covers, metadata, done, image_html, legacy_html;
} metadata_scan;
static int metadata_read(void *user, void *buffer, size_t bytes)
{
    metadata_scan *scan = user;
    size_t left = EPUB_METADATA_SCAN_BYTES - scan->consumed;
    if (!left) return scan->consumed == scan->input.size ? 0 : BOOK_UNSUPPORTED;
    if (bytes > left) bytes = left;
    int n = scan->input.read(scan->input.user, buffer, bytes);
    if (n > 0) scan->consumed += (size_t)n;
    return n;
}
static int metadata_event(void *user, int event, const char *name, const char *ns,
                          const bf_attrs *attrs, const char *text, size_t length)
{
    metadata_scan *scan = user;
    epub *e = scan->book;
    bool opf = !strcmp(ns, BF_NS_OPF);
    if (!scan->covers) {
        if (opf && !strcmp(name, "metadata")) {
            if (event == 1) scan->metadata = true;
            else if (event == 0 && scan->metadata) {
                scan->done = true; return BOOK_CANCELLED;
            }
        }
        if (scan->metadata && (event == 2 || !strcmp(ns, BF_NS_DC) ||
                              (opf && !strcmp(name, "meta"))))
            return opf_event(e, event, name, ns, attrs, text, length);
        return BOOK_OK;
    }
    if (!opf) return BOOK_OK;
    if (event == 0 && !strcmp(name, "manifest") && (scan->image[0] || scan->legacy[0])) {
        scan->done = true; return BOOK_CANCELLED;
    }
    if (event != 1) return BOOK_OK;
    if (!strcmp(name, "reference")) return opf_event(e, event, name, ns, attrs, text, length);
    if (!strcmp(name, "item")) {
        char id[256], href[BF_PATH], props[512], media[128];
        int r = bf_attr(attrs, "properties", "", props, sizeof(props));
        if (r < 0) return r;
        bool image = !scan->image[0] && property(props, "cover-image");
        bool legacy = !scan->legacy[0] && e->cover_id[0] &&
            bf_attr(attrs, "id", "", id, sizeof(id)) > 0 && !strcmp(id, e->cover_id);
        if (!image && !legacy) return BOOK_OK;
        if (bf_attr(attrs, "href", "", href, sizeof(href)) <= 0) return BOOK_ERROR;
        if ((r = bf_attr(attrs, "media-type", "", media, sizeof(media))) < 0) return r;
        bool html = !strcmp(media, "application/xhtml+xml") || !strcmp(media, "text/html");
        if (image) {
            r = bf_resolve(e->opf, href, scan->image, sizeof(scan->image));
            scan->image_html = html;
        } else {
            r = bf_resolve(e->opf, href, scan->legacy, sizeof(scan->legacy));
            scan->legacy_html = html;
        }
        if (r < 0) return r;
        /* The first EPUB3 cover wins over legacy IDs and guide references. */
        if (image) { scan->done = true; return BOOK_CANCELLED; }
    }
    return BOOK_OK;
}
static int metadata_pass(metadata_scan *scan)
{
    int r = bf_zip_entry(&scan->book->base, scan->book->opf, &scan->input);
    if (r < 0) return r;
    scan->consumed = 0; scan->done = false;
    book_stream_t bounded = scan->input;
    bounded.user = scan; bounded.read = metadata_read; bounded.close = NULL;
    r = bf_xml(&bounded, scan->book->sink, metadata_event, scan, 0);
    r = bf_finish(&scan->input, r);
    if (r == BOOK_CANCELLED && scan->done) r = bf_cancel(scan->book->sink);
    return r;
}
static int read_metadata(epub *e, bool *html)
{
    metadata_scan *scan = bf_alloc(sizeof(*scan));
    if (!scan) return BOOK_NO_MEMORY;
    scan->book = e;
    int r = metadata_pass(scan);
    if (r == BOOK_OK) r = bf_metadata(e->sink, e->title, e->author);
    if (r == BOOK_OK) {
        scan->covers = true;
        r = metadata_pass(scan);
        if (r == BOOK_OK && (scan->image[0] || scan->legacy[0])) {
            const char *path = scan->image[0] ? scan->image : scan->legacy;
            r = bf_copy(e->cover_path, sizeof(e->cover_path), path);
            *html = scan->image[0] ? scan->image_html : scan->legacy_html;
        }
    }
    bf_free(scan);
    return r;
}
static int read_cover(epub *e, const char *path, bool html)
{
    char normalized[BF_PATH];
    if (!*path || bf_external(path)) return BOOK_OK;
    bf_copy(normalized, sizeof(normalized), path);
    char *fragment = strchr(normalized, '#');
    if (fragment) *fragment = 0;
    path = normalized;
    cover_page page = {0};
    book_stream_t s = {0};
    int r = bf_zip_entry(&e->base, path, &s);
    if (r < 0) return r;
    if (html || bf_suffix(path, ".xhtml") || bf_suffix(path, ".html") || bf_suffix(path, ".htm")) {
        bf_copy(page.base, sizeof(page.base), path);
        r = s.size <= 128u * 1024u ? bf_xml(&s, e->sink, cover_event, &page, 0) : BOOK_UNSUPPORTED;
        r = bf_finish(&s, r);
        if (r < 0 || !page.image[0] || bf_external(page.image)) return r;
        path = page.image;
        if ((r = bf_zip_entry(&e->base, path, &s)) < 0) return r;
    }
    if (s.size > 4u * 1024u * 1024u) r = BOOK_UNSUPPORTED;
    else r = bf_suffix(path, ".svg") ? bf_svg_stream(&s, e->sink, path, &e->resources) :
        e->sink->image ? e->sink->image(e->sink->user, path, &s) : BOOK_OK;
    return bf_finish(&s, r);
}
#endif
static int epub_convert(const char *path, const book_sink_t *sink, int split,
                        const void *selection, const void *observer)
{
    epub *e = bf_alloc(sizeof(*e));
    char base[BF_PATH];
    size_t i, j;
    int r;
    unsigned rendered = 0;
    if (!e)
        return BOOK_NO_MEMORY;
    e->sink = sink;
#ifdef BOOK_FORMAT_ACPU
    e->selection = selection; e->observer = observer;
#else
    (void)selection; (void)observer;
#endif
    e->split = split;
    e->package.base = &e->base;
    e->package.current = &e->base;
    e->resources.user = &e->package;
    e->resources.open = bf_package_open;
    e->mapped.user = e;
    e->mapped.cancelled = mapped_cancel;
    e->mapped.text = mapped_text;
    e->mapped.paragraph = mapped_para;
    e->mapped.anchor = mapped_anchor;
    e->mapped.link = mapped_link;
    e->mapped.image = mapped_image;
    e->mapped.svg = mapped_svg;
    e->mapped.heading_target = mapped_heading_target;
    if (split)
    {
        if (snprintf(base, sizeof(base), "%s/basepackage.zip", path) >= (int)sizeof(base))
        {
            r = BOOK_UNSUPPORTED;
            goto done;
        }
    }
    else if ((r = bf_copy(base, sizeof(base), path)) < 0)
        goto done;
    if ((r = bf_zip_open(&e->base, base, sink)) < 0)
        goto done;
    if (split
#ifdef BOOK_FORMAT_ACPU
        && (!e->selection || !(e->selection->flags & (BOOK_COMPUTE_NOTE | BOOK_COMPUTE_METADATA)))
#endif
        && (r = scan_split(e, path)) < 0)
        goto done;
#ifdef BOOK_FORMAT_ACPU
    if (entry_try(e, path, &r)) goto done;
    if (e->selection && (e->selection->flags & BOOK_COMPUTE_NOTE)) {
        char target[BF_PATH]; book_stream_t stream = {0};
        r = bf_copy(target, sizeof(target), e->selection->note_target);
        char *fragment = strchr(target, '#'), *entry = target, *part = strchr(target, '!');
        if (r < 0 || !fragment || !fragment[1]) { r = BOOK_ERROR; goto done; }
        *fragment++ = 0;
        if (part) {
            *part++ = 0; entry = part;
            if (!split || !bf_chapter_zip(target) || strchr(target, '/') || strchr(target, '\\') ||
                snprintf(base, sizeof(base), "%s/%s", path, target) >= (int)sizeof(base))
            { r = BOOK_ERROR; goto done; }
            r = bf_zip_open(&e->current, base, sink);
            if (r < 0) goto done;
        }
        r = bf_zip_entry(part ? &e->current : &e->base, entry, &stream);
        if (r == BOOK_OK) r = book_markup_note(sink, &stream, fragment);
        if (stream.close) stream.close(stream.user);
        goto done;
    }
#endif
    r = package_xml(e, "META-INF/container.xml", container_event);
    if (r < 0 || !*e->opf)
    {
        if (r == BOOK_OK)
            r = BOOK_ERROR;
        goto done;
    }
#ifdef BOOK_FORMAT_ACPU
    if (e->selection && (e->selection->flags & BOOK_COMPUTE_METADATA)) {
        bool html = false;
        r = read_metadata(e, &html);
        if (r == BOOK_OK) r = read_cover(e, e->cover_path, html);
        goto done;
    }
#endif
    if ((r = package_xml(e, e->opf, opf_event)) < 0)
        goto done;
#ifdef BOOK_FORMAT_ACPU
    if (e->selection) { r = render_selected(e, path); goto done; }
#endif
    if ((r = bf_metadata(sink, e->title, e->author)) < 0 || (r = read_navigation(e)) < 0)
        goto done;
    for (i = 0; i < e->spine_count; ++i)
    {
        epub_item *item = item_at(e, e->spine[i]);
        epub_part *part = NULL;
        if (!item)
        {
            r = BOOK_ERROR;
            goto done;
        }
        if (!item->html)
        {
            r = BOOK_UNSUPPORTED;
            goto done;
        }
        for (j = 0; j < e->part_count; ++j)
            if (!e->parts[j].used && !strcmp(e->parts[j].href, item->href))
            {
                part = &e->parts[j];
                break;
            }
        if ((r = render_part(e, path, item->href, part)) < 0)
            goto done;
        ++rendered;
    }
    if (split)
        for (i = 0; i < e->part_count; ++i)
            if (!e->parts[i].used)
            {
                if ((r = render_part(e, path, e->parts[i].href, &e->parts[i])) < 0)
                    goto done;
                ++rendered;
            }
    if (!rendered)
        r = BOOK_ERROR;
done:
    bf_zip_close(&e->current);
    bf_zip_close(&e->base);
    clear_package(e);
    for (i = 0; i < e->part_count; ++i)
    {
        bf_free(e->parts[i].zip);
        bf_free(e->parts[i].href);
    }
    bf_free(e->parts);
    if (!e->heading_borrowed) bf_free(e->heading_targets);
    bf_free(e);
    return r;
}
int bf_epub(const char *path, const book_sink_t *sink, int split)
{ return epub_convert(path, sink, split, NULL, NULL); }
#ifdef BOOK_FORMAT_ACPU
int bf_epub_select(const char *path, const book_sink_t *sink, int split,
    const book_compute_selection_t *selection, const book_compute_observer_t *observer)
{ return epub_convert(path, sink, split, selection, observer); }
#endif
