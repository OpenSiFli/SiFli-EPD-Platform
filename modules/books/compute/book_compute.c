/* SPDX-License-Identifier: Apache-2.0 */
#define BF_RAW_STDIO
#include "../formats/book_internal.h"
#include "book_compute_internal.h"
#include <stdio.h>
#include <string.h>

static int busy;

/* Only the remote scheduler may interrupt a suspended converter. The parent
 * retains its XML runtime and allocation scope until the note has unwound. */
int book_compute_note_interrupt(const char *path, const book_sink_t *sink,
    const book_allocator_t *allocator, const book_compute_resource_ops_t *resources,
    void *user, const book_compute_selection_t *selection, char *error, size_t capacity)
{
    if (!busy)
        return book_compute_convert_at(path, sink, allocator, resources, user,
                                        selection, NULL, error, capacity);
    if (!selection || selection->flags != BOOK_COMPUTE_NOTE ||
        !selection->note_target || !strchr(selection->note_target, '#') ||
        bf_external(selection->note_target)) return BOOK_COMPUTE_BUSY;
    if (!bf_active()) {
        void *scope = book_compute_scope_bind(NULL);
        book_compute_io *previous = book_compute_fs_bind(NULL);
        busy = 0;
        int rc = book_compute_convert_at(path, sink, allocator, resources, user,
                                         selection, NULL, error, capacity);
        busy = 1;
        book_compute_fs_bind(previous);
        book_compute_scope_bind(scope);
        return rc;
    }
    book_compute_io io = {*resources, user, sink, BOOK_OK};
    book_compute_io *previous = book_compute_fs_bind(&io);
    int previous_oom = book_compute_oom_exchange(0);
    struct stat st;
    int rc = bf_stat(path, &st) ? BOOK_IO_ERROR : BOOK_OK;
    if (rc == BOOK_OK) {
        book_format_t format = book_format_detect(path, S_ISDIR(st.st_mode));
        if (selection->format_hint == BOOK_COMPUTE_FORMAT_XHTML || book_compute_is_markup(path))
            rc = book_compute_markup(path, sink, selection, NULL);
        else if (format == BOOK_FORMAT_EPUB || format == BOOK_FORMAT_SPLIT_EPUB)
            rc = bf_epub_select(path, sink, format == BOOK_FORMAT_SPLIT_EPUB, selection, NULL);
        else rc = BOOK_UNSUPPORTED;
    }
    if (bf_oom() && rc != BOOK_CANCELLED) rc = BOOK_NO_MEMORY;
    book_compute_check(&io);
    if (io.status < 0) rc = io.status;
    book_compute_fs_bind(previous);
    book_compute_oom_exchange(previous_oom);
    return rc;
}

void book_compute_record(book_compute_io *io, int status)
{
    if (status == BOOK_CANCELLED || (status < 0 && io->status == BOOK_OK))
        io->status = status;
}

int book_compute_check(book_compute_io *io)
{
    if (io->sink->cancelled && io->sink->cancelled(io->sink->user))
        io->status = BOOK_CANCELLED;
    return io->status;
}

int book_compute_convert_at(const char *path, const book_sink_t *sink,
                         const book_allocator_t *allocator,
                         const book_compute_resource_ops_t *resources, void *resource_user,
                         const book_compute_selection_t *selection,
                         const book_compute_observer_t *observer,
                         char *error, size_t error_capacity)
{
    book_compute_io io;
    void *scope_memory;
    int rc, changed = 0;
    if (error && error_capacity) error[0] = 0;
    if (busy)
    {
        if (error && error_capacity) snprintf(error, error_capacity, "Document converter busy");
        return BOOK_COMPUTE_BUSY;
    }
    if (!path || !sink || !allocator || !allocator->alloc || !allocator->realloc ||
        !allocator->free || !resources || !resources->stat || !resources->open ||
        !resources->read_at || !resources->close ||
        ((resources->dir_open || resources->dir_next || resources->dir_close) &&
         !(resources->dir_open && resources->dir_next && resources->dir_close)) ||
        (selection && ((selection->flags & ~(BOOK_COMPUTE_SELECT_CHAPTER | BOOK_COMPUTE_CONTEXT | BOOK_COMPUTE_FULL_SOURCE | BOOK_COMPUTE_NOTE | BOOK_COMPUTE_METADATA | BOOK_COMPUTE_BODY_ONLY | BOOK_COMPUTE_NAV_ONLY | BOOK_COMPUTE_FIRST_BODY)) ||
          ((selection->flags & BOOK_COMPUTE_METADATA) && selection->flags != BOOK_COMPUTE_METADATA) ||
          ((selection->flags & (BOOK_COMPUTE_BODY_ONLY | BOOK_COMPUTE_NAV_ONLY)) &&
           (!(selection->flags & BOOK_COMPUTE_SELECT_CHAPTER) ||
            (selection->flags & (BOOK_COMPUTE_NOTE | BOOK_COMPUTE_METADATA | BOOK_COMPUTE_FULL_SOURCE)) ||
            (selection->flags & (BOOK_COMPUTE_BODY_ONLY | BOOK_COMPUTE_NAV_ONLY)) ==
                (BOOK_COMPUTE_BODY_ONLY | BOOK_COMPUTE_NAV_ONLY))) ||
          ((selection->flags & BOOK_COMPUTE_FIRST_BODY) &&
           (!(selection->flags & BOOK_COMPUTE_BODY_ONLY) || selection->anchor_byte || selection->skip_utf8)) ||
          selection->context_bytes > BOOK_COMPUTE_TXT_BACKSCAN ||
          ((selection->flags & BOOK_COMPUTE_NOTE) && (!selection->note_target || !strchr(selection->note_target, '#') || bf_external(selection->note_target))) ||
          (selection->format_hint && selection->format_hint != BOOK_COMPUTE_FORMAT_XHTML) ||
          selection->slice_bytes > BOOK_COMPUTE_DEFAULT_SLICE)))
    {
        if (error && error_capacity) snprintf(error, error_capacity, "Invalid converter arguments");
        return BOOK_ERROR;
    }
    busy = 1;
    io.ops = *resources;
    io.user = resource_user;
    io.sink = sink;
    io.status = BOOK_OK;
    scope_memory = NULL;
    rc = book_compute_check(&io);
    if (rc == BOOK_OK)
    {
        scope_memory = allocator->alloc(book_compute_scope_size());
        if (!scope_memory) rc = BOOK_NO_MEMORY;
    }
    if (scope_memory)
    {
        memset(scope_memory, 0, book_compute_scope_size());
        book_compute_scope_bind(scope_memory);
        book_compute_fs_bind(&io);
        if ((selection && ((selection->flags & (BOOK_COMPUTE_SELECT_CHAPTER | BOOK_COMPUTE_NOTE | BOOK_COMPUTE_METADATA)) ||
             selection->format_hint == BOOK_COMPUTE_FORMAT_XHTML)) || book_compute_is_markup(path))
        {
            struct stat st;
            rc = bf_stat(path, &st) ? BOOK_IO_ERROR : book_markup_begin(allocator);
            if (rc == BOOK_OK)
            {
                book_format_t format = book_format_detect(path, S_ISDIR(st.st_mode));
                if (selection && (selection->flags & (BOOK_COMPUTE_METADATA | BOOK_COMPUTE_BODY_ONLY | BOOK_COMPUTE_NAV_ONLY)) &&
                    format != BOOK_FORMAT_EPUB && format != BOOK_FORMAT_SPLIT_EPUB)
                    rc = BOOK_UNSUPPORTED;
                else if ((selection && selection->format_hint == BOOK_COMPUTE_FORMAT_XHTML) ||
                    (format == BOOK_FORMAT_UNKNOWN && book_compute_is_markup(path)))
                    rc = book_compute_markup(path, sink, selection, observer);
                else if (format == BOOK_FORMAT_EPUB || format == BOOK_FORMAT_SPLIT_EPUB)
                    rc = bf_epub_select(path, sink, format == BOOK_FORMAT_SPLIT_EPUB, selection, observer);
                else if (format == BOOK_FORMAT_TXT)
                    rc = book_compute_txt_slice(path, sink, selection, observer);
                else rc = BOOK_UNSUPPORTED;
                if (bf_oom() && rc != BOOK_CANCELLED) rc = BOOK_NO_MEMORY;
                book_markup_end();
            }
        }
        else rc = book_format_convert(path, sink, allocator, error, error_capacity);
        book_compute_check(&io);
        if (io.status == BOOK_CANCELLED ||
            (io.status < 0 && (rc == BOOK_OK || rc == BOOK_IO_ERROR || rc == BOOK_UNSUPPORTED)))
        {
            changed = io.status != rc;
            rc = io.status;
        }
        book_compute_fs_bind(NULL);
        book_compute_scope_bind(NULL);
        allocator->free(scope_memory);
    }
    if (rc < 0 && error && error_capacity && (changed || !error[0]))
        snprintf(error, error_capacity, "%s",
                 rc == BOOK_CANCELLED ? "Conversion cancelled" :
                 rc == BOOK_NO_MEMORY ? "Document memory allocation failed" :
                 "Cannot read document or referenced resource");
    busy = 0;
    return rc;
}
int book_compute_convert(const char *path, const book_sink_t *sink,
                         const book_allocator_t *allocator,
                         const book_compute_resource_ops_t *resources, void *resource_user,
                         char *error, size_t error_capacity)
{
    return book_compute_convert_at(path, sink, allocator, resources, resource_user,
                                   NULL, NULL, error, error_capacity);
}
