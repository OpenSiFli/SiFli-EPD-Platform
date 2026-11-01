/* SPDX-License-Identifier: Apache-2.0 */
#include "mobi_compute_port.h"
#define book_converter book_compute_mobi_converter
#include "../codecs/mobi/mobi_converter.c"
#undef book_converter
int book_compute_mobi(const char *path, const book_sink_t *sink,
                      const book_allocator_t *allocator, char *error, size_t capacity)
{ return book_compute_mobi_converter.convert(path, sink, allocator, error, capacity); }
