#include "../../components/compute_reader_font/font_ftz.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#include <signal.h>
static void abort_without_dialog(int value) { (void)value; ExitProcess(3); }
#endif
static uint8_t *packed;
static size_t packed_size;
static unsigned reads, cancel_at, index_reads, max_read;
static crf_status fault;
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static crf_status read_source(void *user, uint32_t offset, void *out, size_t size)
{
    (void)user; ++reads;
    assert(size <= 16384 && offset <= packed_size && size <= packed_size - offset);
    if (size > max_read) max_read = (unsigned)size;
    if (offset < u32(packed + 16)) ++index_reads;
    if (fault) return fault;
    memcpy(out, packed + offset, size); return CRF_OK;
}
static int cancelled(void *user) { (void)user; return cancel_at && reads >= cancel_at; }
static uint8_t *load(const char *name, size_t *size)
{
    FILE *f = fopen(name, "rb"); assert(f); fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); rewind(f);
    uint8_t *bytes = malloc(*size); assert(bytes && fread(bytes, 1, *size, f) == *size); fclose(f); return bytes;
}
int main(int argc, char **argv)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    signal(SIGABRT, abort_without_dialog);
#endif
    assert(argc == 3); size_t raw_size;
    packed = load(argv[1], &packed_size); uint8_t *raw = load(argv[2], &raw_size);
    uint32_t block_bytes = u32(packed + 8);
    assert(block_bytes == 16384u || block_bytes == 32768u);
    unsigned slots = (512u * 1024u) / block_bytes;
    crf_ftz *s = malloc(crf_ftz_bytes()); assert(s);
#define OPEN() crf_ftz_open(s, (uint32_t)packed_size, (uint32_t)raw_size, read_source, cancelled, NULL)
    assert(OPEN() == CRF_OK);
    uint8_t bytes[65536];
    for (size_t offset = 0; offset < raw_size;) {
        size_t n = raw_size - offset; if (n > sizeof(bytes)) n = sizeof(bytes);
        assert(crf_ftz_read(s, (uint32_t)offset, bytes, n) == CRF_OK);
        assert(!memcmp(bytes, raw + offset, n)); offset += n;
    }
    assert(crf_ftz_read(s, block_bytes - 8, bytes, 32) == CRF_OK && !memcmp(bytes, raw + block_bytes - 8, 32));
    unsigned before = reads;
    assert(crf_ftz_read(s, block_bytes, bytes, 8) == CRF_OK && reads == before);
    crf_ftz_stats stats; crf_ftz_get_stats(s, &stats);
    assert(stats.input_calls == reads && stats.input_bytes && stats.block_hits && stats.blocks);
    assert(stats.block_misses == stats.blocks && !stats.errors && stats.inflate_calls >= stats.blocks);
    assert(stats.inflate_ticks >= stats.inflate_max_ticks);
    assert(crf_ftz_read(s, (uint32_t)raw_size - 1, bytes, 2) == CRF_INVALID_FONT);
    /* The same output space holds 32 16 KiB or 16 32 KiB blocks. Hit
     * promotion chooses the LRU victim. The 704 KiB source contract also
     * needs room for the engine, output packet and fiber stack/control. */
    assert(crf_ftz_bytes() >= 512u * 1024u && crf_ftz_bytes() <= 548u * 1024u);
    assert(OPEN() == CRF_OK);
    unsigned indexes = index_reads;
    for (unsigned i = 0; i < slots; ++i)
        assert(crf_ftz_read(s, i * block_bytes, bytes, 16) == CRF_OK && !memcmp(bytes, raw + i * block_bytes, 16));
    before = reads;
    assert(crf_ftz_read(s, 0, bytes, 16) == CRF_OK && reads == before && !memcmp(bytes, raw, 16));
    assert(crf_ftz_read(s, slots * block_bytes, bytes, 16) == CRF_OK && reads > before);
    before = reads;
    for (unsigned i = 2; i < slots; ++i)
        assert(crf_ftz_read(s, i * block_bytes, bytes, 16) == CRF_OK && reads == before && !memcmp(bytes, raw + i * block_bytes, 16));
    assert(crf_ftz_read(s, block_bytes, bytes, 16) == CRF_OK && reads > before && !memcmp(bytes, raw + block_bytes, 16));
    assert(index_reads == indexes && max_read <= 16384);
    if (block_bytes == 32768) assert(max_read == 16384);
    /* A cancelled replacement invalidates only its selected slot. */
    assert(OPEN() == CRF_OK);
    for (unsigned i = 0; i < slots; ++i) assert(crf_ftz_read(s, i * block_bytes, bytes, 16) == CRF_OK);
    cancel_at = reads + 1;
    assert(crf_ftz_read(s, slots * block_bytes, bytes, 16) == CRF_CANCELLED);
    cancel_at = 0; before = reads;
    for (unsigned i = 1; i < slots; ++i)
        assert(crf_ftz_read(s, i * block_bytes, bytes, 16) == CRF_OK && reads == before && !memcmp(bytes, raw + i * block_bytes, 16));
    assert(crf_ftz_read(s, slots * block_bytes, bytes, 16) == CRF_OK && reads > before && !memcmp(bytes, raw + slots * block_bytes, 16));
    /* A checksum failure also leaves the other slots usable and forces the
     * damaged block to be read and verified again after the source is fixed. */
    assert(OPEN() == CRF_OK);
    for (unsigned i = 0; i < slots; ++i) assert(crf_ftz_read(s, i * block_bytes, bytes, 16) == CRF_OK);
    uint32_t corrupt = u32(packed + 16 + (slots + 1u) * 4u) - 1u;
    packed[corrupt] ^= 1;
    assert(crf_ftz_read(s, slots * block_bytes, bytes, 16) == CRF_INVALID_FONT);
    packed[corrupt] ^= 1; before = reads;
    for (unsigned i = 1; i < slots; ++i)
        assert(crf_ftz_read(s, i * block_bytes, bytes, 16) == CRF_OK && reads == before && !memcmp(bytes, raw + i * block_bytes, 16));
    assert(crf_ftz_read(s, slots * block_bytes, bytes, 16) == CRF_OK && reads > before && !memcmp(bytes, raw + slots * block_bytes, 16));
    uint32_t large = UINT32_MAX;
    for (uint32_t i = 0; i < u32(packed + 12); ++i)
        if (u32(packed + 20 + i * 4) - u32(packed + 16 + i * 4) > 16384) { large = i; break; }
    if (block_bytes == 32768) assert(large != UINT32_MAX);
    if (large != UINT32_MAX) {
        assert(large * block_bytes < raw_size);
        assert(OPEN() == CRF_OK); cancel_at = reads + 1;
        assert(crf_ftz_read(s, large * block_bytes, bytes, 16) == CRF_CANCELLED);
        cancel_at = 0;
        assert(crf_ftz_read(s, large * block_bytes, bytes, 16) == CRF_OK && !memcmp(bytes, raw + large * block_bytes, 16));
        assert(max_read == 16384);
    }
    const unsigned malformed[] = {0, 8, 12, 16};
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(*malformed); ++i) {
        unsigned at = malformed[i]; uint8_t saved = packed[at]; packed[at] = at == 20 ? 0 : (uint8_t)(saved ^ 0xff);
        assert(OPEN() == CRF_INVALID_FONT); packed[at] = saved;
    }
    uint32_t tail = u32(packed + 20) - 1; packed[tail] ^= 1;
    assert(OPEN() == CRF_OK && crf_ftz_read(s, 0, bytes, 16) == CRF_INVALID_FONT);
    packed[tail] ^= 1;
    assert(OPEN() == CRF_OK); fault = CRF_SHORT_READ;
    assert(crf_ftz_read(s, 0, bytes, 16) == CRF_SHORT_READ);
    fault = CRF_STALE_SOURCE; assert(crf_ftz_read(s, 0, bytes, 16) == CRF_STALE_SOURCE);
    crf_ftz_get_stats(s, &stats); assert(stats.errors == 2);
    fault = CRF_OK;
    assert(crf_ftz_open(s, (uint32_t)packed_size - 1, (uint32_t)raw_size,
                        read_source, cancelled, NULL) == CRF_INVALID_FONT);
    printf("PASS FTZ1 full bytes, dynamic-slot LRU, cancelled replacement isolation, cross-block/cache, cancellation, malformed index/checksum, short/stale source; block=%u slots=%u chunked=%u workspace=%zu\n", block_bytes, slots, large != UINT32_MAX, crf_ftz_bytes());
    free(s); free(raw); free(packed); return 0;
}
