/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_audio.h"
#include <string.h>
#include <limits.h>
#ifdef EPD_BOARD_SF32LB57_EPD
#include "bsp_speech_profile.h"
#endif

#ifdef AUDIO_USING_MANAGER
#include "epd_pcm_output.h"

struct speech_audio_output {
    epd_pcm_output_t *pcm;
    speech_cancel_fn cancel;
    void *user;
    const void *retry;
    size_t retry_bytes;
    uint32_t primer_bytes, started, retry_since;
    bool switching;
};

static bool output_cancelled(speech_audio_output_t *s)
{ return s->cancel && s->cancel(s->user); }

static epd_speech_error_t output_error(int n)
{ return n == -2 ? EPD_SPEECH_INVALID : EPD_SPEECH_OUTPUT_INTERRUPTED; }

epd_speech_error_t speech_audio_output_open(speech_cancel_fn cancel, void *user,
                                          speech_audio_output_t **out)
{
    if (!out) return EPD_SPEECH_INVALID;
    *out = NULL;
    if (cancel && cancel(user)) return EPD_SPEECH_CANCELLED;
    speech_audio_output_t *s = speech_alloc(sizeof(*s));
    if (!s) return EPD_SPEECH_NO_MEMORY;
    memset(s, 0, sizeof(*s));
    s->cancel = cancel; s->user = user;
    s->pcm = epd_pcm_output_open_controlled(AUDIO_TYPE_LOCAL_MUSIC, EPD_SPEECH_SAMPLE_RATE, NULL, NULL);
    if (!s->pcm) { speech_free(s); return EPD_SPEECH_OUTPUT_INTERRUPTED; }
    if (output_cancelled(s))
    { speech_audio_output_close(s); return EPD_SPEECH_CANCELLED; }
    s->primer_bytes = EPD_SPEECH_SAMPLE_RATE * 2u * EPD_PCM_STARTUP_SILENCE_MS / 1000u;
    s->started = speech_now_ms();
    *out = s;
    return EPD_SPEECH_OK;
}

int speech_audio_output_write(speech_audio_output_t *s, const void *pcm, size_t bytes)
{
    static const int16_t silence[512] = {0};
    if (!s || !pcm || !bytes || bytes > INT_MAX || bytes % 2) return -EPD_SPEECH_INVALID;
    if (output_cancelled(s)) return -EPD_SPEECH_CANCELLED;
    if (s->retry && (pcm != s->retry || bytes != s->retry_bytes)) return -EPD_SPEECH_INVALID;
    if (s->retry && speech_now_ms() - s->retry_since >= 2000u) return -EPD_SPEECH_OUTPUT_TIMEOUT;
    if (s->switching) return -EPD_SPEECH_BUSY;
    bool priming = s->primer_bytes != 0;
    uint32_t count = priming ? s->primer_bytes : (uint32_t)bytes;
    if (priming && count > sizeof(silence)) count = sizeof(silence);
    int n = epd_pcm_output_write(s->pcm, priming ? silence : pcm, count);
    if (n == -3)
    {
        s->switching = true;
        if (!s->retry) s->retry_since = speech_now_ms();
        s->retry = pcm; s->retry_bytes = bytes;
        return -EPD_SPEECH_BUSY;
    }
    if (n < 0) return -(int)output_error(n);
    if (priming)
    {
        s->primer_bytes -= (uint32_t)n;
        if (speech_now_ms() - s->started >= 2000u) return -EPD_SPEECH_OUTPUT_TIMEOUT;
        n = 0;
    }
    if (!n)
    {
        if (!s->retry) s->retry_since = speech_now_ms();
        s->retry = pcm; s->retry_bytes = bytes;
    }
    else { s->retry = NULL; s->retry_bytes = 0; }
    return n;
}

epd_speech_error_t speech_audio_output_drain(speech_audio_output_t *s)
{
    if (!s) return EPD_SPEECH_INVALID;
    if (s->retry) return EPD_SPEECH_BUSY;
    uint32_t started = speech_now_ms();
    for (;;)
    {
        if (output_cancelled(s)) return EPD_SPEECH_CANCELLED;
        int n = epd_pcm_output_drain_step(s->pcm, speech_now_ms());
        if (n < 0) return output_error(n);
        if (n) return EPD_SPEECH_OK;
        if (speech_now_ms() - started >= 2000u) return EPD_SPEECH_OUTPUT_TIMEOUT;
        rt_thread_mdelay(5);
    }
}

epd_speech_error_t speech_audio_output_handoff(speech_audio_output_t *s)
{
    if (!s) return EPD_SPEECH_INVALID;
    if (!s->switching) return EPD_SPEECH_OK;
    const void *retry = s->retry;
    size_t bytes = s->retry_bytes;
    s->retry = NULL;
    epd_speech_error_t error = speech_audio_output_drain(s);
    s->retry = retry; s->retry_bytes = bytes;
    if (error) return error;
    if (epd_pcm_output_close(s->pcm)) return EPD_SPEECH_OUTPUT_INTERRUPTED;
    s->pcm = NULL;
    if (output_cancelled(s)) return EPD_SPEECH_CANCELLED;
    s->pcm = epd_pcm_output_open_controlled(AUDIO_TYPE_LOCAL_MUSIC, EPD_SPEECH_SAMPLE_RATE, NULL, NULL);
    if (!s->pcm) return EPD_SPEECH_OUTPUT_INTERRUPTED;
    s->primer_bytes = EPD_SPEECH_SAMPLE_RATE * 2u * EPD_PCM_STARTUP_SILENCE_MS / 1000u;
    s->started = speech_now_ms();
    s->switching = false;
    return EPD_SPEECH_OK;
}

void speech_audio_output_close(speech_audio_output_t *s)
{
    if (!s) return;
    /* SDK close owns callback lifetime; retain state until it has completed. */
    while (epd_pcm_output_close(s->pcm)) rt_thread_mdelay(5);
    speech_free(s);
}
#else
epd_speech_error_t speech_audio_output_open(speech_cancel_fn cancel, void *user, speech_audio_output_t **out)
{ (void)cancel; (void)user; if (out) *out = NULL; return EPD_SPEECH_UNSUPPORTED; }
int speech_audio_output_write(speech_audio_output_t *s, const void *pcm, size_t bytes)
{ (void)s; (void)pcm; (void)bytes; return -EPD_SPEECH_UNSUPPORTED; }
epd_speech_error_t speech_audio_output_drain(speech_audio_output_t *s)
{ (void)s; return EPD_SPEECH_UNSUPPORTED; }
epd_speech_error_t speech_audio_output_handoff(speech_audio_output_t *s)
{ (void)s; return EPD_SPEECH_UNSUPPORTED; }
void speech_audio_output_close(speech_audio_output_t *s) { (void)s; }
#endif

/* The board selects the physical input channel; DSP is an explicit option. */
#if defined(AUDIO_USING_MANAGER) && defined(BSP_USING_PDM1) && \
    defined(EPD_SPEECH_INPUT_CHANNEL) && \
    (!EPD_SPEECH_USE_ANYKA || (defined(EPD_SPEECH_ANYKA_COMPONENT) && defined(EPD_SPEECH_ANYKA_NEAR_PARAMETERS)))
#include <rthw.h>
#include "bsp_microphone.h"
#if EPD_SPEECH_USE_ANYKA
#include "epd_anyka.h"
#include "epd_compute.h"
#endif
typedef char speech_input_channel_valid[(EPD_SPEECH_INPUT_CHANNEL < 2) ? 1 : -1];

#define RAW_FRAME 640u
#define MONO_FRAME 320u
#define WARMUP_BYTES 12800u

typedef struct {
    uint8_t *data;
    uint32_t capacity, read, write;
} audio_ring_t;
struct speech_audio_input {
    audio_ring_t raw, send;
    rt_sem_t wake, exited;
    speech_cancel_fn cancel;
    void *user;
    audio_client_t client;
#if EPD_SPEECH_USE_ANYKA
    epd_anyka_t dsp;
#endif
    volatile bool abort, finish, finished, ready, armed, duration_limit;
    volatile epd_speech_error_t error;
    uint32_t warmup, samples, processed, produced, delivered, sent, limit_ms;
    uint32_t started, ready_at, last_callback, raw_high_water, send_high_water;
};
static bool input_in_use;

static uint32_t ring_size(const audio_ring_t *r) { return r->write - r->read; }
/* Called with interrupts masked. Each ring has one producer and one consumer. */
static void ring_copy(audio_ring_t *r, void *data, uint32_t bytes, bool write)
{
    uint32_t offset = (write ? r->write : r->read) % r->capacity;
    uint32_t first = r->capacity - offset;
    if (first > bytes) first = bytes;
    if (write)
    {
        memcpy(r->data + offset, data, first);
        memcpy(r->data, (uint8_t *)data + first, bytes - first);
        r->write += bytes;
    }
    else
    {
        memcpy(data, r->data + offset, first);
        memcpy((uint8_t *)data + first, r->data, bytes - first);
        r->read += bytes;
    }
}

static bool input_cancelled(void *context)
{
    speech_audio_input_t *s = context;
    return s->abort || (s->cancel && s->cancel(s->user));
}

static void input_fail(speech_audio_input_t *s, epd_speech_error_t error)
{ if (!s->error) s->error = error; }

static int receive_audio(audio_server_callback_cmt_t command, void *user, uint32_t reserved)
{
    speech_audio_input_t *s = user;
    if (command == as_callback_cmd_suspended || command == as_callback_cmd_resumed)
    { input_fail(s, EPD_SPEECH_INPUT_UNAVAILABLE); rt_sem_release(s->wake); return 0; }
    if (command != as_callback_cmd_data_coming) return 0;
    const audio_server_coming_data_t *data = (const void *)(uintptr_t)reserved;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!s->armed || s->abort || s->finish || s->error) goto done;
    if (!data || !data->data || data->data_len != RAW_FRAME || data->reserved)
    { input_fail(s, EPD_SPEECH_INPUT_OVERRUN); goto done; }
    uint32_t now = speech_now_ms();
    if (s->warmup)
    {
        s->warmup -= RAW_FRAME;
        if (!s->warmup)
        {
            s->ready_at = now;
            s->ready = true;
        }
        goto done;
    }
    if (s->samples && now - s->last_callback > 30u)
    { input_fail(s, EPD_SPEECH_INPUT_OVERRUN); goto done; }
    if (!s->samples) s->started = now - 10u;
    s->last_callback = now;
    uint32_t elapsed = now - s->started;
    uint32_t remaining = s->limit_ms * 16u - s->samples;
    uint32_t count = RAW_FRAME / 4u;
    if (remaining < count) count = remaining;
    if (elapsed > s->limit_ms)
    {
        uint32_t over = (elapsed - s->limit_ms) * 16u;
        count = over >= count ? 0 : count - over;
    }
    if (ring_size(&s->raw) + count * 4u > s->raw.capacity)
    { input_fail(s, EPD_SPEECH_INPUT_OVERRUN); goto done; }
    ring_copy(&s->raw, (void *)data->data, count * 4u, true);
    s->samples += count;
    if (ring_size(&s->raw) > s->raw_high_water) s->raw_high_water = ring_size(&s->raw);
    if (s->samples == s->limit_ms * 16u || elapsed >= s->limit_ms)
        s->finish = s->duration_limit = true;
done:
    rt_hw_interrupt_enable(level);
    rt_sem_release(s->wake);
    return 0;
}

static bool backlog_ok(speech_audio_input_t *s)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t elapsed = speech_now_ms() - s->started;
    bool bad = s->samples &&
        ((s->samples > s->processed && elapsed > s->processed / 16u + 200u) ||
         (s->samples * 2u > s->sent && elapsed > s->sent / 32u + 2000u));
    if (bad) input_fail(s, EPD_SPEECH_INPUT_OVERRUN);
    rt_hw_interrupt_enable(level);
    return !bad;
}

static bool process_frame(speech_audio_input_t *s, const int16_t *raw, uint32_t count, uint64_t timestamp)
{
#if EPD_SPEECH_USE_ANYKA
    (void)count;
    static const int16_t reference[160] = {0};
    int16_t mono[160];
    uint32_t began = speech_now_ms();
    for (;;)
    {
        if (input_cancelled(s)) { input_fail(s, EPD_SPEECH_CANCELLED); return false; }
        if (!backlog_ok(s)) return false;
        epd_anyka_result_t r;
        int status = epd_anyka_process(s->dsp, EPD_ANYKA_UPLINK, timestamp,
            raw, RAW_FRAME, reference, MONO_FRAME, mono, MONO_FRAME, &r);
        if (status == EPD_COMPUTE_CANCELLED)
        { input_fail(s, EPD_SPEECH_CANCELLED); return false; }
        if ((status != EPD_COMPUTE_OK && status != EPD_COMPUTE_BUSY) || r.error ||
            (r.input_consumed && r.input_consumed != RAW_FRAME) ||
            (r.reference_consumed && r.reference_consumed != MONO_FRAME) ||
            (!!r.input_consumed != !!r.reference_consumed) || r.output_bytes % 2)
        { input_fail(s, EPD_SPEECH_INPUT_UNAVAILABLE); return false; }
        rt_base_t level = rt_hw_interrupt_disable();
        uint32_t n = r.output_bytes;
        if (n > s->samples * 2u - s->produced) n = s->samples * 2u - s->produced;
        if (ring_size(&s->send) + n > s->send.capacity) input_fail(s, EPD_SPEECH_INPUT_OVERRUN);
        else
        {
            ring_copy(&s->send, mono, n, true);
            s->produced += n;
            if (ring_size(&s->send) > s->send_high_water) s->send_high_water = ring_size(&s->send);
        }
        rt_hw_interrupt_enable(level);
        if (s->error) return false;
        if (r.input_consumed) return true;
        if (speech_now_ms() - began > 200u)
        { input_fail(s, EPD_SPEECH_INPUT_OVERRUN); return false; }
        rt_thread_mdelay(1);
    }
#else
    (void)timestamp;
    if (input_cancelled(s)) { input_fail(s, EPD_SPEECH_CANCELLED); return false; }
    if (!backlog_ok(s)) return false;
    int16_t mono[160];
    for (uint32_t i = 0; i < count / 4u; ++i)
        mono[i] = raw[i * 2u + EPD_SPEECH_INPUT_CHANNEL];
    uint32_t bytes = count / 2u;
    rt_base_t level = rt_hw_interrupt_disable();
    if (ring_size(&s->send) + bytes > s->send.capacity) input_fail(s, EPD_SPEECH_INPUT_OVERRUN);
    else
    {
        ring_copy(&s->send, mono, bytes, true);
        s->produced += bytes;
        if (ring_size(&s->send) > s->send_high_water) s->send_high_water = ring_size(&s->send);
    }
    rt_hw_interrupt_enable(level);
    return !s->error;
#endif
}

static void input_worker(void *argument)
{
    speech_audio_input_t *s = argument;
    bool powered = false;
#if EPD_SPEECH_USE_ANYKA
    epd_anyka_config_t config = {0};
    config.sample_rate = EPD_SPEECH_SAMPLE_RATE;
    config.microphones = 2;
    int opened_dsp = epd_anyka_open_cancelable(&config, EPD_SPEECH_ANYKA_NEAR_PARAMETERS, NULL,
                      EPD_COMPUTE_CONTINUOUS, input_cancelled, s, &s->dsp);
    if (opened_dsp != EPD_COMPUTE_OK)
    {
        input_fail(s, opened_dsp == EPD_COMPUTE_CANCELLED ? EPD_SPEECH_CANCELLED : EPD_SPEECH_INPUT_UNAVAILABLE);
        goto done;
    }
#endif
    if (input_cancelled(s)) { input_fail(s, EPD_SPEECH_CANCELLED); goto done; }
    audio_parameter_t parameters = {0};
    parameters.read_samplerate = EPD_SPEECH_SAMPLE_RATE;
    parameters.read_bits_per_sample = 16;
    parameters.read_channnel_num = 2;
    parameters.read_which_mic = AUDIO_MIC0_ONLY;
    parameters.read_cache_size = 4096;
    parameters.is_need_3a = 0;
    BSP_Microphone_PowerOn();
    powered = true;
    s->client = audio_open2(AUDIO_TYPE_LOCAL_RECORD, AUDIO_RX, &parameters,
                           receive_audio, s, AUDIO_DEVICE_SPEAKER);
    if (!s->client || BSP_Microphone_SetGain(24) != 0)
    { input_fail(s, EPD_SPEECH_INPUT_UNAVAILABLE); goto stop; }
    s->warmup = WARMUP_BYTES;
    s->armed = true;
    uint32_t opened_ms = speech_now_ms();
    uint64_t timestamp = 0;
    for (;;)
    {
        if (input_cancelled(s)) input_fail(s, EPD_SPEECH_CANCELLED);
        if (s->error || !backlog_ok(s)) break;
        uint32_t now = speech_now_ms();
        if ((!s->ready && now - opened_ms > 1000u) ||
            (s->ready && !s->finish && now - (s->samples ? s->last_callback : s->ready_at) > 200u))
        { input_fail(s, EPD_SPEECH_INPUT_OVERRUN); break; }
        if (s->finish && s->client)
        {
            s->armed = false;
            while (audio_close(s->client)) rt_thread_mdelay(5);
            s->client = NULL;
            BSP_Microphone_PowerOff();
            powered = false;
        }
        int16_t raw[320] = {0};
        rt_base_t level = rt_hw_interrupt_disable();
        uint32_t count = ring_size(&s->raw);
        if (count > RAW_FRAME) count = RAW_FRAME;
        if (count && (count == RAW_FRAME || s->finish)) ring_copy(&s->raw, raw, count, false);
        else count = 0;
        rt_hw_interrupt_enable(level);
        if (count)
        {
            if (!process_frame(s, raw, count, timestamp)) break;
            s->processed += count / 4u;
            timestamp += 10000u;
        }
        else if (s->finish)
        {
#if EPD_SPEECH_USE_ANYKA
            /* Advance the finite vendor delay with zeros, emitting only the
             * accepted capture interval; padding never extends recording time. */
            for (unsigned tail = 0; s->produced < s->samples * 2u && tail < 20u; ++tail)
            {
                if (!process_frame(s, raw, RAW_FRAME, timestamp)) break;
                timestamp += 10000u;
            }
#endif
            if (!s->error && s->produced != s->samples * 2u) input_fail(s, EPD_SPEECH_INPUT_UNAVAILABLE);
            break;
        }
        else rt_sem_take(s->wake, rt_tick_from_millisecond(10));
    }
stop:
    s->armed = false;
    if (s->client)
    {
        while (audio_close(s->client)) rt_thread_mdelay(5);
        s->client = NULL;
    }
    if (powered) BSP_Microphone_PowerOff();
done:
#if EPD_SPEECH_USE_ANYKA
    if (s->dsp)
    {
        if (s->error || s->abort) epd_anyka_cancel(s->dsp);
        while (epd_anyka_close(s->dsp) != EPD_COMPUTE_OK) rt_thread_mdelay(5);
    }
#endif
    return;
}

static void input_cleanup(struct rt_thread *thread)
{
    speech_audio_input_t *s = thread->parameter;
    /* The entry has returned. The idle thread owns/frees the RT-Thread stack
     * and thread object; the owner may now free only the capture context. */
    s->finished = true;
    rt_sem_release(s->exited);
}

bool speech_audio_input_available(void)
{
#if EPD_SPEECH_USE_ANYKA
    const char *p = EPD_SPEECH_ANYKA_NEAR_PARAMETERS;
    epd_compute_capability_t capability;
    return epd_compute_query(EPD_ANYKA_CAPABILITY, EPD_ANYKA_VERSION, &capability) == EPD_COMPUTE_OK &&
           capability.ready && strstr(p, "--aecEna=0") && !strstr(p, "--aecEna=1") &&
           strstr(p, "--dencLoad=1") && strstr(p, "--arrayGeometry=\"2,") &&
           strstr(p, "--targetSpherical=");
#else
    return true;
#endif
}

epd_speech_error_t speech_audio_input_open_limited(speech_cancel_fn cancel, void *user, uint32_t limit_ms, speech_audio_input_t **out)
{
    if (!out || !limit_ms || limit_ms > 3600000u) return EPD_SPEECH_INVALID;
    *out = NULL;
    if (cancel && cancel(user)) return EPD_SPEECH_CANCELLED;
    if (!speech_audio_input_available()) return EPD_SPEECH_INPUT_UNAVAILABLE;
    rt_base_t level = rt_hw_interrupt_disable();
    bool busy = input_in_use;
    input_in_use = true;
    rt_hw_interrupt_enable(level);
    if (busy) return EPD_SPEECH_BUSY;
    speech_audio_input_t *s = speech_alloc(sizeof(*s));
    if (!s) { input_in_use = false; return EPD_SPEECH_NO_MEMORY; }
    memset(s, 0, sizeof(*s));
    s->cancel = cancel; s->user = user; s->limit_ms = limit_ms;
    s->raw.capacity = SPEECH_AUDIO_RAW_BYTES; s->send.capacity = SPEECH_AUDIO_SEND_BYTES;
    s->raw.data = speech_alloc(s->raw.capacity); s->send.data = speech_alloc(s->send.capacity);
    s->wake = rt_sem_create("sp-in", 0, RT_IPC_FLAG_PRIO);
    s->exited = rt_sem_create("sp-exit", 0, RT_IPC_FLAG_PRIO);
    rt_thread_t thread = NULL;
    if (s->raw.data && s->send.data && s->wake && s->exited)
        thread = rt_thread_create("sp-mic", input_worker, s, 4096, 13, 10);
    if (!thread)
    {
        if (s->wake) rt_sem_delete(s->wake);
        if (s->exited) rt_sem_delete(s->exited);
        speech_free(s->raw.data); speech_free(s->send.data); speech_free(s);
        input_in_use = false;
        return EPD_SPEECH_NO_MEMORY;
    }
    thread->cleanup = input_cleanup;
    rt_thread_startup(thread);
    while (!s->ready && !s->finished)
    {
        if (input_cancelled(s)) s->abort = true;
        rt_thread_mdelay(5);
    }
    if (s->error || s->abort)
    {
        epd_speech_error_t error = s->error ? s->error : EPD_SPEECH_CANCELLED;
        speech_audio_input_close(s);
        return error;
    }
    *out = s;
    return EPD_SPEECH_OK;
}

int speech_audio_input_read(speech_audio_input_t *s, void *pcm, size_t capacity)
{
    if (!s || !pcm || !capacity || capacity > INT_MAX || capacity % 2) return -EPD_SPEECH_INVALID;
    if (input_cancelled(s)) return -EPD_SPEECH_CANCELLED;
    if (!backlog_ok(s) || s->error) return -(int)s->error;
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t n = ring_size(&s->send);
    if (n > capacity) n = (uint32_t)capacity;
    ring_copy(&s->send, pcm, n, false);
    s->delivered += n;
    rt_hw_interrupt_enable(level);
    return (int)n;
}

void speech_audio_input_ack(speech_audio_input_t *s, size_t bytes)
{
    if (!s) return;
    if (!backlog_ok(s)) return;
    rt_base_t level = rt_hw_interrupt_disable();
    if (bytes % 2 || bytes > s->delivered - s->sent) input_fail(s, EPD_SPEECH_INVALID);
    else s->sent += (uint32_t)bytes;
    rt_hw_interrupt_enable(level);
}

void speech_audio_input_status(speech_audio_input_t *s, speech_audio_input_status_t *out)
{
    if (!s || !out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = (speech_audio_input_status_t){s->samples, s->samples / 16u,
        s->raw_high_water, s->send_high_water, s->finished, s->duration_limit, s->error};
    rt_hw_interrupt_enable(level);
}
void speech_audio_input_finish(speech_audio_input_t *s)
{ if (s) { s->finish = true; rt_sem_release(s->wake); } }
void speech_audio_input_close(speech_audio_input_t *s)
{
    if (!s) return;
    s->abort = true;
    rt_sem_release(s->wake);
    rt_sem_take(s->exited, RT_WAITING_FOREVER);
    rt_sem_delete(s->wake); rt_sem_delete(s->exited);
    speech_free(s->raw.data); speech_free(s->send.data); speech_free(s);
    rt_base_t level = rt_hw_interrupt_disable();
    input_in_use = false;
    rt_hw_interrupt_enable(level);
}
#else
bool speech_audio_input_available(void) { return false; }
epd_speech_error_t speech_audio_input_open_limited(speech_cancel_fn cancel, void *user, uint32_t limit_ms, speech_audio_input_t **out)
{
    (void)limit_ms;
    if (!out) return EPD_SPEECH_INVALID;
    *out = NULL;
    return cancel && cancel(user) ? EPD_SPEECH_CANCELLED : EPD_SPEECH_INPUT_UNAVAILABLE;
}
int speech_audio_input_read(speech_audio_input_t *s, void *pcm, size_t capacity)
{ (void)s; (void)pcm; (void)capacity; return -EPD_SPEECH_INPUT_UNAVAILABLE; }
void speech_audio_input_ack(speech_audio_input_t *s, size_t bytes) { (void)s; (void)bytes; }
void speech_audio_input_status(speech_audio_input_t *s, speech_audio_input_status_t *out)
{ (void)s; if (out) { memset(out, 0, sizeof(*out)); out->error = EPD_SPEECH_INPUT_UNAVAILABLE; } }
void speech_audio_input_finish(speech_audio_input_t *s) { (void)s; }
void speech_audio_input_close(speech_audio_input_t *s) { (void)s; }
#endif

epd_speech_error_t speech_audio_input_open(speech_cancel_fn cancel, void *user, speech_audio_input_t **out)
{ return speech_audio_input_open_limited(cancel, user, EPD_SPEECH_ASR_LIMIT_MS, out); }
