/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_COMPUTE_MOBI_FILE_H
#define BOOK_COMPUTE_MOBI_FILE_H
#include "../../formats/book_fs.h"
/* Vendor compatibility type; the handle is owned by the injected resource API. */
struct dfs_fd { bf_FILE *handle; uint64_t size; };
#endif
