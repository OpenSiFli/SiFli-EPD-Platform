/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute.h"
#include "epd_mp3.h"
#include "epd_mp3_compute.h"
#include <rtthread.h>
#include <string.h>
#ifndef BUILD_DLMODULE
#include <rtm.h>
#endif

struct epd_mp3_client
{
    HMP3Decoder local;
    epd_compute_session_t remote;
    bool terminal;
    epd_compute_buffer_t params, result, input, pcm;
    MP3FrameInfo last;
};

static int lifecycle_error(int status)
{
    if (status == EPD_COMPUTE_OK) return ERR_MP3_NONE;
    return status == EPD_COMPUTE_NO_MEMORY ? ERR_MP3_OUT_OF_MEMORY : ERR_UNKNOWN;
}

int epd_mp3_close(epd_mp3_t client)
{
    if (!client) return ERR_MP3_NONE;
    client->terminal = true;
    if (client->remote)
    {
        int status = epd_compute_close(client->remote);
        if (status != EPD_COMPUTE_OK) return lifecycle_error(status);
    }
    if (client->local) MP3FreeDecoder(client->local);
    epd_compute_buffer_free(&client->input);
    epd_compute_buffer_free(&client->pcm);
    epd_compute_buffer_free(&client->result);
    epd_compute_buffer_free(&client->params);
    rt_free(client);
    return ERR_MP3_NONE;
}

int epd_mp3_open(epd_mp3_t *out)
{
    if (!out) return ERR_MP3_NULL_POINTER;
    *out = NULL;
    epd_mp3_t client = rt_calloc(1, sizeof(*client));
    if (!client) return ERR_MP3_OUT_OF_MEMORY;
    epd_compute_capability_t capability;
    if (epd_compute_query(EPD_MP3_CAPABILITY, EPD_MP3_VERSION, &capability) == EPD_COMPUTE_OK && capability.ready)
    {
        if (!epd_compute_buffer_alloc(&client->params, sizeof(epd_mp3_request_t)) ||
            !epd_compute_buffer_alloc(&client->result, sizeof(epd_mp3_result_t)) ||
            !epd_compute_buffer_alloc(&client->pcm, EPD_MP3_PCM_SAMPLES * sizeof(short)))
        { epd_mp3_close(client); return ERR_MP3_OUT_OF_MEMORY; }
        epd_compute_requirements_t requirements = {EPD_COMPUTE_CONTINUOUS, 4};
        epd_compute_request_t request = {0};
        int status = epd_compute_open(&capability, &requirements, &request, &client->remote);
        if (status == EPD_COMPUTE_OK) { *out = client; return ERR_MP3_NONE; }
        if (client->remote) {
            client->terminal = true; *out = client; return lifecycle_error(status);
        }
        if (status != EPD_COMPUTE_BUSY && status != EPD_COMPUTE_UNAVAILABLE && status != EPD_COMPUTE_UNSUPPORTED)
        { epd_mp3_close(client); return lifecycle_error(status); }
        epd_compute_buffer_free(&client->pcm);
        epd_compute_buffer_free(&client->params);
        epd_compute_buffer_free(&client->result);
    }
    client->local = MP3InitDecoder();
    if (!client->local) { epd_mp3_close(client); return ERR_MP3_OUT_OF_MEMORY; }
    *out = client;
    return ERR_MP3_NONE;
}

int epd_mp3_reset(epd_mp3_t client)
{
    if (!client) return ERR_MP3_NULL_POINTER;
    if (client->terminal) return ERR_UNKNOWN;
    memset(&client->last, 0, sizeof(client->last));
    if (!client->remote)
    {
        MP3FreeDecoder(client->local);
        client->local = MP3InitDecoder();
        return client->local ? ERR_MP3_NONE : ERR_MP3_OUT_OF_MEMORY;
    }
    epd_compute_request_t request = {0};
    return lifecycle_error(epd_compute_reset(client->remote, &request));
}

static void copy_info(MP3FrameInfo *info, const epd_mp3_result_t *result)
{
    info->bitrate = result->bitrate;
    info->nChans = result->channels;
    info->samprate = result->sample_rate;
    info->bitsPerSample = result->bits_per_sample;
    info->outputSamps = result->output_samples;
    info->layer = result->layer;
    info->version = result->version;
}

static int invoke(epd_mp3_t client, const unsigned char *input, uint32_t bytes,
                   uint32_t operation, int use_size, int is_seeking,
                   epd_mp3_result_t **completed)
{
    *completed = NULL;
    uint32_t readable = bytes;
    if (operation == EPD_MP3_DECODE && readable < EPD_MP3_DECODE_READAHEAD)
        readable = EPD_MP3_DECODE_READAHEAD;
    if (client->input.capacity < readable)
    {
        epd_compute_buffer_t replacement;
        if (!epd_compute_buffer_alloc(&replacement, readable)) return ERR_MP3_OUT_OF_MEMORY;
        epd_compute_buffer_free(&client->input);
        client->input = replacement;
    }
    memcpy(client->input.data, input, bytes);
    if (readable > bytes)
        memset((unsigned char *)client->input.data + bytes, 0, readable - bytes);
    epd_mp3_request_t *params = client->params.data;
    *params = (epd_mp3_request_t){operation, {2, 0, bytes}, {3, 0, EPD_MP3_PCM_SAMPLES * sizeof(short)}, use_size, is_seeking};
    epd_compute_region_t regions[] = {
        epd_compute_buffer_region(&client->params, sizeof(*params), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&client->result, 0, EPD_COMPUTE_WRITE),
        epd_compute_buffer_region(&client->input, readable, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&client->pcm, EPD_MP3_PCM_SAMPLES * sizeof(short),
                                 EPD_COMPUTE_READ | EPD_COMPUTE_WRITE)
    };
    epd_compute_request_t request = {regions, 4, {0, 0, sizeof(*params)}, {1, 0, sizeof(epd_mp3_result_t)}, NULL, NULL};
    int status = epd_compute_step(client->remote, &request);
    if (status != EPD_COMPUTE_OK)
        return status == EPD_COMPUTE_NO_MEMORY ? ERR_MP3_OUT_OF_MEMORY : ERR_MP3_INVALID_FRAMEHEADER;
    *completed = client->result.data;
    return (*completed)->error;
}

int epd_mp3_decode(epd_mp3_t client, unsigned char **input, int *bytes_left, short *pcm, int use_size, int is_seeking)
{
    if (!client || !input || !*input || !bytes_left || *bytes_left < 0 || !pcm) return ERR_MP3_NULL_POINTER;
    if (client->terminal) return ERR_UNKNOWN;
    if (!client->remote)
        return client->local ? MP3Decode(client->local, input, bytes_left, pcm, use_size, is_seeking) : ERR_MP3_OUT_OF_MEMORY;
    /* Helix rejects these lengths before reading the header or changing state. */
    if (*bytes_left < 32) return ERR_MP3_NULL_POINTER;
    /* Preserve Helix's partial/no-write semantics on errors and seeking. */
    memcpy(client->pcm.data, pcm, EPD_MP3_PCM_SAMPLES * sizeof(short));
    epd_mp3_result_t *result;
    int error = invoke(client, *input, *bytes_left, EPD_MP3_DECODE, use_size, is_seeking, &result);
    if (!result) return error;
    if (result->consumed < 0 || result->consumed > *bytes_left || result->remaining < 0 ||
        result->remaining > *bytes_left || result->output_samples < 0 || result->output_samples > EPD_MP3_PCM_SAMPLES)
        return ERR_MP3_INVALID_FRAMEHEADER;
    *input += result->consumed;
    *bytes_left = result->remaining;
    copy_info(&client->last, result);
    memcpy(pcm, client->pcm.data, EPD_MP3_PCM_SAMPLES * sizeof(short));
    return error;
}

void epd_mp3_last_info(epd_mp3_t client, MP3FrameInfo *info)
{
    if (!info) return;
    if (!client) { memset(info, 0, sizeof(*info)); return; }
    if (!client->remote) MP3GetLastFrameInfo(client->local, info);
    else *info = client->last;
}

int epd_mp3_next_info(epd_mp3_t client, MP3FrameInfo *info, unsigned char *input)
{
    if (!client || !info || !input) return ERR_MP3_NULL_POINTER;
    if (client->terminal) return ERR_UNKNOWN;
    if (!client->remote)
        return client->local ? MP3GetNextFrameInfo(client->local, info, input) : ERR_MP3_OUT_OF_MEMORY;
    uint32_t bytes = (input[1] & 1) ? EPD_MP3_HEADER_BYTES : EPD_MP3_CRC_HEADER_BYTES;
    epd_mp3_result_t *result;
    int error = invoke(client, input, bytes, EPD_MP3_NEXT_INFO, 0, 0, &result);
    if (result)
    {
        copy_info(&client->last, result);
        if (!error) copy_info(info, result);
    }
    return error;
}

#ifndef BUILD_DLMODULE
RTM_EXPORT(epd_mp3_open);
RTM_EXPORT(epd_mp3_reset);
RTM_EXPORT(epd_mp3_close);
RTM_EXPORT(epd_mp3_decode);
RTM_EXPORT(epd_mp3_last_info);
RTM_EXPORT(epd_mp3_next_info);
#endif
