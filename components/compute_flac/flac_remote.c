/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_flac.h"
#include <FLAC/stream_decoder.h>
#include <rtthread.h>
#include <string.h>
#include <limits.h>

typedef struct
{
    FLAC__StreamDecoder *decoder;
    const epd_compute_wire_t *wire;
    const uint8_t *input;
    uint32_t bytes, cursor, max_blocksize, rate, channels, bits;
    int16_t *pcm;
    uint32_t capacity;
    epd_flac_result_t decoded;
    bool exhausted, metadata;
} flac_remote_t;

static bool interrupted(flac_remote_t *s)
{
    return s->wire && (epd_compute_signals(s->wire) & EPD_COMPUTE_CANCEL);
}

static FLAC__StreamDecoderReadStatus read_input(const FLAC__StreamDecoder *decoder,
    FLAC__byte data[], size_t *bytes, void *context)
{
    (void)decoder;
    flac_remote_t *s = context;
    if (!*bytes || interrupted(s)) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    size_t available = s->bytes - s->cursor;
    if (*bytes > available) *bytes = available;
    if (!*bytes) { s->exhausted = true; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM; }
    memcpy(data, s->input + s->cursor, *bytes);
    s->cursor += (uint32_t)*bytes;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderTellStatus tell_input(const FLAC__StreamDecoder *decoder,
    FLAC__uint64 *offset, void *context)
{
    (void)decoder;
    *offset = ((flac_remote_t *)context)->cursor;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static void read_metadata(const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata, void *context)
{
    (void)decoder;
    flac_remote_t *s = context;
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) return;
    const FLAC__StreamMetadata_StreamInfo *info = &metadata->data.stream_info;
    if ((info->channels != 1 && info->channels != 2) ||
        (info->bits_per_sample != 16 && info->bits_per_sample != 24) ||
        !info->sample_rate || !info->max_blocksize || info->max_blocksize > 65535 ||
        info->min_blocksize > info->max_blocksize) return;
    s->max_blocksize = info->max_blocksize;
    s->rate = info->sample_rate;
    s->channels = info->channels;
    s->bits = info->bits_per_sample;
    s->metadata = true;
}

static void decoder_error(const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus error, void *context)
{
    (void)decoder;
    flac_remote_t *s = context;
    if (!s->decoded.decoder_error) s->decoded.decoder_error = (int32_t)error + 1;
}

static FLAC__StreamDecoderWriteStatus write_frame(const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame, const FLAC__int32 *const input[], void *context)
{
    (void)decoder;
    flac_remote_t *s = context;
    uint32_t count = frame->header.blocksize;
    if (interrupted(s) || s->decoded.samples || !count || count > s->max_blocksize ||
        count > s->capacity || frame->header.channels != s->channels ||
        frame->header.sample_rate != s->rate || frame->header.bits_per_sample != s->bits ||
        frame->header.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER)
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    int64_t divisor = (s->bits == 24 ? 256 : 1) * s->channels;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!(i & 255) && interrupted(s)) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        int64_t value = input[0][i];
        if (s->channels == 2) value += input[1][i];
        value = value < 0 ? -((-value + divisor / 2) / divisor) : (value + divisor / 2) / divisor;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        s->pcm[i] = (int16_t)value;
    }
    s->decoded.samples = count;
    s->decoded.sample_low = (uint32_t)frame->header.number.sample_number;
    s->decoded.sample_high = (uint32_t)(frame->header.number.sample_number >> 32);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static int32_t open_decoder(void **state, const epd_compute_wire_t *wire)
{
    *state = NULL;
    const epd_flac_config_t *config = epd_compute_resolve(wire, wire->params, EPD_COMPUTE_READ);
    if (!config || wire->params.length != sizeof(*config)) return EPD_COMPUTE_INVALID;
    flac_remote_t *s = rt_calloc(1, sizeof(*s));
    if (!s) return EPD_COMPUTE_NO_MEMORY;
    s->decoder = FLAC__stream_decoder_new();
    if (!s->decoder) { rt_free(s); return EPD_COMPUTE_NO_MEMORY; }
    /* Each packet is a complete CRC-checked frame, not a whole-stream MD5 pass. */
    FLAC__stream_decoder_set_md5_checking(s->decoder, false);
    uint8_t metadata[8 + EPD_FLAC_STREAMINFO_BYTES] = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
    memcpy(metadata + 8, config->streaminfo, EPD_FLAC_STREAMINFO_BYTES);
    s->wire = wire; s->input = metadata; s->bytes = sizeof(metadata);
    FLAC__StreamDecoderInitStatus init = FLAC__stream_decoder_init_stream(s->decoder,
        read_input, NULL, tell_input, NULL, NULL, write_frame, read_metadata, decoder_error, s);
    bool ok = init == FLAC__STREAM_DECODER_INIT_STATUS_OK &&
              FLAC__stream_decoder_process_until_end_of_metadata(s->decoder) && s->metadata;
    s->input = NULL; s->wire = NULL;
    if (!ok)
    {
        int status = FLAC__stream_decoder_get_state(s->decoder) == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR ?
                     EPD_COMPUTE_NO_MEMORY : EPD_COMPUTE_UNSUPPORTED;
        FLAC__stream_decoder_delete(s->decoder); rt_free(s); return status;
    }
    *state = s;
    return EPD_COMPUTE_OK;
}

static int32_t step_decoder(void *state, const epd_compute_wire_t *wire)
{
    flac_remote_t *s = state;
    const epd_flac_request_t *p = epd_compute_resolve(wire, wire->params, EPD_COMPUTE_READ);
    epd_flac_result_t *result = epd_compute_resolve(wire, wire->result, EPD_COMPUTE_WRITE);
    if (!s || !p || !result || wire->params.length != sizeof(*p) ||
        wire->result.length != sizeof(*result) || p->end_of_input > 1) return EPD_COMPUTE_INVALID;
    const uint8_t *input = epd_compute_resolve(wire, p->input, EPD_COMPUTE_READ);
    int16_t *pcm = epd_compute_resolve(wire, p->pcm, EPD_COMPUTE_WRITE);
    if (!input || !pcm || p->pcm.length / sizeof(int16_t) < s->max_blocksize)
        return EPD_COMPUTE_INVALID;
    memset(result, 0, sizeof(*result));
    memset(&s->decoded, 0, sizeof(s->decoded));
    if (!FLAC__stream_decoder_flush(s->decoder)) return EPD_COMPUTE_COMPONENT_ERROR;
    s->wire = wire; s->input = input; s->bytes = p->input.length; s->cursor = 0;
    s->pcm = pcm; s->capacity = p->pcm.length / sizeof(int16_t); s->exhausted = false;
    bool ok = FLAC__stream_decoder_process_single(s->decoder);
    int status = EPD_COMPUTE_OK;
    FLAC__uint64 consumed = 0;
    if (interrupted(s)) status = EPD_COMPUTE_CANCELLED;
    else if (FLAC__stream_decoder_get_state(s->decoder) == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR)
        status = EPD_COMPUTE_NO_MEMORY;
    /* libFLAC may report LOST_SYNC after a short subframe read. The next step
     * replays the complete prefix, so defer that error until input is final. */
    else if (!s->decoded.samples && s->exhausted && !p->end_of_input)
        status = EPD_COMPUTE_MORE;
    else if (!ok || !s->decoded.samples || s->decoded.decoder_error ||
             !FLAC__stream_decoder_get_decode_position(s->decoder, &consumed) ||
             !consumed || consumed > p->input.length) status = EPD_COMPUTE_COMPONENT_ERROR;
    if (status == EPD_COMPUTE_OK)
    {
        s->decoded.consumed = (uint32_t)consumed;
        *result = s->decoded;
    }
    else result->decoder_error = s->decoded.decoder_error;
    s->wire = NULL; s->input = NULL; s->pcm = NULL;
    return status;
}

static int32_t reset_decoder(void *state, const epd_compute_wire_t *wire)
{
    (void)wire;
    flac_remote_t *s = state;
    return s && FLAC__stream_decoder_flush(s->decoder) ? EPD_COMPUTE_OK : EPD_COMPUTE_COMPONENT_ERROR;
}

static void close_decoder(void *state, const epd_compute_wire_t *wire)
{
    (void)wire;
    flac_remote_t *s = state;
    FLAC__stream_decoder_delete(s->decoder);
    rt_free(s);
}

const epd_compute_ops_t epd_flac_remote_ops = {open_decoder, step_decoder, reset_decoder, close_decoder};
