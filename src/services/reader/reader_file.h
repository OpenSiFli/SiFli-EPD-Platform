/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_FILE_H
#define READER_FILE_H
#include "platform/epd_reader_source.h"
#include "storage.h"
typedef struct { storage_volume_t volume; uint32_t session; } reader_file_access_t;
/* capture only reads storage state. File operations are worker-only; each
 * stream keeps its admission-time card identity until its close callback. */
int reader_file_capture(const char *path, reader_file_access_t *access);
bool reader_file_valid(const reader_file_access_t *access);
int reader_file_open(const reader_file_access_t *access, const char *path, book_stream_t *stream);
#endif
