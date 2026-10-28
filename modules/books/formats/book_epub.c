/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"

typedef struct epub_item
{
    char *id, *href;
    int html, nav, ncx, linear;
} epub_item;
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
    epub_item *items;
    size_t count;
    char **spine;
    size_t spine_count;
    epub_part *parts;
    size_t part_count;
    const epub_part *active_part;
    int capture, split;
} epub;
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
        if (!strcmp(name, "title"))
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
        if (e->count >= SIZE_MAX / sizeof(*p))
            return BOOK_UNSUPPORTED;
        if ((r = bf_resolve(e->opf, href, path, sizeof(path))) < 0)
            return r;
        if (bf_external(path))
            return BOOK_UNSUPPORTED;
        p = bf_realloc(e->items, (e->count + 1) * sizeof(*p));
        if (!p)
            return BOOK_NO_MEMORY;
        e->items = p;
        p = &e->items[e->count++];
        memset(p, 0, sizeof(*p));
        p->id = bf_strdup(id);
        p->href = bf_strdup(path);
        if (!p->id || !p->href)
            return BOOK_NO_MEMORY;
        p->html = !strcmp(media, "application/xhtml+xml") || !strcmp(media, "text/html");
        p->nav = strstr(props, "nav") != NULL;
        p->ncx = !strcmp(media, "application/x-dtbncx+xml");
    }
    if (!strcmp(name, "itemref"))
    {
        char **p;
        if (bf_attr(a, "linear", "", props, sizeof(props)) < 0)
            return BOOK_UNSUPPORTED;
        if (!strcmp(props, "no"))
            return BOOK_OK;
        if (bf_attr(a, "idref", "", id, sizeof(id)) <= 0)
            return BOOK_ERROR;
        if (e->spine_count >= SIZE_MAX / sizeof(*p))
            return BOOK_UNSUPPORTED;
        p = bf_realloc(e->spine, (e->spine_count + 1) * sizeof(*p));
        if (!p)
            return BOOK_NO_MEMORY;
        e->spine = p;
        e->spine[e->spine_count] = bf_strdup(id);
        if (!e->spine[e->spine_count++])
            return BOOK_NO_MEMORY;
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
static int nav_emit(nav_state *v, nav_frame *f, unsigned level)
{
    char p[BF_PATH];
    int r;
    if (f->sent || !*f->href)
        return BOOK_OK;
    f->sent = 1;
    r = bf_resolve(v->base, f->href, p, sizeof(p));
    return r < 0 ? r : mapped_anchor(v->e, p, *f->title ? f->title : f->href, level ? level : 1);
}
static int nav_event(void *u, int event, const char *name, const char *ns, const bf_attrs *a,
                     const char *s, size_t n)
{
    nav_state *v = u;
    nav_frame *f;
    char type[128];
    int r;
    (void)ns;
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
static int read_navigation(epub *e)
{
    size_t i;
    epub_item *item = NULL;
    nav_state *n;
    int r;
    for (i = 0; i < e->count; ++i)
        if (e->items[i].nav)
        {
            item = &e->items[i];
            break;
        }
    if (!item)
        for (i = 0; i < e->count; ++i)
            if (e->items[i].ncx && (!*e->toc || !strcmp(e->toc, e->items[i].id)))
            {
                item = &e->items[i];
                break;
            }
    if (!item)
        return BOOK_OK;
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
static int render_part(epub *e, const char *dir, const char *href, epub_part *part)
{
    book_stream_t s;
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
        r = bf_zip_entry(&e->current, href, &s);
    }
    else
    {
        e->package.current = &e->base;
        r = bf_zip_entry(&e->base, href, &s);
    }
    if (r < 0)
        return r;
    /* markup emits anchor(href, NULL, 0) before content. Keep this root
     * location distinct from titled navigation and fragment anchors: the
     * document index uses it as the spine/part boundary without a sink ABI change. */
    r = book_markup_convert(&e->mapped, &s, href, &e->resources);
    return bf_finish(&s, r);
}
int bf_epub(const char *path, const book_sink_t *sink, int split)
{
    epub *e = bf_alloc(sizeof(*e));
    char base[BF_PATH];
    size_t i, j;
    int r;
    unsigned rendered = 0;
    if (!e)
        return BOOK_NO_MEMORY;
    e->sink = sink;
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
    if (split && (r = scan_split(e, path)) < 0)
        goto done;
    r = bf_xml_entry(&e->base, "META-INF/container.xml", sink, container_event, e);
    if (r < 0 || !*e->opf)
    {
        if (r == BOOK_OK)
            r = BOOK_ERROR;
        goto done;
    }
    if ((r = bf_xml_entry(&e->base, e->opf, sink, opf_event, e)) < 0)
        goto done;
    if ((r = bf_metadata(sink, e->title, e->author)) < 0 || (r = read_navigation(e)) < 0)
        goto done;
    for (i = 0; i < e->spine_count; ++i)
    {
        epub_item *item = NULL;
        epub_part *part = NULL;
        for (j = 0; j < e->count; ++j)
            if (!strcmp(e->spine[i], e->items[j].id))
            {
                item = &e->items[j];
                break;
            }
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
    for (i = 0; i < e->count; ++i)
    {
        bf_free(e->items[i].id);
        bf_free(e->items[i].href);
    }
    bf_free(e->items);
    for (i = 0; i < e->spine_count; ++i)
        bf_free(e->spine[i]);
    bf_free(e->spine);
    for (i = 0; i < e->part_count; ++i)
    {
        bf_free(e->parts[i].zip);
        bf_free(e->parts[i].href);
    }
    bf_free(e->parts);
    bf_free(e);
    return r;
}
