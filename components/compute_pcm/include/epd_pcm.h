/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_PCM_H
#define EPD_PCM_H
#include <stdbool.h>
#include "epd_compute_protocol.h"
#define EPD_PCM_CAPABILITY "epd/pcm"
#define EPD_PCM_VERSION 1u
#define EPD_PCM_MAX_SAMPLES 1024u
typedef struct epd_pcm_client *epd_pcm_t;
/* Interleaved little-endian integer PCM: unsigned 8-bit or signed 16/24-bit,
 * one/two input channels, rounded/clamped signed 16-bit mono output. */
typedef struct { uint32_t bits, channels; } epd_pcm_format_t;
int epd_pcm_open(epd_pcm_format_t format, epd_compute_purpose_t purpose, epd_pcm_t *client);
int epd_pcm_convert(epd_pcm_t client, const void *input, uint32_t samples, int16_t *output,
                    bool (*cancelled)(void *), void *context);
int epd_pcm_close(epd_pcm_t client);
bool epd_pcm_is_remote(epd_pcm_t client);
#endif
