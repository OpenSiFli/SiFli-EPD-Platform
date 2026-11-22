/* SPDX-License-Identifier: Apache-2.0 */
#include "formats.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    assert(book_format_detect("Test.EPUB", false) == BOOK_FORMAT_EPUB);
    assert(book_format_detect("Test.AZW3", false) == BOOK_FORMAT_MOBI);
    assert(book_format_detect("Test.txt", false) == BOOK_FORMAT_TXT);
    assert(book_format_detect("fixtures/split", true) == BOOK_FORMAT_SPLIT_EPUB);
    assert(book_format_detect("fixtures", true) == BOOK_FORMAT_UNKNOWN);
    assert(!strcmp(book_format_name(BOOK_FORMAT_EPUB), "EPUB"));
    assert(!strcmp(book_format_name((book_format_t)6), "XHTML"));
    return 0;
}
