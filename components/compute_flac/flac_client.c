/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_flac.h"
#include "epd_compute.h"
#include <rtthread.h>
#include <string.h>
#ifndef BUILD_DLMODULE
#include <rtm.h>
#endif

struct epd_flac_client
{
    epd_compute_session_t session;
    bool terminal;
    epd_compute_buffer_t params, result, input, pcm;
};

int epd_flac_close(epd_flac_t c)
{
    if (!c) return EPD_COMPUTE_OK;
    c->terminal = true;
    int status = c->session ? epd_compute_close(c->session) : EPD_COMPUTE_OK;
    if (status != EPD_COMPUTE_OK) return status;
    epd_compute_buffer_free(&c->pcm);
    epd_compute_buffer_free(&c->input);
    epd_compute_buffer_free(&c->result);
    epd_compute_buffer_free(&c->params);
    rt_free(c);
    return EPD_COMPUTE_OK;
}

int epd_flac_open(const epd_flac_config_t *config, epd_compute_purpose_t purpose, epd_flac_t *client)
{
    if (!config || !client) return EPD_COMPUTE_INVALID;
    *client = NULL;
    epd_compute_capability_t capability;
    int status = epd_compute_query(EPD_FLAC_CAPABILITY, EPD_FLAC_VERSION, &capability);
    if (status != EPD_COMPUTE_OK) return status;
    if (!capability.ready) return EPD_COMPUTE_UNAVAILABLE;
    epd_flac_t c = rt_calloc(1, sizeof(*c));
    if (!c) return EPD_COMPUTE_NO_MEMORY;
    if (!epd_compute_buffer_alloc(&c->params, sizeof(*config)) ||
        !epd_compute_buffer_alloc(&c->result, sizeof(epd_flac_result_t)))
    { epd_flac_close(c); return EPD_COMPUTE_NO_MEMORY; }
    memcpy(c->params.data, config, sizeof(*config));
    epd_compute_region_t region = epd_compute_buffer_region(&c->params, sizeof(*config), EPD_COMPUTE_READ);
    epd_compute_request_t request = {&region, 1, {0, 0, sizeof(*config)}, {EPD_COMPUTE_REGION_NONE, 0, 0}, NULL, NULL};
    epd_compute_requirements_t requirements = {purpose, 4};
    status = epd_compute_open(&capability, &requirements, &request, &c->session);
    if (status != EPD_COMPUTE_OK) {
        if (c->session) { c->terminal = true; *client = c; }
        else epd_flac_close(c);
        return status;
    }
    *client = c;
    return EPD_COMPUTE_OK;
}

static bool reserve(epd_compute_buffer_t *buffer, uint32_t bytes)
{
    if (buffer->capacity >= bytes) return true;
    epd_compute_buffer_t replacement = {0};
    if (!epd_compute_buffer_alloc(&replacement, bytes)) return false;
    epd_compute_buffer_free(buffer);
    *buffer = replacement;
    return true;
}

int epd_flac_decode(epd_flac_t c, const void *input, uint32_t bytes, bool end_of_input,
                    int16_t *pcm, uint32_t capacity, epd_flac_result_t *result,
                    bool (*cancelled)(void *), void *context)
{
    if (!c || c->terminal || !input || !bytes || !pcm || !capacity || capacity > UINT32_MAX / sizeof(*pcm) || !result)
        return EPD_COMPUTE_INVALID;
    memset(result, 0, sizeof(*result));
    if (cancelled && cancelled(context)) return EPD_COMPUTE_CANCELLED;
    uint32_t output_bytes = capacity * sizeof(*pcm);
    if (!reserve(&c->input, bytes) || !reserve(&c->pcm, output_bytes)) return EPD_COMPUTE_NO_MEMORY;
    memcpy(c->input.data, input, bytes);
    epd_flac_request_t *p = c->params.data;
    *p = (epd_flac_request_t){{2, 0, bytes}, {3, 0, output_bytes}, end_of_input ? 1u : 0u};
    epd_compute_region_t regions[] = {
        epd_compute_buffer_region(&c->params, sizeof(*p), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->result, 0, EPD_COMPUTE_WRITE),
        epd_compute_buffer_region(&c->input, bytes, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->pcm, 0, EPD_COMPUTE_WRITE)
    };
    epd_compute_request_t request = {regions, 4, {0, 0, sizeof(*p)}, {1, 0, sizeof(*result)}, cancelled, context};
    int status = epd_compute_step(c->session, &request);
    if (status != EPD_COMPUTE_OK) return status;
    epd_flac_result_t completed = *(epd_flac_result_t *)c->result.data;
    if (!completed.consumed || completed.consumed > bytes || !completed.samples ||
        completed.samples > capacity || completed.decoder_error) return EPD_COMPUTE_COMPONENT_ERROR;
    memcpy(pcm, c->pcm.data, completed.samples * sizeof(*pcm));
    *result = completed;
    return EPD_COMPUTE_OK;
}

int epd_flac_reset(epd_flac_t c)
{
    epd_compute_request_t request = {0};
    return c && !c->terminal ? epd_compute_reset(c->session, &request) : EPD_COMPUTE_INVALID;
}

#ifndef BUILD_DLMODULE
RTM_EXPORT(epd_flac_open);
RTM_EXPORT(epd_flac_decode);
RTM_EXPORT(epd_flac_reset);
RTM_EXPORT(epd_flac_close);
#endif
