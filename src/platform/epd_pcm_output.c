#include <rtthread.h>

#ifdef AUDIO_USING_MANAGER
#include "epd_pcm_output.h"
#include "epd_memory.h"
#include <sifli_resample.h>
#include <rtm.h>
#include <string.h>

#ifndef SOFTWARE_TX_MIX_ENABLE
#define SOFTWARE_TX_MIX_ENABLE 0
#endif

#define PCM_BLOCK_SAMPLES 512u
#define A2DP_RATE 44100u
#define PCM_CACHE_BYTES 8192u

struct epd_pcm_output
{
    audio_client_t client;
    audio_type_t type;
    uint32_t rate;
    audio_server_callback_func callback;
    void *callback_data;
    volatile bool reopening;
    sifli_resample_t *resample;
    uint32_t input_bytes;
    uint32_t output_bytes;
    uint32_t output_offset;
    bool a2dp, convert_a2dp;
    bool controlled, wrote, draining_write;
    volatile bool suspended, lost;
    const void *pending_source;
    uint32_t pending_length;
    uint32_t drain_phase, silence_bytes, padding_bytes, settle_until;
    int16_t pcm[PCM_BLOCK_SAMPLES * 2];
};

static void cache_format(epd_pcm_output_t *output)
{
#if !SOFTWARE_TX_MIX_ENABLE
    audio_ioctl(output->client, AUDIO_IOCTL_SET_CACHE_SAMPLERATE,
        (void *)(uintptr_t)(output->convert_a2dp ? A2DP_RATE : output->rate));
    audio_ioctl(output->client, AUDIO_IOCTL_SET_CACHE_CH,
        (void *)(uintptr_t)(output->convert_a2dp ? 2 : 1));
#else
    (void)output;
#endif
}

static int client_callback(audio_server_callback_cmt_t command, void *data,
    uint32_t reserved)
{
    epd_pcm_output_t *output = data;
    if (command == as_callback_cmd_suspended) output->suspended = true;
    if (command == as_callback_cmd_resumed) output->suspended = false;
    if (command == as_callback_cmd_closed) output->lost = true;
    if (output->reopening && (command == as_callback_cmd_opened ||
                             command == as_callback_cmd_closed)) return 0;
    return output->callback ? output->callback(command, output->callback_data, reserved) : 0;
}

static int open_client(epd_pcm_output_t *output)
{
    audio_parameter_t parameters = {0};
    parameters.write_samplerate = output->rate;
    parameters.write_bits_per_sample = 16;
    parameters.write_channnel_num = 1;
    parameters.write_cache_size = output->controlled ? 2048u : PCM_CACHE_BYTES;
    output->a2dp = get_server_current_device() == AUDIO_DEVICE_A2DP_SINK;
    output->client = output->controlled ?
        audio_open2(output->type, AUDIO_TX, &parameters, client_callback, output,
                    output->a2dp ? AUDIO_DEVICE_A2DP_SINK : AUDIO_DEVICE_SPEAKER) :
        audio_open(output->type, AUDIO_TX, &parameters, client_callback, output);
    if (!output->client) return -2;
    if (!output->controlled)
        output->a2dp = get_server_current_device() == AUDIO_DEVICE_A2DP_SINK;
    /* The SDK owns conversion when software mixing is enabled. */
    output->convert_a2dp = output->a2dp && !SOFTWARE_TX_MIX_ENABLE && audio_device_is_a2dp_sink();
    cache_format(output);
    return 0;
}

static epd_pcm_output_t *create_output(audio_type_t type, uint32_t rate,
    audio_server_callback_func callback, void *data, bool controlled)
{
    if ((unsigned)type >= AUDIO_TYPE_NUMBER || rate < 8000 || rate > 48000)
        return NULL;
    epd_pcm_output_t *output = epd_app_alloc(sizeof(*output), EPD_APP_PSRAM);
    if (!output) return NULL;
    memset(output, 0, sizeof(*output));
    output->rate = rate;
    output->type = type;
    output->callback = callback;
    output->callback_data = data;
    output->controlled = controlled;
    if (open_client(output) != 0)
    {
        epd_app_free(output);
        return NULL;
    }
    return output;
}

epd_pcm_output_t *epd_pcm_output_open(audio_type_t type, uint32_t rate,
    audio_server_callback_func callback, void *data)
{ return create_output(type, rate, callback, data, false); }

epd_pcm_output_t *epd_pcm_output_open_controlled(audio_type_t type, uint32_t rate,
    audio_server_callback_func callback, void *data)
{ return create_output(type, rate, callback, data, true); }

bool epd_pcm_output_route_changed(epd_pcm_output_t *output)
{ return output && output->a2dp != (get_server_current_device() == AUDIO_DEVICE_A2DP_SINK); }

static bool route_lost(epd_pcm_output_t *output)
{
    return output->lost || output->suspended || get_server_current_device() !=
        (output->a2dp ? AUDIO_DEVICE_A2DP_SINK : AUDIO_DEVICE_SPEAKER);
}

static void bluetooth_volume(epd_pcm_output_t *output, uint32_t bytes)
{
#if !SOFTWARE_TX_MIX_ENABLE
    if (get_server_current_device() != AUDIO_DEVICE_A2DP_SINK) return;
    uint32_t volume = audio_server_get_private_volume(output->type);
    if (volume > AUDIO_MAX_VOLUME) volume = AUDIO_MAX_VOLUME;
    audio_set_tws_volume_type(1);
    audio_set_tws_volume((uint8_t)volume);
    if (!volume || audio_server_get_public_speaker_mute())
        memset(output->pcm, 0, bytes);
#else
    (void)output;
    (void)bytes;
#endif
}

static void record_drain_padding(epd_pcm_output_t *output, const void *pcm,
    uint32_t bytes)
{
    if (!output->draining_write) return;
    const uint8_t *data = pcm;
    uint32_t end = bytes;
    /* Resampling the first silence block can retain the last source sample.
     * Count only its zero suffix, using PCM that audio_write cannot mutate. */
    while (end && !data[end - 1]) --end;
    if (end) output->padding_bytes = 0;
    output->padding_bytes += bytes - end;
}

int epd_pcm_output_write(epd_pcm_output_t *output, const void *mono_pcm,
    uint32_t bytes)
{
    if (!output || !output->client || !mono_pcm || !bytes || bytes % sizeof(int16_t)) return -2;
    if (output->controlled)
    {
        if (route_lost(output)) return -4;
        if (output->pending_source && (mono_pcm != output->pending_source || bytes != output->pending_length))
            return -2;
        if (!output->draining_write && output->drain_phase && output->drain_phase != 4) return -2;
        if (!output->draining_write && !output->pending_source && epd_pcm_output_route_changed(output)) return -3;
        if (!output->draining_write) output->drain_phase = 0;
    }
    bool a2dp = get_server_current_device() == AUDIO_DEVICE_A2DP_SINK;
    bool convert_a2dp = a2dp && !SOFTWARE_TX_MIX_ENABLE && audio_device_is_a2dp_sink();
    if (output->controlled) convert_a2dp = output->convert_a2dp;
    else if (a2dp != output->a2dp || convert_a2dp != output->convert_a2dp)
    {
        /* Reopening discards queued PCM in the previous route's format. */
        output->reopening = true;
        int result = audio_close(output->client);
        if (result == 0)
        {
            output->client = NULL;
            sifli_resample_close(output->resample);
            output->resample = NULL;
            output->input_bytes = output->output_bytes = output->output_offset = 0;
            result = open_client(output);
        }
        output->reopening = false;
        if (result != 0) return -2;
        convert_a2dp = output->convert_a2dp;
    }
    if (output->controlled)
    {
        output->pending_source = mono_pcm;
        output->pending_length = bytes;
    }
    if (!convert_a2dp)
    {
        if (bytes > PCM_BLOCK_SAMPLES * sizeof(int16_t))
            bytes = PCM_BLOCK_SAMPLES * sizeof(int16_t);
        memcpy(output->pcm, mono_pcm, bytes);
        bluetooth_volume(output, bytes);
        int written = audio_write(output->client, (uint8_t *)output->pcm, bytes);
        if (written > 0)
        {
            if ((uint32_t)written > bytes || written % 2) return -2;
            record_drain_padding(output, mono_pcm, (uint32_t)written);
            output->wrote = true;
            output->pending_source = NULL;
        }
        return written;
    }

    if (!output->input_bytes)
    {
        if (!output->resample)
        {
            output->resample = sifli_resample_open(2, output->rate, A2DP_RATE);
            if (!output->resample) return -2;
        }
        /* Leave room for the resampler's interpolation across block boundaries. */
        uint32_t samples = (PCM_BLOCK_SAMPLES - 2) * output->rate / A2DP_RATE;
        if (samples > PCM_BLOCK_SAMPLES) samples = PCM_BLOCK_SAMPLES;
        if (samples > bytes / sizeof(int16_t)) samples = bytes / sizeof(int16_t);
        for (uint32_t i = 0; i < samples; ++i)
        {
            int16_t sample;
            memcpy(&sample, (const uint8_t *)mono_pcm + i * sizeof(sample), sizeof(sample));
            output->pcm[i * 2] = output->pcm[i * 2 + 1] = sample;
        }
        output->input_bytes = samples * sizeof(int16_t);
        output->output_bytes = sifli_resample_process(output->resample, output->pcm,
            samples * 2 * sizeof(int16_t), 0);
    }
    if (bytes < output->input_bytes) return -2;
    if (output->output_bytes)
    {
        if (output->output_bytes > sizeof(output->pcm)) return -2;
        uint32_t remaining = output->output_bytes - output->output_offset;
        memcpy(output->pcm, (uint8_t *)sifli_resample_get_output(output->resample) + output->output_offset, remaining);
        /* audio_write may modify its buffer; retain the unscaled conversion. */
        bluetooth_volume(output, remaining);
        int written = audio_write(output->client, (uint8_t *)output->pcm, remaining);
        if (written <= 0) return written;
        if ((uint32_t)written > remaining || written % 4) return -2;
        record_drain_padding(output,
            (uint8_t *)sifli_resample_get_output(output->resample) + output->output_offset,
            (uint32_t)written);
        output->output_offset += (uint32_t)written;
        output->wrote = true;
        if (output->output_offset != output->output_bytes) return 0;
    }
    int accepted = (int)output->input_bytes;
    output->input_bytes = output->output_bytes = output->output_offset = 0;
    output->pending_source = NULL;
    output->wrote = true;
    return accepted;
}

int epd_pcm_output_drain_step(epd_pcm_output_t *output, uint32_t now_ms)
{
    static const int16_t silence[PCM_BLOCK_SAMPLES] = {0};
    if (!output || !output->client || !output->controlled) return -2;
    if (route_lost(output)) return -4;
    if (!output->drain_phase && (output->pending_source || output->input_bytes)) return -2;
    if (output->drain_phase == 4 || !output->wrote) return 1;
    if (!output->drain_phase)
    {
        if (output->pending_source || output->input_bytes) return -2;
        output->silence_bytes = ((output->rate * EPD_PCM_TAIL_SILENCE_MS + 999u) / 1000u) * 2u;
        output->padding_bytes = 0;
        output->drain_phase = 1;
    }
    if (output->drain_phase == 1)
    {
        uint32_t bytes = output->silence_bytes;
        if (bytes > sizeof(silence)) bytes = sizeof(silence);
        if (bytes)
        {
            output->draining_write = true;
            int n = epd_pcm_output_write(output, silence, bytes);
            output->draining_write = false;
            if (n < 0) return n == -1 ? -4 : n;
            output->silence_bytes -= (uint32_t)n;
        }
        if (output->silence_bytes) return 0;
        output->drain_phase = 2;
    }
    if (output->drain_phase == 2)
    {
#if SOFTWARE_TX_MIX_ENABLE
        uint32_t cached_ms = 0;
        if (audio_ioctl(output->client, AUDIO_IOCTL_FLUSH_TIME_MS, &cached_ms) != 0) return -4;
        output->settle_until = now_ms + cached_ms +
            (output->a2dp ? EPD_PCM_A2DP_PIPELINE_MS : EPD_PCM_SPEAKER_PIPELINE_MS);
#else
        uint32_t cached = 0;
        if (audio_ioctl(output->client, AUDIO_IOCTL_BYTES_IN_CACHE, &cached) != 0) return -4;
        /* SDK consumers leave sub-block tails queued. FIFO ordering proves all
         * source PCM has left the cache once only appended silence remains. */
        if (cached > output->padding_bytes) return 0;
        uint32_t bytes_per_second = output->convert_a2dp ? A2DP_RATE * 4u : output->rate * 2u;
        output->settle_until = now_ms + (cached * 1000u + bytes_per_second - 1u) / bytes_per_second +
            (output->a2dp ? EPD_PCM_A2DP_PIPELINE_MS : EPD_PCM_SPEAKER_PIPELINE_MS);
#endif
        output->drain_phase = 3;
    }
    if ((int32_t)(now_ms - output->settle_until) < 0) return 0;
    output->drain_phase = 4;
    return 1;
}

uint32_t epd_pcm_output_cached_ms(epd_pcm_output_t *output)
{
    uint32_t milliseconds = 0;
    if (!output || !output->client) return 0;
    audio_ioctl(output->client, AUDIO_IOCTL_FLUSH_TIME_MS, &milliseconds);
    return milliseconds;
}

int epd_pcm_output_close(epd_pcm_output_t *output)
{
    if (!output) return 0;
    int result = output->client ? audio_close(output->client) : 0;
    if (result != 0) return result;
    sifli_resample_close(output->resample);
    epd_app_free(output);
    return 0;
}

RTM_EXPORT(epd_pcm_output_open);
RTM_EXPORT(epd_pcm_output_write);
RTM_EXPORT(epd_pcm_output_cached_ms);
RTM_EXPORT(epd_pcm_output_close);
#endif
