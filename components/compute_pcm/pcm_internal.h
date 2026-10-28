/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_PCM_INTERNAL_H
#define EPD_PCM_INTERNAL_H
#include "epd_pcm.h"
typedef struct
{
    epd_compute_ref_t input, output;
    epd_pcm_format_t format;
    uint32_t samples;
} epd_pcm_request_t;
bool epd_pcm_format_valid(epd_pcm_format_t format);
int epd_pcm_convert_local(epd_pcm_format_t format, const void *input, uint32_t samples,
                          int16_t *output, bool (*cancelled)(void *), void *context);
typedef char epd_pcm_request_layout[(sizeof(epd_pcm_request_t) == 36) ? 1 : -1];
#endif
