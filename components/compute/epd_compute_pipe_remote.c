/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute_pipe.h"

bool epd_compute_pipe_bind(epd_compute_pipe_remote_t *p, const epd_compute_wire_t *r)
{
    if (!p || !r || r->region_count != 2 || r->params.region != 0 || r->params.offset ||
        r->params.length != sizeof(*p->message) || r->result.region != 0 ||
        r->result.offset || r->result.length != sizeof(*p->message)) return false;
    epd_compute_ref_t data = {1, 0, EPD_COMPUTE_PIPE_BYTES};
    p->message = epd_compute_resolve(r, r->params, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    p->data = epd_compute_resolve(r, data, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    return p->message && p->data && p->message->version == EPD_COMPUTE_PIPE_VERSION;
}

bool epd_compute_pipe_validate(const epd_compute_pipe_remote_t *p, const epd_compute_wire_t *r)
{
    epd_compute_pipe_remote_t actual = {0};
    return epd_compute_pipe_bind(&actual, r) && actual.message == p->message && actual.data == p->data;
}

bool epd_compute_pipe_exchange(epd_compute_pipe_remote_t *p, uint32_t operation)
{
    if (!p || epd_compute_fiber_cancelled(p->fiber)) return false;
    p->message->operation = operation;
    do { ++p->sequence; } while (!p->sequence);
    p->message->sequence = p->sequence;
    p->message->response_sequence = 0;
    if (!epd_compute_fiber_yield(p->fiber)) return false;
    return p->message->version == EPD_COMPUTE_PIPE_VERSION &&
        p->message->response_sequence == p->sequence &&
        p->message->length <= EPD_COMPUTE_PIPE_BYTES;
}

bool epd_compute_pipe_checkpoint(epd_compute_pipe_remote_t *p)
{
    if (epd_compute_fiber_cancelled(p->fiber)) return false;
    if (!epd_compute_fiber_should_yield(p->fiber)) return true;
    p->message->length = 0;
    return epd_compute_pipe_exchange(p, EPD_PIPE_IDLE);
}
