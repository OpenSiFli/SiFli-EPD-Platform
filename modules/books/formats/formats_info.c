/* SPDX-License-Identifier: Apache-2.0 */
#include "formats.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#ifndef BOOK_FORMAT_INFO_EMBEDDED
#ifdef BOOK_FORMAT_TARGET
#include <dfs_posix.h>
#include "services/storage.h"
#else
#include <sys/stat.h>
#include <dirent.h>
#endif
#endif

static int info_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    if (n < k) return 0;
    s += n - k;
    while (*s && tolower((unsigned char)*s) == tolower((unsigned char)*suffix)) { ++s; ++suffix; }
    return !*s;
}
static int info_chapter(const char *s)
{
    return !strncmp(s, "chapter", 7) && (s[7] == '_' || isdigit((unsigned char)s[7])) && info_suffix(s, ".zip");
}
static int info_split(const char *path)
{
    char base[1024]; struct stat st; int found = 0;
    if (snprintf(base, sizeof(base), "%s/basepackage.zip", path) >= (int)sizeof(base)) return 0;
#ifdef BOOK_FORMAT_INFO_EMBEDDED
    bf_directory dir; struct dirent *entry;
    if (bf_stat(base, &st) || !S_ISREG(st.st_mode) || bf_dir_open(&dir, path) < 0) return 0;
    while ((entry = bf_dir_next(&dir)) != NULL)
        if (info_chapter(entry->d_name)) { found = 1; break; }
    bf_dir_close(&dir);
#else
    DIR *dir; struct dirent *entry;
#ifdef BOOK_FORMAT_TARGET
    uint32_t revision;
    storage_lock();
    revision = storage_revision();
    int ready = storage_path_available(path) && !stat(base, &st) && S_ISREG(st.st_mode);
    dir = ready ? opendir(path) : NULL;
    storage_unlock();
#else
    dir = !stat(base, &st) && S_ISREG(st.st_mode) ? opendir(path) : NULL;
#endif
    if (!dir) return 0;
    for (;;)
    {
#ifdef BOOK_FORMAT_TARGET
        storage_lock();
        int valid = revision == storage_revision() && storage_path_available(path);
        entry = valid ? readdir(dir) : NULL;
#else
        entry = readdir(dir);
#endif
        if (entry) found = info_chapter(entry->d_name);
#ifdef BOOK_FORMAT_TARGET
        storage_unlock();
#endif
        if (!entry || found) break;
    }
#ifdef BOOK_FORMAT_TARGET
    storage_lock();
    if (revision != storage_revision() || !storage_path_available(path)) found = 0;
#endif
    closedir(dir);
#ifdef BOOK_FORMAT_TARGET
    storage_unlock();
#endif
#endif
    return found;
}
book_format_t book_format_detect(const char *path, bool directory)
{
    static const struct { const char *suffix; book_format_t format; } ext[] = {
        {".txt", BOOK_FORMAT_TXT}, {".md", BOOK_FORMAT_MARKDOWN},
        {".markdown", BOOK_FORMAT_MARKDOWN}, {".epub", BOOK_FORMAT_EPUB},
        {".mobi", BOOK_FORMAT_MOBI}, {".azw", BOOK_FORMAT_MOBI}, {".azw3", BOOK_FORMAT_MOBI}
    };
    unsigned i;
    if (!path) return BOOK_FORMAT_UNKNOWN;
    if (directory) return info_split(path) ? BOOK_FORMAT_SPLIT_EPUB : BOOK_FORMAT_UNKNOWN;
    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); ++i)
        if (info_suffix(path, ext[i].suffix)) return ext[i].format;
    return BOOK_FORMAT_UNKNOWN;
}
const char *book_format_name(book_format_t format)
{
    if (format == (book_format_t)6) return "XHTML";
    switch (format)
    {
    case BOOK_FORMAT_TXT: return "TXT";
    case BOOK_FORMAT_MARKDOWN: return "Markdown";
    case BOOK_FORMAT_EPUB: return "EPUB";
    case BOOK_FORMAT_SPLIT_EPUB: return "Split EPUB";
    case BOOK_FORMAT_MOBI: return "MOBI";
    default: return "Unknown";
    }
}
