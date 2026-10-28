#include "reading_font_validate.h"
#include "storage_file.h"
#include "epd_memory.h"
#include <dfs_posix.h>
#include <string.h>

/* Bounds also cover expanded composites, not just the maxp declarations. */
#define VF_TABLES 64u
#define VF_POINTS 4096u
#define VF_CONTOURS 256u
#define VF_DEPTH 16u
#define VF_COMPONENTS 256u
#define VF_WORK (16u * 1024u * 1024u)
#define VF_TAG(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

typedef struct { uint32_t tag, offset, length; } vf_table_t;
typedef struct
{
    int fd;
    rt_err_t error;
    uint32_t size, cached_at, cached_size, work;
    const char *path;
    uint32_t revision, session;
    storage_volume_t volume;
    reading_font_cancel_t cancelled;
    void *context;
    vf_table_t tables[VF_TABLES], head, hhea, hmtx, maxp, cmap, loca, glyf, name;
    uint16_t glyphs, loca_format, ancestors[VF_DEPTH];
    uint8_t cache[4096], flags[VF_POINTS];
} vf_t;

static bool vf_fail(vf_t *v, rt_err_t error)
{
    if (!v->error) v->error = error;
    return false;
}

static bool vf_tick(vf_t *v)
{
    if (v->error) return false;
    if (v->cancelled && v->cancelled(v->context)) return vf_fail(v, -RT_EINTR);
    if (++v->work > VF_WORK) return vf_fail(v, -RT_EINVAL);
    return true;
}

static bool vf_range(uint32_t offset, uint32_t length, uint32_t end)
{ return offset <= end && length <= end - offset; }

static bool vf_block(vf_t *v, uint32_t at, void *out, uint32_t n)
{
    storage_lock();
    bool ok = vf_tick(v);
    if (ok && (!storage_session_valid(v->volume, v->session) ||
               storage_file_writers(v->path) || storage_path_revision(v->path) != v->revision))
        ok = vf_fail(v, -RT_EIO);
    if (ok && (lseek(v->fd, (off_t)at, SEEK_SET) != (off_t)at || read(v->fd, out, n) != (int)n))
        ok = vf_fail(v, -RT_EIO);
    storage_unlock();
    return ok;
}

static bool vf_stat(vf_t *v, struct stat *st)
{
    storage_lock();
    bool ok = vf_tick(v);
    if (ok && fstat(v->fd, st) != 0) ok = vf_fail(v, -RT_EIO);
    storage_unlock();
    return ok;
}

static bool vf_read(vf_t *v, uint32_t at, void *out, uint32_t size)
{
    uint8_t *p = out;
    if (!vf_tick(v)) return false;
    if (!vf_range(at, size, v->size)) return vf_fail(v, -RT_EINVAL);
    while (size)
    {
        if (at < v->cached_at || at - v->cached_at >= v->cached_size)
        {
            uint32_t n = v->size - at;
            if (n > sizeof(v->cache)) n = sizeof(v->cache);
            v->cached_size = 0;
            if (!vf_block(v, at, v->cache, n)) return false;
            v->cached_at = at;
            v->cached_size = n;
        }
        uint32_t n = v->cached_size - (at - v->cached_at);
        if (n > size) n = size;
        memcpy(p, v->cache + at - v->cached_at, n);
        p += n; at += n; size -= n;
    }
    return true;
}

static uint16_t vf_u16(vf_t *v, uint32_t at)
{
    uint8_t b[2] = {0};
    vf_read(v, at, b, sizeof(b));
    return (uint16_t)((b[0] << 8) | b[1]);
}

static uint32_t vf_u32(vf_t *v, uint32_t at)
{
    uint8_t b[4] = {0};
    vf_read(v, at, b, sizeof(b));
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static bool vf_take(vf_t *v, uint32_t *at, uint32_t length, uint32_t end)
{
    if (!vf_range(*at, length, end)) return vf_fail(v, -RT_EINVAL);
    *at += length;
    return !v->error;
}

static bool vf_directory(vf_t *v)
{
    uint32_t version = vf_u32(v, 0);
    if (version != 0x00010000 && version != VF_TAG('t','r','u','e')) return false;
    unsigned count = vf_u16(v, 4);
    if (!count || count > VF_TABLES || !vf_range(12, count * 16, v->size)) return false;
    for (unsigned i = 0; i < count; ++i)
    {
        vf_table_t *t = &v->tables[i];
        uint32_t at = 12 + i * 16;
        t->tag = vf_u32(v, at);
        t->offset = vf_u32(v, at + 8);
        t->length = vf_u32(v, at + 12);
        if (v->error || t->offset < 12 + count * 16 || (t->offset & 3) ||
            !vf_range(t->offset, t->length, v->size)) return false;
        for (unsigned j = 0; j < i; ++j)
        {
            vf_table_t *q = &v->tables[j];
            if (q->tag == t->tag || (t->length && q->length &&
                t->offset < q->offset + q->length && q->offset < t->offset + t->length)) return false;
        }
        switch (t->tag)
        {
        case VF_TAG('h','e','a','d'): v->head = *t; break;
        case VF_TAG('h','h','e','a'): v->hhea = *t; break;
        case VF_TAG('h','m','t','x'): v->hmtx = *t; break;
        case VF_TAG('m','a','x','p'): v->maxp = *t; break;
        case VF_TAG('c','m','a','p'): v->cmap = *t; break;
        case VF_TAG('l','o','c','a'): v->loca = *t; break;
        case VF_TAG('g','l','y','f'): v->glyf = *t; break;
        case VF_TAG('n','a','m','e'): v->name = *t; break;
        case VF_TAG('C','F','F',' '): case VF_TAG('C','F','F','2'):
        case VF_TAG('f','v','a','r'): case VF_TAG('g','v','a','r'):
        case VF_TAG('a','v','a','r'): case VF_TAG('c','v','a','r'):
        case VF_TAG('H','V','A','R'): case VF_TAG('V','V','A','R'):
        case VF_TAG('M','V','A','R'): case VF_TAG('V','A','R','C'):
        case VF_TAG('C','O','L','R'): case VF_TAG('C','P','A','L'):
        case VF_TAG('C','B','D','T'): case VF_TAG('C','B','L','C'):
        case VF_TAG('s','b','i','x'): case VF_TAG('S','V','G',' '): return false;
        default: break;
        }
    }
    if (v->head.length < 54 || v->hhea.length < 36 || v->maxp.length < 32 ||
        v->cmap.length < 4 || !v->glyf.offset || v->name.length < 6) return false;
    if (vf_u32(v, v->head.offset) != 0x10000 || vf_u32(v, v->maxp.offset) != 0x10000 ||
        vf_u32(v, v->head.offset + 12) != 0x5f0f3cf5 || vf_u16(v, v->head.offset + 52)) return false;
    unsigned units = vf_u16(v, v->head.offset + 18);
    v->glyphs = vf_u16(v, v->maxp.offset + 4);
    v->loca_format = vf_u16(v, v->head.offset + 50);
    unsigned metrics = vf_u16(v, v->hhea.offset + 34);
    if (units < 16 || units > 16384 || !v->glyphs || v->loca_format > 1 ||
        !metrics || metrics > v->glyphs || vf_u16(v, v->hhea.offset + 32) ||
        v->hmtx.length < metrics * 4 + (v->glyphs - metrics) * 2 ||
        v->loca.length < ((uint32_t)v->glyphs + 1) * (v->loca_format ? 4 : 2)) return false;
    int ascent = (int16_t)vf_u16(v, v->hhea.offset + 4);
    int descent = (int16_t)vf_u16(v, v->hhea.offset + 6);
    int gap = (int16_t)vf_u16(v, v->hhea.offset + 8);
    return !v->error && ascent > descent && ascent - descent + gap > 0;
}

static uint32_t vf_location(vf_t *v, unsigned glyph)
{
    return v->loca_format ? vf_u32(v, v->loca.offset + glyph * 4) :
                           (uint32_t)vf_u16(v, v->loca.offset + glyph * 2) * 2;
}

static bool vf_simple(vf_t *v, uint32_t at, uint32_t end, unsigned contours,
                      uint32_t *points_total, uint32_t *contours_total)
{
    if (contours > VF_CONTOURS || !vf_range(at, contours * 2 + 2, end)) return false;
    unsigned points = 0;
    for (unsigned i = 0; i < contours; ++i)
    {
        unsigned next = (unsigned)vf_u16(v, at + i * 2) + 1;
        if (next <= points || next > VF_POINTS) return false;
        points = next;
    }
    at += contours * 2;
    unsigned instructions = vf_u16(v, at);
    at += 2;
    if (!vf_take(v, &at, instructions, end)) return false;
    for (unsigned i = 0; i < points;)
    {
        uint8_t flag = 0, repeat = 0;
        if (!vf_range(at, 1, end) || !vf_read(v, at++, &flag, 1) || (flag & 0x80)) return false;
        if (flag & 8)
            if (!vf_range(at, 1, end) || !vf_read(v, at++, &repeat, 1)) return false;
        if ((unsigned)repeat + 1 > points - i) return false;
        do { v->flags[i++] = flag; } while (repeat--);
    }
    for (unsigned axis = 0; axis < 2; ++axis)
    {
        int coordinate = 0;
        unsigned short_bit = axis ? 4 : 2, same_bit = axis ? 32 : 16;
        for (unsigned i = 0; i < points; ++i)
        {
            uint8_t flag = v->flags[i], delta = 0;
            if (flag & short_bit)
            {
                if (!vf_range(at, 1, end) || !vf_read(v, at++, &delta, 1)) return false;
                coordinate += (flag & same_bit) ? delta : -(int)delta;
            }
            else if (!(flag & same_bit))
            {
                if (!vf_range(at, 2, end)) return false;
                coordinate += (int16_t)vf_u16(v, at); at += 2;
            }
            if (coordinate < -32768 || coordinate > 32767 || v->error) return false;
        }
    }
    *points_total += points;
    *contours_total += contours;
    return *points_total <= VF_POINTS && *contours_total <= VF_CONTOURS;
}

static bool vf_glyph(vf_t *v, unsigned glyph, unsigned depth, unsigned *visits,
                     uint32_t *points, uint32_t *contours)
{
    if (!vf_tick(v) || glyph >= v->glyphs || depth >= VF_DEPTH || ++*visits > VF_COMPONENTS) return false;
    for (unsigned i = 0; i < depth; ++i) if (v->ancestors[i] == glyph) return false;
    v->ancestors[depth] = (uint16_t)glyph;
    uint32_t first = vf_location(v, glyph), last = vf_location(v, glyph + 1);
    if (first > last || last > v->glyf.length || v->error) return false;
    if (first == last) return true;
    uint32_t at = v->glyf.offset + first, end = v->glyf.offset + last;
    if (!vf_range(at, 10, end)) return false;
    int count = (int16_t)vf_u16(v, at);
    if ((int16_t)vf_u16(v, at + 2) > (int16_t)vf_u16(v, at + 6) ||
        (int16_t)vf_u16(v, at + 4) > (int16_t)vf_u16(v, at + 8)) return false;
    at += 10;
    if (count >= 0)
    {
        if (!count && at == end) return true;
        return vf_simple(v, at, end, (unsigned)count, points, contours);
    }
    if (count != -1) return false;
    unsigned flags;
    do
    {
        if (!vf_range(at, 4, end)) return false;
        flags = vf_u16(v, at);
        unsigned child = vf_u16(v, at + 2);
        at += 4;
        /* TinyTTF supports XY offsets, but not matching-point attachment. */
        if (!(flags & 2) || (flags & ~0x1fefu) || ((flags & 0x1800) == 0x1800) ||
            ((flags & 0x100) && (flags & 0x20))) return false;
        unsigned transform = flags & (8 | 64 | 128);
        if (transform && transform != 8 && transform != 64 && transform != 128) return false;
        unsigned bytes = ((flags & 1) ? 4 : 2) + (transform == 8 ? 2 : transform == 64 ? 4 : transform == 128 ? 8 : 0);
        if (!vf_take(v, &at, bytes, end) || !vf_glyph(v, child, depth + 1, visits, points, contours)) return false;
    } while (flags & 0x20);
    if (flags & 0x100)
    {
        if (!vf_range(at, 2, end)) return false;
        unsigned bytes = vf_u16(v, at); at += 2;
        if (!vf_take(v, &at, bytes, end)) return false;
    }
    return !v->error;
}

static bool vf_cmap_subtable(vf_t *v, uint32_t at, uint32_t length, unsigned format)
{
    uint32_t end = at + length;
    if (format == 0)
    {
        uint8_t glyphs[256];
        if (length != 262 || !vf_read(v, at + 6, glyphs, sizeof(glyphs))) return false;
        for (unsigned i = 0; i < 256; ++i) if (glyphs[i] >= v->glyphs) return false;
    }
    else if (format == 6)
    {
        if (length < 10) return false;
        unsigned first = vf_u16(v, at + 6), count = vf_u16(v, at + 8);
        if (count > 65536u - first || length < 10 + count * 2) return false;
        for (unsigned i = 0; i < count; ++i) if (vf_u16(v, at + 10 + i * 2) >= v->glyphs || v->error) return false;
    }
    else if (format == 4)
    {
        if (length < 24) return false;
        unsigned twice = vf_u16(v, at + 6), n = twice / 2, power = 1, selector = 0;
        if (!n || (twice & 1) || length < 16 + n * 8) return false;
        while (power * 2 <= n) { power *= 2; ++selector; }
        if (vf_u16(v, at + 8) != power * 2 || vf_u16(v, at + 10) != selector ||
            vf_u16(v, at + 12) != twice - power * 2 || vf_u16(v, at + 14 + n * 2)) return false;
        uint32_t previous = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            unsigned last = vf_u16(v, at + 14 + i * 2);
            unsigned first = vf_u16(v, at + 16 + n * 2 + i * 2);
            unsigned delta = vf_u16(v, at + 16 + n * 4 + i * 2);
            uint32_t word = at + 16 + n * 6 + i * 2;
            unsigned offset = vf_u16(v, word);
            if (first > last || (i && first <= previous) || (offset & 1)) return false;
            if (offset && (word + offset < at + 16 + n * 8 ||
                !vf_range(word + offset, ((uint32_t)last - first + 1) * 2, end))) return false;
            for (uint32_t cp = first; cp <= last; ++cp)
            {
                unsigned glyph = offset ? vf_u16(v, word + offset + (cp - first) * 2) : cp;
                if (!offset || glyph) glyph = (glyph + delta) & 0xffff;
                if (glyph >= v->glyphs || v->error) return false;
            }
            previous = last;
        }
        if (previous != 0xffff) return false;
    }
    else if (format == 12 || format == 13)
    {
        if (length < 16 || vf_u16(v, at + 2)) return false;
        uint32_t count = vf_u32(v, at + 12), previous = 0;
        if (count > (length - 16) / 12 || count > 65536) return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t record = at + 16 + i * 12;
            uint32_t first = vf_u32(v, record), last = vf_u32(v, record + 4), glyph = vf_u32(v, record + 8);
            if (first > last || last > 0x10ffff || (i && first <= previous) || glyph >= v->glyphs ||
                (format == 12 && last - first >= v->glyphs - glyph) || v->error) return false;
            previous = last;
        }
    }
    else return false;
    return !v->error;
}

static uint32_t vf_u24(vf_t *v, uint32_t at)
{
    uint8_t b[3] = {0};
    vf_read(v, at, b, sizeof(b));
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}

/* Variation-selector mappings are optional; TinyTTF uses the base cmap. */
static bool vf_cmap_variations(vf_t *v, uint32_t at, uint32_t length)
{
    if (length < 10) return false;
    uint32_t count = vf_u32(v, at + 6), previous = 0;
    if (count > 256 || count > (length - 10) / 11) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t record = at + 10 + i * 11, selector = vf_u24(v, record);
        if ((i && selector <= previous) ||
            !((selector >= 0xfe00 && selector <= 0xfe0f) || (selector >= 0xe0100 && selector <= 0xe01ef))) return false;
        previous = selector;
        for (unsigned kind = 0; kind < 2; ++kind)
        {
            uint32_t offset = vf_u32(v, record + 3 + kind * 4);
            if (!offset) continue;
            if (offset < 10 + count * 11 || !vf_range(offset, 4, length)) return false;
            uint32_t n = vf_u32(v, at + offset), step = kind ? 5 : 4, last = 0;
            if (n > 0x110000 || n > (length - offset - 4) / step) return false;
            for (uint32_t j = 0; j < n; ++j)
            {
                uint32_t entry = at + offset + 4 + j * step, cp = vf_u24(v, entry), end = cp;
                if (kind)
                {
                    if (vf_u16(v, entry + 3) >= v->glyphs) return false;
                }
                else
                {
                    uint8_t extra = 0;
                    vf_read(v, entry + 3, &extra, 1); end += extra;
                }
                if ((j && cp <= last) || end > 0x10ffff || v->error) return false;
                last = end;
            }
        }
    }
    return !v->error;
}

static bool vf_cmap(vf_t *v)
{
    uint32_t base = v->cmap.offset;
    unsigned count = vf_u16(v, base + 2);
    if (vf_u16(v, base) || !count || count > 64 || v->cmap.length < 4 + count * 8) return false;
    bool usable = false;
    for (unsigned i = 0; i < count; ++i)
    {
        uint32_t record = base + 4 + i * 8, offset = vf_u32(v, record + 4);
        unsigned platform = vf_u16(v, record), encoding = vf_u16(v, record + 2);
        if (offset < 4 + count * 8 || !vf_range(offset, 4, v->cmap.length)) return false;
        unsigned format = vf_u16(v, base + offset);
        uint32_t length;
        if (format >= 8 && format <= 13)
        {
            if (!vf_range(offset, 8, v->cmap.length)) return false;
            length = vf_u32(v, base + offset + 4);
        }
        else if (format == 14)
        {
            if (!vf_range(offset, 6, v->cmap.length)) return false;
            length = vf_u32(v, base + offset + 2);
        }
        else length = vf_u16(v, base + offset + 2);
        if (length < 4 || !vf_range(offset, length, v->cmap.length)) return false;
        if (platform == 0 && encoding == 5 && format == 14)
        {
            if (!vf_cmap_variations(v, base + offset, length)) return false;
            continue;
        }
        if (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10)))
        {
            if (!vf_cmap_subtable(v, base + offset, length, format)) return false;
            usable = true;
        }
    }
    return usable && !v->error;
}

static bool vf_name_decode(vf_t *v, uint32_t at, unsigned length, bool unicode, char *out, size_t capacity)
{
    size_t used = 0;
    bool full = false;
    if (unicode && (length & 1)) return false;
    for (unsigned i = 0; i < length;)
    {
        uint32_t cp;
        if (unicode)
        {
            cp = vf_u16(v, at + i); i += 2;
            if (cp >= 0xd800 && cp <= 0xdbff)
            {
                if (i == length) return false;
                unsigned low = vf_u16(v, at + i); i += 2;
                if (low < 0xdc00 || low > 0xdfff) return false;
                cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            }
            else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
        }
        else
        {
            uint8_t b = 0;
            vf_read(v, at + i++, &b, 1); cp = b;
            if (cp >= 0x80) return false; /* Only ASCII from legacy Mac records. */
        }
        if (v->error || cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return false;
        unsigned n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (full || used + n >= capacity) { full = true; continue; }
        if (n == 1) out[used++] = (char)cp;
        else
        {
            out[used++] = (char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | (cp >> (6 * (n - 1))));
            for (unsigned j = n - 1; j; --j) out[used++] = (char)(0x80 | ((cp >> (6 * (j - 1))) & 0x3f));
        }
    }
    out[used] = 0;
    return used != 0;
}

static bool vf_name(vf_t *v, char *name)
{
    uint32_t base = v->name.offset, size = v->name.length;
    unsigned format = vf_u16(v, base), count = vf_u16(v, base + 2), strings = vf_u16(v, base + 4);
    if (format > 1 || count > 4096 || !vf_range(6, count * 12, size) || strings < 6 + count * 12 || strings > size) return false;
    if (format == 1)
    {
        uint32_t at = 6 + count * 12;
        if (!vf_range(at, 2, strings)) return false;
        unsigned languages = vf_u16(v, base + at); at += 2;
        if (!vf_range(at, languages * 4, strings)) return false;
        for (unsigned i = 0; i < languages; ++i)
            if (!vf_range(vf_u16(v, base + at + i * 4 + 2), vf_u16(v, base + at + i * 4), size - strings)) return false;
    }
    char family[READING_FONT_NAME_MAX] = {0}, style[READING_FONT_NAME_MAX] = {0};
    int scores[2] = {-1, -1};
    for (unsigned i = 0; i < count; ++i)
    {
        uint32_t at = base + 6 + i * 12;
        unsigned platform = vf_u16(v, at), encoding = vf_u16(v, at + 2), language = vf_u16(v, at + 4);
        unsigned id = vf_u16(v, at + 6), length = vf_u16(v, at + 8), offset = vf_u16(v, at + 10);
        if (!vf_range(offset, length, size - strings) || v->error) return false;
        if (id != 1 && id != 2 && id != 16 && id != 17) continue;
        bool unicode = platform == 0 || (platform == 3 && (encoding == 0 || encoding == 1 || encoding == 10));
        if (!unicode && !(platform == 1 && encoding == 0)) continue;
        unsigned slot = (id == 2 || id == 17);
        int score = (id >= 16 ? 16 : 0) + (unicode ? 8 : 0) + (language == 0x804 ? 4 : language == 0x409 ? 2 : 0);
        if (score <= scores[slot]) continue;
        char candidate[READING_FONT_NAME_MAX];
        if (vf_name_decode(v, base + strings + offset, length, unicode, candidate, sizeof(candidate)))
        {
            strcpy(slot ? style : family, candidate); scores[slot] = score;
        }
    }
    if (v->error) return false;
    if (!family[0]) strcpy(family, "TrueType");
    strcpy(name, family);
    size_t used = strlen(name), n = strlen(style);
    if (n && used + n + 1 < READING_FONT_NAME_MAX)
    { name[used++] = ' '; memcpy(name + used, style, n + 1); }
    return true;
}

rt_err_t reading_font_validate(const char *path, reading_font_info_t *info,
                               reading_font_cancel_t cancelled, void *context)
{
    if (!path || !info) return -RT_EINVAL;
    vf_t *v = epd_app_alloc(sizeof(*v), EPD_APP_PSRAM);
    if (!v) return -RT_ENOMEM;
    memset(v, 0, sizeof(*v));
    v->cancelled = cancelled; v->context = context;
    v->path = path;
    v->volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    v->session = storage_card_session();
    v->revision = storage_path_revision(path);
    storage_lock();
    v->fd = vf_tick(v) && storage_session_valid(v->volume, v->session) &&
        !storage_file_writers(path) && storage_path_revision(path) == v->revision ? open(path, O_RDONLY, 0) : -1;
    storage_unlock();
    if (v->fd < 0)
    {
        rt_err_t error = v->error ? v->error : -RT_EIO;
        epd_app_free(v); return error;
    }
    struct stat st;
    bool ok = vf_stat(v, &st) && st.st_size >= 12 && (uint64_t)st.st_size <= READING_FONT_FILE_MAX;
    char name[READING_FONT_NAME_MAX] = {0};
    uint32_t signature = 2166136261u;
    if (ok)
    {
        v->size = (uint32_t)st.st_size;
        ok = vf_directory(v) && vf_cmap(v);
    }
    for (unsigned i = 0; ok && i < v->glyphs; ++i)
    {
        unsigned visits = 0;
        uint32_t points = 0, contours = 0;
        ok = vf_glyph(v, i, 0, &visits, &points, &contours);
    }
    if (ok) ok = vf_name(v, name);
    /* Read every byte again, including padding and tables unused by the renderer. */
    v->cached_size = 0;
    for (uint32_t at = 0; ok && at < v->size;)
    {
        if (!vf_tick(v)) { ok = false; break; }
        uint32_t n = v->size - at;
        if (n > sizeof(v->cache)) n = sizeof(v->cache);
        if (!vf_block(v, at, v->cache, n)) { ok = false; break; }
        for (uint32_t i = 0; i < n; ++i) signature = (signature ^ v->cache[i]) * 16777619u;
        at += n;
    }
    if (ok && (!vf_stat(v, &st) || (uint64_t)st.st_size != v->size)) ok = vf_fail(v, -RT_EIO);
    storage_lock();
    if (v->cancelled && v->cancelled(v->context)) ok = vf_fail(v, -RT_EINTR);
    if (!storage_session_valid(v->volume, v->session) || storage_file_writers(path) ||
        storage_path_revision(path) != v->revision) ok = vf_fail(v, -RT_EIO);
    if (close(v->fd) != 0) ok = vf_fail(v, -RT_EIO);
    storage_unlock();
    rt_err_t result = ok && !v->error ? RT_EOK : v->error ? v->error : -RT_EINVAL;
    if (result == RT_EOK)
    {
        info->signature = signature; info->size = v->size;
        memcpy(info->name, name, sizeof(info->name));
    }
    epd_app_free(v);
    return result;
}
