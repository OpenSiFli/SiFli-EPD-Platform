/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_MOBI_PORT_H
#define BOOK_MOBI_PORT_H
#include <stddef.h>
#include "book_codec.h"
struct dfs_fd;
void *mobi_alloc(size_t size);
void *mobi_calloc(size_t count, size_t size);
void *mobi_realloc(void *memory, size_t size);
void mobi_release(void *memory);
char *bk_mobi_strdup(const char *text);
int mobi_dfs_read(struct dfs_fd *file, void *buffer, size_t size);
int mobi_dfs_open(struct dfs_fd *file, const char *path, int flags);
int mobi_dfs_seek(struct dfs_fd *file, long offset);
int mobi_dfs_close(struct dfs_fd *file);
int mobi_read_at(const char *path, size_t offset, void *buffer, size_t size);
int mobi_port_start(const book_allocator_t *allocator, const book_sink_t *sink);
void mobi_port_finish(void);
int mobi_port_status(void);
#endif
