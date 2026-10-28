/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_WINDOW_H
#define READER_WINDOW_H
#include "reader_records.h"

#define READER_WINDOW_SLOTS 4u
typedef enum {
    READER_WINDOW_FREE, READER_WINDOW_WRITING, READER_WINDOW_READY,
    READER_WINDOW_RETIRED_WRITE, READER_WINDOW_RETIRED_READ
} reader_window_phase_t;
typedef struct {
    epd_read_batch_t batch;
    uint64_t serial, session;
    uint32_t generation, references;
    reader_window_phase_t phase;
    int result;
} reader_window_slot_t;
typedef struct {
    reader_window_slot_t slots[READER_WINDOW_SLOTS];
    book_allocator_t memory;
    size_t limit, block_size, allocated, peak;
    uint64_t sequence;
} reader_window_t;
typedef struct { uint64_t serial; unsigned slot; } reader_window_key_t;
typedef struct {
    reader_window_key_t key;
    epd_read_batch_t batch;
    int result;
    uint64_t session;
    uint32_t generation;
} reader_window_lease_t;

/* The owner serializes metadata calls. Parsing happens outside that lock in a
 * reserved buffer; readers only access published immutable batches. Configure
 * once, before use. limit bounds buffer bytes; sizeof(reader_window_t) is the
 * separately accounted, fixed metadata cost. No speculative allocation spills
 * past limit. Leases are linear: initialize to zero and do not copy ownership. */
int reader_window_init(reader_window_t *window, const book_allocator_t *memory,
                       size_t limit, size_t block_size);
int reader_window_reserve(reader_window_t *window, uint64_t session, uint32_t generation,
                          reader_window_key_t *key, epd_read_batch_t *batch);
/* Publish consumes the write reservation, including stale/error completion.
 * END may accompany a final nonempty batch; errors never publish partial data. */
int reader_window_publish(reader_window_t *window, reader_window_key_t key,
                          const epd_read_batch_t *batch, int result);
int reader_window_acquire(reader_window_t *window, reader_window_key_t key,
                          uint64_t session, uint32_t generation, reader_window_lease_t *lease);
int reader_window_release(reader_window_t *window, reader_window_lease_t *lease);
/* Retire every generation other than the named active one. Retired reads and
 * writes keep their bytes until the last consumer/writer returns ownership.
 * session=0 retires all data, including completed unborrowed batches. */
void reader_window_retire(reader_window_t *window, uint64_t session, uint32_t generation);
/* Returns false while a writer or reader still owns data. Safe to retry. */
bool reader_window_close(reader_window_t *window);
#endif
