/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute_pipe.h"
#include <rtthread.h>
#include <rthw.h>
#include <rtm.h>
#include <string.h>

struct epd_compute_pipe_host {
    epd_compute_session_t session;
    epd_compute_buffer_t control, payload;
    epd_compute_region_t regions[2];
    epd_compute_request_t request;
    uint32_t serviced;
    bool cancelled;
};

static bool pipe_cancelled(epd_compute_pipe_host_t p)
{
    rt_base_t key = rt_hw_interrupt_disable();
    bool cancelled = p->cancelled;
    rt_hw_interrupt_enable(key);
    return cancelled;
}

static int pipe_open(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *initial, const void *data, size_t length,
    epd_compute_wait_t wait, epd_compute_pipe_host_t *out)
{
    if (!out || !initial || length > EPD_COMPUTE_PIPE_BYTES || (length && !data))
        return EPD_COMPUTE_INVALID;
    *out = NULL;
    epd_compute_capability_t capability;
    int status = epd_compute_query(name, version, &capability);
    if (status != EPD_COMPUTE_OK) return status;
    epd_compute_pipe_host_t p = rt_calloc(1, sizeof(*p));
    if (!p) return EPD_COMPUTE_NO_MEMORY;
    if (!epd_compute_buffer_alloc(&p->control, sizeof(*initial)) ||
        !epd_compute_buffer_alloc(&p->payload, EPD_COMPUTE_PIPE_BYTES)) {
        status = EPD_COMPUTE_NO_MEMORY;
        goto fail;
    }
    memcpy(p->control.data, initial, sizeof(*initial));
    epd_compute_pipe_message_t *m = p->control.data;
    m->version = EPD_COMPUTE_PIPE_VERSION;
    m->length = (uint32_t)length;
    m->sequence = m->response_sequence = 0;
    if (length) memcpy(p->payload.data, data, length);
    p->regions[0] = epd_compute_buffer_region(&p->control, sizeof(*initial), EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    p->regions[1] = epd_compute_buffer_region(&p->payload, EPD_COMPUTE_PIPE_BYTES, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    p->request.regions = p->regions;
    p->request.region_count = 2;
    p->request.params = (epd_compute_ref_t){0, 0, sizeof(*initial)};
    p->request.result = p->request.params;
    epd_compute_requirements_t requirements = {purpose, 2};
    status = wait ? epd_compute_open_wait(&capability, &requirements, &p->request, wait, &p->session) :
                    epd_compute_open(&capability, &requirements, &p->request, &p->session);
    if (status != EPD_COMPUTE_OK) goto fail;
    *out = p;
    return EPD_COMPUTE_OK;
fail:
    if (p->session) {
        p->cancelled = true;
        *out = p;
    } else epd_compute_pipe_close(p);
    return status;
}

int epd_compute_pipe_open(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *initial, const void *data, size_t length,
    epd_compute_pipe_host_t *out)
{ return pipe_open(name, version, purpose, initial, data, length, NULL, out); }

int epd_compute_pipe_open_wait(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *initial, const void *data, size_t length,
    epd_compute_wait_t wait, epd_compute_pipe_host_t *out)
{
    if (!wait) { if (out) *out = NULL; return EPD_COMPUTE_INVALID; }
    return pipe_open(name, version, purpose, initial, data, length, wait, out);
}

int epd_compute_pipe_step(epd_compute_pipe_host_t p, epd_compute_pipe_service_t service, void *user)
{
    if (!p || !p->session) return EPD_COMPUTE_INVALID;
    if (pipe_cancelled(p)) return EPD_COMPUTE_CANCELLED;
    epd_compute_pipe_message_t *m = p->control.data;
    if (m->operation == EPD_PIPE_COMMAND && m->sequence && m->sequence != p->serviced)
        return EPD_COMPUTE_BUSY;
    int status = epd_compute_step(p->session, &p->request);
    if (status != EPD_COMPUTE_MORE) return status;
    if (m->version != EPD_COMPUTE_PIPE_VERSION || m->length > EPD_COMPUTE_PIPE_BYTES ||
        !m->sequence || m->sequence == p->serviced) return EPD_COMPUTE_INVALID;
    if (m->operation == EPD_PIPE_IDLE) m->status = EPD_COMPUTE_OK;
    else if (m->operation == EPD_PIPE_COMMAND) return EPD_COMPUTE_MORE;
    else m->status = service ? service(user, m, p->payload.data) : EPD_COMPUTE_UNSUPPORTED;
    if (m->length > EPD_COMPUTE_PIPE_BYTES) return EPD_COMPUTE_INVALID;
    m->response_sequence = m->sequence;
    p->serviced = m->sequence;
    return EPD_COMPUTE_MORE;
}

int epd_compute_pipe_command(epd_compute_pipe_host_t p, const epd_compute_pipe_message_t *message,
    const void *data, size_t length)
{
    if (!p || !message || length > EPD_COMPUTE_PIPE_BYTES || (length && !data)) return EPD_COMPUTE_INVALID;
    if (pipe_cancelled(p)) return EPD_COMPUTE_CANCELLED;
    epd_compute_pipe_message_t *m = p->control.data;
    if (m->operation != EPD_PIPE_COMMAND || !m->sequence || m->sequence == p->serviced) return EPD_COMPUTE_BUSY;
    uint32_t sequence = m->sequence;
    *m = *message;
    m->version = EPD_COMPUTE_PIPE_VERSION;
    m->sequence = m->response_sequence = p->serviced = sequence;
    m->length = (uint32_t)length;
    if (length) memcpy(p->payload.data, data, length);
    return EPD_COMPUTE_OK;
}

const epd_compute_pipe_message_t *epd_compute_pipe_result(epd_compute_pipe_host_t p)
{ return p ? p->control.data : NULL; }
const uint8_t *epd_compute_pipe_payload(epd_compute_pipe_host_t p)
{ return p ? p->payload.data : NULL; }
void epd_compute_pipe_cancel(epd_compute_pipe_host_t p)
{
    if (!p || !p->session) return;
    rt_base_t key = rt_hw_interrupt_disable();
    p->cancelled = true;
    epd_compute_cancel(p->session);
    rt_hw_interrupt_enable(key);
}
int epd_compute_pipe_purpose(epd_compute_pipe_host_t p, epd_compute_purpose_t purpose)
{ return p ? epd_compute_set_purpose(p->session, purpose) : EPD_COMPUTE_INVALID; }
static int pipe_close(epd_compute_pipe_host_t p)
{
    if (!p) return EPD_COMPUTE_OK;
    if (p->session) {
        int status = epd_compute_close(p->session);
        if (status != EPD_COMPUTE_OK) return status;
    }
    epd_compute_buffer_free(&p->payload);
    epd_compute_buffer_free(&p->control);
    rt_free(p);
    return EPD_COMPUTE_OK;
}
void epd_compute_pipe_close(epd_compute_pipe_host_t p) { (void)pipe_close(p); }
int epd_compute_pipe_shutdown(epd_compute_pipe_host_t *pipe)
{
    if (!pipe) return EPD_COMPUTE_INVALID;
    epd_compute_pipe_cancel(*pipe);
    int status = pipe_close(*pipe);
    if (status == EPD_COMPUTE_OK) *pipe = NULL;
    return status;
}

RTM_EXPORT(epd_compute_pipe_open);
RTM_EXPORT(epd_compute_pipe_open_wait);
RTM_EXPORT(epd_compute_pipe_step);
RTM_EXPORT(epd_compute_pipe_command);
RTM_EXPORT(epd_compute_pipe_result);
RTM_EXPORT(epd_compute_pipe_payload);
RTM_EXPORT(epd_compute_pipe_cancel);
RTM_EXPORT(epd_compute_pipe_purpose);
RTM_EXPORT(epd_compute_pipe_close);
RTM_EXPORT(epd_compute_pipe_shutdown);
