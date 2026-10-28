#include "ui_font_pack.h"
#include <string.h>

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool ui_font_pack_init(ui_font_pack_t *pack, const void *data, size_t size)
{
    if (!pack) return false;
    pack->data = NULL;
    if (!data || size < 24 || size > UINT32_MAX) return false;
    const uint8_t *bytes = data;
    uint32_t length = read_u32(bytes + 4), count = read_u32(bytes + 12);
    if (memcmp(bytes, "FTZ1", 4) || !length ||
        read_u32(bytes + 8) != UI_FONT_BLOCK_SIZE ||
        count != (length - 1) / UI_FONT_BLOCK_SIZE + 1 ||
        count > (size - 20) / 4) return false;
    uint32_t previous = 20 + count * 4;
    if (read_u32(bytes + 16) != previous) return false;
    for (uint32_t i = 1; i <= count; ++i)
    {
        uint32_t end = read_u32(bytes + 16 + i * 4);
        if (end <= previous || end > size) return false;
        previous = end;
    }
    if (previous != size) return false;
    memset(pack, 0, sizeof(*pack));
    pack->data = bytes;
    pack->size = size;
    pack->length = length;
    pack->count = count;
    for (unsigned i = 0; i < UI_FONT_CACHE_BLOCKS; ++i)
        pack->slots[i].block = UINT32_MAX;
    return true;
}

static const uint8_t *read_block(ui_font_pack_t *pack, uint32_t block)
{
    unsigned selected = 0;
    bool found = false;
    for (unsigned i = 0; i < UI_FONT_CACHE_BLOCKS; ++i)
    {
        if (pack->slots[i].block == block) { selected = i; found = true; break; }
        if (pack->slots[selected].block != UINT32_MAX &&
            (pack->slots[i].block == UINT32_MAX ||
             pack->slots[i].age > pack->slots[selected].age)) selected = i;
    }
    if (!found)
    {
        uint32_t begin = read_u32(pack->data + 16 + block * 4);
        uint32_t end = read_u32(pack->data + 20 + block * 4);
        uint32_t expected = pack->length - block * UI_FONT_BLOCK_SIZE;
        if (expected > UI_FONT_BLOCK_SIZE) expected = UI_FONT_BLOCK_SIZE;
        size_t input = end - begin, output = expected;
        pack->slots[selected].block = UINT32_MAX;
        tinfl_init(&pack->inflater);
        tinfl_status result = tinfl_decompress(&pack->inflater, pack->data + begin,
            &input, pack->slots[selected].bytes, pack->slots[selected].bytes, &output,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
            TINFL_FLAG_COMPUTE_ADLER32);
        if (result != TINFL_STATUS_DONE || input != end - begin || output != expected)
            return NULL;
        pack->slots[selected].block = block;
    }
    /* Saturating ages avoid a timestamp-wrap special case. */
    for (unsigned i = 0; i < UI_FONT_CACHE_BLOCKS; ++i)
        if (pack->slots[i].age < UINT32_MAX) ++pack->slots[i].age;
    pack->slots[selected].age = 0;
    return pack->slots[selected].bytes;
}

bool ui_font_pack_validate(ui_font_pack_t *pack)
{
    if (!pack || !pack->data) return false;
    pack->boxes_ready = false;
    uint32_t box = 0;
    for (uint32_t block = 0; block < pack->count; ++block)
    {
        const uint8_t *bytes = read_block(pack, block);
        if (!bytes) return false;
        uint32_t begin = block * UI_FONT_BLOCK_SIZE;
        uint32_t length = pack->length - begin;
        if (length > UI_FONT_BLOCK_SIZE) length = UI_FONT_BLOCK_SIZE;
        uint32_t end = begin + length;
        while (box < pack->box_count && pack->boxes[box].position < end)
        {
            ui_font_box_t *entry = &pack->boxes[box];
            uint32_t offset = entry->position < begin ? begin - entry->position : 0;
            uint32_t take = sizeof(entry->bytes) - offset;
            if (take > end - (entry->position + offset)) take = end - (entry->position + offset);
            memcpy(entry->bytes + offset, bytes + entry->position + offset - begin, take);
            if (offset + take < sizeof(entry->bytes)) break;
            ++box;
        }
    }
    pack->boxes_ready = box == pack->box_count;
    return true;
}

static bool read_exact(ui_font_pack_t *pack, uint32_t position, void *buffer, uint32_t size)
{
    uint32_t count;
    return ui_font_pack_read(pack, position, buffer, size, &count) && count == size;
}

static uint32_t read_be(const uint8_t *p, unsigned width)
{
    uint32_t value = 0;
    while (width--) value = value << 8 | *p++;
    return value;
}

typedef struct { uint32_t offset, length; } font_table_t;

static bool face_boxes(ui_font_pack_t *pack, uint32_t base, uint32_t length,
                       ui_font_box_t *boxes, uint32_t capacity, uint32_t *count)
{
    uint8_t header[54];
    if (length < 12 || !read_exact(pack, base, header, 12) ||
        (read_be(header, 4) != 0x00010000u && memcmp(header, "true", 4))) return false;
    unsigned tables = read_be(header + 4, 2);
    if (tables > (length - 12) / 16) return false;
    font_table_t table[4] = {0};
    static const char tags[4][5] = {"head", "maxp", "loca", "glyf"};
    for (unsigned i = 0; i < tables; ++i)
    {
        if (!read_exact(pack, base + 12 + i * 16, header, 16)) return false;
        for (unsigned t = 0; t < 4; ++t)
            if (!memcmp(header, tags[t], 4))
            {
                uint32_t offset = read_be(header + 8, 4), size = read_be(header + 12, 4);
                if (table[t].length || !size || offset > length || size > length - offset) return false;
                table[t] = (font_table_t){offset, size};
            }
    }
    if (table[0].length < 54 || table[1].length < 6 || !table[2].length || !table[3].length ||
        !read_exact(pack, base + table[0].offset, header, 54)) return false;
    unsigned format = read_be(header + 50, 2);
    if (format > 1 || !read_exact(pack, base + table[1].offset, header, 6)) return false;
    uint32_t glyphs = read_be(header + 4, 2);
    unsigned width = format ? 4 : 2;
    if ((glyphs + 1) > table[2].length / width) return false;
    uint32_t previous = 0, entry = 0;
    uint8_t locations[1024];
    while (entry <= glyphs)
    {
        uint32_t n = glyphs + 1 - entry;
        if (n > sizeof(locations) / width) n = sizeof(locations) / width;
        if (!read_exact(pack, base + table[2].offset + entry * width, locations, n * width)) return false;
        for (uint32_t i = 0; i < n; ++i)
        {
            uint32_t next = read_be(locations + i * width, width);
            if (!format) next *= 2;
            if (next > table[3].length || next < previous) return false;
            if (entry + i && next != previous)
            {
                if (next - previous < 10 || *count == UINT32_MAX) return false;
                if (boxes)
                {
                    if (*count >= capacity) return false;
                    boxes[*count].position = base + table[3].offset + previous + 2;
                }
                ++*count;
            }
            previous = next;
        }
        entry += n;
    }
    return true;
}

static bool scan_boxes(ui_font_pack_t *pack, const uint32_t *sizes, unsigned faces,
                       ui_font_box_t *boxes, uint32_t capacity, uint32_t *count)
{
    if (!pack || !pack->data || !sizes || !faces) return false;
    uint32_t base = 0;
    *count = 0;
    for (unsigned i = 0; i < faces; ++i)
    {
        if (sizes[i] > pack->length - base ||
            !face_boxes(pack, base, sizes[i], boxes, capacity, count)) return false;
        base += sizes[i];
    }
    return base == pack->length;
}

size_t ui_font_pack_boxes_size(ui_font_pack_t *pack, const uint32_t *face_sizes, unsigned faces)
{
    uint32_t count;
    if (!scan_boxes(pack, face_sizes, faces, NULL, 0, &count) || count > SIZE_MAX / sizeof(ui_font_box_t)) return 0;
    return (size_t)count * sizeof(ui_font_box_t);
}

bool ui_font_pack_boxes_init(ui_font_pack_t *pack, void *storage, size_t size,
                            const uint32_t *face_sizes, unsigned faces)
{
    if (!pack) return false;
    pack->boxes = NULL;
    pack->box_count = 0;
    pack->boxes_ready = false;
    if (!storage || size / sizeof(ui_font_box_t) > UINT32_MAX) return false;
    uint32_t count;
    if (!scan_boxes(pack, face_sizes, faces, storage, (uint32_t)(size / sizeof(ui_font_box_t)), &count)) return false;
    pack->boxes = storage;
    pack->box_count = count;
    return true;
}

bool ui_font_pack_read(ui_font_pack_t *pack, uint32_t position, void *output,
                       uint32_t count, uint32_t *read_count)
{
    if (!read_count) return false;
    *read_count = 0;
    if (!pack || !pack->data || position > pack->length || (!output && count)) return false;
    if (count > pack->length - position) count = pack->length - position;
    if (pack->boxes_ready && count && count <= 8)
    {
        uint32_t low = 0, high = pack->box_count;
        while (low < high)
        {
            uint32_t mid = low + (high - low) / 2;
            if (pack->boxes[mid].position <= position) low = mid + 1;
            else high = mid;
        }
        if (low)
        {
            const ui_font_box_t *box = &pack->boxes[low - 1];
            uint32_t offset = position - box->position;
            if (offset < sizeof(box->bytes) && count <= sizeof(box->bytes) - offset)
            {
                memcpy(output, box->bytes + offset, count);
                *read_count = count;
                return true;
            }
        }
    }
    uint8_t *destination = output;
    while (count)
    {
        uint32_t offset = position % UI_FONT_BLOCK_SIZE;
        uint32_t take = UI_FONT_BLOCK_SIZE - offset;
        if (take > count) take = count;
        const uint8_t *block = read_block(pack, position / UI_FONT_BLOCK_SIZE);
        if (!block) return false;
        memcpy(destination, block + offset, take);
        position += take;
        destination += take;
        count -= take;
        *read_count += take;
    }
    return true;
}
