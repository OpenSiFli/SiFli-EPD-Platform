#include "font_ftz.h"
#include "font_diagnostics.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define tinfl_decompress crf_tinfl_decompress
#include "../../src/thirdparty/miniz/miniz.h"
#include <string.h>

#define BLOCK_BYTES 32768u
#define MIN_BLOCK_BYTES 16384u
/* READ transfers contain raw bytes and can use the full 16 KiB pipe payload. */
#define INPUT_BYTES 16384u
/* The complete bounded workspace is charged by crf_ftz_bytes(). */
#define BLOCK_SLOTS 16u
#define OUTPUT_BYTES (BLOCK_SLOTS * BLOCK_BYTES)
#define MAX_SLOTS (OUTPUT_BYTES / MIN_BLOCK_BYTES)
_Static_assert(CRF_MAX_FILE_BYTES / MIN_BLOCK_BYTES < UINT16_MAX, "FTZ block tag capacity");
struct crf_ftz {
    crf_ftz_stats stats;
    crf_ftz_read_fn read;
    crf_ftz_cancel_fn cancelled;
    void *user;
    uint32_t packed, length, font_bytes, count, block_bytes;
    uint16_t cached[MAX_SLOTS];
    uint8_t recent[MAX_SLOTS], block_shift, active;
    uint32_t offsets[CRF_MAX_FILE_BYTES / MIN_BLOCK_BYTES + 1u];
    tinfl_decompressor inflater;
    uint8_t input[INPUT_BYTES], output[OUTPUT_BYTES];
};
static unsigned slots(const crf_ftz *s) { return OUTPUT_BYTES >> s->block_shift; }
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
size_t crf_ftz_bytes(void) { return sizeof(crf_ftz); }
static crf_status fetch(crf_ftz *s, uint32_t offset, void *out, size_t bytes)
{
    if (s->cancelled(s->user)) return CRF_CANCELLED;
    if (offset > s->packed || bytes > s->packed - offset) return CRF_INVALID_FONT;
    ++s->stats.input_calls;
    crf_status result = s->read(s->user, offset, out, bytes);
    if (result == CRF_OK) s->stats.input_bytes += bytes;
    return result;
}
crf_status crf_ftz_open(crf_ftz *s, uint32_t packed, uint32_t font_bytes,
                      crf_ftz_read_fn read, crf_ftz_cancel_fn cancelled, void *user)
{
    memset(s, 0, sizeof(*s)); s->packed = packed; s->font_bytes = font_bytes;
    s->read = read; s->cancelled = cancelled; s->user = user;
    for (unsigned i = 0; i < MAX_SLOTS; ++i) {
        s->cached[i] = UINT16_MAX; s->recent[i] = (uint8_t)i;
    }
    crf_status status = fetch(s, 0, s->input, 20);
    if (status != CRF_OK) return status;
    s->length = u32(s->input + 4); s->count = u32(s->input + 12);
    s->block_bytes = u32(s->input + 8);
    s->block_shift = s->block_bytes == MIN_BLOCK_BYTES ? 14u : 15u;
    if (memcmp(s->input, "FTZ1", 4) || !s->length || s->length > CRF_MAX_FILE_BYTES ||
        font_bytes > s->length || (s->block_bytes != MIN_BLOCK_BYTES && s->block_bytes != BLOCK_BYTES) ||
        s->count != ((s->length - 1) >> s->block_shift) + 1 || s->count > (packed - 20) / 4 ||
        u32(s->input + 16) != 20 + s->count * 4) return CRF_INVALID_FONT;
    uint32_t previous = 20 + s->count * 4;
    s->offsets[0] = previous;
    for (uint32_t i = 1; i <= s->count;) {
        uint32_t n = s->count - i + 1;
        if (n > INPUT_BYTES / 4) n = INPUT_BYTES / 4;
        status = fetch(s, 16 + i * 4, s->input, n * 4);
        if (status != CRF_OK) return status;
        for (uint32_t j = 0; j < n; ++j) {
            uint32_t end = u32(s->input + j * 4);
            if (end <= previous || end > packed) return CRF_INVALID_FONT;
            previous = end; s->offsets[i + j] = end;
        }
        i += n;
    }
    return previous == packed ? CRF_OK : CRF_INVALID_FONT;
}
static void promote(crf_ftz *s, unsigned slot)
{
    for (unsigned i = 0; i < slots(s); ++i) if (s->recent[i] == slot) {
        memmove(s->recent + 1, s->recent, i);
        s->recent[0] = (uint8_t)slot; break;
    }
    s->active = (uint8_t)slot;
}
static crf_status block(crf_ftz *s, uint32_t index)
{
    for (unsigned i = 0; i < slots(s); ++i) if (s->cached[i] == index) {
        promote(s, i); ++s->stats.block_hits; return CRF_OK;
    }
    ++s->stats.block_misses;
    unsigned slot = s->recent[slots(s) - 1];
    for (unsigned i = 0; i < slots(s); ++i) if (s->cached[i] == UINT16_MAX) { slot = i; break; }
    /* A cancelled or corrupt replacement must not publish partial output or
     * invalidate the other complete blocks. */
    s->cached[slot] = UINT16_MAX;
    crf_status status;
    /* Open validated every extent; retain the index across block evictions. */
    uint32_t position = s->offsets[index], end = s->offsets[index + 1u];
    uint32_t expected = s->length - (index << s->block_shift);
    if (expected > s->block_bytes) expected = s->block_bytes;
    if (position < 20 + s->count * 4 || position >= end || end > s->packed) return CRF_INVALID_FONT;
    size_t produced = 0;
    tinfl_init(&s->inflater);
    while (position < end) {
        uint32_t take = end - position;
        if (take > INPUT_BYTES) take = INPUT_BYTES;
        status = fetch(s, position, s->input, take);
        if (status != CRF_OK) return status;
        size_t input = take, output = expected - produced;
        uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                         TINFL_FLAG_COMPUTE_ADLER32;
        if (take < end - position) flags |= TINFL_FLAG_HAS_MORE_INPUT;
        uint32_t started = crf_ticks();
        tinfl_status result = tinfl_decompress(&s->inflater, s->input, &input,
            (s->output + (slot << s->block_shift)), (s->output + (slot << s->block_shift)) + produced, &output, flags);
        uint32_t elapsed = crf_ticks() - started;
        ++s->stats.inflate_calls; s->stats.inflate_ticks += elapsed;
        if (elapsed > s->stats.inflate_max_ticks) s->stats.inflate_max_ticks = elapsed;
        position += (uint32_t)input; produced += output;
        if (result == TINFL_STATUS_DONE) {
            if (s->cancelled(s->user)) return CRF_CANCELLED;
            if (position != end || produced != expected) return CRF_INVALID_FONT;
            ++s->stats.blocks; s->cached[slot] = (uint16_t)index; promote(s, slot); return CRF_OK;
        }
        if (result != TINFL_STATUS_NEEDS_MORE_INPUT || input != take || position == end)
            return CRF_INVALID_FONT;
    }
    return CRF_INVALID_FONT;
}
crf_status crf_ftz_read(crf_ftz *s, uint32_t offset, void *out, size_t bytes)
{
    if (offset > s->font_bytes || bytes > s->font_bytes - offset) return CRF_INVALID_FONT;
    while (bytes) {
        if (s->cancelled(s->user)) return CRF_CANCELLED;
        crf_status status = block(s, offset >> s->block_shift);
        if (status != CRF_OK) { ++s->stats.errors; return status; }
        uint32_t within = offset & (s->block_bytes - 1u);
        size_t take = s->block_bytes - within;
        if (take > bytes) take = bytes;
        memcpy(out, (s->output + ((uint32_t)s->active << s->block_shift)) + within, take);
        out = (uint8_t *)out + take; offset += (uint32_t)take; bytes -= take;
    }
    return CRF_OK;
}

void crf_ftz_get_stats(const crf_ftz *s, crf_ftz_stats *out)
{ if (out) { if (s) *out = s->stats; else memset(out, 0, sizeof(*out)); } }
