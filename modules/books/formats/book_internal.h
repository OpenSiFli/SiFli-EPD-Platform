/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_INTERNAL_H
#define BOOK_INTERNAL_H
#include "../third_party/miniz/miniz.h"
#include "book_fs.h"
#include "formats.h"
#include <ctype.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BF_RAW_STDIO
#define FILE bf_FILE
#define fopen bf_fopen
#define fread bf_fread
#define fseek bf_fseek
#define fclose bf_fclose
#undef ferror
#define ferror bf_ferror
#define fgetc bf_fgetc
#define ungetc bf_ungetc
#define stat(path, st) bf_stat(path, st)
#endif

#define BF_PATH 1024
#define BF_TEXT 2048
#define BF_DEPTH 96
#define BF_SVG_LIMIT (256u * 1024u)
#define BF_NS_OPF "http://www.idpf.org/2007/opf"
#define BF_NS_DC "http://purl.org/dc/elements/1.1/"

void *bf_alloc(size_t n);
void *bf_realloc(void *p, size_t n);
void bf_free(void *p);
char *bf_strdup(const char *s);
int bf_oom(void);
int bf_active(void);
int bf_cancel(const book_sink_t *s);
int bf_text(const book_sink_t *s, const char *p, size_t n, unsigned style);
int bf_para(const book_sink_t *s);
int bf_anchor(const book_sink_t *s, const char *id, const char *title, unsigned level);
int bf_link(const book_sink_t *s, const char *id, bool begin);
int bf_metadata(const book_sink_t *s, const char *title, const char *author);
int bf_copy(char *dst, size_t cap, const char *src);
void bf_append(char *dst, size_t cap, const char *s, size_t n);
int bf_casecmp(const char *a, const char *b);
int bf_suffix(const char *s, const char *suffix);
int bf_resolve(const char *base, const char *ref, char *out, size_t cap);
int bf_external(const char *ref);
int bf_file(const char *path, book_stream_t *s);
int bf_finish(book_stream_t *s, int status);
int bf_picture(const book_sink_t *sink, const book_resources_t *res, const char *base,
               const char *ref);

typedef struct bf_zip
{
    mz_zip_archive zip;
    FILE *file;
    const book_sink_t *sink;
    int status;
} bf_zip;
int bf_zip_open(bf_zip *z, const char *path, const book_sink_t *sink);
void bf_zip_close(bf_zip *z);
int bf_zip_entry(bf_zip *z, const char *name, book_stream_t *s);
int bf_zip_has(bf_zip *z, const char *name);
int bf_zip_name(bf_zip *z, unsigned i, char *out, size_t cap);
typedef struct bf_package
{
    bf_zip *current, *base;
} bf_package;
int bf_package_open(void *user, const char *base, const char *ref, book_stream_t *s);

typedef struct bf_attrs
{
    int count;
    const xmlChar **values;
    int html;
} bf_attrs;
int bf_attr(const bf_attrs *attrs, const char *name, const char *ns, char *out, size_t cap);
/* start=1, end=0, text=2. text length is explicit; XML names are local names. */
typedef int (*bf_event)(void *user, int event, const char *name, const char *ns,
                        const bf_attrs *attrs, const char *text, size_t length);
int bf_xml(book_stream_t *s, const book_sink_t *sink, bf_event event, void *user, int html);
int bf_svg_markup(book_stream_t *s, const book_sink_t *sink, bf_event event, void *user,
                  const char *base, const book_resources_t *resources);
int bf_svg_stream(book_stream_t *s, const book_sink_t *sink, const char *base,
                  const book_resources_t *resources);
long bf_xml_position(void);
int bf_xml_entry(bf_zip *z, const char *name, const book_sink_t *sink, bf_event event, void *user);
int bf_markdown(const char *path, const book_sink_t *sink);
int bf_epub(const char *path, const book_sink_t *sink, int split);
int bf_chapter_zip(const char *name);
#endif
