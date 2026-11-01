/* SPDX-License-Identifier: Apache-2.0 */
#include "../formats/book_internal.h"
#include "book_compute_internal.h"

int book_compute_is_markup(const char *path)
{
    return bf_suffix(path, ".xhtml") || bf_suffix(path, ".html") ||
           bf_suffix(path, ".htm") || bf_suffix(path, ".xht");
}
static int resource(void *u, const char *base, const char *ref, book_stream_t *stream)
{
    char path[BF_PATH], absolute[BF_PATH], *fragment;
    int rc; (void)u;
    if (bf_external(ref)) return BOOK_UNSUPPORTED;
    if ((rc = bf_resolve(base, ref, path, sizeof(path))) < 0) return rc;
    fragment = strchr(path, '#');
    if (fragment) *fragment = 0;
    if (base[0] == '/')
    {
        if (snprintf(absolute, sizeof(absolute), "/%s", path) >= (int)sizeof(absolute)) return BOOK_UNSUPPORTED;
        return bf_file(absolute, stream);
    }
    return bf_file(path, stream);
}
int book_compute_markup(const char *path, const book_sink_t *sink,
    const book_compute_selection_t *selection, const book_compute_observer_t *observer)
{
    book_compute_selection_t whole = {0};
    book_resources_t resources = {NULL, resource};
    book_stream_t stream = {0}; int rc;
    if (!selection || !(selection->flags & (BOOK_COMPUTE_SELECT_CHAPTER | BOOK_COMPUTE_NOTE))) selection = &whole;
    if (selection->flags & BOOK_COMPUTE_NOTE) {
        char target[BF_PATH];
        if (bf_copy(target, sizeof(target), selection->note_target) < 0) return BOOK_UNSUPPORTED;
        char *fragment = strchr(target, '#');
        if (!fragment || !fragment[1]) return BOOK_ERROR;
        *fragment++ = 0;
        char absolute[BF_PATH];
        if (snprintf(absolute, sizeof(absolute), "%s%s", path[0] == '/' && target[0] != '/' ? "/" : "", target) >= (int)sizeof(absolute)) return BOOK_UNSUPPORTED;
        if ((rc = bf_file(absolute, &stream)) < 0) return rc;
        rc = book_markup_note(sink, &stream, fragment);
        if (stream.close) stream.close(stream.user);
        return rc;
    }
    if (selection->spine) return BOOK_ERROR;
    if (observer && observer->chapter &&
        (rc = observer->chapter(observer->user, 0, 1, (book_format_t)BOOK_COMPUTE_FORMAT_XHTML,
                                true, path, path)) < 0) return rc;
    if ((rc = bf_file(path, &stream)) < 0) return rc;
    rc = book_compute_epub_slice(sink, &stream, path, &resources, selection, observer, 0, path, NULL);
    return bf_finish(&stream, rc);
}
