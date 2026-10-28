/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_CACHE_H
#define BOOK_DOCUMENT_CACHE_H
#include "../bookshelf.h"
#include <stddef.h>
#include <stdint.h>

typedef struct document_cache document_cache_t;

/* create/lookup/destroy may run in the UI; mutable state and storage operations
 * are protected by storage_lock. All other mutations run on the document
 * worker. Destroy only after conversion and all readers/index jobs release it.
 * create uses the scanned source identity, without opening the source or codec.
 * The caller creates document_cache_directory(). NULL/false disables optional
 * persistence. Books on a TF card keep derived content on that card. */
bool document_cache_directory(const bookshelf_item_t *file, const char *app_id,
                              char *directory, size_t capacity);
document_cache_t *document_cache_create(const bookshelf_item_t *file);
/* Explicit identity includes every chapter/resource revision used by convert.
 * Same cache directory may hold multiple apps; app_id is part of the key. */
document_cache_t *document_cache_create_source(const bookshelf_item_t *file,
    const char *app_id, const char *directory, const char *identity);
/* Borrowed, immutable paths, valid until destroy. base has no extension. */
const char *document_cache_base(const document_cache_t *cache);
const char *document_cache_binary(const document_cache_t *cache);
const char *document_cache_meta(const document_cache_t *cache);
const char *document_cache_index(const document_cache_t *cache);
/* Nonzero after a successful lookup or commit. Includes dependency identity;
 * index readers must additionally validate their layout and offset bounds. */
uint32_t document_cache_identity(const document_cache_t *cache);
bool document_cache_lookup(document_cache_t *cache, char *title,
                           size_t title_capacity, uint32_t *end);
/* Persist an incomplete marker BEFORE replacing binary. Failure forbids that
 * replacement. Also refuses replacement while another object uses this base.
 * Call after observe_end; preserves collected dependencies. Conversion uses
 * a separate session-temporary file until this final commit preparation. */
bool document_cache_prepare(document_cache_t *cache);
/* Mark proven corruption incomplete even while other objects use this base.
 * Does not delete binary. A failed marker save disables reuse for this app run. */
bool document_cache_invalidate(document_cache_t *cache);
/* Independently reset and scope actual conversion, including unsuccessful
 * resource opens. End before prepare or leaving conversion. Failure affects
 * reuse only. Sequence: begin, convert, end, prepare, binary rename, commit. */
bool document_cache_observe_begin(document_cache_t *cache);
void document_cache_observe_end(document_cache_t *cache);
/* Call after observe_end, all binary handles are closed, and the caller has
 * committed the synced binary via storage_file_commit under storage_lock.
 * Writes metadata only; never changes record bytes or saves an index. */
bool document_cache_commit(document_cache_t *cache, const char *title, uint32_t end);
/* Optional worker-only recency update after a hit; failure does not stop reading. */
bool document_cache_touch(document_cache_t *cache);
/* Delete at most one oldest inactive derived family in this cache directory.
 * Call on space exhaustion; the caller controls its single I/O retry. */
bool document_cache_reclaim(document_cache_t *cache);
/* Memory only: does not unlink files or access a replacement card. */
void document_cache_destroy(document_cache_t *cache);
/* Release the bounded runtime dependency cache at document-service shutdown. */
void document_cache_release_memory(void);
#endif
