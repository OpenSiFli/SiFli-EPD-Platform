#ifndef EPD_TONE_ASSETS_H
#define EPD_TONE_ASSETS_H
#include <stdint.h>

typedef struct
{
    const unsigned char *pcm;
    uint32_t bytes, sample_rate;
} epd_tone_asset_t;

/* Mono signed 16-bit little-endian PCM in the firmware resource region. */
const epd_tone_asset_t *epd_tone_asset(unsigned tone);
#endif
