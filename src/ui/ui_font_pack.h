#ifndef UI_FONT_PACK_H
#define UI_FONT_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#define UI_FONT_BLOCK_SIZE 32768u
#define UI_FONT_CACHE_BLOCKS 16u

typedef struct
{
    uint32_t position;
    uint8_t bytes[8];
} ui_font_box_t;

typedef struct
{
    void *(*allocate)(size_t bytes, size_t *charge, void *context);
    void (*release)(void *memory, size_t charge, void *context);
    void *context;
} ui_font_pack_allocator_t;

/* One shared cache for all font faces. Callers serialize reads, validation and
 * reclamation; no pointer into a slot escapes these operations. */
typedef struct
{
    const uint8_t *data;
    size_t size;
    uint32_t length, count, block_bytes;
    uint8_t block_shift;
    ui_font_box_t *boxes;
    uint32_t box_count;
    bool boxes_ready;
    ui_font_pack_allocator_t allocator;
    size_t cache_bytes;
    tinfl_decompressor inflater;
    struct
    {
        uint32_t block, age;
        uint8_t *bytes;
        size_t charge;
    } slots[UI_FONT_CACHE_BLOCKS];
} ui_font_pack_t;

/* Reclaim all slots before reinitializing an existing pack. */
bool ui_font_pack_init(ui_font_pack_t *pack, const void *data, size_t size,
                       const ui_font_pack_allocator_t *allocator);
/* Drop least recently used blocks, returning their physical allocation charge.
 * The font source and the optional box index remain valid. */
size_t ui_font_pack_reclaim(ui_font_pack_t *pack, size_t requested);
bool ui_font_pack_validate(ui_font_pack_t *pack);
/* Optional immutable TTF bounding boxes, populated by the validation pass.
 * Storage is caller-owned and must remain alive as long as the pack. */
size_t ui_font_pack_boxes_size(ui_font_pack_t *pack, const uint32_t *face_sizes, unsigned faces);
bool ui_font_pack_boxes_init(ui_font_pack_t *pack, void *storage, size_t size,
                            const uint32_t *face_sizes, unsigned faces);
bool ui_font_pack_read(ui_font_pack_t *pack, uint32_t position, void *output,
                       uint32_t count, uint32_t *read_count);
#endif
