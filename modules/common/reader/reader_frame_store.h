/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_FRAME_STORE_H
#define READER_FRAME_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Independent 4 KiB byte blocks. A token encodes 1..128 literal bytes or a
 * repeated byte. Incompressible blocks use their original representation. */
#define READER_FRAME_BLOCK 4096u
typedef struct reader_frame_block {
    struct reader_frame_block *next;
    uint16_t raw, stored;
    uint8_t data[];
} reader_frame_block_t;
typedef struct {
    reader_frame_block_t *first, *last;
    size_t raw, charge;
} reader_frame_store_t;
typedef struct {
    void *(*allocate)(size_t bytes, void *user);
    void (*release)(void *memory, size_t bytes, void *user);
    size_t (*charge)(size_t bytes);
    void *user;
} reader_frame_allocator_t;

/* A NULL output performs the exact sizing pass without a workspace. */
static size_t reader_frame_encode(const uint8_t *in, size_t bytes, uint8_t *out)
{
    size_t at = 0, used = 0;
    while (at < bytes) {
        size_t run = 1;
        while (run < 128 && run < bytes - at && in[at + run] == in[at]) ++run;
        if (run >= 3) {
            if (out) { out[used] = (uint8_t)(0x80u | (run - 1)); out[used + 1] = in[at]; }
            used += 2; at += run;
        } else {
            size_t start = at;
            do {
                ++at;
                if (at == bytes || at - start == 128) break;
            } while (bytes - at < 3 || in[at] != in[at + 1] || in[at] != in[at + 2]);
            size_t length = at - start;
            if (out) { out[used] = (uint8_t)(length - 1); memcpy(out + used + 1, in + start, length); }
            used += 1 + length;
        }
    }
    return used;
}
static void reader_frame_store_clear(reader_frame_store_t *store, const reader_frame_allocator_t *allocator)
{
    reader_frame_block_t *block = store->first;
    memset(store, 0, sizeof(*store));
    while (block) {
        reader_frame_block_t *next = block->next;
        allocator->release(block, sizeof(*block) + block->stored, allocator->user);
        block = next;
    }
}
/* Append at most one block. The source remains owned by the caller until all
 * blocks succeed. The physical charge limit includes every block header. */
static bool reader_frame_store_append(reader_frame_store_t *store, const uint8_t *source,
                                      size_t bytes, size_t limit, const reader_frame_allocator_t *allocator)
{
    if (store->raw >= bytes) return false;
    size_t raw = bytes - store->raw;
    if (raw > READER_FRAME_BLOCK) raw = READER_FRAME_BLOCK;
    source += store->raw;
    size_t packed = reader_frame_encode(source, raw, NULL);
    size_t stored = packed < raw ? packed : raw;
    size_t allocation = sizeof(reader_frame_block_t) + stored;
    size_t charge = allocator->charge(allocation);
    if (charge >= limit || store->charge >= limit - charge) return false;
    reader_frame_block_t *block = allocator->allocate(allocation, allocator->user);
    if (!block) return false;
    block->next = NULL; block->raw = (uint16_t)raw; block->stored = (uint16_t)stored;
    if (stored == raw) memcpy(block->data, source, raw);
    else reader_frame_encode(source, raw, block->data);
    if (store->last) store->last->next = block;
    else store->first = block;
    store->last = block; store->raw += raw; store->charge += charge;
    return true;
}
/* Decode validates each block before advancing to the next destination range. */
static bool reader_frame_block_read(const reader_frame_block_t *block, uint8_t *out, size_t capacity)
{
    if (!block->raw || block->raw > READER_FRAME_BLOCK || block->raw > capacity ||
        !block->stored || block->stored > block->raw) return false;
    if (block->stored == block->raw) { memcpy(out, block->data, block->raw); return true; }
    size_t at = 0, produced = 0;
    while (at < block->stored) {
        unsigned token = block->data[at++];
        size_t count = (token & 127u) + 1u;
        if (count > block->raw - produced) return false;
        if (token & 128u) {
            if (at == block->stored) return false;
            memset(out + produced, block->data[at++], count);
        } else {
            if (count > block->stored - at) return false;
            memcpy(out + produced, block->data + at, count); at += count;
        }
        produced += count;
    }
    return produced == block->raw;
}
#endif
