/* SPDX-License-Identifier: Apache-2.0 */
#include "pcm_internal.h"
#include "epd_compute.h"
#include <rtthread.h>
#include <string.h>
#ifndef BUILD_DLMODULE
#include <rtm.h>
#endif
struct epd_pcm_client
{
    epd_compute_session_t session;
    bool terminal;
    epd_pcm_format_t format;
    epd_compute_buffer_t params, input, output;
};
int epd_pcm_close(epd_pcm_t c)
{
    if (!c) return EPD_COMPUTE_OK;
    c->terminal = true;
    int status = c->session ? epd_compute_close(c->session) : EPD_COMPUTE_OK;
    if (status != EPD_COMPUTE_OK) return status;
    epd_compute_buffer_free(&c->params);
    epd_compute_buffer_free(&c->input);
    epd_compute_buffer_free(&c->output);
    rt_free(c);
    return EPD_COMPUTE_OK;
}
int epd_pcm_open(epd_pcm_format_t format, epd_compute_purpose_t purpose, epd_pcm_t *client)
{
    if (!client || !epd_pcm_format_valid(format)) return EPD_COMPUTE_INVALID;
    *client = NULL;
    epd_pcm_t c = rt_calloc(1, sizeof(*c));
    if (!c) return EPD_COMPUTE_NO_MEMORY;
    c->format = format;
    epd_compute_capability_t cap;
    int status = epd_compute_query(EPD_PCM_CAPABILITY, EPD_PCM_VERSION, &cap);
    if (status == EPD_COMPUTE_OK && cap.ready) {
        if (!epd_compute_buffer_alloc(&c->params, sizeof(epd_pcm_request_t)) ||
            !epd_compute_buffer_alloc(&c->input, EPD_PCM_MAX_SAMPLES * format.channels * format.bits / 8) ||
            !epd_compute_buffer_alloc(&c->output, EPD_PCM_MAX_SAMPLES * sizeof(int16_t))) {
            epd_pcm_close(c); return EPD_COMPUTE_NO_MEMORY;
        }
        epd_compute_requirements_t requirements = {purpose, 3};
        epd_compute_request_t request = {0};
        status = epd_compute_open(&cap, &requirements, &request, &c->session);
    } else if (status == EPD_COMPUTE_OK) status = EPD_COMPUTE_UNAVAILABLE;
    if (status != EPD_COMPUTE_OK && c->session) {
        c->terminal = true; *client = c; return status;
    }
    if (status != EPD_COMPUTE_OK && status != EPD_COMPUTE_UNAVAILABLE &&
        status != EPD_COMPUTE_UNSUPPORTED && status != EPD_COMPUTE_BUSY) {
        epd_pcm_close(c); return status;
    }
    if (!c->session) {
        epd_compute_buffer_free(&c->params);
        epd_compute_buffer_free(&c->input);
        epd_compute_buffer_free(&c->output);
    }
    *client = c;
    return EPD_COMPUTE_OK;
}
bool epd_pcm_is_remote(epd_pcm_t c) { return c && c->session; }
int epd_pcm_convert(epd_pcm_t c, const void *input, uint32_t samples, int16_t *output,
                    bool (*cancelled)(void *), void *context)
{
    if (!c || c->terminal || !input || !output || !samples || samples > EPD_PCM_MAX_SAMPLES) return EPD_COMPUTE_INVALID;
    if (cancelled && cancelled(context)) return EPD_COMPUTE_CANCELLED;
    if (!c->session) return epd_pcm_convert_local(c->format, input, samples, output, cancelled, context);
    uint32_t bytes = samples * c->format.channels * c->format.bits / 8;
    memcpy(c->input.data, input, bytes);
    epd_pcm_request_t *p = c->params.data;
    *p = (epd_pcm_request_t){{1, 0, bytes}, {2, 0, samples * sizeof(int16_t)}, c->format, samples};
    epd_compute_region_t regions[] = {
        epd_compute_buffer_region(&c->params, sizeof(*p), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->input, bytes, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->output, 0, EPD_COMPUTE_WRITE)
    };
    epd_compute_request_t request = {regions, 3, {0, 0, sizeof(*p)},
        {EPD_COMPUTE_REGION_NONE, 0, 0}, cancelled, context};
    int status = epd_compute_step(c->session, &request);
    if (status == EPD_COMPUTE_OK) memcpy(output, c->output.data, samples * sizeof(int16_t));
    return status;
}
#ifndef BUILD_DLMODULE
RTM_EXPORT(epd_pcm_open);
RTM_EXPORT(epd_pcm_convert);
RTM_EXPORT(epd_pcm_close);
RTM_EXPORT(epd_pcm_is_remote);
#endif
