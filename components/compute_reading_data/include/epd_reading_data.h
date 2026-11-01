/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READING_DATA_H
#define EPD_READING_DATA_H
#include "epd_compute.h"

#define EPD_READING_DATA_COMPONENT "epd/reading-data"
#define EPD_READING_DATA_VERSION 1u
#define EPD_READING_DATA_CHUNK 4096u
#define EPD_READING_DATA_FNV_SEED 2166136261u

typedef struct epd_reading_data_client *epd_reading_data_t;

/* Allocate a ready-only client without waiting for admission. Publish the
 * client before starting worker operations so cancel can wake OPEN or STEP.
 * Operations are synchronous and serialized by the worker. Only cancel may
 * run concurrently; keep the client alive until all cancel callers return. */
int epd_reading_data_create(epd_compute_purpose_t purpose, epd_reading_data_t *out);
/* Try admission once. Required before hashing from a provider that already
 * holds another compute session: BUSY must be deferred or treated as an
 * optional-cache miss, never followed by waiting for the same admission. */
int epd_reading_data_try_open(epd_reading_data_t client);
void epd_reading_data_cancel(epd_reading_data_t client);
int epd_reading_data_set_purpose(epd_reading_data_t client, epd_compute_purpose_t purpose);
int epd_reading_data_status(epd_reading_data_t client);
/* Call after worker operations and cancellation callers have returned. Failed
 * CLOSE retains *client and its buffers for retry; success clears *client. */
int epd_reading_data_close(epd_reading_data_t *client);

/* Incremental FNV-1a, with caller-supplied seed. Hash changes only on success.
 * The callback signature matches storage_signature_update_t exactly. */
int epd_reading_data_hash(epd_reading_data_t client, const void *data, size_t size, uint32_t *hash);
bool epd_reading_data_signature_update(void *client, const void *data, size_t size, uint32_t *hash);
#define EPD_READING_CACHE_MAX_BYTES (64u * 1024u)
#define EPD_READING_CACHE_MAX_DEPS 512u
#define EPD_READING_CACHE_EXPLICIT_ID 1u
typedef struct { uint32_t source_bytes, title_bytes, flags; } epd_reading_cache_spec_t;
/* Document-cache v3 payload: 12 little-endian words, fixed source/title arrays,
 * then {volume,present,size,signature,path_bytes}+NUL-terminated relative path.
 * Validate all lengths, counts, strings and path segments on ACPU and return
 * its FNV-1a checksum. Payload/entry ceilings above also apply to untrusted wire input. */
int epd_reading_data_cache_check(epd_reading_data_t client, const void *blob, size_t size,
    const epd_reading_cache_spec_t *spec, uint32_t *checksum);
/* Exclude one byte range, e.g. the checksum field of a serialized header. */
int epd_reading_data_hash_skip(epd_reading_data_t client, const void *data, size_t size,
                               size_t skip_offset, size_t skip_size, uint32_t *hash);

/* Reader index v2: this header, count+1 {record,byte} cursor pairs, then
 * toc_count {record,level} pairs; every field is a little-endian uint32_t.
 * checksum is FNV-1a with seed FNV_SEED, excluding its own four bytes. */
typedef struct {
    uint32_t magic, version, layout, end, count, toc_count, toc_scanned, complete, checksum;
} epd_reading_index_header_t;
typedef struct {
    uint32_t magic, version, layout, end, page_max, text_limit;
} epd_reading_index_spec_t;
/* Validate checksum, exact size, header identity, cursor bounds/order and TOC
 * record bounds/order on ACPU. No input is retained after the call returns. */
int epd_reading_data_index_check(epd_reading_data_t client, const void *blob, size_t size,
                                 const epd_reading_index_spec_t *spec);
#endif
