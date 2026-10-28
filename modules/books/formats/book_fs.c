/* SPDX-License-Identifier: Apache-2.0 */
#define BF_RAW_STDIO
#include "book_internal.h"
#include <errno.h>
#ifdef BOOK_FORMAT_TARGET
#include "services/storage.h"
#include "services/storage_file.h"
#else
static void storage_lock(void)
{
}
static void storage_unlock(void)
{
}
static bool storage_path_available(const char *p)
{
    (void)p;
    return true;
}
static uint32_t storage_revision(void)
{
    return 0;
}
static uint32_t storage_path_revision(const char *path) { (void)path; return 0; }
static uint32_t storage_file_writers(const char *path) { (void)path; return 0; }
#endif

static book_file_observer_t file_observer;
static void *file_observer_user;
bool book_file_observer_begin(book_file_observer_t observer, void *user)
{
    if (!observer || file_observer) return false;
    file_observer = observer;
    file_observer_user = user;
    return true;
}
void book_file_observer_end(void *user)
{
    if (file_observer && user == file_observer_user)
    {
        file_observer = NULL;
        file_observer_user = NULL;
    }
}
static void observe_file(const char *path, book_file_event_t event)
{
    if (file_observer) file_observer(file_observer_user, path, event);
}

struct bf_file_handle
{
#ifdef BOOK_FORMAT_TARGET
    int descriptor;
#else
    FILE *file;
#endif
    char *path;
    uint32_t revision;
    int error, pushback;
    unsigned at, count;
    unsigned char buffer[512];
};
/* Called only while holding storage_lock. */
static size_t raw_read(bf_FILE *f, void *buffer, size_t bytes)
{
#ifdef BOOK_FORMAT_TARGET
    int n = read(f->descriptor, buffer, bytes);
    if (n < 0)
    {
        f->error = 1;
        return 0;
    }
    return (size_t)n;
#else
    size_t n = fread(buffer, 1, bytes, f->file);
    if (ferror(f->file))
        f->error = 1;
    return n;
#endif
}
static int available(bf_FILE *f)
{
    return f->revision == storage_revision() && storage_path_available(f->path);
}
bf_FILE *bf_fopen(const char *path, const char *mode)
{
    bf_FILE *f = bf_alloc(sizeof(*f));
    int opened;
    if (!f)
    {
        observe_file(path, BOOK_FILE_UNAVAILABLE);
        return NULL;
    }
    f->path = bf_strdup(path);
    f->pushback = -1;
    if (!f->path)
    {
        observe_file(path, BOOK_FILE_UNAVAILABLE);
        bf_free(f);
        return NULL;
    }
    storage_lock();
    f->revision = storage_revision();
    uint32_t version = storage_path_revision(path);
    errno = 0;
#ifdef BOOK_FORMAT_TARGET
    (void)mode;
    f->descriptor = storage_path_available(path) ? open(path, O_RDONLY, 0) : -1;
    opened = f->descriptor >= 0;
#else
    if (storage_path_available(path))
        f->file = fopen(path, mode);
    opened = f->file != NULL;
#endif
    int saved_errno = errno;
    storage_unlock();
    observe_file(path, opened ? BOOK_FILE_OPENED :
                 ((saved_errno == ENOENT || saved_errno == -ENOENT ||
                   saved_errno == ENOTDIR || saved_errno == -ENOTDIR) ?
                  BOOK_FILE_MISSING : BOOK_FILE_UNAVAILABLE));
    if (!available(f) || version != storage_path_revision(path) || storage_file_writers(path))
    {
        if (!available(f)) f->error = 1;
        observe_file(path, BOOK_FILE_UNAVAILABLE);
    }
    errno = saved_errno;
    if (!opened)
    {
        bf_free(f->path);
        bf_free(f);
        return NULL;
    }
    return f;
}
size_t bf_fread(void *buffer, size_t size, size_t count, bf_FILE *f)
{
    unsigned char *out = buffer;
    size_t bytes, done = 0, n;
    if (!size || count > SIZE_MAX / size)
        return 0;
    bytes = size * count;
    storage_lock();
    if (!available(f))
    {
        f->error = 1;
        storage_unlock();
        return 0;
    }
    if (f->pushback >= 0 && bytes)
    {
        out[done++] = (unsigned char)f->pushback;
        f->pushback = -1;
    }
    n = f->count - f->at;
    if (n > bytes - done)
        n = bytes - done;
    if (n)
    {
        memcpy(out + done, f->buffer + f->at, n);
        f->at += (unsigned)n;
        done += n;
    }
    if (done < bytes)
    {
        done += raw_read(f, out + done, bytes - done);
    }
    storage_unlock();
    return done / size;
}
int bf_fseek(bf_FILE *f, long offset, int origin)
{
    int r;
    storage_lock();
#ifdef BOOK_FORMAT_TARGET
    r = available(f) && lseek(f->descriptor, offset, origin) >= 0 ? 0 : -1;
#else
    r = available(f) ? fseek(f->file, offset, origin) : -1;
#endif
    if (r)
        f->error = 1;
    else
    {
        f->at = f->count = 0;
        f->pushback = -1;
    }
    storage_unlock();
    return r;
}
int bf_fclose(bf_FILE *f)
{
    int r;
    storage_lock();
#ifdef BOOK_FORMAT_TARGET
    r = close(f->descriptor);
#else
    r = fclose(f->file);
#endif
    storage_unlock();
    bf_free(f->path);
    bf_free(f);
    return r;
}
int bf_ferror(bf_FILE *f)
{
    return f->error;
}
int bf_fgetc(bf_FILE *f)
{
    if (f->pushback >= 0)
    {
        int c = f->pushback;
        f->pushback = -1;
        return c;
    }
    if (f->at == f->count)
    {
        storage_lock();
        if (!available(f))
        {
            f->error = 1;
            storage_unlock();
            return EOF;
        }
        f->count = (unsigned)raw_read(f, f->buffer, sizeof(f->buffer));
        f->at = 0;
        storage_unlock();
        if (!f->count)
            return EOF;
    }
    return f->buffer[f->at++];
}
int bf_ungetc(int c, bf_FILE *f)
{
    if (c == EOF || f->pushback >= 0)
        return EOF;
    f->pushback = (unsigned char)c;
    return c;
}
int bf_stat(const char *path, struct stat *st)
{
    int r;
    storage_lock();
    uint32_t version = storage_path_revision(path);
    uint32_t revision = storage_revision();
    errno = 0;
    r = storage_path_available(path) ? stat(path, st) : -1;
    int saved_errno = errno;
    storage_unlock();
    /* bf_file rejects missing resources before attempting fopen. Observe that
     * decision too, so adding a previously absent image invalidates reuse. */
    if (r)
        observe_file(path, (saved_errno == ENOENT || saved_errno == -ENOENT ||
                           saved_errno == ENOTDIR || saved_errno == -ENOTDIR) ?
                     BOOK_FILE_MISSING : BOOK_FILE_UNAVAILABLE);
    else if (!S_ISREG(st->st_mode) || (uint64_t)st->st_size > LONG_MAX)
        observe_file(path, BOOK_FILE_UNAVAILABLE);
    if (revision != storage_revision() || version != storage_path_revision(path) ||
        storage_file_writers(path) || !storage_path_available(path))
    {
        observe_file(path, BOOK_FILE_UNAVAILABLE);
        if (revision != storage_revision() || !storage_path_available(path)) r = -1;
    }
    errno = saved_errno;
    return r;
}
int bf_dir_open(bf_directory *d, const char *path)
{
    memset(d, 0, sizeof(*d));
    d->path = path;
    storage_lock();
    d->revision = storage_revision();
    if (storage_path_available(path))
        d->dir = opendir(path);
    storage_unlock();
    return d->dir ? BOOK_OK : BOOK_IO_ERROR;
}
struct dirent *bf_dir_next(bf_directory *d)
{
    struct dirent *e = NULL;
    storage_lock();
    if (d->revision != storage_revision() || !storage_path_available(d->path))
        d->status = BOOK_IO_ERROR;
    else
    {
        e = readdir(d->dir);
        if (e)
        {
            memcpy(&d->entry, e, sizeof(d->entry));
            e = &d->entry;
        }
    }
    storage_unlock();
    return e;
}
void bf_dir_close(bf_directory *d)
{
    if (d->dir)
    {
        storage_lock();
        closedir(d->dir);
        storage_unlock();
        d->dir = NULL;
    }
}
