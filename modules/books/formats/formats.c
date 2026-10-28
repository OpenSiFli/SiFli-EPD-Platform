/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"
#include <libxml/xmlmemory.h>

typedef union bf_mem bf_mem;
union bf_mem
{
    struct
    {
        bf_mem *prev, *next;
        size_t size;
    } h;
    long double align;
    void *ptr;
};
static struct
{
    book_allocator_t a;
    bf_mem *head;
    int active, oom;
} scope;
static xmlFreeFunc saved_free;
static xmlMallocFunc saved_malloc, saved_atomic;
static xmlReallocFunc saved_realloc;
static xmlStrdupFunc saved_strdup;
static xmlExternalEntityLoader saved_loader;

void *bf_alloc(size_t n)
{
    bf_mem *m;
    if (!scope.active)
        return NULL;
    if (!n)
        n = 1;
    if (n > SIZE_MAX - sizeof(*m))
    {
        scope.oom = 1;
        return NULL;
    }
    m = scope.a.alloc(sizeof(*m) + n);
    if (!m)
    {
        scope.oom = 1;
        return NULL;
    }
    m->h.size = n;
    m->h.prev = NULL;
    m->h.next = scope.head;
    if (scope.head)
        scope.head->h.prev = m;
    scope.head = m;
    memset(m + 1, 0, n);
    return m + 1;
}
void bf_free(void *p)
{
    bf_mem *m;
    if (!p)
        return;
    m = (bf_mem *)p - 1;
    if (m->h.prev)
        m->h.prev->h.next = m->h.next;
    else
        scope.head = m->h.next;
    if (m->h.next)
        m->h.next->h.prev = m->h.prev;
    scope.a.free(m);
}
void *bf_realloc(void *p, size_t n)
{
    void *q;
    size_t old;
    if (!p)
        return bf_alloc(n);
    if (!n)
    {
        bf_free(p);
        return NULL;
    }
    old = ((bf_mem *)p - 1)->h.size;
    q = bf_alloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, old < n ? old : n);
    bf_free(p);
    return q;
}
char *bf_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = bf_alloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}
int bf_oom(void)
{
    return scope.oom;
}
int bf_active(void)
{
    return scope.active;
}
static xmlParserInputPtr no_external(const char *url, const char *id, xmlParserCtxtPtr c)
{
    (void)url;
    (void)id;
    (void)c;
    return NULL;
}
int book_markup_begin(const book_allocator_t *a)
{
    if (scope.active || !a || !a->alloc || !a->free || !a->realloc)
        return BOOK_ERROR;
    memset(&scope, 0, sizeof(scope));
    scope.a = *a;
    scope.active = 1;
    xmlGcMemGet(&saved_free, &saved_malloc, &saved_atomic, &saved_realloc, &saved_strdup);
    xmlGcMemSetup(bf_free, bf_alloc, bf_alloc, bf_realloc, bf_strdup);
    saved_loader = xmlGetExternalEntityLoader();
    xmlSetExternalEntityLoader(no_external);
    xmlInitParser();
    if (scope.oom)
    {
        book_markup_end();
        return BOOK_NO_MEMORY;
    }
    return BOOK_OK;
}
void book_markup_end(void)
{
    if (!scope.active)
        return;
    xmlCleanupParser();
    xmlSetExternalEntityLoader(saved_loader);
    xmlGcMemSetup(saved_free, saved_malloc, saved_atomic, saved_realloc, saved_strdup);
    while (scope.head)
        bf_free(scope.head + 1);
    memset(&scope, 0, sizeof(scope));
    saved_free = NULL;
    saved_malloc = NULL;
    saved_atomic = NULL;
    saved_realloc = NULL;
    saved_strdup = NULL;
    saved_loader = NULL;
}
int bf_cancel(const book_sink_t *s)
{
    return s->cancelled && s->cancelled(s->user) ? BOOK_CANCELLED : BOOK_OK;
}
int bf_text(const book_sink_t *s, const char *p, size_t n, unsigned style)
{
    int rc;
    size_t k;
    while (n)
    {
        if ((rc = bf_cancel(s)) < 0)
            return rc;
        k = n > BF_TEXT ? BF_TEXT : n;
        if (k < n)
            while (k && ((unsigned char)p[k] & 0xc0) == 0x80)
                --k;
        if (!k)
            return BOOK_ERROR;
        rc = s->text ? s->text(s->user, p, k, style) : BOOK_OK;
        if (rc < 0)
            return rc;
        p += k;
        n -= k;
    }
    return BOOK_OK;
}
int bf_para(const book_sink_t *s)
{
    int r = bf_cancel(s);
    return r < 0 ? r : s->paragraph ? s->paragraph(s->user) : BOOK_OK;
}
int bf_anchor(const book_sink_t *s, const char *id, const char *title, unsigned level)
{
    int r = bf_cancel(s);
    return r < 0 ? r : s->anchor ? s->anchor(s->user, id, title, level) : BOOK_OK;
}
int bf_link(const book_sink_t *s, const char *id, bool begin)
{
    int r = bf_cancel(s);
    return r < 0 ? r : s->link ? s->link(s->user, id, begin) : BOOK_OK;
}
int bf_metadata(const book_sink_t *s, const char *title, const char *author)
{
    int r = bf_cancel(s);
    return r < 0 ? r : s->metadata ? s->metadata(s->user, title, author) : BOOK_OK;
}
int bf_copy(char *d, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n >= cap)
        return BOOK_UNSUPPORTED;
    memcpy(d, s, n + 1);
    return BOOK_OK;
}
void bf_append(char *d, size_t cap, const char *s, size_t n)
{
    size_t at = strlen(d);
    if (at >= cap - 1)
        return;
    if (n > cap - 1 - at)
    {
        n = cap - 1 - at;
        while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
            --n;
    }
    memcpy(d + at, s, n);
    d[at + n] = 0;
}
int bf_casecmp(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
    {
        ++a;
        ++b;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int bf_suffix(const char *s, const char *x)
{
    size_t n = strlen(s), k = strlen(x);
    return n >= k && !bf_casecmp(s + n - k, x);
}
int bf_chapter_zip(const char *s)
{
    return !strncmp(s, "chapter", 7) && (s[7] == '_' || isdigit((unsigned char)s[7])) &&
           bf_suffix(s, ".zip");
}
book_format_t book_format_detect(const char *path, bool directory)
{
    static const struct { const char *suffix; book_format_t format; } ext[] = {
        {".txt", BOOK_FORMAT_TXT}, {".md", BOOK_FORMAT_MARKDOWN},
        {".markdown", BOOK_FORMAT_MARKDOWN}, {".epub", BOOK_FORMAT_EPUB},
        {".mobi", BOOK_FORMAT_MOBI}
    };
    unsigned i;
    if (!path)
        return BOOK_FORMAT_UNKNOWN;
    if (directory)
    {
        char p[BF_PATH];
        struct stat st;
        bf_directory d;
        struct dirent *e;
        int found = 0;
        if (snprintf(p, sizeof(p), "%s/basepackage.zip", path) >= (int)sizeof(p) || stat(p, &st) ||
            !S_ISREG(st.st_mode))
            return BOOK_FORMAT_UNKNOWN;
        if (bf_dir_open(&d, path) < 0)
            return BOOK_FORMAT_UNKNOWN;
        while ((e = bf_dir_next(&d)) != NULL)
            if (bf_chapter_zip(e->d_name))
            {
                found = 1;
                break;
            }
        bf_dir_close(&d);
        return found ? BOOK_FORMAT_SPLIT_EPUB : BOOK_FORMAT_UNKNOWN;
    }
    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); ++i)
        if (bf_suffix(path, ext[i].suffix))
            return ext[i].format;
    return BOOK_FORMAT_UNKNOWN;
}
const char *book_format_name(book_format_t f)
{
    switch (f)
    {
    case BOOK_FORMAT_TXT: return "TXT";
    case BOOK_FORMAT_MARKDOWN: return "Markdown";
    case BOOK_FORMAT_EPUB: return "EPUB";
    case BOOK_FORMAT_SPLIT_EPUB: return "Split EPUB";
    case BOOK_FORMAT_MOBI: return "MOBI";
    default: return "Unknown";
    }
}
int book_format_convert(const char *path, const book_sink_t *sink, const book_allocator_t *a,
                        char *error, size_t cap)
{
    struct stat st;
    int rc;
    book_format_t f;
    if (error && cap)
        error[0] = 0;
    if (!path || !sink)
        return BOOK_ERROR;
    if (stat(path, &st))
        rc = BOOK_IO_ERROR;
    else if ((rc = book_markup_begin(a)) == BOOK_OK)
    {
        f = book_format_detect(path, S_ISDIR(st.st_mode));
        rc = bf_cancel(sink);
        if (rc == BOOK_OK)
            switch (f)
            {
            case BOOK_FORMAT_MARKDOWN:
                rc = bf_markdown(path, sink);
                break;
            case BOOK_FORMAT_EPUB:
            case BOOK_FORMAT_SPLIT_EPUB:
                rc = bf_epub(path, sink, f == BOOK_FORMAT_SPLIT_EPUB);
                break;
            default:
                rc = BOOK_UNSUPPORTED;
                break;
            }
        if (bf_oom() && rc != BOOK_CANCELLED)
            rc = BOOK_NO_MEMORY;
        book_markup_end();
    }
    /* A sink can report the failing image or resource through the same buffer. */
    if (rc < 0 && error && cap && !error[0])
        snprintf(error, cap, "%s",
                 rc == BOOK_CANCELLED   ? "Conversion cancelled"
                 : rc == BOOK_NO_MEMORY ? "Document memory allocation failed"
                 : rc == BOOK_NO_SPACE  ? "Document cache storage space exhausted"
                 : rc == BOOK_IO_ERROR  ? "Cannot read document or referenced resource"
                 : rc == BOOK_UNSUPPORTED
                     ? "Unsupported format, protected content, or document limit"
                     : "Invalid document or ZIP checksum mismatch");
    return rc;
}
