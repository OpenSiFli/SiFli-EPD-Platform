/* SPDX-License-Identifier: Apache-2.0 */
#define BF_RAW_STDIO
#include "../formats/book_internal.h"
#include "book_compute_internal.h"

static book_compute_io *current;
static book_file_observer_t observer;
static void *observer_user;

struct bf_file_handle {
    void *handle;
    uint64_t offset, size;
    size_t at, count;
    int error, pushback;
    unsigned char buffer[512];
};
long book_compute_file_tell(bf_FILE *file)
{
    uint64_t at = file->offset - (file->count - file->at) - (file->pushback >= 0 ? 1u : 0u);
    return at > LONG_MAX ? -1 : (long)at;
}

book_compute_io *book_compute_fs_bind(book_compute_io *io)
{
    book_compute_io *previous = current;
    current = io;
    return previous;
}

bool book_file_observer_begin(book_file_observer_t callback, void *user)
{
    if (!callback || observer) return false;
    observer = callback;
    observer_user = user;
    return true;
}
void book_file_observer_end(void *user)
{
    if (observer && user == observer_user)
    {
        observer = NULL;
        observer_user = NULL;
    }
}
static void observe(const char *path, book_file_event_t event)
{
    if (observer) observer(observer_user, path, event);
}

bf_FILE *bf_fopen(const char *path, const char *mode)
{
    bf_FILE *f;
    int rc;
    if (!current || !path || !mode || (strcmp(mode, "r") && strcmp(mode, "rb")) ||
        book_compute_check(current) < 0) return NULL;
    f = bf_alloc(sizeof(*f));
    if (!f) return NULL;
    f->pushback = -1;
    rc = current->ops.open(current->user, path, &f->handle, &f->size);
    if (rc == BOOK_CANCELLED) book_compute_record(current, rc);
    if (book_compute_check(current) < 0 || rc != BOOK_OK || !f->handle)
    {
        if (f->handle) current->ops.close(current->user, f->handle);
        observe(path, BOOK_FILE_UNAVAILABLE);
        bf_free(f);
        return NULL;
    }
    observe(path, BOOK_FILE_OPENED);
    return f;
}

static size_t raw_read(bf_FILE *f, void *buffer, size_t bytes)
{
    int n;
    if (f->error || book_compute_check(current) < 0)
    {
        f->error = 1;
        return 0;
    }
    if (f->offset >= f->size || !bytes) return 0;
    if (bytes > INT_MAX) bytes = INT_MAX;
    if (bytes > f->size - f->offset) bytes = (size_t)(f->size - f->offset);
    n = current->ops.read_at(current->user, f->handle, f->offset, buffer, bytes);
    if (n < 0) book_compute_record(current, n);
    /* The size supplied at open describes a stable resource snapshot. */
    if (!n || (n > 0 && (size_t)n > bytes))
        book_compute_record(current, BOOK_IO_ERROR);
    if (book_compute_check(current) < 0)
    {
        f->error = 1;
        return 0;
    }
    f->offset += (unsigned)n;
    return (size_t)n;
}

size_t bf_fread(void *buffer, size_t size, size_t count, bf_FILE *f)
{
    unsigned char *out = buffer;
    size_t bytes, done = 0, n;
    if (!f || !size || !count) return 0;
    if (!buffer || count > SIZE_MAX / size)
    {
        f->error = 1;
        return 0;
    }
    if (book_compute_check(current) < 0) { f->error = 1; return 0; }
    bytes = size * count;
    if (f->pushback >= 0)
    {
        out[done++] = (unsigned char)f->pushback;
        f->pushback = -1;
    }
    n = f->count - f->at;
    if (n > bytes - done) n = bytes - done;
    if (n)
    {
        memcpy(out + done, f->buffer + f->at, n);
        f->at += n;
        done += n;
    }
    while (done < bytes && (n = raw_read(f, out + done, bytes - done)) != 0)
        done += n;
    return done / size;
}

int bf_fseek(bf_FILE *f, long offset, int origin)
{
    uint64_t base, magnitude, unread;
    if (!f) return -1;
    if (book_compute_check(current) < 0) goto fail;
    if (origin == SEEK_SET) base = 0;
    else if (origin == SEEK_END) base = f->size;
    else if (origin == SEEK_CUR)
    {
        unread = f->count - f->at + (f->pushback >= 0 ? 1u : 0u);
        if (unread > f->offset) goto fail;
        base = f->offset - unread;
    }
    else goto fail;
    if (offset < 0)
    {
        magnitude = (uint64_t)(-(offset + 1)) + 1;
        if (magnitude > base) goto fail;
        base -= magnitude;
    }
    else
    {
        if ((uint64_t)offset > UINT64_MAX - base) goto fail;
        base += (uint64_t)offset;
    }
    f->offset = base;
    f->at = f->count = 0;
    f->pushback = -1;
    return 0;
fail:
    f->error = 1;
    return -1;
}
int bf_fclose(bf_FILE *f)
{
    int rc;
    if (!f) return -1;
    rc = current->ops.close(current->user, f->handle);
    book_compute_record(current, rc);
    bf_free(f);
    return rc == BOOK_OK ? 0 : -1;
}
int bf_ferror(bf_FILE *f) { return f ? f->error : 1; }
int bf_fgetc(bf_FILE *f)
{
    int c;
    if (!f) return EOF;
    if (book_compute_check(current) < 0) { f->error = 1; return EOF; }
    if (f->pushback >= 0)
    {
        c = f->pushback;
        f->pushback = -1;
        return c;
    }
    if (f->at == f->count)
    {
        f->count = raw_read(f, f->buffer, sizeof(f->buffer));
        f->at = 0;
        if (!f->count) return EOF;
    }
    return f->buffer[f->at++];
}
int bf_ungetc(int c, bf_FILE *f)
{
    if (!f || c == EOF || f->pushback >= 0) return EOF;
    f->pushback = (unsigned char)c;
    return f->pushback;
}
int bf_stat(const char *path, struct stat *st)
{
    int rc;
    if (!current || !path || !st || book_compute_check(current) < 0) return -1;
    rc = current->ops.stat(current->user, path, st);
    if (rc == BOOK_CANCELLED) book_compute_record(current, rc);
    if (rc != BOOK_OK) observe(path, BOOK_FILE_UNAVAILABLE);
    return book_compute_check(current) < 0 || rc != BOOK_OK ? -1 : 0;
}
int bf_dir_open(bf_directory *d, const char *path)
{
    int rc;
    memset(d, 0, sizeof(*d));
    d->path = path;
    if (!current) return d->status = BOOK_IO_ERROR;
    if ((rc = book_compute_check(current)) < 0) return d->status = rc;
    if (!current->ops.dir_open)
        return d->status = BOOK_UNSUPPORTED;
    rc = current->ops.dir_open(current->user, path, &d->dir);
    if (rc == BOOK_OK && !d->dir) rc = BOOK_IO_ERROR;
    book_compute_record(current, rc);
    if ((rc = book_compute_check(current)) < 0)
    {
        bf_dir_close(d);
        return d->status = rc;
    }
    return BOOK_OK;
}
struct dirent *bf_dir_next(bf_directory *d)
{
    int rc;
    if (!d->dir || d->status < 0) return NULL;
    if ((rc = book_compute_check(current)) < 0) { d->status = rc; return NULL; }
    memset(&d->entry, 0xff, sizeof(d->entry));
    rc = current->ops.dir_next(current->user, d->dir, d->entry.d_name, sizeof(d->entry.d_name));
    if (rc > 1 || (rc == 1 && !memchr(d->entry.d_name, 0, sizeof(d->entry.d_name))))
        rc = BOOK_IO_ERROR;
    book_compute_record(current, rc < 0 ? rc : BOOK_OK);
    if ((d->status = book_compute_check(current)) < 0 || rc == 0) return NULL;
    return &d->entry;
}
void bf_dir_close(bf_directory *d)
{
    if (d->dir)
    {
        int rc = current->ops.dir_close(current->user, d->dir);
        d->dir = NULL;
        book_compute_record(current, rc);
        if (rc < 0 && d->status == BOOK_OK) d->status = rc;
    }
}
