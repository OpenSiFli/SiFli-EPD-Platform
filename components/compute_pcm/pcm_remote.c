/* SPDX-License-Identifier: Apache-2.0 */
#include "pcm_internal.h"
static bool cancelled(void *context)
{ return (epd_compute_signals(context) & EPD_COMPUTE_CANCEL) != 0; }
static int32_t open_pcm(void **state, const epd_compute_wire_t *wire)
{ (void)wire; *state = (void *)(uintptr_t)1; return EPD_COMPUTE_OK; }
static int32_t step_pcm(void *state, const epd_compute_wire_t *wire)
{
    (void)state;
    const epd_pcm_request_t *p = epd_compute_resolve(wire, wire->params, EPD_COMPUTE_READ);
    if (!p || wire->params.length != sizeof(*p) || !epd_pcm_format_valid(p->format) ||
        !p->samples || p->samples > EPD_PCM_MAX_SAMPLES ||
        p->input.length != p->samples * p->format.channels * (p->format.bits / 8) ||
        p->output.length != p->samples * sizeof(int16_t)) return EPD_COMPUTE_INVALID;
    const void *input = epd_compute_resolve(wire, p->input, EPD_COMPUTE_READ);
    int16_t *output = epd_compute_resolve(wire, p->output, EPD_COMPUTE_WRITE);
    return epd_pcm_convert_local(p->format, input, p->samples, output, cancelled, (void *)wire);
}
static int32_t reset_pcm(void *state, const epd_compute_wire_t *wire)
{ (void)state; (void)wire; return EPD_COMPUTE_OK; }
static void close_pcm(void *state, const epd_compute_wire_t *wire) { (void)state; (void)wire; }
const epd_compute_ops_t epd_pcm_remote_ops = {open_pcm, step_pcm, reset_pcm, close_pcm};
