/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READING_DATA_WIRE_H
#define READING_DATA_WIRE_H
#include "epd_compute_protocol.h"
#include <stdint.h>
#define RD_VERSION 1u
#define RD_CHUNK 4096u
#define RD_BACKEND_ACPU 0x41435055u
#define RD_HASH 1u
#define RD_INDEX_BEGIN 2u
#define RD_INDEX_DATA 3u
#define RD_CACHE_BEGIN 4u
#define RD_CACHE_DATA 5u
typedef struct {
    uint32_t version, size, sequence, operation;
    uint32_t length, offset, seed, skip_offset, skip_size;
    uint32_t total, magic, index_version, layout, end, page_max, text_limit;
} rd_request_t;
typedef struct {
    uint32_t version, size, sequence, operation, backend, processed, value, reserved;
} rd_reply_t;
typedef char rd_request_size[(sizeof(rd_request_t) == 64) ? 1 : -1];
typedef char rd_reply_size[(sizeof(rd_reply_t) == 32) ? 1 : -1];
extern const epd_compute_ops_t epd_reading_data_remote_ops;
#endif
