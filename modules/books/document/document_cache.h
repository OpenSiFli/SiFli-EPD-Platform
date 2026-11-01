/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_CACHE_H
#define BOOK_DOCUMENT_CACHE_H
#include "../bookshelf.h"
#include <stddef.h>
#include <stdint.h>

typedef struct document_cache document_cache_t;
#define DOCUMENT_CACHE_METADATA_MAX (64u * 1024u)
#define DOCUMENT_CACHE_DEPENDENCIES_MAX 512u
/* Limits apply only to optional persistent reuse. Exceeding either limit
 * leaves the current conversion and its readable temporary spool available. */

/* create is memory-only. Lookup/observe/commit execute on the worker without an
 * outer storage_lock: hashing and compute admission occur outside that lock.
 * The dependency payload/LRU belongs to the serialized worker. Files use 4 KiB
 * lock scopes; ACPU validation and dependency traversal run outside that lock,
 * with file tokens and volume generations rechecked before publication.
 * Mutable lifecycle state and storage I/O are protected internally. Destroy only after
 * conversion and all readers/index jobs release it and cancel callers return.
 * create uses the scanned source identity, without opening the source or codec.
 * The caller creates document_cache_bucket(). NULL/false disables optional
 * persistence. Books on a TF card keep derived content on that card. */
/* New namespace root; does not inspect or migrate the legacy documents tree. */
bool document_cache_directory(const bookshelf_item_t *file, const char *app_id,
                              char *directory, size_t capacity);
document_cache_t *document_cache_create(const bookshelf_item_t *file);
/* Explicit identity includes every chapter/resource revision used by convert.
 * A supplied directory receives a doccache subtree; app_id is part of the key. */
document_cache_t *document_cache_create_source(const bookshelf_item_t *file,
    const char *app_id, const char *directory, const char *identity);
/* Caller holds storage_lock and must still open with O_EXCL. On repeated name
 * collisions, inspect at most 512 leaf entries with 128 bytes of stack scratch.
 * extension is .tmp or .new; stopped must include the caller's media session. */
bool document_cache_temporary_number(const char *directory, const char *extension,
    uint32_t *number, bool (*stopped)(void *), void *user);
/* Borrowed, immutable paths, valid until destroy. base has no extension. */
const char *document_cache_bucket(const document_cache_t *cache);
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
/* Worker resource-provider observation, shared by local I/O and ACPU requests. */
void document_cache_observe_resource(document_cache_t *cache, const char *path, int event);
/* Sticky cancellation can race with worker hashing; retain cache until the
 * worker and all cancellation callers return. */
void document_cache_cancel(document_cache_t *cache);
/* File payload FNV on ACPU using the shared storage signature LRU. No outer
 * storage lock; failed admission/hash/close leaves *signature unchanged. Uses
 * nonblocking admission so a held document session cannot cause nested waits. */
bool document_cache_file_signature(const char *path, uint32_t *signature);
/* Call after observe_end, all binary handles are closed, and the caller has
 * committed the synced binary via storage_file_commit under storage_lock.
 * Writes metadata only; never changes record bytes or saves an index. */
bool document_cache_commit(document_cache_t *cache, const char *title, uint32_t end);
/* Optional worker-only recency update after a hit; failure does not stop reading. */
bool document_cache_touch(document_cache_t *cache);
/* On space exhaustion, visit at most 256 buckets / 512 entries / 16 metadata
 * frames; delete one abandoned temporary from an inactive bucket, or one
 * inactive family, oldest among the scanned leaf.
 * The cursor rotates after each leaf. The caller controls its single I/O retry.
 * Legacy flat files and allocation-fallback loose/ spools are not traversed.
 * Supplied/shared directories retain temporaries and other apps' families;
 * another loaded module can own those files outside this active list. */
bool document_cache_reclaim(document_cache_t *cache);
/* Memory only: does not unlink files or access a replacement card. */
void document_cache_destroy(document_cache_t *cache);
/* Release the bounded runtime dependency cache at document-service shutdown. */
void document_cache_release_memory(void);
/* Memory-only query after release_memory; the worker must remain alive while
 * a failed CLOSE or an in-flight signature still owns a compute client. */
bool document_cache_cleanup_pending(void);
#endif
