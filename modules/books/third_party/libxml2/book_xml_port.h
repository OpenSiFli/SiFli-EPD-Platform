/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_XML_PORT_H
#define BOOK_XML_PORT_H
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

/* The library is private to the serial books conversion scope. */
void *bf_alloc(size_t size);
void *bf_realloc(void *memory, size_t size);
void bf_free(void *memory);
char *bf_strdup(const char *text);
#define malloc bf_alloc
#define realloc bf_realloc
#define free bf_free
#define strdup bf_strdup

/* Diagnostics are returned through the parser's structured error callback.
 * Source access is exclusively through the converter's supplied streams. */
#undef stdin
#undef stdout
#undef stderr
#define stdin ((FILE *)0)
#define stdout ((FILE *)0)
#define stderr ((FILE *)0)
#define fopen(...) ((FILE *)0)
#define fclose(...) (0)
#define fflush(...) (0)
#define fread(...) ((size_t)0)
#define fwrite(...) ((size_t)0)
static inline int bf_xml_fprintf(FILE *stream, const char *format, ...)
{
    (void)stream;
    (void)format;
    return 0;
}
#define fprintf bf_xml_fprintf
#define vfprintf(...) (0)
#define printf(...) (0)
#define fputs(...) (0)
#define fputc(...) (0)
#define getenv(...) ((char *)0)
#define sscanf(...) (0)
#define read(...) (-1)
#define close(...) (0)
#define getcwd(...) ((char *)0)
#endif
