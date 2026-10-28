/* SPDX-FileCopyrightText: 2020-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 * Component adapter using the bundled Anyka library and its audio_3a initialization sequence.
 */
#include "epd_compute_memory.h"
#include <string.h>
#include "sdfilter.h"
#include "sdParamFactory.h"
#include "epd_anyka.h"

typedef char anyka_mic_count[(MAX_MIC_NUM == EPD_ANYKA_MIC_COUNT) ? 1 : -1];
typedef char anyka_frequency_count[(MAX_SINGLE_FREQUENCY_NUM == EPD_ANYKA_FREQUENCIES) ? 1 : -1];
typedef struct
{
    epd_anyka_config_t config;
    void *near_path, *far_path, *ssl_filter;
    T_pSD_PARAM_FACTORY near_factory, far_factory;
    char *near_text, *far_text;
    T_AUDIO_FILTER_INPUT ssl_input;
    T_SSL_EVENT ssl_output;
} anyka_state_t;

static T_pVOID allocate(T_U32 size) { return epd_compute_malloc(size); }
static T_VOID release(T_pVOID p) { epd_compute_free(p); }
static T_VOID silent_log(T_pCSTR format, ...) { (void)format; }
/* Processing uses the CPU, not DMA. Cross-core buffers are synchronized by
 * the compute transport; library-private state is never shared with HCPU. */
static void local_cache(T_U32 address, T_U32 size) { (void)address; (void)size; }
static void notify(T_ECHO_EVENT e, T_pVOID p, T_HANDLE h, T_U8 id)
{ (void)e; (void)p; (void)h; (void)id; }
static void dump(T_ECHO_DUMP_ITEM_ID e, T_S32 n, T_pCVOID p, T_pCVOID m, T_HANDLE h, T_U8 id)
{ (void)e; (void)n; (void)p; (void)m; (void)h; (void)id; }

static void close_anyka(void *state, const epd_compute_wire_t *wire)
{
    (void)wire;
    anyka_state_t *s = state;
    if (!s) return;
    if (s->ssl_filter) _SD_Filter_Close(s->ssl_filter);
    if (s->near_path) { _SD_Echo_Reset(s->near_path); _SD_Echo_Close(s->near_path); }
    if (s->far_path) { _SD_Echo_Reset(s->far_path); _SD_Echo_Close(s->far_path); }
    if (s->near_factory) SD_ParamFactory_Destroy(s->near_factory);
    if (s->far_factory) SD_ParamFactory_Destroy(s->far_factory);
    epd_compute_free(s->near_text);
    epd_compute_free(s->far_text);
    epd_compute_free(s);
}

static int copy_text(const epd_compute_wire_t *wire, epd_compute_ref_t ref, char **text)
{
    const char *source = epd_compute_resolve(wire, ref, EPD_COMPUTE_READ);
    if (!source || !ref.length || ref.length > INT32_MAX ||
        source[ref.length - 1] != '\0' || memchr(source, '\0', ref.length - 1))
        return EPD_COMPUTE_INVALID;
    *text = epd_compute_malloc(ref.length);
    if (!*text) return EPD_COMPUTE_NO_MEMORY;
    memcpy(*text, source, ref.length);
    return EPD_COMPUTE_OK;
}

static int open_ssl(anyka_state_t *s)
{
    const epd_anyka_ssl_config_t *p = &s->config.ssl;
    if (s->config.microphones < 2 || (p->dimensions != 2 && p->dimensions != 3) ||
        p->frequency_mode > 2 || !p->frame_samples || !p->sound_speed || !p->sources ||
        p->sources >= s->config.microphones || !p->snapshots ||
        p->frequency_count > MAX_SINGLE_FREQUENCY_NUM) return EPD_COMPUTE_INVALID;
    T_AUDIO_FILTER_INPUT *in = &s->ssl_input;
    in->strVersion = AUDIO_FILTER_VERSION_STRING;
    in->cb_fun.Malloc = allocate;
    in->cb_fun.Free = release;
    in->cb_fun.printf = silent_log;
    in->cb_fun.flushDCache = local_cache;
    in->m_info.m_Type = _SD_FILTER_SRP_SSL;
    in->m_info.m_SampleRate = s->config.sample_rate;
    in->m_info.m_Channels = s->config.microphones;
    in->m_info.m_BitsPerSample = 16;
    in->m_info.m_PrivateSize = sizeof(struct sd_param_srp_ssl);
    struct sd_param_srp_ssl *v = &in->m_info.m_Private.m_srp_ssl;
    v->dim = p->dimensions; v->freqMode = p->frequency_mode; v->frameSize = p->frame_samples;
    v->soundSpeed = p->sound_speed; v->srcNums = p->sources; v->numSnap = p->snapshots;
    v->minFreqHz = p->min_frequency; v->maxFreqHz = p->max_frequency;
    v->singleFreqHz.numFreqHz = p->frequency_count;
    for (uint32_t i = 0; i < p->frequency_count; ++i) v->singleFreqHz.freqHz[i] = p->frequencies[i];
    v->arrayGeometry.numMic = s->config.microphones;
    for (uint32_t i = 0; i < s->config.microphones; ++i)
    {
        v->arrayGeometry.micsPos[i].x = p->microphones[i].x;
        v->arrayGeometry.micsPos[i].y = p->microphones[i].y;
        v->arrayGeometry.micsPos[i].z = p->microphones[i].z;
    }
    v->thresh = p->threshold; v->level = p->level;
    in->ploginInfo = _SD_SrpSSL_login(AK_NULL);
    s->ssl_filter = _SD_Filter_Open(in);
    if (!s->ssl_filter) return EPD_COMPUTE_COMPONENT_ERROR;
    return _SD_Filter_SetParam(s->ssl_filter, &in->m_info) == AK_TRUE ?
        EPD_COMPUTE_OK : EPD_COMPUTE_COMPONENT_ERROR;
}

static int32_t open_anyka(void **state, const epd_compute_wire_t *wire)
{
    *state = NULL;
    const epd_anyka_open_t *p = epd_compute_resolve(wire, wire->params, EPD_COMPUTE_READ);
    if (!p || wire->params.length != sizeof(*p) || !p->config.sample_rate ||
        p->config.sample_rate > UINT16_MAX || !p->config.microphones ||
        p->config.microphones > MAX_MIC_NUM || p->config.far_enabled > 1 || p->config.ssl_enabled > 1)
        return EPD_COMPUTE_INVALID;
    anyka_state_t *s = epd_compute_calloc(1, sizeof(*s));
    if (!s) return EPD_COMPUTE_NO_MEMORY;
    s->config = p->config;
    int status = copy_text(wire, p->near_parameters, &s->near_text);
    if (status != EPD_COMPUTE_OK) goto fail;
    if (s->config.far_enabled)
    {
        status = copy_text(wire, p->far_parameters, &s->far_text);
        if (status != EPD_COMPUTE_OK) goto fail;
    }
    T_SDLIB_PLATFORM_DEPENDENT_LIST *cb = _SD_GetPlatformDependentList();
    status = EPD_COMPUTE_COMPONENT_ERROR;
    if (!cb) goto fail;
    cb->Malloc = allocate; cb->Free = release; cb->printf = silent_log; cb->flushDCache = local_cache;
    T_ECHO_IN_INFO in = {0};
    in.strVersion = AUDIO_FILTER_VERSION_STRING;
    in.cb_fun.Malloc = allocate; in.cb_fun.Free = release; in.cb_fun.Printf = silent_log;
    in.cb_fun.flushDCache = local_cache; in.cb_fun.notify = notify; in.cb_fun.dump = dump;
    in.m_Channels = 1; in.m_BitsPerSample = 16;
    in.m_jitterBufLen = 5 * EPD_ANYKA_FRAME_BYTES * MAX_MIC_NUM;
    in.m_SampleRate = in.m_hwSampleRate = s->config.sample_rate;
    if (s->config.far_enabled)
    {
        in.m_pathId = 1; in.m_mode = ECHO_MODE_AO_NORMAL;
        s->far_path = _SD_Echo_Open(&in);
        if (!s->far_path) goto fail;
        s->far_factory = SD_ParamFactory_Create_ByCmdLine(s->far_text, p->far_parameters.length);
        if (!s->far_factory || _SD_Echo_SetFarPathParam(s->far_path, s->far_factory) != AK_TRUE) goto fail;
    }
    in.m_pathId = 0; in.m_syncedInputs = 0; in.m_syncBufLen = 12000; in.m_syncCompensate = 10;
    in.m_mode = s->config.microphones > 1 ? ECHO_MODE_AI_MIC_ARRAY_PROCESS : ECHO_MODE_AI_NORMAL;
    s->near_path = _SD_Echo_Open(&in);
    if (!s->near_path) goto fail;
    _SD_EQ_login(AK_NULL); _SD_NR_login(AK_NULL); _SD_AEC_login(AK_NULL); _SD_ASLC_login(AK_NULL);
    if (s->config.microphones > 1) _SD_Denc_login(AK_NULL);
    s->near_factory = SD_ParamFactory_Create_ByCmdLine(s->near_text, p->near_parameters.length);
    if (!s->near_factory || _SD_Echo_SetNearPathParam(s->near_path, s->near_factory) != AK_TRUE) goto fail;
    if (s->config.ssl_enabled)
    {
        status = open_ssl(s);
        if (status != EPD_COMPUTE_OK) goto fail;
    }
    *state = s;
    return EPD_COMPUTE_OK;
fail:
    close_anyka(s, wire);
    return status;
}

static int run_ssl(anyka_state_t *s, void *input, uint32_t bytes, epd_anyka_result_t *r)
{
    if (!s->ssl_filter) return EPD_COMPUTE_UNSUPPORTED;
    if (bytes != s->config.ssl.frame_samples * s->config.microphones * sizeof(int16_t))
        return EPD_COMPUTE_INVALID;
    memset(&s->ssl_output, 0, sizeof(s->ssl_output));
    T_AUDIO_FILTER_BUF_STRC b = {0};
    b.buf_in = input; b.len_in = bytes;
    b.buf_out = &s->ssl_output; b.len_out = sizeof(s->ssl_output);
    int processed = _SD_Filter_Control(s->ssl_filter, &b);
    if (b.len_in > bytes) return EPD_COMPUTE_COMPONENT_ERROR;
    r->input_consumed = bytes - b.len_in;
    if (processed < 0) { r->error = processed; return EPD_COMPUTE_OK; }
    if (processed == 0) return EPD_COMPUTE_OK;
    if (s->ssl_output.sourcesNumbers > MAX_MIC_NUM - 1) return EPD_COMPUTE_COMPONENT_ERROR;
    r->sources = s->ssl_output.sourcesNumbers;
    for (uint32_t i = 0; i < r->sources; ++i)
    {
        r->directions[i].azimuth = s->ssl_output.soundSourceDirection[i].azimuth;
        r->directions[i].elevation = s->ssl_output.soundSourceDirection[i].elevation;
        r->directions[i].radius = s->ssl_output.soundSourceDirection[i].radius;
    }
    if (r->sources)
    {
        struct sd_param_denc denc = {0};
        if (_SD_Echo_GetDencParam(s->near_path, &denc) != AK_TRUE) return EPD_COMPUTE_COMPONENT_ERROR;
        denc.targetSpherical = s->ssl_output.soundSourceDirection[0];
        denc.numInterf = 0;
        if (_SD_Echo_SetDencParam(s->near_path, 1, &denc) != AK_TRUE) return EPD_COMPUTE_COMPONENT_ERROR;
    }
    return EPD_COMPUTE_OK;
}

static int read_pcm(anyka_state_t *s, uint32_t operation, void *output, epd_anyka_result_t *r)
{
    T_AUDIO_FILTER_TS timestamp = 0;
    int n = operation == EPD_ANYKA_UPLINK ?
        _SD_Echo_GetResult(s->near_path, output, EPD_ANYKA_FRAME_BYTES, &timestamp, 1) :
        _SD_Echo_GetDacStream(s->far_path, output, EPD_ANYKA_FRAME_BYTES, &timestamp, 1);
    if (n < 0) r->error = n;
    else if (n > EPD_ANYKA_FRAME_BYTES) return EPD_COMPUTE_COMPONENT_ERROR;
    else { r->output_bytes = n; r->timestamp_us = timestamp; }
    return EPD_COMPUTE_OK;
}

static int drain_busy(anyka_state_t *s, uint32_t operation, void *output, epd_anyka_result_t *r)
{
    /* The caller retries unconsumed input, but must receive already pending
     * output so a full vendor queue cannot prevent its own draining. */
    int status = read_pcm(s, operation, output, r);
    return status == EPD_COMPUTE_OK && !r->error ? EPD_COMPUTE_BUSY : status;
}

static int32_t step_anyka(void *state, const epd_compute_wire_t *wire)
{
    anyka_state_t *s = state;
    const epd_anyka_request_t *p = epd_compute_resolve(wire, wire->params, EPD_COMPUTE_READ);
    epd_anyka_result_t *r = epd_compute_resolve(wire, wire->result, EPD_COMPUTE_WRITE);
    if (!s || !p || !r || wire->params.length != sizeof(*p) || wire->result.length != sizeof(*r))
        return EPD_COMPUTE_INVALID;
    memset(r, 0, sizeof(*r));
    if (epd_compute_signals(wire) & EPD_COMPUTE_CANCEL) return EPD_COMPUTE_CANCELLED;
    void *input = epd_compute_resolve(wire, p->input, EPD_COMPUTE_READ);
    if (!input || !p->input.length || ((uintptr_t)input & 1)) return EPD_COMPUTE_INVALID;
    if (p->operation == EPD_ANYKA_SSL) return run_ssl(s, input, p->input.length, r);
    void *output = epd_compute_resolve(wire, p->output, EPD_COMPUTE_WRITE);
    if (!output || p->output.length < EPD_ANYKA_FRAME_BYTES || ((uintptr_t)output & 1))
        return EPD_COMPUTE_INVALID;
    int n;
    if (p->operation == EPD_ANYKA_UPLINK)
    {
        void *reference = epd_compute_resolve(wire, p->reference, EPD_COMPUTE_READ);
        if (!reference || ((uintptr_t)reference & 1) || p->reference.length != EPD_ANYKA_FRAME_BYTES ||
            p->input.length != EPD_ANYKA_FRAME_BYTES * s->config.microphones ||
            p->timestamp_us > UINT64_MAX - 10000000ULL / s->config.sample_rate) return EPD_COMPUTE_INVALID;
        /* Check both input queues before accepting either half of this pair. */
        n = _SD_Echo_FillDacLoopback(s->near_path, NULL, 0, 0, 1);
        if (n < 0) { r->error = n; return EPD_COMPUTE_OK; }
        if ((uint32_t)n < p->reference.length) return drain_busy(s, p->operation, output, r);
        n = _SD_Echo_FillAdcStream(s->near_path, NULL, 0, 0, 1);
        if (n < 0) { r->error = n; return EPD_COMPUTE_OK; }
        if ((uint32_t)n < p->input.length) return drain_busy(s, p->operation, output, r);
        n = _SD_Echo_FillDacLoopback(s->near_path, reference, p->reference.length, p->timestamp_us, 1);
        if (n < 0) { r->error = n; return EPD_COMPUTE_OK; }
        r->reference_consumed = n;
        if ((uint32_t)n != p->reference.length) return EPD_COMPUTE_COMPONENT_ERROR;
        n = _SD_Echo_FillAdcStream(s->near_path, input, p->input.length,
                                  p->timestamp_us + 10000000ULL / s->config.sample_rate, 1);
        if (n < 0) { r->error = n; return EPD_COMPUTE_OK; }
        r->input_consumed = n;
        if ((uint32_t)n != p->input.length) return EPD_COMPUTE_COMPONENT_ERROR;
    }
    else if (p->operation == EPD_ANYKA_DOWNLINK)
    {
        if (!s->far_path) return EPD_COMPUTE_UNSUPPORTED;
        if (p->input.length != EPD_ANYKA_FRAME_BYTES) return EPD_COMPUTE_INVALID;
        n = _SD_Echo_FillFarStream(s->far_path, input, p->input.length, p->timestamp_us, 1);
        if (n < 0) { r->error = n; return EPD_COMPUTE_OK; }
        r->input_consumed = n;
        if (!n) return drain_busy(s, p->operation, output, r);
        if ((uint32_t)n != p->input.length) return EPD_COMPUTE_COMPONENT_ERROR;
    }
    else return EPD_COMPUTE_UNSUPPORTED;
    return read_pcm(s, p->operation, output, r);
}

static int32_t reset_anyka(void *state, const epd_compute_wire_t *wire)
{
    (void)wire;
    anyka_state_t *s = state;
    int a = _SD_Echo_Reset(s->near_path);
    int b = s->far_path ? _SD_Echo_Reset(s->far_path) : AK_TRUE;
    int c = s->ssl_filter ? _SD_Filter_Reset(s->ssl_filter) : AK_TRUE;
    return a == AK_TRUE && b == AK_TRUE && c == AK_TRUE ? EPD_COMPUTE_OK : EPD_COMPUTE_COMPONENT_ERROR;
}

const epd_compute_ops_t epd_anyka_remote_ops = {open_anyka, step_anyka, reset_anyka, close_anyka};
