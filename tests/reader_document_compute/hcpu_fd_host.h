/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
static unsigned fd_live, fd_peak, fd_limit = 512, fd_reserved;
static bool fd_tracked[2048];
static char fd_source_path[600], fd_last_log[1024];
static int fd_read_error, fd_seek_error;
static int fd_open_error, fd_close_error;
static unsigned fd_open_attempts, fd_heap_used, fd_heap_limit = UINT32_MAX;
static unsigned fd_heap_charge[2048];
static char fd_pressure_log[1024];
static void (*fd_close_probe)(void);
static const char *fd_path(const char *path)
{ return !strcmp(path, "/flash/fd-source.epub") ? fd_source_path : path; }
static int fd_open(const char *path, int flags, ...)
{
    ++fd_open_attempts;
    if (fd_open_error) { errno = fd_open_error; return -1; }
    if (fd_live + fd_reserved >= fd_limit) { errno = -ENOMEM; return -1; }
    unsigned charge = !strcmp(path, "/flash/fd-source.epub") ? 4140 : 512;
    if (charge > fd_heap_limit - fd_heap_used) { errno = -ENOMEM; return -1; }
    int fd = host_open(fd_path(path), flags);
    if (fd >= 0) {
        assert(fd < 2048 && !fd_tracked[fd]); fd_tracked[fd] = true;
        fd_heap_charge[fd] = charge; fd_heap_used += charge;
        if (++fd_live > fd_peak) fd_peak = fd_live;
    }
    return fd;
}
static int fd_close(int fd)
{
    assert(fd >= 0 && fd < 2048 && fd_tracked[fd]);
    if (fd_close_error) { errno = fd_close_error; return -1; }
    int rc = _close(fd);
    if (!rc) { fd_tracked[fd] = false; --fd_live; fd_heap_used -= fd_heap_charge[fd]; fd_heap_charge[fd] = 0; }
    if (fd_close_probe) { void (*probe)(void) = fd_close_probe; fd_close_probe = NULL; probe(); }
    return rc;
}
static int fd_read(int fd, void *out, size_t size)
{
    if (fd_read_error) { errno = fd_read_error; return -1; }
    return host_read(fd, out, size);
}
static long fd_seek(int fd, long offset, int whence)
{
    if (fd_seek_error) { errno = fd_seek_error; return -1; }
    return _lseek(fd, offset, whence);
}
static int fd_stat(const char *path, struct stat *st) { return stat(fd_path(path), st); }
static void fd_log(const char *format, ...)
{
    char message[1024]; va_list args; va_start(args, format);
    vsnprintf(message, sizeof(message), format, args); va_end(args);
    if (strstr(message, "[document] resource_io ")) strcpy(fd_last_log, message);
    if (strstr(message, "[document] open_pressure ")) strcpy(fd_pressure_log, message);
    rt_kprintf("%s", message);
}
#undef open
#undef close
#undef read
#undef lseek
#define open fd_open
#define close fd_close
#define read fd_read
#define lseek fd_seek
#define stat(path, st) fd_stat(path, st)
#define rt_kprintf fd_log
