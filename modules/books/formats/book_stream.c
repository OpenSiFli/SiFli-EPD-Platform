/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"

int bf_external(const char *s)
{
    const char *p = s;
    while (*p && *p != '/' && *p != '#')
    {
        if (*p == ':')
            return 1;
        ++p;
    }
    return s[0] == '/' && s[1] == '/';
}
static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
typedef struct path_work
{
    char buf[BF_PATH], decoded[BF_PATH], fragment[BF_PATH];
    size_t marks[128];
} path_work;
static int resolve_work(const char *base, const char *ref, char *out, size_t cap, path_work *work)
{
    char *buf = work->buf, *decoded = work->decoded, *fragment = work->fragment;
    size_t *marks = work->marks;
    size_t i = 0, n = 0, start = 0, used = 0, depth = 0;
    const char *hash, *slash;
    if (!base)
        base = "";
    if (!ref)
        return BOOK_ERROR;
    if (bf_external(ref))
        return bf_copy(out, cap, ref);
    hash = strchr(ref, '#');
    fragment[0] = 0;
    if (hash && bf_copy(fragment, BF_PATH, hash) < 0)
        return BOOK_UNSUPPORTED;
    while (ref[i] && ref + i != hash)
    {
        int a, b;
        unsigned char c = (unsigned char)ref[i++];
        if (c == '%' && ref[i] && ref[i + 1] && (a = hexval(ref[i])) >= 0 &&
            (b = hexval(ref[i + 1])) >= 0)
        {
            c = (unsigned char)(a * 16 + b);
            i += 2;
        }
        if (!c || c == '\\' || c < 32)
            return BOOK_ERROR;
        if (n + 1 >= BF_PATH)
            return BOOK_UNSUPPORTED;
        decoded[n++] = (char)c;
    }
    decoded[n] = 0;
    if (!n)
    {
        slash = strchr(base, '#');
        n = slash ? (size_t)(slash - base) : strlen(base);
        if (n >= BF_PATH)
            return BOOK_UNSUPPORTED;
        memcpy(buf, base, n);
        buf[n] = 0;
    }
    else if (decoded[0] == '/')
    {
        if (bf_copy(buf, BF_PATH, decoded + 1) < 0)
            return BOOK_UNSUPPORTED;
    }
    else
    {
        slash = strrchr(base, '/');
        n = slash ? (size_t)(slash - base + 1) : 0;
        if (n + strlen(decoded) >= BF_PATH)
            return BOOK_UNSUPPORTED;
        memcpy(buf, base, n);
        strcpy(buf + n, decoded);
    }
    n = strlen(buf);
    out[0] = 0;
    for (i = 0; i <= n; ++i)
        if (buf[i] == '/' || !buf[i])
        {
            size_t len = i - start;
            if (len == 2 && buf[start] == '.' && buf[start + 1] == '.')
            {
                if (!depth)
                    return BOOK_ERROR;
                used = marks[--depth];
                out[used] = 0;
            }
            else if (len && !(len == 1 && buf[start] == '.'))
            {
                if (depth >= 128 || used + len + 2 >= cap)
                    return BOOK_UNSUPPORTED;
                marks[depth++] = used;
                if (used)
                    out[used++] = '/';
                memcpy(out + used, buf + start, len);
                used += len;
                out[used] = 0;
            }
            start = i + 1;
        }
    if (used + strlen(fragment) >= cap)
        return BOOK_UNSUPPORTED;
    strcpy(out + used, fragment);
    return BOOK_OK;
}
int bf_resolve(const char *base, const char *ref, char *out, size_t cap)
{
    path_work *work = bf_alloc(sizeof(*work));
    int result;
    if (!work)
        return BOOK_NO_MEMORY;
    result = resolve_work(base, ref, out, cap, work);
    bf_free(work);
    return result;
}
static int file_read(void *u, void *b, size_t n)
{
    FILE *f = u;
    size_t r;
    if (n > INT_MAX)
        n = INT_MAX;
    r = fread(b, 1, n, f);
    return r ? (int)r : ferror(f) ? BOOK_IO_ERROR : 0;
}
static int file_seek(void *u, uint64_t n)
{
    return n > LONG_MAX || fseek(u, (long)n, SEEK_SET) ? BOOK_IO_ERROR : BOOK_OK;
}
static void file_close(void *u)
{
    fclose(u);
}
int bf_file(const char *path, book_stream_t *s)
{
    struct stat st;
    FILE *f;
    memset(s, 0, sizeof(*s));
    if (stat(path, &st) || (uint64_t)st.st_size > LONG_MAX || !S_ISREG(st.st_mode))
        return BOOK_IO_ERROR;
    f = fopen(path, "rb");
    if (!f)
        return BOOK_IO_ERROR;
    s->user = f;
    s->read = file_read;
    s->seek = file_seek;
    s->close = file_close;
    s->size = (uint64_t)st.st_size;
    return BOOK_OK;
}
int bf_finish(book_stream_t *s, int status)
{
    char b[512];
    int n;
    if (s->read && status == BOOK_OK)
        while ((n = s->read(s->user, b, sizeof(b))) > 0)
        {
        }
    else
        n = 0;
    if (status == BOOK_OK && n < 0)
        status = n;
    if (s->close)
        s->close(s->user);
    memset(s, 0, sizeof(*s));
    return status;
}
static void *zip_alloc(void *u, size_t n, size_t size)
{
    (void)u;
    return size && n > SIZE_MAX / size ? NULL : bf_alloc(n * size);
}
static void *zip_realloc(void *u, void *p, size_t n, size_t size)
{
    (void)u;
    return size && n > SIZE_MAX / size ? NULL : bf_realloc(p, n * size);
}
static void zip_free(void *u, void *p)
{
    (void)u;
    bf_free(p);
}
static size_t zip_read(void *u, mz_uint64 offset, void *b, size_t n)
{
    bf_zip *z = u;
    size_t r;
    if ((z->status = bf_cancel(z->sink)) < 0)
        return 0;
    if (offset > LONG_MAX || fseek(z->file, (long)offset, SEEK_SET))
    {
        z->status = BOOK_IO_ERROR;
        return 0;
    }
    r = fread(b, 1, n, z->file);
    if (r != n)
        z->status = BOOK_IO_ERROR;
    return r;
}
int bf_zip_open(bf_zip *z, const char *path, const book_sink_t *sink)
{
    struct stat st;
    memset(z, 0, sizeof(*z));
    z->sink = sink;
    if (stat(path, &st) || st.st_size < 22 || st.st_size > LONG_MAX)
        return BOOK_IO_ERROR;
    z->file = fopen(path, "rb");
    if (!z->file)
        return BOOK_IO_ERROR;
    z->zip.m_pAlloc = zip_alloc;
    z->zip.m_pRealloc = zip_realloc;
    z->zip.m_pFree = zip_free;
    z->zip.m_pRead = zip_read;
    z->zip.m_pIO_opaque = z;
    if (!mz_zip_reader_init(&z->zip, (mz_uint64)st.st_size, 0))
    {
        int r = bf_oom() ? BOOK_NO_MEMORY : z->status < 0 ? z->status : BOOK_ERROR;
        bf_zip_close(z);
        return r;
    }
    return BOOK_OK;
}
void bf_zip_close(bf_zip *z)
{
    if (z->zip.m_pState)
        mz_zip_reader_end(&z->zip);
    if (z->file)
        fclose(z->file);
    memset(z, 0, sizeof(*z));
}
typedef struct zip_stream
{
    bf_zip *zip;
    mz_zip_reader_extract_iter_state *iter;
    uint64_t left;
    int status;
} zip_stream;
static int entry_read(void *u, void *b, size_t n)
{
    zip_stream *s = u;
    size_t r;
    int rc = bf_cancel(s->zip->sink);
    if (rc < 0)
        return s->status = rc;
    if (s->status < 0)
        return s->status;
    if (!s->iter)
        return 0;
    if (n > INT_MAX)
        n = INT_MAX;
    r = mz_zip_reader_extract_iter_read(s->iter, b, n);
    if (r > s->left)
        return s->status = BOOK_ERROR;
    s->left -= r;
    if (!r || !s->left)
    {
        int ok = mz_zip_reader_extract_iter_free(s->iter);
        s->iter = NULL;
        if (!ok || s->left)
        {
            s->status = bf_oom()             ? BOOK_NO_MEMORY
                        : s->zip->status < 0 ? s->zip->status
                                             : BOOK_ERROR;
            /* Decoders may stop after the last payload bytes. Report integrity
             * failure now instead of requiring another read to observe it. */
            return s->status;
        }
    }
    return (int)r;
}
static void entry_close(void *u)
{
    zip_stream *s = u;
    if (s->iter)
        mz_zip_reader_extract_iter_free(s->iter);
    bf_free(s);
}
int bf_zip_has(bf_zip *z, const char *name)
{
    return z && mz_zip_reader_locate_file(&z->zip, name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE) >= 0;
}
int bf_zip_name(bf_zip *z, unsigned i, char *out, size_t cap)
{
    mz_uint n = mz_zip_reader_get_filename(&z->zip, i, NULL, 0);
    if (!n || n > cap)
        return BOOK_UNSUPPORTED;
    return mz_zip_reader_get_filename(&z->zip, i, out, (mz_uint)cap) ? BOOK_OK : BOOK_ERROR;
}
int bf_zip_entry(bf_zip *z, const char *name, book_stream_t *out)
{
    int index;
    zip_stream *s;
    mz_zip_archive_file_stat st;
    memset(out, 0, sizeof(*out));
    if (!z)
        return BOOK_IO_ERROR;
    index = mz_zip_reader_locate_file(&z->zip, name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (index < 0)
        return BOOK_IO_ERROR;
    if (!mz_zip_reader_file_stat(&z->zip, (mz_uint)index, &st))
        return BOOK_ERROR;
    if (st.m_is_encrypted || (st.m_method != 0 && st.m_method != 8))
        return BOOK_UNSUPPORTED;
    s = bf_alloc(sizeof(*s));
    if (!s)
        return BOOK_NO_MEMORY;
    s->zip = z;
    s->left = st.m_uncomp_size;
    s->iter = mz_zip_reader_extract_iter_new(&z->zip, (mz_uint)index, 0);
    if (!s->iter)
    {
        bf_free(s);
        return bf_oom() ? BOOK_NO_MEMORY : z->status < 0 ? z->status : BOOK_ERROR;
    }
    out->user = s;
    out->size = st.m_uncomp_size;
    out->read = entry_read;
    out->close = entry_close;
    return BOOK_OK;
}
int bf_package_open(void *user, const char *base, const char *ref, book_stream_t *s)
{
    bf_package *p = user;
    char name[BF_PATH], *hash;
    int rc;
    if (bf_external(ref))
        return BOOK_UNSUPPORTED;
    if ((rc = bf_resolve(base, ref, name, sizeof(name))) < 0)
        return rc;
    hash = strchr(name, '#');
    if (hash)
        *hash = 0;
    if (bf_zip_has(p->current, name))
        return bf_zip_entry(p->current, name, s);
    return bf_zip_entry(p->base, name, s);
}
int bf_picture(const book_sink_t *sink, const book_resources_t *res, const char *base,
               const char *ref)
{
    book_stream_t s;
    char id[BF_PATH];
    int r, svg;
    if (!ref || !*ref)
        return BOOK_OK;
    svg = bf_suffix(ref, ".svg");
    if (!svg && !sink->image)
        return BOOK_OK;
    if ((svg && !sink->svg) || bf_suffix(ref, ".gif"))
        return BOOK_UNSUPPORTED;
    if (!res || !res->open || bf_external(ref))
        return BOOK_OK;
    if ((r = bf_resolve(base, ref, id, sizeof(id))) < 0)
        return r;
    if ((r = bf_cancel(sink)) < 0)
        return r;
    if ((r = res->open(res->user, base, ref, &s)) < 0)
        return r;
    r = svg ? bf_svg_stream(&s, sink, id, res) :
        s.read ? sink->image(sink->user, id, &s) : BOOK_IO_ERROR;
    return bf_finish(&s, r);
}
