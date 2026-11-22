/* SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
enum { DOCUMENT_TXT, DOCUMENT_MD, DOCUMENT_EPUB, DOCUMENT_SPLIT_EPUB, DOCUMENT_MOBI = 5 };
#define DOCUMENT_CONVERSION_VERSION 10u
#define DOCUMENT_SOURCE_SPOOL 3u
typedef struct {
    struct { unsigned document_kind, document_version; struct { uint32_t unit; } source; } position;
    struct { unsigned format; } file;
} ui_bookshelf_book_t;
#include "position_compatible.inc"
int main(void)
{
    const unsigned formats[] = {DOCUMENT_TXT, DOCUMENT_EPUB, DOCUMENT_SPLIT_EPUB};
    for (unsigned i = 0; i < 3; ++i) {
        ui_bookshelf_book_t book = {{formats[i], 9, {i ? 2 : 1}}, {formats[i]}};
        assert(position_compatible(&book));
        book.position.source.unit = 3; assert(!position_compatible(&book));
        book.position.document_version = 8; assert(!position_compatible(&book));
        book.position.document_version = DOCUMENT_CONVERSION_VERSION; assert(position_compatible(&book));
        book.position.document_kind = 99; assert(!position_compatible(&book));
    }
    ui_bookshelf_book_t spool = {{DOCUMENT_MD, 9, {3}}, {DOCUMENT_MD}};
    assert(position_compatible(&spool));
    spool.position.document_kind = spool.file.format = DOCUMENT_MOBI;
    assert(position_compatible(&spool));
    spool.position.document_version = 8; assert(!position_compatible(&spool));
    puts("PASS v9 EPUB/split/TXT source positions and unchanged MD/MOBI spool positions; unrelated versions rejected");
    return 0;
}
