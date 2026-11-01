/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_ENTRY_H
#define BOOK_ENTRY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BOOK_ENTRY_MAGIC 0x31484542u
#define BOOK_ENTRY_MAX 4096u
#define BOOK_ENTRY_FIRST 1u
#define BOOK_ENTRY_FIRST_EFFECTIVE 2u
enum { BOOK_ENTRY_TITLE, BOOK_ENTRY_AUTHOR, BOOK_ENTRY_HREF, BOOK_ENTRY_OPF,
       BOOK_ENTRY_NAV, BOOK_ENTRY_ANCHOR, BOOK_ENTRY_FIELDS };
typedef struct { uint32_t crc, compressed, uncompressed, method; } book_entry_file_t;
typedef struct {
    uint32_t magic, bytes, checksum, chapter, chapters, flags, source_size;
    uint32_t length[BOOK_ENTRY_FIELDS];
    book_entry_file_t file[4]; /* container, OPF, NAV (zero if absent), XHTML */
    uint32_t heading_bytes;
} book_entry_t;

static inline uint32_t book_entry_checksum(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes; ++i)
        h = (h ^ (i >= offsetof(book_entry_t, checksum) &&
            i < offsetof(book_entry_t, checksum) + sizeof(uint32_t) ? 0 : p[i])) * 16777619u;
    return h;
}
static inline const char *book_entry_field(const book_entry_t *e, unsigned field)
{
    const char *p = (const char *)(e + 1);
    for (unsigned i = 0; i < field; ++i) p += e->length[i];
    return p;
}
static inline const char *book_entry_headings(const book_entry_t *e)
{ return book_entry_field(e, BOOK_ENTRY_FIELDS); }
static inline bool book_entry_path(const char *p)
{
    if (!p || !*p || *p == '/') return false;
    const char *part = p;
    for (;; ++p) {
        if (*p && ((unsigned char)*p < 32 || *p == '\\' || *p == ':' || *p == '#')) return false;
        if (!*p || *p == '/') {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && *part == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (!*p) return true;
            part = p + 1;
        }
    }
}
static inline bool book_entry_valid(const void *data, size_t bytes)
{
    if (!data || bytes < sizeof(book_entry_t) || bytes > BOOK_ENTRY_MAX) return false;
    const book_entry_t *e = data;
    if (e->magic != BOOK_ENTRY_MAGIC || e->bytes != bytes || !e->chapters ||
        e->chapters > 8192 || e->chapter >= e->chapters || !e->source_size ||
        (e->flags & ~(BOOK_ENTRY_FIRST | BOOK_ENTRY_FIRST_EFFECTIVE)) ||
        ((e->flags & BOOK_ENTRY_FIRST_EFFECTIVE) && !(e->flags & BOOK_ENTRY_FIRST)) ||
        e->checksum != book_entry_checksum(data, bytes)) return false;
    size_t used = sizeof(*e);
    for (unsigned i = 0; i < BOOK_ENTRY_FIELDS; ++i) {
        size_t n = e->length[i];
        if (!n || n > (i <= BOOK_ENTRY_AUTHOR ? 512u : 1024u) || n > bytes - used) return false;
        const char *p = (const char *)data + used;
        if (p[n - 1] || memchr(p, 0, n - 1)) return false;
        used += n;
    }
    if (e->heading_bytes != bytes - used) return false;
    const char *href = book_entry_field(e, BOOK_ENTRY_HREF);
    const char *nav = book_entry_field(e, BOOK_ENTRY_NAV);
    if (!book_entry_path(href) || !book_entry_path(book_entry_field(e, BOOK_ENTRY_OPF)) ||
        (*nav && !book_entry_path(nav))) return false;
    size_t prefix = strlen(href);
    const char *anchor = book_entry_field(e, BOOK_ENTRY_ANCHOR);
    if (*anchor && (strncmp(anchor, href, prefix) || anchor[prefix] != '#' || !anchor[prefix + 1])) return false;
    while (used < bytes) {
        const char *p = (const char *)data + used;
        const char *end = memchr(p, 0, bytes - used);
        if (!end || (size_t)(end - p) <= prefix + 1 ||
            strncmp(p, href, prefix) || p[prefix] != '#') return false;
        used += (size_t)(end - p) + 1;
    }
    return true;
}
#endif
