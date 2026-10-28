/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <rtthread.h>
#include <dfs_file.h>
#include "storage.h"
#include "mobi_port.h"
#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef union allocation allocation_t;
union allocation {
    struct { allocation_t *prev, *next; size_t size; } info;
    uint64_t alignment;
};
static const book_allocator_t *memory_api;
static const book_sink_t *output;
static allocation_t *allocations;
static int status;
static uint32_t media_revision;
enum { READ_AHEAD_SIZE = 16384 };
static struct dfs_fd source_file;
static const char *source_path;
static unsigned char *read_ahead;
static size_t read_start, read_size;

int mobi_port_start(const book_allocator_t *allocator, const book_sink_t *sink)
{
    if (memory_api) return BOOK_ERROR;
    memory_api = allocator;
    output = sink;
    status = BOOK_OK;
    source_path = NULL;
    read_ahead = NULL;
    read_start = read_size = 0;
    storage_lock();
    media_revision = storage_revision();
    if (storage_changing()) status = BOOK_IO_ERROR;
    storage_unlock();
    return BOOK_OK;
}
int mobi_port_status(void)
{
    if (output && output->cancelled && output->cancelled(output->user)) status = BOOK_CANCELLED;
    if (status == BOOK_OK && (storage_changing() || storage_revision() != media_revision)) status = BOOK_IO_ERROR;
    return status;
}
static int io_begin(void)
{
    if (mobi_port_status() != BOOK_OK) return -1;
    storage_lock();
    if (storage_changing() || storage_revision() != media_revision) {
        status = BOOK_IO_ERROR;
        storage_unlock();
        return -1;
    }
    return 0;
}
/* The document worker owns this handle. Only DFS access holds storage_lock;
 * parsing and sink callbacks never run under the storage lock. */
int mobi_read_at(const char *path, size_t offset, void *buffer, size_t size)
{
    unsigned char *out = buffer;
    size_t remaining = size;
    if (!path || offset > LONG_MAX || size > INT_MAX || size > (size_t)LONG_MAX - offset)
        return BOOK_IO_ERROR;
    if (!read_ahead) {
        read_ahead = mobi_alloc(READ_AHEAD_SIZE);
        if (!read_ahead) return mobi_port_status();
    }
    while (remaining) {
        size_t n;
        if (io_begin()) return mobi_port_status();
        if (!source_path) {
            memset(&source_file, 0, sizeof(source_file));
            if (dfs_file_open(&source_file, path, O_RDONLY | O_BINARY) != 0) {
                status = BOOK_IO_ERROR;
                storage_unlock();
                return status;
            }
            source_path = path;
        }
        if (strcmp(source_path, path) || offset > (size_t)source_file.size ||
            remaining > (size_t)source_file.size - offset) {
            status = BOOK_IO_ERROR;
            storage_unlock();
            return status;
        }
        if (!read_size || offset < read_start || offset - read_start >= read_size) {
            size_t start = offset - offset % READ_AHEAD_SIZE;
            size_t length = (size_t)source_file.size - start;
            if (length > READ_AHEAD_SIZE) length = READ_AHEAD_SIZE;
            if (dfs_file_lseek(&source_file, (long)start) < 0 ||
                dfs_file_read(&source_file, read_ahead, length) != (int)length) {
                status = BOOK_IO_ERROR;
                storage_unlock();
                return status;
            }
            read_start = start; read_size = length;
        }
        n = read_size - (offset - read_start);
        if (n > remaining) n = remaining;
        memcpy(out, read_ahead + offset - read_start, n);
        storage_unlock();
        out += n; offset += n; remaining -= n;
    }
    return BOOK_OK;
}
int mobi_dfs_open(struct dfs_fd *file, const char *path, int flags)
{
    int result;
    if (io_begin()) return -1;
    result = dfs_file_open(file, path, flags);
    storage_unlock();
    if (result < 0) status = BOOK_IO_ERROR;
    return result;
}
int mobi_dfs_seek(struct dfs_fd *file, long offset)
{
    int result;
    if (io_begin()) return -1;
    result = dfs_file_lseek(file, offset);
    storage_unlock();
    if (result < 0) status = BOOK_IO_ERROR;
    return result;
}
int mobi_dfs_close(struct dfs_fd *file)
{
    int result;
    storage_lock();
    if (status == BOOK_OK && (storage_changing() || storage_revision() != media_revision)) status = BOOK_IO_ERROR;
    result = dfs_file_close(file);
    storage_unlock();
    if (result < 0 && status == BOOK_OK) status = BOOK_IO_ERROR;
    return result;
}
void *mobi_alloc(size_t size)
{
    allocation_t *block;
    if (mobi_port_status() != BOOK_OK) return NULL;
    if (size > SIZE_MAX - sizeof(*block)) { status = BOOK_NO_MEMORY; return NULL; }
    block = memory_api->alloc(sizeof(*block) + (size ? size : 1));
    if (!block) { status = BOOK_NO_MEMORY; return NULL; }
    block->info.size = size;
    block->info.prev = NULL;
    block->info.next = allocations;
    if (allocations) allocations->info.prev = block;
    allocations = block;
    return block + 1;
}
void mobi_release(void *memory)
{
    allocation_t *block;
    if (!memory) return;
    block = (allocation_t *)memory - 1;
    if (block->info.prev) block->info.prev->info.next = block->info.next;
    else allocations = block->info.next;
    if (block->info.next) block->info.next->info.prev = block->info.prev;
    memory_api->free(block);
}
void *mobi_calloc(size_t count, size_t size)
{
    void *p;
    if (size && count > SIZE_MAX / size) { status = BOOK_NO_MEMORY; return NULL; }
    p = mobi_alloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void *mobi_realloc(void *memory, size_t size)
{
    allocation_t *block;
    void *p;
    if (!memory) return mobi_alloc(size);
    if (!size) { mobi_release(memory); return NULL; }
    block = (allocation_t *)memory - 1;
    p = mobi_alloc(size);
    if (!p) return NULL;
    memcpy(p, memory, size < block->info.size ? size : block->info.size);
    mobi_release(memory);
    return p;
}
char *bk_mobi_strdup(const char *text)
{
    size_t size = strlen(text) + 1;
    char *p = mobi_alloc(size);
    if (p) memcpy(p, text, size);
    return p;
}
int mobi_dfs_read(struct dfs_fd *file, void *buffer, size_t size)
{
    int result;
    if (io_begin()) return -1;
    result = dfs_file_read(file, buffer, size);
    storage_unlock();
    if (result < 0 || (size_t)result != size) status = BOOK_IO_ERROR;
    return result;
}
void mobi_port_finish(void)
{
    if (source_path) {
        mobi_dfs_close(&source_file);
        source_path = NULL;
    }
    while (allocations) mobi_release(allocations + 1);
    read_ahead = NULL;
    output = NULL;
    memory_api = NULL;
}
