#ifndef EPD_TONE_ASSETS_H
#define EPD_TONE_ASSETS_H
#include <stdint.h>
#define EPD_TONE_DIFF_PLANES_ZLIB_V1 1u

typedef struct
{
    const unsigned char *packed;
    uint32_t packed_bytes, bytes, sample_rate;
    uint32_t encoding, difference_order, pcm_crc32;
} epd_tone_asset_t;

/* V1: modulo-65536 differences (zero history), signed zigzag, low-byte plane
 * followed by high-byte plane, then zlib. bytes is the exact PCM length.
 * pcm_crc32 checks restored signed 16-bit LE mono PCM, not transformed bytes.
 * Sample rate and PCM gain are unchanged. */
const epd_tone_asset_t *epd_tone_asset(unsigned tone);
#endif
