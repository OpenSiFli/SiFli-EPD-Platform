/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_window.h"
#include <limits.h>
#include <string.h>

static reader_window_slot_t *find(reader_window_t *w, reader_window_key_t key)
{
    if (!w || !key.serial || key.slot >= READER_WINDOW_SLOTS) return NULL;
    reader_window_slot_t *s = &w->slots[key.slot];
    return s->phase != READER_WINDOW_FREE && s->serial == key.serial ? s : NULL;
}
static void drop(reader_window_t *w, reader_window_slot_t *s)
{
    if (s->batch.data) { w->allocated -= s->batch.capacity; w->memory.free(s->batch.data); }
    memset(s, 0, sizeof(*s));
}
int reader_window_init(reader_window_t *w, const book_allocator_t *memory,
                       size_t limit, size_t block_size)
{
    if (!w || !memory || !memory->alloc || !memory->free ||
        block_size < sizeof(epd_read_record_t) + EPD_READER_RECORD_TEXT_MAX ||
        (block_size & 7u) || limit < block_size) return EPD_READ_INVALID;
    memset(w, 0, sizeof(*w));
    w->memory = *memory; w->limit = limit; w->block_size = block_size;
    return EPD_READ_OK;
}
int reader_window_reserve(reader_window_t *w, uint64_t session, uint32_t generation,
                          reader_window_key_t *key, epd_read_batch_t *batch)
{
    if (!w || !key || !batch || !session || !generation || !w->block_size) return EPD_READ_INVALID;
    *key = (reader_window_key_t){0}; *batch = (epd_read_batch_t){0};
    if (w->sequence == UINT64_MAX) return EPD_READ_BUSY;
    reader_window_slot_t *s = NULL, *oldest = NULL;
    for (unsigned i = 0; i < READER_WINDOW_SLOTS; ++i)
    {
        reader_window_slot_t *p = &w->slots[i];
        if (p->phase == READER_WINDOW_FREE && !s) s = p;
        if (p->phase == READER_WINDOW_READY && !p->references &&
            (!oldest || p->serial < oldest->serial)) oldest = p;
    }
    if (!s || w->allocated > w->limit - w->block_size) s = oldest;
    if (!s) return EPD_READ_BUSY;
    /* Reuse a completed unborrowed buffer without a transient second copy. */
    if (!s->batch.data)
    {
        s->batch.data = w->memory.alloc(w->block_size);
        if (!s->batch.data) return EPD_READ_NO_MEMORY;
        w->allocated += w->block_size;
        if (w->allocated > w->peak) w->peak = w->allocated;
    }
    reader_batch_init(&s->batch, s->batch.data, w->block_size);
    s->phase = READER_WINDOW_WRITING; s->serial = ++w->sequence;
    s->session = session; s->generation = generation; s->result = EPD_READ_WAIT;
    *key = (reader_window_key_t){s->serial, (unsigned)(s - w->slots)};
    *batch = s->batch;
    return EPD_READ_OK;
}
int reader_window_publish(reader_window_t *w, reader_window_key_t key,
                          const epd_read_batch_t *batch, int result)
{
    reader_window_slot_t *s = find(w, key);
    if (!s || (s->phase != READER_WINDOW_WRITING && s->phase != READER_WINDOW_RETIRED_WRITE))
        return EPD_READ_STALE;
    if (s->phase == READER_WINDOW_RETIRED_WRITE) { drop(w, s); return EPD_READ_STALE; }
    if (result < 0) { drop(w, s); return result; }
    if (!batch || batch->data != s->batch.data || batch->capacity != s->batch.capacity ||
        !reader_batch_valid(batch) ||
        (result != EPD_READ_OK && result != EPD_READ_END && result != EPD_READ_FULL &&
         result != EPD_READ_WAIT && result != EPD_READ_YIELD) ||
        (result == EPD_READ_YIELD && batch->count) ||
        (!batch->count && result != EPD_READ_END && result != EPD_READ_WAIT && result != EPD_READ_YIELD))
    { drop(w, s); return EPD_READ_INVALID; }
    if (!batch->count && result == EPD_READ_WAIT) { drop(w, s); return EPD_READ_WAIT; }
    /* Keep an empty, unborrowed allocation for the next bounded parsing step. */
    if (result == EPD_READ_YIELD)
    { s->batch = *batch; s->phase = READER_WINDOW_READY; s->result = result; return result; }
    s->batch = *batch; s->phase = READER_WINDOW_READY; s->result = result;
    return EPD_READ_OK;
}
int reader_window_acquire(reader_window_t *w, reader_window_key_t key,
                          uint64_t session, uint32_t generation, reader_window_lease_t *lease)
{
    if (!lease || lease->key.serial || lease->batch.data) return EPD_READ_INVALID;
    reader_window_slot_t *s = find(w, key);
    if (!s || s->phase != READER_WINDOW_READY || s->session != session || s->generation != generation)
        return EPD_READ_STALE;
    if (s->references == UINT32_MAX) return EPD_READ_BUSY;
    ++s->references;
    *lease = (reader_window_lease_t){.key = key, .batch = s->batch, .result = s->result,
                                    .session = s->session, .generation = s->generation};
    return EPD_READ_OK;
}
int reader_window_release(reader_window_t *w, reader_window_lease_t *lease)
{
    if (!lease) return EPD_READ_INVALID;
    reader_window_slot_t *s = find(w, lease->key);
    if (!s || !s->references || (s->phase != READER_WINDOW_READY && s->phase != READER_WINDOW_RETIRED_READ))
        return EPD_READ_STALE;
    *lease = (reader_window_lease_t){0};
    if (!--s->references && s->phase == READER_WINDOW_RETIRED_READ) drop(w, s);
    return EPD_READ_OK;
}
void reader_window_retire(reader_window_t *w, uint64_t session, uint32_t generation)
{
    if (!w) return;
    for (unsigned i = 0; i < READER_WINDOW_SLOTS; ++i)
    {
        reader_window_slot_t *s = &w->slots[i];
        if (s->phase == READER_WINDOW_FREE ||
            (session && s->session == session && s->generation == generation)) continue;
        if (s->phase == READER_WINDOW_WRITING) s->phase = READER_WINDOW_RETIRED_WRITE;
        else if (s->phase == READER_WINDOW_READY) s->phase = READER_WINDOW_RETIRED_READ;
        if (s->phase == READER_WINDOW_RETIRED_READ && !s->references) drop(w, s);
    }
}
bool reader_window_close(reader_window_t *w)
{
    if (!w) return true;
    reader_window_retire(w, 0, 0);
    return !w->allocated;
}
