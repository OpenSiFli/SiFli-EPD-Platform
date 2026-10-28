/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute_memory.h"
#include <string.h>
#include "opus.h"
#include "epd_opus.h"

typedef struct
{
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    int32_t rate, channels;
} opus_state_t;

static void close_opus(void *state, const epd_compute_wire_t *wire)
{
    (void)wire;
    opus_state_t *s = state;
    if (!s) return;
    /* Decoder state belongs to the component's private ACPU heap. */
    epd_compute_free(s->encoder);
    epd_compute_free(s->decoder);
    epd_compute_free(s);
}

static int32_t open_opus(void **state, const epd_compute_wire_t *wire)
{
    *state = NULL;
    const epd_opus_config_t *p = epd_compute_resolve(wire, wire->params, EPD_COMPUTE_READ);
    if (!p || wire->params.length != sizeof(*p) || !p->directions ||
        (p->directions & ~(EPD_OPUS_ENCODER | EPD_OPUS_DECODER)) ||
        (p->channels != 1 && p->channels != 2) ||
        (p->sample_rate != 8000 && p->sample_rate != 12000 && p->sample_rate != 16000 &&
         p->sample_rate != 24000 && p->sample_rate != 48000)) return EPD_COMPUTE_INVALID;
    opus_state_t *s = epd_compute_calloc(1, sizeof(*s));
    if (!s) return EPD_COMPUTE_NO_MEMORY;
    int status = EPD_COMPUTE_NO_MEMORY;
    s->rate = p->sample_rate;
    s->channels = p->channels;
    if (p->directions & EPD_OPUS_ENCODER)
    {
        int size = opus_encoder_get_size(p->channels);
        if (size <= 0 || !(s->encoder = epd_compute_malloc(size))) goto fail;
        if (opus_encoder_init(s->encoder, p->sample_rate, p->channels, p->application) != OPUS_OK)
        { status = EPD_COMPUTE_INVALID; goto fail; }
    }
    if (p->directions & EPD_OPUS_DECODER)
    {
        int size = opus_decoder_get_size(p->channels);
        if (size <= 0 || !(s->decoder = epd_compute_malloc(size))) goto fail;
        if (opus_decoder_init(s->decoder, p->sample_rate, p->channels) != OPUS_OK)
        { status = EPD_COMPUTE_INVALID; goto fail; }
    }
    *state = s;
    return EPD_COMPUTE_OK;
fail:
    close_opus(s, wire);
    return status;
}

static int configure(opus_state_t *s, uint32_t control, int32_t value)
{
    if (control == EPD_OPUS_DECODER_GAIN)
        return s->decoder ? opus_decoder_ctl(s->decoder, OPUS_SET_GAIN(value)) : OPUS_INVALID_STATE;
    if (!s->encoder) return OPUS_INVALID_STATE;
    switch (control)
    {
    case EPD_OPUS_BITRATE: return opus_encoder_ctl(s->encoder, OPUS_SET_BITRATE(value));
    case EPD_OPUS_COMPLEXITY: return opus_encoder_ctl(s->encoder, OPUS_SET_COMPLEXITY(value));
    case EPD_OPUS_VBR: return opus_encoder_ctl(s->encoder, OPUS_SET_VBR(value));
    case EPD_OPUS_VBR_CONSTRAINT: return opus_encoder_ctl(s->encoder, OPUS_SET_VBR_CONSTRAINT(value));
    case EPD_OPUS_DTX: return opus_encoder_ctl(s->encoder, OPUS_SET_DTX(value));
    case EPD_OPUS_FEC: return opus_encoder_ctl(s->encoder, OPUS_SET_INBAND_FEC(value));
    case EPD_OPUS_PACKET_LOSS: return opus_encoder_ctl(s->encoder, OPUS_SET_PACKET_LOSS_PERC(value));
    case EPD_OPUS_SIGNAL: return opus_encoder_ctl(s->encoder, OPUS_SET_SIGNAL(value));
    case EPD_OPUS_BANDWIDTH: return opus_encoder_ctl(s->encoder, OPUS_SET_BANDWIDTH(value));
    case EPD_OPUS_MAX_BANDWIDTH: return opus_encoder_ctl(s->encoder, OPUS_SET_MAX_BANDWIDTH(value));
    case EPD_OPUS_LSB_DEPTH: return opus_encoder_ctl(s->encoder, OPUS_SET_LSB_DEPTH(value));
    case EPD_OPUS_FRAME_DURATION: return opus_encoder_ctl(s->encoder, OPUS_SET_EXPERT_FRAME_DURATION(value));
    case EPD_OPUS_PREDICTION_DISABLED: return opus_encoder_ctl(s->encoder, OPUS_SET_PREDICTION_DISABLED(value));
    default: return OPUS_UNIMPLEMENTED;
    }
}

static int32_t step_opus(void *state, const epd_compute_wire_t *wire)
{
    opus_state_t *s = state;
    const epd_opus_request_t *p = epd_compute_resolve(wire, wire->params, EPD_COMPUTE_READ);
    epd_opus_result_t *r = epd_compute_resolve(wire, wire->result, EPD_COMPUTE_WRITE);
    if (!s || !p || !r || wire->params.length != sizeof(*p) || wire->result.length != sizeof(*r))
        return EPD_COMPUTE_INVALID;
    memset(r, 0, sizeof(*r));
    if (epd_compute_signals(wire) & EPD_COMPUTE_CANCEL) return EPD_COMPUTE_CANCELLED;
    if (p->operation == EPD_OPUS_SET)
    { r->value = configure(s, p->control, p->value); return EPD_COMPUTE_OK; }
    if (p->frame_samples <= 0 || p->frame_samples > s->rate * 120 / 1000 ||
        p->input.length > INT32_MAX || p->output.length > INT32_MAX) return EPD_COMPUTE_INVALID;
    uint32_t pcm_bytes = (uint32_t)p->frame_samples * s->channels * sizeof(opus_int16);
    void *in = p->input.length ? epd_compute_resolve(wire, p->input, EPD_COMPUTE_READ) : NULL;
    void *out = epd_compute_resolve(wire, p->output, EPD_COMPUTE_WRITE);
    if ((p->input.length && !in) || !out || !p->output.length) return EPD_COMPUTE_INVALID;
    if (p->operation == EPD_OPUS_ENCODE)
    {
        if (!s->encoder) return EPD_COMPUTE_UNSUPPORTED;
        if (p->input.length != pcm_bytes || ((uintptr_t)in & 1)) return EPD_COMPUTE_INVALID;
        r->value = opus_encode(s->encoder, in, p->frame_samples, out, p->output.length);
        if (r->value >= 0) r->output_bytes = r->value;
    }
    else if (p->operation == EPD_OPUS_DECODE)
    {
        if (!s->decoder) return EPD_COMPUTE_UNSUPPORTED;
        if (p->output.length < pcm_bytes || ((uintptr_t)out & 1) || (p->fec != 0 && p->fec != 1))
            return EPD_COMPUTE_INVALID;
        r->value = opus_decode(s->decoder, in, p->input.length, out, p->frame_samples, p->fec);
        if (r->value >= 0) r->output_bytes = r->value * s->channels * sizeof(opus_int16);
    }
    else return EPD_COMPUTE_UNSUPPORTED;
    return EPD_COMPUTE_OK;
}

static int32_t reset_opus(void *state, const epd_compute_wire_t *wire)
{
    (void)wire;
    opus_state_t *s = state;
    int a = s->encoder ? opus_encoder_ctl(s->encoder, OPUS_RESET_STATE) : OPUS_OK;
    int b = s->decoder ? opus_decoder_ctl(s->decoder, OPUS_RESET_STATE) : OPUS_OK;
    return a == OPUS_OK && b == OPUS_OK ? EPD_COMPUTE_OK : EPD_COMPUTE_COMPONENT_ERROR;
}

const epd_compute_ops_t epd_opus_remote_ops = {open_opus, step_opus, reset_opus, close_opus};
