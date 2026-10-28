/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_MP3_COMPUTE_H
#define EPD_MP3_COMPUTE_H
#include "epd_compute_protocol.h"

#define EPD_MP3_CAPABILITY "epd/helix-mp3"
#define EPD_MP3_VERSION 1u
#define EPD_MP3_DECODE 1u
#define EPD_MP3_NEXT_INFO 2u
#define EPD_MP3_PCM_SAMPLES (2u * 2u * 576u)
#define EPD_MP3_HEADER_BYTES 4u
#define EPD_MP3_CRC_HEADER_BYTES 6u
/* Helix reads the header and side information before checking their combined
 * length. Short decode inputs carry zero padding up to this readable extent;
 * request.input.length still denotes the actual compressed byte count. */
#define EPD_MP3_DECODE_READAHEAD 38u

typedef struct
{
    uint32_t operation;
    epd_compute_ref_t input;
    epd_compute_ref_t pcm;
    int32_t use_size;
    int32_t is_seeking;
} epd_mp3_request_t;

typedef struct
{
    /* Helix can consume input even on an algorithm error. These are the two
     * values returned by the decoder, not inferred from the error category. */
    int32_t error, consumed, remaining;
    int32_t bitrate, channels, sample_rate, bits_per_sample, output_samples, layer, version;
} epd_mp3_result_t;

typedef char epd_mp3_request_layout[(sizeof(epd_mp3_request_t) == 36) ? 1 : -1];
typedef char epd_mp3_result_layout[(sizeof(epd_mp3_result_t) == 40) ? 1 : -1];

#endif
