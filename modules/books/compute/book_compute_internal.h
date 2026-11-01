/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_COMPUTE_INTERNAL_H
#define BOOK_COMPUTE_INTERNAL_H
#include "book_compute.h"
#ifndef BOOK_FORMAT_ACPU
#error "The callback converter requires BOOK_FORMAT_ACPU"
#endif

typedef struct book_compute_io {
    book_compute_resource_ops_t ops;
    void *user;
    const book_sink_t *sink;
    int status;
} book_compute_io;

size_t book_compute_scope_size(void);
void *book_compute_scope_bind(void *memory);
book_compute_io *book_compute_fs_bind(book_compute_io *io);
int book_compute_oom_exchange(int value);
int book_compute_note_interrupt(const char *path, const book_sink_t *sink,
    const book_allocator_t *allocator, const book_compute_resource_ops_t *resources,
    void *user, const book_compute_selection_t *selection, char *error, size_t capacity);
int book_compute_check(book_compute_io *io);
void book_compute_record(book_compute_io *io, int status);
int book_compute_txt(const char *path, const book_sink_t *sink);
int book_compute_mobi(const char *path, const book_sink_t *sink,
    const book_allocator_t *allocator, char *error, size_t capacity);
int book_compute_is_markup(const char *path);
int book_compute_markup(const char *path, const book_sink_t *sink,
    const book_compute_selection_t *selection, const book_compute_observer_t *observer);
int book_compute_txt_slice(const char *path, const book_sink_t *sink,
    const book_compute_selection_t *selection, const book_compute_observer_t *observer);
int bf_epub_select(const char *path, const book_sink_t *sink, int split,
    const book_compute_selection_t *selection, const book_compute_observer_t *observer);
int book_compute_epub_slice(const book_sink_t *sink, book_stream_t *stream,
    const char *base, const book_resources_t *resources,
    const book_compute_selection_t *selection, const book_compute_observer_t *observer,
    uint32_t index, const char *id, const char *first_anchor);
#endif
