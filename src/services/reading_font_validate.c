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
#define VF_CANCEL_WORK 256u
#define VF_CACHE_BYTES 4096u
#define VF_TAG(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

typedef struct { uint32_t tag, offset, length; } vf_table_t;
typedef struct
{
    uint32_t at, size;
    uint8_t bytes[VF_CACHE_BYTES];
} vf_cache_t;
typedef enum
{
    VF_OPEN, VF_DIRECTORY, VF_CMAP, VF_GLYPHS, VF_NAME, VF_SIGNATURE, VF_FINISH, VF_PHASES
} vf_phase_t;
typedef struct
{
    uint64_t elapsed_ticks, read_ticks, lock_ticks, bytes;
    uint32_t read_calls;
} vf_metrics_t;
typedef struct
{
    int fd;
    rt_err_t error;
    uint32_t size, work;
    const char *path;
    uint32_t revision, session, checked_volume_revision;
    storage_volume_t volume;
    reading_font_cancel_t cancelled;
    void *context;
    vf_table_t tables[VF_TABLES], head, hhea, hmtx, maxp, cmap, loca, glyf, name;
    uint16_t glyphs, loca_format, ancestors[VF_DEPTH];
    /* loca and glyf alternate for every glyph, including composite children. */
    vf_cache_t index_cache, data_cache[2];
    unsigned data_recent;
    uint8_t flags[VF_POINTS];
    vf_phase_t phase;
    rt_tick_t phase_started;
    vf_metrics_t metrics[VF_PHASES];
    bool signature_reused;
} vf_t;

static void vf_phase_end(vf_t *v)
{
    v->metrics[v->phase].elapsed_ticks += (rt_tick_t)(rt_tick_get() - v->phase_started);
}

static void vf_phase_begin(vf_t *v, vf_phase_t phase)
{
    vf_phase_end(v);
    v->phase = phase;
    v->phase_started = rt_tick_get();
}

static void vf_lock(vf_t *v)
{
    rt_tick_t start = rt_tick_get();
    storage_lock();
    v->metrics[v->phase].lock_ticks += (rt_tick_t)(rt_tick_get() - start);
}

static unsigned long vf_ms(uint64_t ticks)
{ return (unsigned long)(ticks * 1000u / RT_TICK_PER_SECOND); }

static void vf_log(vf_t *v, rt_err_t result)
{
    static const char *const names[VF_PHASES] =
        {"open", "directory", "cmap", "glyphs", "name", "signature", "finish"};
    vf_metrics_t total = {0};
    for (unsigned i = 0; i < VF_PHASES; ++i)
    {
        vf_metrics_t *m = &v->metrics[i];
        total.elapsed_ticks += m->elapsed_ticks; total.read_ticks += m->read_ticks;
        total.lock_ticks += m->lock_ticks; total.bytes += m->bytes; total.read_calls += m->read_calls;
    }
    rt_kprintf("font validate size=%lu result=%d signature_reused=%u total_ms=%lu\n",
               (unsigned long)v->size, result, (unsigned)v->signature_reused, vf_ms(total.elapsed_ticks));
    for (unsigned i = 0; i <= VF_PHASES; ++i)
    {
        vf_metrics_t *m = i == VF_PHASES ? &total : &v->metrics[i];
        uint64_t io = m->read_ticks + m->lock_ticks;
        /* validation_ms is elapsed work outside seek/read and lock acquisition,
         * including parsing, hashing, metadata checks and scheduler delays. */
        uint64_t validation = m->elapsed_ticks > io ? m->elapsed_ticks - io : 0;
        /* Avoid requiring RT_PRINTF_LONGLONG for large cumulative byte counts. */
        char bytes[21];
        unsigned pos = sizeof(bytes) - 1;
        uint64_t n = m->bytes;
        bytes[pos] = 0;
        do { bytes[--pos] = (char)('0' + n % 10); n /= 10; } while (n);
        rt_kprintf("font validate phase=%s read_ms=%lu lock_wait_ms=%lu validation_ms=%lu\n",
                   i == VF_PHASES ? "total" : names[i], vf_ms(m->read_ticks), vf_ms(m->lock_ticks),
                   vf_ms(validation));
        rt_kprintf("font validate phase=%s bytes=%s read_calls=%lu\n",
                   i == VF_PHASES ? "total" : names[i], bytes + pos, (unsigned long)m->read_calls);
    }
}

static bool vf_fail(vf_t *v, rt_err_t error)
{
    if (!v->error) v->error = error;
    return false;
}

static bool vf_cancel(vf_t *v)
{
    if (v->error) return false;
    if (v->cancelled && v->cancelled(v->context)) return vf_fail(v, -RT_EINTR);
    return true;
}

static bool vf_tick(vf_t *v)
{
    if (v->error) return false;
    if (++v->work > VF_WORK) return vf_fail(v, -RT_EINVAL);
    /* Cached one-byte reads still consume work, but need not poll the service
     * on every coordinate. I/O and final publication force a cancellation check. */
    return ((v->work - 1u) % VF_CANCEL_WORK) || vf_cancel(v);
}

static bool vf_range(uint32_t offset, uint32_t length, uint32_t end)
{ return offset <= end && length <= end - offset; }

/* Caller holds storage_lock(); also check before fstat, never touch a stale fd. */
static bool vf_identity(vf_t *v)
{
    if (!storage_session_valid(v->volume, v->session)) return vf_fail(v, -RT_EIO);
    uint32_t revision = storage_volume_revision(v->volume);
    /* The open fd pins the path token. Every write/open-for-write/rename bumps
     * the volume revision; unchanged volumes need no FAT path normalization. */
    if (revision && revision == v->checked_volume_revision) return true;
    if (storage_file_writers(v->path) || storage_path_revision(v->path) != v->revision)
        return vf_fail(v, -RT_EIO);
    v->checked_volume_revision = revision;
    return true;
}

static bool vf_block(vf_t *v, uint32_t at, void *out, uint32_t n)
{
    vf_lock(v);
    bool ok = vf_tick(v) && vf_cancel(v) && vf_identity(v);
    if (ok)
    {
        vf_metrics_t *m = &v->metrics[v->phase];
        rt_tick_t start = rt_tick_get();
        int got = -1;
        if (lseek(v->fd, (off_t)at, SEEK_SET) == (off_t)at)
        {
            ++m->read_calls;
            got = read(v->fd, out, n);
            if (got > 0) m->bytes += (unsigned)got;
        }
        m->read_ticks += (rt_tick_t)(rt_tick_get() - start);
        if (got != (int)n) ok = vf_fail(v, -RT_EIO);
    }
    storage_unlock();
    return ok;
}

static bool vf_stat(vf_t *v, struct stat *st)
{
    vf_lock(v);
    bool ok = vf_tick(v) && vf_cancel(v) && vf_identity(v);
    if (ok && fstat(v->fd, st) != 0) ok = vf_fail(v, -RT_EIO);
    storage_unlock();
    return ok;
}

static bool vf_cached(const vf_cache_t *cache, uint32_t at)
{ return at >= cache->at && at - cache->at < cache->size; }

static bool vf_read(vf_t *v, uint32_t at, void *out, uint32_t size)
{
    uint8_t *p = out;
    if (!vf_tick(v)) return false;
    if (!vf_range(at, size, v->size)) return vf_fail(v, -RT_EINVAL);
    while (size)
    {
        vf_cache_t *cache;
        if (at >= v->loca.offset && at - v->loca.offset < v->loca.length)
            cache = &v->index_cache;
        else
        {
            unsigned slot;
            if (vf_cached(&v->data_cache[0], at)) slot = 0;
            else if (vf_cached(&v->data_cache[1], at)) slot = 1;
            else slot = 1u - v->data_recent;
            v->data_recent = slot;
            cache = &v->data_cache[slot];
        }
        if (!vf_cached(cache, at))
        {
            uint32_t base = at - at % VF_CACHE_BYTES;
            uint32_t n = v->size - base;
            if (n > sizeof(cache->bytes)) n = sizeof(cache->bytes);
            cache->size = 0;
            if (!vf_block(v, base, cache->bytes, n)) return false;
            cache->at = base;
            cache->size = n;
        }
        uint32_t n = cache->size - (at - cache->at);
        if (n > size) n = size;
        memcpy(p, cache->bytes + at - cache->at, n);
        p += n; at += n; size -= n;
    }
    return true;
}

static uint16_t vf_be16(const uint8_t *b)
{ return (uint16_t)((b[0] << 8) | b[1]); }

static uint16_t vf_u16(vf_t *v, uint32_t at)
{
    uint8_t b[2] = {0};
    vf_read(v, at, b, sizeof(b));
    return vf_be16(b);
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

static bool vf_proof_bytes(vf_t *v, uint32_t at, uint32_t length, uint32_t *hash)
{
    uint8_t bytes[64];
    while (length)
    {
        uint32_t n = length < sizeof(bytes) ? length : sizeof(bytes);
        if (!vf_read(v, at, bytes, n)) return false;
        for (uint32_t i = 0; i < n; ++i) *hash = (*hash ^ bytes[i]) * 16777619u;
        at += n; length -= n;
    }
    return true;
}

static bool vf_install_proof(vf_t *v, uint32_t *proof)
{
    *proof = 2166136261u;
    return vf_proof_bytes(v, 0, 12u + vf_u16(v, 4) * 16u, proof) &&
        vf_proof_bytes(v, v->head.offset, 54, proof) &&
        vf_proof_bytes(v, v->hhea.offset, 36, proof) &&
        vf_proof_bytes(v, v->maxp.offset, 32, proof);
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
        /* Four parallel segment arrays otherwise evict each other on every
         * segment. A bounded batch preserves the same checks with sequential I/O. */
        uint8_t segments[4][64];
        for (unsigned i = 0; i < n; ++i)
        {
            unsigned slot = i % 32;
            if (!slot)
            {
                unsigned count = n - i;
                if (count > 32) count = 32;
                for (unsigned array = 0; array < 4; ++array)
                {
                    uint32_t base = at + (array ? 16 + n * array * 2 : 14) + i * 2;
                    if (!vf_read(v, base, segments[array], count * 2)) return false;
                }
            }
            unsigned last = vf_be16(segments[0] + slot * 2);
            unsigned first = vf_be16(segments[1] + slot * 2);
            unsigned delta = vf_be16(segments[2] + slot * 2);
            uint32_t word = at + 16 + n * 6 + i * 2;
            unsigned offset = vf_be16(segments[3] + slot * 2);
            if (first > last || (i && first <= previous) || (offset & 1)) return false;
            if (offset && (word + offset < at + 16 + n * 8 ||
                !vf_range(word + offset, ((uint32_t)last - first + 1) * 2, end))) return false;
            for (uint32_t cp = first; cp <= last; ++cp)
            {
                if (!vf_tick(v)) return false;
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

static rt_err_t vf_check(const char *path, reading_font_info_t *info,
                                             const reading_font_signature_t *binding,
                                             reading_font_cancel_t cancelled, void *context,
                                             bool quick, uint32_t expected_proof, uint32_t *proof_out)
{
    if (!path || !info) return -RT_EINVAL;
    vf_t *v = epd_app_alloc(sizeof(*v), EPD_APP_PSRAM);
    if (!v) return -RT_ENOMEM;
    memset(v, 0, sizeof(*v));
    v->phase_started = rt_tick_get();
    v->cancelled = cancelled; v->context = context;
    v->path = path;
    v->volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    vf_lock(v);
    v->session = v->volume == STORAGE_SD ? storage_card_session() : 0;
    v->revision = storage_path_revision(path);
    v->fd = vf_tick(v) && vf_identity(v) ? open(path, O_RDONLY, 0) : -1;
    struct stat st;
    bool ok = v->fd >= 0 && vf_tick(v) && vf_cancel(v) && vf_identity(v);
    if (ok && fstat(v->fd, &st) != 0) ok = vf_fail(v, -RT_EIO);
    ok = ok && S_ISREG(st.st_mode) && (!quick || (uint64_t)st.st_size == info->size) &&
        st.st_size >= 12 && (uint64_t)st.st_size <= READING_FONT_FILE_MAX;
    uint32_t signature = 2166136261u;
    if (ok)
    {
        v->size = (uint32_t)st.st_size;
        v->signature_reused = binding && binding->path && !strcmp(binding->path, path) &&
            binding->size == v->size && binding->path_revision == v->revision &&
            binding->storage_session == v->session;
        if (v->signature_reused) signature = binding->signature;
    }
    storage_unlock();
    if (v->fd < 0)
    {
        rt_err_t error = v->error ? v->error : -RT_EIO;
        vf_phase_end(v);
        vf_log(v, error);
        epd_app_free(v); return error;
    }
    uint32_t proof = 0;
    char name[READING_FONT_NAME_MAX] = {0};
    vf_phase_begin(v, VF_DIRECTORY);
    if (ok) ok = vf_directory(v) && vf_install_proof(v, &proof);
    if (ok && quick) ok = proof == expected_proof;
    if (quick) { signature = info->signature; v->signature_reused = true; }
    vf_phase_begin(v, VF_CMAP);
    if (ok && !quick) ok = vf_cmap(v);
    vf_phase_begin(v, VF_GLYPHS);
    for (unsigned i = 0; ok && !quick && i < v->glyphs; ++i)
    {
        unsigned visits = 0;
        uint32_t points = 0, contours = 0;
        ok = vf_glyph(v, i, 0, &visits, &points, &contours);
    }
    vf_phase_begin(v, VF_NAME);
    if (ok && !quick) ok = vf_name(v, name);
    vf_phase_begin(v, VF_SIGNATURE);
    /* Without a matching producer binding, hash padding and unused tables too. */
    uint8_t *buffer = v->data_cache[0].bytes;
    for (uint32_t at = 0; ok && !v->signature_reused && at < v->size;)
    {
        if (!vf_tick(v)) { ok = false; break; }
        uint32_t n = v->size - at;
        if (n > VF_CACHE_BYTES) n = VF_CACHE_BYTES;
        if (!vf_block(v, at, buffer, n)) { ok = false; break; }
        for (uint32_t i = 0; i < n; ++i) signature = (signature ^ buffer[i]) * 16777619u;
        at += n;
    }
    vf_phase_begin(v, VF_FINISH);
    if (ok && (!vf_stat(v, &st) || (uint64_t)st.st_size != v->size)) ok = vf_fail(v, -RT_EIO);
    vf_lock(v);
    if (v->cancelled && v->cancelled(v->context)) ok = vf_fail(v, -RT_EINTR);
    if (!vf_identity(v)) ok = false;
    if (close(v->fd) != 0) ok = vf_fail(v, -RT_EIO);
    storage_unlock();
    rt_err_t result = ok && !v->error ? RT_EOK : v->error ? v->error : -RT_EINVAL;
    if (result == RT_EOK && proof_out) *proof_out = proof;
    if (result == RT_EOK && !quick)
    {
        info->signature = signature; info->size = v->size;
        memcpy(info->name, name, sizeof(info->name));
    }
    vf_phase_end(v);
    vf_log(v, result);
    epd_app_free(v);
    return result;
}

rt_err_t reading_font_validate(const char *path, reading_font_info_t *info,
                               reading_font_cancel_t cancelled, void *context)
{
    return vf_check(path, info, NULL, cancelled, context, false, 0, NULL);
}

rt_err_t reading_font_validate_with_signature(const char *path, reading_font_info_t *info,
    const reading_font_signature_t *binding, reading_font_cancel_t cancelled, void *context)
{ return vf_check(path, info, binding, cancelled, context, false, 0, NULL); }

rt_err_t reading_font_validate_install(const char *path, reading_font_info_t *info,
    const reading_font_signature_t *binding, uint32_t *proof,
    reading_font_cancel_t cancelled, void *context)
{ return vf_check(path, info, binding, cancelled, context, false, 0, proof); }

rt_err_t reading_font_probe_installed(const char *path, reading_font_info_t *info,
    uint32_t proof, reading_font_cancel_t cancelled, void *context)
{ return vf_check(path, info, NULL, cancelled, context, true, proof, NULL); }
