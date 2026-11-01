/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_opus.h"
#include "epd_compute.h"
#include <rtthread.h>
#include <string.h>

struct epd_opus_client
{
    epd_compute_session_t session;
    bool terminal;
    epd_compute_buffer_t params, result, input, output;
};

int epd_opus_close(epd_opus_t c)
{
    if (!c) return EPD_COMPUTE_OK;
    c->terminal = true;
    int status = c->session ? epd_compute_close(c->session) : EPD_COMPUTE_OK;
    if (status != EPD_COMPUTE_OK) return status;
    epd_compute_buffer_free(&c->output);
    epd_compute_buffer_free(&c->input);
    epd_compute_buffer_free(&c->result);
    epd_compute_buffer_free(&c->params);
    rt_free(c);
    return EPD_COMPUTE_OK;
}

int epd_opus_open(const epd_opus_config_t *config, epd_compute_purpose_t purpose, epd_opus_t *client)
{
    if (!config || !client) return EPD_COMPUTE_INVALID;
    *client = NULL;
    epd_compute_capability_t cap;
    int status = epd_compute_query(EPD_OPUS_CAPABILITY, EPD_OPUS_VERSION, &cap);
    if (status != EPD_COMPUTE_OK) return status;
    if (!cap.ready) return EPD_COMPUTE_UNAVAILABLE;
    epd_opus_t c = rt_calloc(1, sizeof(*c));
    if (!c) return EPD_COMPUTE_NO_MEMORY;
    if (!epd_compute_buffer_alloc(&c->params, sizeof(epd_opus_request_t)) ||
        !epd_compute_buffer_alloc(&c->result, sizeof(epd_opus_result_t)))
    { epd_opus_close(c); return EPD_COMPUTE_NO_MEMORY; }
    memcpy(c->params.data, config, sizeof(*config));
    epd_compute_region_t region = epd_compute_buffer_region(&c->params, sizeof(*config), EPD_COMPUTE_READ);
    epd_compute_request_t request = {&region, 1, {0, 0, sizeof(*config)}, {EPD_COMPUTE_REGION_NONE, 0, 0}, NULL, NULL};
    epd_compute_requirements_t requirements = {purpose, 4};
    status = epd_compute_open(&cap, &requirements, &request, &c->session);
    if (status != EPD_COMPUTE_OK) {
        if (c->session) { c->terminal = true; *client = c; }
        else epd_opus_close(c);
        return status;
    }
    *client = c;
    return EPD_COMPUTE_OK;
}

static int reserve(epd_compute_buffer_t *b, uint32_t bytes)
{
    if (b->capacity >= bytes) return EPD_COMPUTE_OK;
    epd_compute_buffer_t next = {0};
    if (!epd_compute_buffer_alloc(&next, bytes)) return EPD_COMPUTE_NO_MEMORY;
    epd_compute_buffer_free(b);
    *b = next;
    return EPD_COMPUTE_OK;
}

static int invoke(epd_opus_t c, epd_opus_request_t *p, epd_opus_result_t *result)
{
    memcpy(c->params.data, p, sizeof(*p));
    epd_compute_region_t regions[4] = {
        epd_compute_buffer_region(&c->params, sizeof(*p), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->result, 0, EPD_COMPUTE_WRITE)
    };
    uint32_t count = 2;
    if (p->operation != EPD_OPUS_SET)
    {
        regions[2] = epd_compute_buffer_region(&c->input, p->input.length, EPD_COMPUTE_READ);
        regions[3] = epd_compute_buffer_region(&c->output, 0, EPD_COMPUTE_WRITE);
        count = 4;
    }
    epd_compute_request_t request = {regions, count, {0, 0, sizeof(*p)}, {1, 0, sizeof(*result)}, NULL, NULL};
    int status = epd_compute_step(c->session, &request);
    if (status == EPD_COMPUTE_OK) memcpy(result, c->result.data, sizeof(*result));
    return status;
}

int epd_opus_process(epd_opus_t c, uint32_t operation,
                     const void *input, uint32_t input_bytes, void *output, uint32_t output_bytes,
                     int32_t samples, int fec, epd_opus_result_t *result)
{
    if (!c || c->terminal || !result || (!input && input_bytes) || !output || !output_bytes ||
        input_bytes > INT32_MAX || output_bytes > INT32_MAX ||
        (operation != EPD_OPUS_ENCODE && operation != EPD_OPUS_DECODE)) return EPD_COMPUTE_INVALID;
    memset(result, 0, sizeof(*result));
    /* A zero-length packet asks Opus for packet-loss concealment. The region
     * itself remains valid and owned even when its effective length is zero. */
    int status = reserve(&c->input, input_bytes ? input_bytes : 1);
    if (status != EPD_COMPUTE_OK) return status;
    status = reserve(&c->output, output_bytes);
    if (status != EPD_COMPUTE_OK) return status;
    if (input_bytes) memcpy(c->input.data, input, input_bytes);
    epd_opus_request_t p = {operation, {2, 0, input_bytes}, {3, 0, output_bytes}, samples, fec, 0, 0};
    status = invoke(c, &p, result);
    if (status == EPD_COMPUTE_OK)
    {
        if (result->output_bytes > output_bytes) return EPD_COMPUTE_COMPONENT_ERROR;
        if (result->output_bytes) memcpy(output, c->output.data, result->output_bytes);
    }
    return status;
}

int epd_opus_set(epd_opus_t c, uint32_t control, int32_t value, epd_opus_result_t *result)
{
    if (!c || c->terminal || !result) return EPD_COMPUTE_INVALID;
    memset(result, 0, sizeof(*result));
    epd_opus_request_t p = {0};
    p.operation = EPD_OPUS_SET; p.control = control; p.value = value;
    return invoke(c, &p, result);
}

int epd_opus_reset(epd_opus_t c)
{
    epd_compute_request_t request = {0};
    return c && !c->terminal ? epd_compute_reset(c->session, &request) : EPD_COMPUTE_INVALID;
}

void epd_opus_cancel(epd_opus_t c)
{
    if (c) epd_compute_cancel(c->session);
}
