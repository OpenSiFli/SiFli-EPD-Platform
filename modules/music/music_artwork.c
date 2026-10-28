#include "music_artwork.h"
#include <limits.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static bool synchsafe(const uint8_t *p, size_t *n)
{
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80) return false;
    *n = (size_t)p[0] << 21 | (size_t)p[1] << 14 | (size_t)p[2] << 7 | p[3];
    return true;
}
static void init(music_artwork_reader_t *r, const music_artwork_source_t *src,
                 size_t start, size_t end, bool unsync)
{
    memset(r, 0, sizeof(*r));
    r->source = *src;
    r->start = r->position = start;
    r->end = end;
    r->remaining = SIZE_MAX;
    r->unsynchronized = unsync;
}
static bool raw_byte(music_artwork_reader_t *r, uint8_t *out)
{
    if (r->position >= r->end) return false;
    size_t offset = r->position - r->cache_start;
    if (r->position < r->cache_start || offset >= r->cache_count) {
        size_t n = r->end - r->position;
        if (n > sizeof(r->cache)) n = sizeof(r->cache);
        if (r->source.read_at(r->source.context, r->position, r->cache, n) != (int)n)
            return false;
        r->cache_start = r->position;
        r->cache_count = n;
        offset = 0;
    }
    *out = r->cache[offset];
    ++r->position;
    return true;
}
static bool take(music_artwork_reader_t *r, void *data, size_t n)
{
    if (n > r->remaining) return false;
    uint8_t *out = data;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c;
        if (!raw_byte(r, &c)) return false;
        if (r->unsynchronized && r->previous_ff && c == 0 && !raw_byte(r, &c))
            return false;
        r->previous_ff = c == 0xff;
        if (out) out[i] = c;
        ++r->logical;
        if (r->remaining != SIZE_MAX) --r->remaining;
    }
    return true;
}
static bool skip(music_artwork_reader_t *r, size_t n)
{
    if (r->unsynchronized) return take(r, NULL, n);
    if (n > r->remaining || n > r->end - r->position) return false;
    r->position += n;
    r->logical += n;
    if (r->remaining != SIZE_MAX) r->remaining -= n;
    return true;
}
static bool terminated(music_artwork_reader_t *r, unsigned unit)
{
    uint8_t bytes[2];
    while (take(r, bytes, unit))
        if (!bytes[0] && (unit == 1 || !bytes[1])) return true;
    return false;
}
static bool picture(music_artwork_reader_t *r, unsigned type,
                    music_artwork_t *best, unsigned *best_type)
{
    uint8_t magic[8];
    size_t start = r->position, logical = r->logical;
    if (!take(r, magic, sizeof(magic))) return false;
    if (memcmp(magic, "\x89PNG\r\n\x1a\n", 8) &&
        !(magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)) return false;
    size_t size;
    if (r->remaining != SIZE_MAX) {
        size = r->remaining + sizeof(magic);
        if (!skip(r, r->remaining)) return false;
    } else if (!r->unsynchronized) {
        size = r->end - start;
        r->position = r->end;
    } else {
        while (r->position < r->end) {
            /* A final inserted zero after FF belongs to no decoded byte. */
            if (r->previous_ff && r->end - r->position == 1) {
                uint8_t c;
                if (!raw_byte(r, &c)) return false;
                if (c) ++r->logical;
                break;
            }
            if (!take(r, NULL, 1)) return false;
        }
        size = r->logical - logical;
    }
    if (!best->bytes || (type == 3 && *best_type != 3)) {
        *best = (music_artwork_t){start, r->position - start, size, r->unsynchronized};
        *best_type = type;
    }
    return true;
}
static void apic(music_artwork_reader_t *r, bool old, unsigned group, unsigned dli,
                 music_artwork_t *best, unsigned *best_type)
{
    uint8_t encoding, type, bytes[4];
    if (!skip(r, group) || (dli && (!take(r, bytes, 4) ||
        ((bytes[0] | bytes[1] | bytes[2] | bytes[3]) & 0x80)))) return;
    if (!take(r, &encoding, 1) || encoding > 3) return;
    if (old) {
        if (!take(r, bytes, 3) || !memcmp(bytes, "-->", 3)) return;
    } else if (!terminated(r, 1)) return;
    if (!take(r, &type, 1) || !terminated(r, encoding == 1 || encoding == 2 ? 2 : 1)) return;
    picture(r, type, best, best_type);
}
static bool frame_id(const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return false;
    return true;
}
static void id3(const music_artwork_source_t *src, size_t start, size_t bytes,
                music_artwork_t *best, unsigned *best_type)
{
    uint8_t header[10];
    size_t tag_size;
    if (bytes < 10 || src->read_at(src->context, start, header, 10) != 10 ||
        memcmp(header, "ID3", 3) || header[3] < 2 || header[3] > 4 ||
        !synchsafe(header + 6, &tag_size) || tag_size > bytes - 10) return;
    unsigned version = header[3], flags = header[5];
    if ((version == 2 && (flags & 0x7f)) || (version == 3 && (flags & 0x1f)) ||
        (version == 4 && (flags & 0x0f))) return;
    music_artwork_reader_t tag;
    init(&tag, src, start + 10, start + 10 + tag_size, version < 4 && (flags & 0x80));
    if (version >= 3 && (flags & 0x40)) {
        uint8_t ext[4];
        size_t n;
        if (!take(&tag, ext, 4)) return;
        if (version == 4) {
            if (!synchsafe(ext, &n) || n < 6) return;
            n -= 4;
        } else n = be32(ext);
        if (!skip(&tag, n)) return;
    }
    unsigned head_size = version == 2 ? 6 : 10;
    while (tag.position < tag.end) {
        uint8_t h[10];
        size_t n;
        if (!take(&tag, h, head_size) || !frame_id(h, version == 2 ? 3 : 4)) return;
        if (version == 2) n = (size_t)h[3] << 16 | (size_t)h[4] << 8 | h[5];
        else if (version == 3) n = be32(h + 4);
        else if (!synchsafe(h + 4, &n)) return;
        music_artwork_reader_t frame = tag;
        if (!skip(&tag, n)) return;
        frame.end = tag.position;
        frame.remaining = version < 4 ? n : SIZE_MAX;
        /* Cached bytes may extend into the next frame; end is checked first. */
        bool is_picture = !memcmp(h, version == 2 ? "PIC" : "APIC", version == 2 ? 3 : 4);
        if (is_picture) {
            unsigned f = version == 2 ? 0 : h[9];
            if (version == 3 && !(f & 0xdf)) apic(&frame, false, (f & 0x20) != 0, 0, best, best_type);
            if (version == 2) apic(&frame, true, 0, 0, best, best_type);
            if (version == 4 && !(f & 0xbc)) {
                frame.unsynchronized = (f & 2) || (flags & 0x80);
                frame.previous_ff = false;
                apic(&frame, false, (f & 0x40) != 0, f & 1, best, best_type);
            }
        }
        if (*best_type == 3) return;
    }
}
static void flac(const music_artwork_source_t *src, music_artwork_t *best, unsigned *best_type)
{
    size_t position = 4;
    while (position <= src->size && src->size - position >= 4) {
        uint8_t h[4];
        if (src->read_at(src->context, position, h, 4) != 4) return;
        size_t n = (size_t)h[1] << 16 | (size_t)h[2] << 8 | h[3];
        position += 4;
        if (n > src->size - position) return;
        if ((h[0] & 0x7f) == 6) {
            music_artwork_reader_t r;
            uint8_t p[8];
            init(&r, src, position, position + n, false);
            if (take(&r, p, 8)) {
                unsigned type = be32(p);
                if (skip(&r, be32(p + 4)) && take(&r, p, 4) && skip(&r, be32(p)) &&
                    skip(&r, 16) && take(&r, p, 4) && be32(p) <= r.end - r.position) {
                    r.end = r.position + be32(p);
                    picture(&r, type, best, best_type);
                }
            }
        }
        if ((h[0] & 0x80) || *best_type == 3) return;
        position += n;
    }
}
bool music_artwork_find(const music_artwork_source_t *src, music_artwork_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    uint8_t h[12];
    unsigned type = UINT_MAX;
    if (!src || !src->read_at || src->size < sizeof(h) ||
        src->read_at(src->context, 0, h, sizeof(h)) != sizeof(h)) return false;
    if (!memcmp(h, "ID3", 3)) id3(src, 0, src->size, out, &type);
    else if (!memcmp(h, "fLaC", 4)) flac(src, out, &type);
    else if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        size_t size = le32(h + 4), position = 12;
        if (size < 4 || size > src->size - 8) return false;
        size += 8;
        while (size - position >= 8) {
            if (src->read_at(src->context, position, h, 8) != 8) break;
            size_t n = le32(h + 4);
            position += 8;
            if (n > size - position) break;
            if (!memcmp(h, "ID3 ", 4) || !memcmp(h, "id3 ", 4))
                id3(src, position, n, out, &type);
            if (type == 3) break;
            position += n;
            if (n & 1) { if (position == size) break; ++position; }
        }
    }
    return out->bytes != 0;
}
void music_artwork_reader_init(music_artwork_reader_t *r,
                              const music_artwork_source_t *src, const music_artwork_t *art)
{
    init(r, src, art->offset, art->offset + art->bytes, art->unsynchronized);
    r->remaining = art->decoded_bytes;
}
int music_artwork_read_at(void *context, size_t offset, void *data, size_t size)
{
    music_artwork_reader_t *r = context;
    size_t total = r->logical + r->remaining;
    if (offset > total || size > total - offset || size > INT_MAX) return -1;
    if (offset < r->logical) {
        r->position = r->start;
        r->logical = 0;
        r->remaining = total;
        r->previous_ff = false;
    }
    if (!skip(r, offset - r->logical)) return -1;
    if (!r->unsynchronized) {
        if (r->source.read_at(r->source.context, r->position, data, size) != (int)size) return -1;
        return skip(r, size) ? (int)size : -1;
    }
    return take(r, data, size) ? (int)size : -1;
}
