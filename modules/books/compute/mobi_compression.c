/* SPDX-License-Identifier: Apache-2.0 */
#include "mobi_compute_port.h"
#include "book_timing.h"
#define mobi_decompress_lz77 book_lz77_impl
#define mobi_decompress_huffman book_huffman_impl
#include "../third_party/libmobi/src/compression.c"
#undef mobi_decompress_lz77
#undef mobi_decompress_huffman

MOBI_RET mobi_decompress_lz77(unsigned char *out, const unsigned char *in, size_t *len_out, const size_t len_in)
{
    unsigned previous = book_timing_enter(BOOK_DOCUMENT_STAGE_INFLATE);
    MOBI_RET result = book_lz77_impl(out, in, len_out, len_in);
    book_timing_leave(previous);
    return result;
}
MOBI_RET mobi_decompress_huffman(unsigned char *out, const unsigned char *in, size_t *len_out,
                                size_t len_in, const MOBIHuffCdic *huffcdic)
{
    unsigned previous = book_timing_enter(BOOK_DOCUMENT_STAGE_INFLATE);
    MOBI_RET result = book_huffman_impl(out, in, len_out, len_in, huffcdic);
    book_timing_leave(previous);
    return result;
}
