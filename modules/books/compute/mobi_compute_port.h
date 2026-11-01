/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_COMPUTE_MOBI_PORT_H
#define BOOK_COMPUTE_MOBI_PORT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "../formats/book_internal.h"
#define BOOK_MOBI 1
/* Select the existing vendor random-access loader; its dfs_* calls are
 * redirected by BOOK_MOBI to our callback-only compatibility functions. */
#define RT_USING_DFS 1
#define MOBI_DEBUG 0
#define MOBI_DEBUG_ALLOC 0
#define HAVE_STRDUP 1
#ifndef O_BINARY
#define O_BINARY 0
#endif
long book_compute_file_tell(bf_FILE *file);
#define ftell book_compute_file_tell
#endif
