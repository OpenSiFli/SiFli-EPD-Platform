/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_FLAC_H
#define EPD_FLAC_H
#include <stdbool.h>
#include "epd_compute_protocol.h"

#define EPD_FLAC_CAPABILITY "epd/flac"
#define EPD_FLAC_VERSION 1u
#define EPD_FLAC_STREAMINFO_BYTES 34u

typedef struct { uint8_t streaminfo[EPD_FLAC_STREAMINFO_BYTES]; uint8_t reserved[2]; } epd_flac_config_t;
typedef struct
{
    epd_compute_ref_t input, pcm;
    uint32_t end_of_input;
} epd_flac_request_t;
typedef struct
{
    uint32_t consumed, samples;
    uint32_t sample_low, sample_high;
    int32_t decoder_error;
} epd_flac_result_t;
typedef struct epd_flac_client *epd_flac_t;
/* Failed open may return a non-NULL close-only client after rollback failure.
 * Retain it until close succeeds; decoding/reset cannot reuse that client. */

/* One complete frame per step, downmixed to signed 16-bit mono. MORE means
 * the input ended inside a frame: retain the entire prefix and append bytes.
 * No bytes or PCM are committed on MORE or errors. No file/audio/UI callbacks
 * execute remotely. The owning worker handles files and audio backpressure. */
int epd_flac_open(const epd_flac_config_t *config, epd_compute_purpose_t purpose, epd_flac_t *client);
int epd_flac_decode(epd_flac_t client, const void *input, uint32_t bytes, bool end_of_input,
                    int16_t *pcm, uint32_t capacity_samples, epd_flac_result_t *result,
                    bool (*cancelled)(void *), void *context);
int epd_flac_reset(epd_flac_t client);
int epd_flac_close(epd_flac_t client);

typedef char epd_flac_config_layout[(sizeof(epd_flac_config_t) == 36) ? 1 : -1];
typedef char epd_flac_request_layout[(sizeof(epd_flac_request_t) == 28) ? 1 : -1];
typedef char epd_flac_result_layout[(sizeof(epd_flac_result_t) == 20) ? 1 : -1];
#endif
