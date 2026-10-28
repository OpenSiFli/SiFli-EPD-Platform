/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_OPUS_H
#define EPD_OPUS_H
#include "epd_compute_protocol.h"

#define EPD_OPUS_CAPABILITY "epd/opus"
#define EPD_OPUS_VERSION 1u
#define EPD_OPUS_ENCODER 1u
#define EPD_OPUS_DECODER 2u
/* Values are the Opus public application constants. No SDK Opus entry point
 * is called on HCPU, where those entry points may route to legacy task IDs. */
#define EPD_OPUS_VOIP 2048
#define EPD_OPUS_AUDIO 2049
#define EPD_OPUS_LOWDELAY 2051

typedef struct
{
    uint32_t directions; /* ENCODER, DECODER, or both in one reservation. */
    int32_t sample_rate, channels, application;
} epd_opus_config_t;

enum { EPD_OPUS_ENCODE = 1, EPD_OPUS_DECODE, EPD_OPUS_SET };
enum
{
    EPD_OPUS_BITRATE = 1, EPD_OPUS_COMPLEXITY, EPD_OPUS_VBR,
    EPD_OPUS_VBR_CONSTRAINT, EPD_OPUS_DTX, EPD_OPUS_FEC,
    EPD_OPUS_PACKET_LOSS, EPD_OPUS_SIGNAL, EPD_OPUS_BANDWIDTH,
    EPD_OPUS_MAX_BANDWIDTH, EPD_OPUS_LSB_DEPTH, EPD_OPUS_FRAME_DURATION,
    EPD_OPUS_PREDICTION_DISABLED, EPD_OPUS_DECODER_GAIN
};

typedef struct
{
    uint32_t operation;
    epd_compute_ref_t input, output;
    int32_t frame_samples; /* Samples per channel, not bytes. */
    int32_t fec;
    uint32_t control;
    int32_t value;
} epd_opus_request_t;

typedef struct
{
    int32_t value; /* Original Opus result: encoded bytes, decoded samples/channel, or error. */
    uint32_t output_bytes;
} epd_opus_result_t;

typedef struct epd_opus_client *epd_opus_t;
/* Calls are synchronous, from the owning worker. Algorithm errors are returned
 * in result.value; the function return describes component execution. */
int epd_opus_open(const epd_opus_config_t *config, epd_compute_purpose_t purpose, epd_opus_t *client);
int epd_opus_process(epd_opus_t client, uint32_t operation,
                     const void *input, uint32_t input_bytes, void *output, uint32_t output_bytes,
                     int32_t frame_samples, int fec, epd_opus_result_t *result);
int epd_opus_set(epd_opus_t client, uint32_t control, int32_t value, epd_opus_result_t *result);
int epd_opus_reset(epd_opus_t client);
void epd_opus_cancel(epd_opus_t client);
/* Keep the handle if close fails. Do not race close against process/cancel. */
int epd_opus_close(epd_opus_t client);
typedef char epd_opus_config_layout[(sizeof(epd_opus_config_t) == 16) ? 1 : -1];
typedef char epd_opus_request_layout[(sizeof(epd_opus_request_t) == 44) ? 1 : -1];
typedef char epd_opus_result_layout[(sizeof(epd_opus_result_t) == 8) ? 1 : -1];
#endif
