/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed-chapter bodies need fragment targets, but no TOC titles or levels.
 * Keep the XML/ZIP reader so malformed input, CRC and cancellation still fail. */
typedef struct {
    epub *e;
    const char *base;
    size_t prefix;
    char href[BF_PATH];
    bool sent[32];
    unsigned depth;
    int ncx, in_toc, nav_depth;
} body_nav;

static int body_nav_target(body_nav *v, bool *sent)
{
    char path[BF_PATH];
    if (*sent || !*v->href) return BOOK_OK;
    *sent = true;
    int r = bf_resolve(v->base, v->href, path, sizeof(path));
    if (r < 0) return r;
    const char *hash = strchr(path, '#');
    if (!hash || (size_t)(hash - path) != v->prefix ||
        strncmp(path, v->e->body_href, v->prefix)) return BOOK_OK;
    return heading_target(v->e, path);
}

static int body_nav_event(void *user, int event, const char *name, const char *ns,
                          const bf_attrs *attrs, const char *text, size_t length)
{
    body_nav *v = user;
    (void)ns; (void)text; (void)length;
    if (event == 2) return BOOK_OK;
    if (v->ncx) {
        if (event == 1 && !strcmp(name, "navPoint")) {
            if (v->depth >= 32) return BOOK_UNSUPPORTED;
            v->sent[v->depth++] = false;
        }
        if (!v->depth) return BOOK_OK;
        if (event == 1 && !strcmp(name, "content")) {
            if (bf_attr(attrs, "src", "", v->href, sizeof(v->href)) < 0)
                return BOOK_UNSUPPORTED;
            return body_nav_target(v, &v->sent[v->depth - 1]);
        }
        /* Every nonempty content target is emitted at its start tag. */
        if (event == 0 && !strcmp(name, "navPoint")) --v->depth;
    } else {
        if (event == 1 && !strcmp(name, "nav")) {
            char type[128];
            ++v->nav_depth;
            if (bf_attr(attrs, "type", "http://www.idpf.org/2007/ops", type, sizeof(type)) < 0)
                return BOOK_UNSUPPORTED;
            if (strstr(type, "toc")) v->in_toc = v->nav_depth;
        }
        if (!v->in_toc) return BOOK_OK;
        if (event == 1 && !strcmp(name, "a")) {
            v->sent[0] = false;
            if (bf_attr(attrs, "href", "", v->href, sizeof(v->href)) < 0)
                return BOOK_UNSUPPORTED;
        }
        if (event == 0 && !strcmp(name, "a")) return body_nav_target(v, &v->sent[0]);
        if (event == 0 && !strcmp(name, "nav")) {
            if (v->in_toc == v->nav_depth) v->in_toc = 0;
            --v->nav_depth;
        }
    }
    return BOOK_OK;
}

static int read_body_navigation(epub *e, const epub_item *item)
{
    body_nav *v = bf_alloc(sizeof(*v));
    if (!v) return BOOK_NO_MEMORY;
    v->e = e; v->base = item->href; v->ncx = item->ncx;
    v->prefix = strlen(e->body_href);
    int r = bf_xml_entry(&e->base, item->href, e->sink, body_nav_event, v);
    bf_free(v);
    return r;
}
