/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_anyka.h"
#include "epd_compute.h"
#include <rtthread.h>
#include <string.h>

struct epd_anyka_client
{
    epd_compute_session_t session;
    bool terminal;
    epd_compute_buffer_t params, result, input, reference, output;
    bool (*cancelled)(void *context);
    void *context;
};

int epd_anyka_close(epd_anyka_t c)
{
    if (!c) return EPD_COMPUTE_OK;
    c->terminal = true;
    int status = c->session ? epd_compute_close(c->session) : EPD_COMPUTE_OK;
    if (status != EPD_COMPUTE_OK) return status;
    epd_compute_buffer_free(&c->output);
    epd_compute_buffer_free(&c->reference);
    epd_compute_buffer_free(&c->input);
    epd_compute_buffer_free(&c->result);
    epd_compute_buffer_free(&c->params);
    rt_free(c);
    return EPD_COMPUTE_OK;
}

static int reserve(epd_compute_buffer_t *buffer, uint32_t bytes)
{
    if (buffer->capacity >= bytes) return EPD_COMPUTE_OK;
    epd_compute_buffer_t next = {0};
    if (!epd_compute_buffer_alloc(&next, bytes)) return EPD_COMPUTE_NO_MEMORY;
    epd_compute_buffer_free(buffer);
    *buffer = next;
    return EPD_COMPUTE_OK;
}

int epd_anyka_open_cancelable(const epd_anyka_config_t *config, const char *near_text,
    const char *far_text, epd_compute_purpose_t purpose,
    bool (*cancelled)(void *context), void *context, epd_anyka_t *client)
{
    if (!config || !client || !near_text || (config->far_enabled && !far_text)) return EPD_COMPUTE_INVALID;
    *client = NULL;
    if (cancelled && cancelled(context)) return EPD_COMPUTE_CANCELLED;
    size_t near_size = strlen(near_text) + 1;
    size_t far_size = config->far_enabled ? strlen(far_text) + 1 : 1;
    if (near_size > INT32_MAX || far_size > INT32_MAX) return EPD_COMPUTE_INVALID;
    epd_compute_capability_t cap;
    int status = epd_compute_query(EPD_ANYKA_CAPABILITY, EPD_ANYKA_VERSION, &cap);
    if (status != EPD_COMPUTE_OK) return status;
    if (!cap.ready) return EPD_COMPUTE_UNAVAILABLE;
    epd_anyka_t c = rt_calloc(1, sizeof(*c));
    if (!c) return EPD_COMPUTE_NO_MEMORY;
    c->cancelled = cancelled; c->context = context;
    if (!epd_compute_buffer_alloc(&c->params, sizeof(epd_anyka_open_t)) ||
        !epd_compute_buffer_alloc(&c->result, sizeof(epd_anyka_result_t)) ||
        !epd_compute_buffer_alloc(&c->input, near_size) ||
        !epd_compute_buffer_alloc(&c->reference, far_size) ||
        !epd_compute_buffer_alloc(&c->output, EPD_ANYKA_FRAME_BYTES))
    { epd_anyka_close(c); return EPD_COMPUTE_NO_MEMORY; }
    memcpy(c->input.data, near_text, near_size);
    memcpy(c->reference.data, config->far_enabled ? far_text : "", far_size);
    epd_anyka_open_t *p = c->params.data;
    *p = (epd_anyka_open_t){*config, {1, 0, (uint32_t)near_size}, {2, 0, (uint32_t)far_size}};
    epd_compute_region_t regions[] = {
        epd_compute_buffer_region(&c->params, sizeof(*p), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->input, near_size, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->reference, far_size, EPD_COMPUTE_READ)
    };
    epd_compute_request_t request = {regions, 3, {0, 0, sizeof(*p)}, {EPD_COMPUTE_REGION_NONE, 0, 0}, cancelled, context};
    epd_compute_requirements_t requirements = {purpose, 5};
    status = epd_compute_open(&cap, &requirements, &request, &c->session);
    if (status != EPD_COMPUTE_OK) {
        if (c->session) { c->terminal = true; *client = c; }
        else epd_anyka_close(c);
        return status;
    }
    *client = c;
    return EPD_COMPUTE_OK;
}

int epd_anyka_open(const epd_anyka_config_t *config, const char *near_text,
    const char *far_text, epd_compute_purpose_t purpose, epd_anyka_t *client)
{ return epd_anyka_open_cancelable(config, near_text, far_text, purpose, NULL, NULL, client); }

int epd_anyka_process(epd_anyka_t c, uint32_t operation, uint64_t timestamp,
                      const void *input, uint32_t bytes, const void *reference, uint32_t ref_bytes,
                      void *output, uint32_t output_bytes, epd_anyka_result_t *result)
{
    if (!c || c->terminal || !result || !input || !bytes || bytes > INT32_MAX || ref_bytes > INT32_MAX ||
        (!reference && ref_bytes) || operation < EPD_ANYKA_UPLINK || operation > EPD_ANYKA_SSL ||
        (operation != EPD_ANYKA_SSL && (!output || output_bytes < EPD_ANYKA_FRAME_BYTES)))
        return EPD_COMPUTE_INVALID;
    memset(result, 0, sizeof(*result));
    int status = reserve(&c->input, bytes);
    if (status != EPD_COMPUTE_OK) return status;
    status = reserve(&c->reference, ref_bytes ? ref_bytes : 1);
    if (status != EPD_COMPUTE_OK) return status;
    memcpy(c->input.data, input, bytes);
    if (ref_bytes) memcpy(c->reference.data, reference, ref_bytes);
    epd_anyka_request_t *p = c->params.data;
    *p = (epd_anyka_request_t){timestamp, operation, {2, 0, bytes}, {3, 0, ref_bytes}, {4, 0, EPD_ANYKA_FRAME_BYTES}};
    epd_compute_region_t regions[] = {
        epd_compute_buffer_region(&c->params, sizeof(*p), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->result, 0, EPD_COMPUTE_WRITE),
        epd_compute_buffer_region(&c->input, bytes, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->reference, ref_bytes, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->output, 0, EPD_COMPUTE_WRITE)
    };
    epd_compute_request_t request = {regions, 5, {0, 0, sizeof(*p)}, {1, 0, sizeof(*result)}, c->cancelled, c->context};
    memset(c->result.data, 0, sizeof(*result));
    status = epd_compute_step(c->session, &request);
    /* A completed algorithm step may already have consumed input before a
     * later operation fails. Preserve its counts, not just success results. */
    if (status == EPD_COMPUTE_OK || status == EPD_COMPUTE_COMPONENT_ERROR || status == EPD_COMPUTE_BUSY)
    {
        memcpy(result, c->result.data, sizeof(*result));
        if (result->output_bytes > output_bytes || result->output_bytes > EPD_ANYKA_FRAME_BYTES ||
            result->input_consumed > bytes || result->reference_consumed > ref_bytes)
            return EPD_COMPUTE_COMPONENT_ERROR;
        if (result->output_bytes && output) memcpy(output, c->output.data, result->output_bytes);
    }
    return status;
}

int epd_anyka_reset(epd_anyka_t c)
{
    epd_compute_request_t request = {0};
    return c && !c->terminal ? epd_compute_reset(c->session, &request) : EPD_COMPUTE_INVALID;
}

void epd_anyka_cancel(epd_anyka_t c)
{
    if (c) epd_compute_cancel(c->session);
}
