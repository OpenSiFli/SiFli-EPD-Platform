/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_RECORDS_H
#define READER_RECORDS_H
#include "../../ui/platform/epd_reader_source.h"

void reader_batch_init(epd_read_batch_t *batch, void *buffer, size_t capacity);
int reader_batch_append(epd_read_batch_t *batch, const epd_read_record_t *record,
                        const void *payload);
/* Validation is independent of parser code and performs no allocations. */
bool reader_batch_valid(const epd_read_batch_t *batch);
bool reader_record_valid(const epd_read_record_t *record, const void *payload);
/* offset starts at zero. Returned payload is borrowed until the batch returns. */
int reader_batch_next(const epd_read_batch_t *batch, size_t *offset,
                      epd_read_record_t *record, const void **payload);
#endif
