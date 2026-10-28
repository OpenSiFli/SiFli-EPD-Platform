/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_reader_layout.h"
#include "epd_compute_protocol.h"

static int32_t open_reader(void **state, const epd_compute_wire_t *request)
{
    (void)request;
    *state = (void *)1;
    return EPD_COMPUTE_OK;
}

static int32_t step_reader(void *state, const epd_compute_wire_t *request)
{
    (void)state;
    epd_reader_batch_t *batch = epd_compute_resolve(request, request->params, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    if (!batch || request->params.length != sizeof(*batch) || request->result.region != request->params.region ||
        request->result.offset != request->params.offset || request->result.length != sizeof(*batch) ||
        !request->step_limit || batch->limit > request->step_limit) return EPD_COMPUTE_INVALID;
    if (epd_compute_signals(request) & EPD_COMPUTE_CANCEL) return EPD_COMPUTE_CANCELLED;
    return epd_reader_layout(batch) ? EPD_COMPUTE_COMPONENT_ERROR : EPD_COMPUTE_OK;
}

static void close_reader(void *state, const epd_compute_wire_t *request)
{ (void)state; (void)request; }

const epd_compute_ops_t epd_reader_remote_ops = {open_reader, step_reader, NULL, close_reader};
