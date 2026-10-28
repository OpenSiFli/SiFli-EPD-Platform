/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <string.h>
#include "mp3dec.h"
#include "epd_mp3_compute.h"

typedef struct { HMP3Decoder decoder; } mp3_remote_t;

static int32_t open_decoder(void **state, const epd_compute_wire_t *request)
{
    (void)request;
    *state = NULL;
    mp3_remote_t *instance = rt_calloc(1, sizeof(*instance));
    if (!instance) return EPD_COMPUTE_NO_MEMORY;
    instance->decoder = MP3InitDecoder();
    if (!instance->decoder) { rt_free(instance); return EPD_COMPUTE_NO_MEMORY; }
    *state = instance;
    return EPD_COMPUTE_OK;
}

static void frame_result(epd_mp3_result_t *result, const MP3FrameInfo *info)
{
    result->bitrate = info->bitrate;
    result->channels = info->nChans;
    result->sample_rate = info->samprate;
    result->bits_per_sample = info->bitsPerSample;
    result->output_samples = info->outputSamps;
    result->layer = info->layer;
    result->version = info->version;
}

static int32_t step_decoder(void *state, const epd_compute_wire_t *wire)
{
    mp3_remote_t *instance = state;
    if (!instance || !instance->decoder) return EPD_COMPUTE_NO_MEMORY;
    const epd_mp3_request_t *request = epd_compute_resolve(wire, wire->params, EPD_COMPUTE_READ);
    epd_mp3_result_t *result = epd_compute_resolve(wire, wire->result, EPD_COMPUTE_WRITE);
    if (!request || !result || wire->params.length != sizeof(*request) || wire->result.length != sizeof(*result))
        return EPD_COMPUTE_INVALID;
    memset(result, 0, sizeof(*result));
    unsigned char *input = epd_compute_resolve(wire, request->input, EPD_COMPUTE_READ);
    if (!input || request->input.length > INT32_MAX) return EPD_COMPUTE_INVALID;
    MP3FrameInfo info = {0};
    if (request->operation == EPD_MP3_DECODE)
    {
        epd_compute_ref_t readable = request->input;
        if (readable.length >= 32 && readable.length < EPD_MP3_DECODE_READAHEAD)
        {
            readable.length = EPD_MP3_DECODE_READAHEAD;
            if (!epd_compute_resolve(wire, readable, EPD_COMPUTE_READ)) return EPD_COMPUTE_INVALID;
        }
        short *pcm = epd_compute_resolve(wire, request->pcm, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
        if (!pcm || request->pcm.length < EPD_MP3_PCM_SAMPLES * sizeof(short)) return EPD_COMPUTE_INVALID;
        int remaining = request->input.length;
        unsigned char *cursor = input;
        result->error = MP3Decode(instance->decoder, &cursor, &remaining, pcm,
                                  request->use_size, request->is_seeking);
        result->consumed = (int)(cursor - input);
        result->remaining = remaining;
        MP3GetLastFrameInfo(instance->decoder, &info);
    }
    else if (request->operation == EPD_MP3_NEXT_INFO)
    {
        uint32_t length = request->input.length;
        if (length < EPD_MP3_HEADER_BYTES ||
            (!(input[1] & 1) && length < EPD_MP3_CRC_HEADER_BYTES))
            result->error = ERR_MP3_INDATA_UNDERFLOW;
        else
            result->error = MP3GetNextFrameInfo(instance->decoder, &info, input);
        /* Failed header parsing may leave the prior frame information intact.
         * Always report the actual decoder state, not a zeroed temporary. */
        MP3GetLastFrameInfo(instance->decoder, &info);
    }
    else return EPD_COMPUTE_UNSUPPORTED;
    frame_result(result, &info);
    return EPD_COMPUTE_OK;
}

static int32_t reset_decoder(void *state, const epd_compute_wire_t *request)
{
    (void)request;
    mp3_remote_t *instance = state;
    if (instance->decoder) MP3FreeDecoder(instance->decoder);
    instance->decoder = MP3InitDecoder();
    return instance->decoder ? EPD_COMPUTE_OK : EPD_COMPUTE_NO_MEMORY;
}

static void close_decoder(void *state, const epd_compute_wire_t *request)
{
    (void)request;
    mp3_remote_t *instance = state;
    if (instance->decoder) MP3FreeDecoder(instance->decoder);
    rt_free(instance);
}

const epd_compute_ops_t epd_mp3_remote_ops = {open_decoder, step_decoder, reset_decoder, close_decoder};
