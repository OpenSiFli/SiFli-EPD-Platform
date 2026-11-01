/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"

#define NOTE_BYTES 4096u
#define NOTE_DONE (-81)
typedef struct {
    const char *fragment;
    unsigned depth, selected, skip, used;
    bool found, space, truncated;
    char text[NOTE_BYTES + 1];
} note_text;

static int note_event(void *user, int event, const char *name, const char *ns,
                      const bf_attrs *attrs, const char *text, size_t length)
{
    note_text *n = user;
    (void)ns;
    if (event == 1) {
        char id[BF_PATH], role[128], type[128];
        ++n->depth;
        if (bf_attr(attrs, "id", NULL, id, sizeof(id)) < 0) return BOOK_UNSUPPORTED;
        if (!id[0] && !strcmp(name, "a") && bf_attr(attrs, "name", NULL, id, sizeof(id)) < 0)
            return BOOK_UNSUPPORTED;
        if (!n->found && !strcmp(id, n->fragment)) {
            n->found = true; n->selected = n->depth;
            if (!strcmp(name, "a") && n->depth > 1) {
                char href[BF_PATH];
                if (bf_attr(attrs, "href", NULL, href, sizeof(href)) < 0) return BOOK_UNSUPPORTED;
                if (!href[0]) --n->selected;
            }
        }
        if (!n->selected || n->skip) return BOOK_OK;
        if (bf_attr(attrs, "role", NULL, role, sizeof(role)) < 0 ||
            bf_attr(attrs, "type", "http://www.idpf.org/2007/ops", type, sizeof(type)) < 0)
            return BOOK_UNSUPPORTED;
        if (!strcmp(name, "script") || !strcmp(name, "style") ||
            !strcmp(role, "doc-backlink") || !strcmp(type, "backlink")) n->skip = n->depth;
        if ((!strcmp(name, "p") || !strcmp(name, "div") || !strcmp(name, "li") || !strcmp(name, "br")) &&
            n->used && n->text[n->used - 1] != '\n' && n->used < NOTE_BYTES)
            n->text[n->used++] = '\n';
    } else if (event == 0) {
        if (n->depth == n->selected) return NOTE_DONE;
        if (n->depth == n->skip) n->skip = 0;
        if (n->depth) --n->depth;
    } else if (n->selected && !n->skip) {
        for (size_t i = 0; i < length;) {
            unsigned char c = (unsigned char)text[i];
            if (isspace(c)) { n->space = true; ++i; continue; }
            unsigned bytes = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
            if (bytes > length - i) return BOOK_ERROR;
            unsigned space = n->space && n->used && n->text[n->used - 1] != '\n';
            if (bytes + space > NOTE_BYTES - n->used) { n->truncated = true; return NOTE_DONE; }
            if (space) n->text[n->used++] = ' ';
            n->space = false;
            memcpy(n->text + n->used, text + i, bytes); n->used += bytes; i += bytes;
        }
    }
    return BOOK_OK;
}
int book_markup_note(const book_sink_t *sink, book_stream_t *stream, const char *fragment)
{
    if (!fragment || !*fragment) return BOOK_ERROR;
    note_text *n = bf_alloc(sizeof(*n));
    if (!n) return BOOK_NO_MEMORY;
    n->fragment = fragment;
    /* Parse the selected element only. No images, CSS, or trailing-body scan. */
    int rc = bf_xml(stream, sink, note_event, n, 0);
    if (rc == NOTE_DONE || rc == BOOK_OK) {
        if (!n->found || !n->used) rc = BOOK_ERROR;
        else {
            while (n->used && n->text[n->used - 1] == '\n') --n->used;
            rc = bf_text(sink, n->text, n->used, BOOK_STYLE_NORMAL);
            if (rc == BOOK_OK && n->truncated) rc = 1;
        }
    }
    bf_free(n);
    return rc;
}
