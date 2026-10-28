/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_file.h"
#include "platform/epd_memory.h"
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>

typedef struct { FILE *file; reader_file_access_t access; } file_stream_t;
static int volume(const char *path, storage_volume_t *out)
{
    if (!path) return EPD_READ_INVALID;
    for (unsigned i = 0; i < STORAGE_COUNT; ++i)
    {
        const char *root = storage_root((storage_volume_t)i);
        size_t n = strlen(root);
        if (!strncmp(path, root, n) && path[n] == '/')
        {
            /* Resolve no parent traversal across storage-session boundaries. */
            const char *part = path + n + 1;
            while (*part)
            {
                const char *end = strchr(part, '/');
                size_t length = end ? (size_t)(end - part) : strlen(part);
                if (length == 2 && part[0] == '.' && part[1] == '.') return EPD_READ_INVALID;
                if (!end) break;
                part = end + 1;
            }
            *out = (storage_volume_t)i; return EPD_READ_OK;
        }
    }
    return EPD_READ_INVALID;
}
int reader_file_capture(const char *path, reader_file_access_t *access)
{
    if (!access) return EPD_READ_INVALID;
    int result = volume(path, &access->volume);
    if (result) return result;
    access->session = storage_card_session();
    return reader_file_valid(access) ? EPD_READ_OK : EPD_READ_IO;
}
bool reader_file_valid(const reader_file_access_t *access)
{ return access && storage_session_valid(access->volume, access->session); }
static int read_file(void *user, void *buffer, size_t bytes)
{
    file_stream_t *s = user;
    if (!reader_file_valid(&s->access)) return EPD_READ_IO;
    if (bytes > INT_MAX) bytes = INT_MAX;
    size_t n = fread(buffer, 1, bytes, s->file);
    if (!reader_file_valid(&s->access) || ferror(s->file)) return EPD_READ_IO;
    return (int)n;
}
static int seek_file(void *user, uint64_t offset)
{
    file_stream_t *s = user;
    if (!reader_file_valid(&s->access) || offset > LONG_MAX) return EPD_READ_IO;
    return fseek(s->file, (long)offset, SEEK_SET) ? EPD_READ_IO : EPD_READ_OK;
}
static void close_file(void *user)
{
    file_stream_t *s = user;
    fclose(s->file); epd_app_free(s);
}
int reader_file_open(const reader_file_access_t *access, const char *path, book_stream_t *stream)
{
    if (!stream) return EPD_READ_INVALID;
    memset(stream, 0, sizeof(*stream));
    storage_volume_t actual;
    int result = volume(path, &actual);
    if (result) return result;
    if (!reader_file_valid(access) || actual != access->volume) return EPD_READ_IO;
    struct stat info;
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size > LONG_MAX) return EPD_READ_IO;
    file_stream_t *s = epd_app_alloc(sizeof(*s), EPD_APP_PSRAM);
    if (!s) return EPD_READ_NO_MEMORY;
    s->access = *access; s->file = fopen(path, "rb");
    if (!s->file || !reader_file_valid(access))
    {
        if (s->file) fclose(s->file);
        epd_app_free(s); return EPD_READ_IO;
    }
    *stream = (book_stream_t){.user = s, .size = (uint64_t)info.st_size,
        .read = read_file, .seek = seek_file, .close = close_file};
    return EPD_READ_OK;
}
