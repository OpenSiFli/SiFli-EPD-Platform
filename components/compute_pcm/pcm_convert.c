/* SPDX-License-Identifier: Apache-2.0 */
#include "pcm_internal.h"
#include <limits.h>
bool epd_pcm_format_valid(epd_pcm_format_t f)
{
    return (f.bits == 8 || f.bits == 16 || f.bits == 24) && (f.channels == 1 || f.channels == 2);
}
int epd_pcm_convert_local(epd_pcm_format_t f, const void *input, uint32_t samples,
                          int16_t *output, bool (*cancelled)(void *), void *context)
{
    if (!epd_pcm_format_valid(f) || !input || !output || !samples || samples > EPD_PCM_MAX_SAMPLES)
        return EPD_COMPUTE_INVALID;
    const uint8_t *p = input;
    for (uint32_t i = 0; i < samples; ++i) {
        if (!(i & 255) && cancelled && cancelled(context)) return EPD_COMPUTE_CANCELLED;
        int64_t value = 0;
        for (uint32_t c = 0; c < f.channels; ++c) {
            if (f.bits == 8) value += ((int)*p++ - 128) * 256;
            else {
                uint32_t raw = p[0] | ((uint32_t)p[1] << 8);
                if (f.bits == 24) raw |= (uint32_t)p[2] << 16;
                value += (raw & (1u << (f.bits - 1))) ? (int64_t)raw - (1u << f.bits) : raw;
                p += f.bits / 8;
            }
        }
        int64_t divisor = (f.bits == 24 ? 256 : 1) * f.channels;
        value = value < 0 ? -((-value + divisor / 2) / divisor) : (value + divisor / 2) / divisor;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        output[i] = (int16_t)value;
    }
    return EPD_COMPUTE_OK;
}
