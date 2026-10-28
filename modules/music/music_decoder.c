#include "music_decoder.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <dfs_posix.h>
#include <rtthread.h>
#include "epd_memory.h"
#include <audio_server.h>
#include <FLAC/stream_decoder.h>
#include "epd_pcm_output.h"
#include "music_resample.h"

enum {
    EVENT_SUSPEND = 1u << 1,
    EVENT_RESUME = 1u << 2
};

struct music_decoder {
    music_backend_wake_fn wake;
    void *wake_userdata;
    music_backend_snapshot_t snapshot;
    char *filename;
    FLAC__StreamDecoder *decoder;
    int file_fd;
    bool file_eof;
    uint64_t file_bytes;
    epd_pcm_output_t *output;
    int16_t *pcm;
    size_t pcm_capacity;
    size_t pcm_bytes;
    size_t pcm_offset;
    uint64_t total_samples;
    uint64_t base_sample;
    uint64_t resume_sample;
    unsigned max_blocksize;
    uint32_t output_rate;
    music_resample_t resample;
    bool metadata_seen;
    bool eof;
    bool draining;
    bool suspended;
    rt_tick_t drain_start, drain_ticks;
    uint32_t tail_bytes;
    bool resume_seek;
    bool accepting_callbacks;
    bool cancelled;
    unsigned events;
};

static bool cancelled(music_decoder_t *b)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool value = b->cancelled;
    rt_hw_interrupt_enable(level);
    return value;
}

static void wake_service(music_decoder_t *b)
{
    if (b->wake) b->wake(b->wake_userdata);
}

static int fail(music_decoder_t *b, music_backend_error_t error, int detail)
{
    if (b->snapshot.state != MUSIC_BACKEND_ERROR) {
        b->snapshot.error = error;
        b->snapshot.error_detail = detail;
        b->snapshot.state = MUSIC_BACKEND_ERROR;
        wake_service(b);
    }
    return -(int)b->snapshot.error;
}

static bool supported_rate(uint32_t rate)
{
    static const uint32_t rates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000
    };
    size_t i;
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
        if (rates[i] == rate) return true;
    return false;
}

static uint64_t samples_to_ms(uint64_t samples, uint32_t rate)
{
    return samples / rate * 1000 + samples % rate * 1000 / rate;
}

static bool pcm_size(size_t samples, size_t *bytes)
{
    if (samples > SIZE_MAX / sizeof(int16_t)) return false;
    *bytes = samples * sizeof(int16_t);
    return true;
}

/* Audio Manager callbacks wake the worker; only the worker touches files,
 * decoders and output buffers. */
static int audio_callback(audio_server_callback_cmt_t command, void *userdata,
                          uint32_t reserved)
{
    music_decoder_t *b = userdata;
    (void)reserved;
    rt_base_t level = rt_hw_interrupt_disable();
    bool accepted = b->accepting_callbacks && !b->cancelled;
    if (accepted) {
        switch (command) {
        case as_callback_cmd_suspended:
            b->events = (b->events & ~EVENT_RESUME) | EVENT_SUSPEND;
            break;
        case as_callback_cmd_resumed:
            b->events = (b->events & ~EVENT_SUSPEND) | EVENT_RESUME;
            break;
        default: break;
        }
    }
    rt_hw_interrupt_enable(level);
    if (accepted) wake_service(b);
    return 0;
}

static void callback_gate(music_decoder_t *b, bool accepting)
{
    rt_base_t level = rt_hw_interrupt_disable();
    b->accepting_callbacks = accepting;
    b->events = 0;
    rt_hw_interrupt_enable(level);
}

static void update_state(music_decoder_t *b)
{
    if (b->snapshot.state == MUSIC_BACKEND_ERROR ||
        b->snapshot.state == MUSIC_BACKEND_ENDED ||
        b->snapshot.state == MUSIC_BACKEND_IDLE) return;
    if (!b->snapshot.want_play) {
        if (b->snapshot.state != MUSIC_BACKEND_PREPARED)
            b->snapshot.state = MUSIC_BACKEND_PAUSED;
    } else if (b->suspended) b->snapshot.state = MUSIC_BACKEND_SUSPENDED;
    else if (b->draining) b->snapshot.state = MUSIC_BACKEND_DRAINING;
    else b->snapshot.state = MUSIC_BACKEND_PLAYING;
}

static int consume_events(music_decoder_t *b)
{
    unsigned events;
    rt_base_t level = rt_hw_interrupt_disable();
    events = b->events;
    b->events = 0;
    rt_hw_interrupt_enable(level);
    if (cancelled(b)) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
    if (b->snapshot.state == MUSIC_BACKEND_ERROR) return -(int)b->snapshot.error;
    if (b->snapshot.state == MUSIC_BACKEND_ENDED) return 0;
    if (events & EVENT_SUSPEND) b->suspended = true;
    if (events & EVENT_RESUME) {
        b->suspended = false;
        if (b->draining && b->output) {
            b->drain_start = rt_tick_get();
            b->drain_ticks = rt_tick_from_millisecond(
                epd_pcm_output_cached_ms(b->output) + 20);
        }
    }
    update_state(b);
    return 0;
}

static FLAC__StreamDecoderReadStatus flac_read(const FLAC__StreamDecoder *decoder,
    FLAC__byte buffer[], size_t *bytes, void *userdata)
{
    music_decoder_t *b = userdata;
    ssize_t count;
    (void)decoder;
    if (!*bytes || cancelled(b)) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    if (*bytes > INT_MAX) *bytes = INT_MAX;
    count = read(b->file_fd, buffer, *bytes);
    if (count < 0) {
        fail(b, MUSIC_BACKEND_IO, errno);
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    *bytes = (size_t)count;
    b->file_eof = count == 0;
    if (cancelled(b)) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    return count ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE :
                   FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

static FLAC__StreamDecoderSeekStatus flac_file_seek(const FLAC__StreamDecoder *decoder,
    FLAC__uint64 offset, void *userdata)
{
    music_decoder_t *b = userdata;
    off_t target = (off_t)offset;
    (void)decoder;
    if (cancelled(b)) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    if (target < 0 || (uint64_t)target != offset || offset > b->file_bytes) {
        fail(b, MUSIC_BACKEND_SEEK, EOVERFLOW);
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    if (lseek(b->file_fd, target, SEEK_SET) != target) {
        fail(b, MUSIC_BACKEND_IO, errno);
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    b->file_eof = false;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus flac_tell(const FLAC__StreamDecoder *decoder,
    FLAC__uint64 *offset, void *userdata)
{
    music_decoder_t *b = userdata;
    off_t position;
    (void)decoder;
    if (cancelled(b)) return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    position = lseek(b->file_fd, 0, SEEK_CUR);
    if (position < 0) {
        fail(b, MUSIC_BACKEND_IO, errno);
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
    *offset = (uint64_t)position;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus flac_length(const FLAC__StreamDecoder *decoder,
    FLAC__uint64 *length, void *userdata)
{
    music_decoder_t *b = userdata;
    (void)decoder;
    if (cancelled(b)) return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    *length = b->file_bytes;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool flac_eof(const FLAC__StreamDecoder *decoder, void *userdata)
{
    (void)decoder;
    return ((music_decoder_t *)userdata)->file_eof;
}

/* Average channels before quantization, round ties away from zero, and clamp.
 * The wide intermediate also handles the positive 24-bit full-scale round-up. */
static int16_t pcm_sample(const FLAC__int32 *const input[], unsigned index,
                          unsigned channels, unsigned bits)
{
    int64_t value = input[0][index];
    int64_t divisor = bits == 24 ? 256 : 1;
    if (channels == 2) {
        value += input[1][index];
        divisor *= 2;
    }
    value = value < 0 ? -((-value + divisor / 2) / divisor) :
                        (value + divisor / 2) / divisor;
    if (value > INT16_MAX) value = INT16_MAX;
    if (value < INT16_MIN) value = INT16_MIN;
    return (int16_t)value;
}

static FLAC__StreamDecoderWriteStatus flac_write(const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame, const FLAC__int32 *const input[], void *userdata)
{
    music_decoder_t *b = userdata;
    size_t bytes;
    unsigned i;
    (void)decoder;
    if (cancelled(b)) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    if (!b->metadata_seen || !frame->header.blocksize ||
        frame->header.blocksize > b->max_blocksize ||
        frame->header.sample_rate != b->snapshot.sample_rate ||
        frame->header.channels != b->snapshot.channels ||
        frame->header.bits_per_sample != b->snapshot.bits_per_sample ||
        b->pcm_offset != b->pcm_bytes) {
        fail(b, MUSIC_BACKEND_DECODE, 0);
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    if (!pcm_size(frame->header.blocksize, &bytes)) {
        fail(b, MUSIC_BACKEND_NO_MEMORY, EOVERFLOW);
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    if (bytes > b->pcm_capacity) {
        int16_t *pcm = epd_app_realloc(b->pcm, bytes, EPD_APP_PSRAM);
        if (!pcm) {
            fail(b, MUSIC_BACKEND_NO_MEMORY, 0);
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        b->pcm = pcm;
        b->pcm_capacity = bytes;
    }
    for (i = 0; i < frame->header.blocksize; ++i)
        b->pcm[i] = pcm_sample(input, i, frame->header.channels,
                              frame->header.bits_per_sample);
    b->pcm_offset = 0;
    b->pcm_bytes = b->snapshot.sample_rate == 96000 ?
        music_resample_by2(&b->resample, b->pcm, frame->header.blocksize) * sizeof(int16_t) : bytes;
    b->snapshot.decoded_samples += frame->header.blocksize;
    b->snapshot.pending_samples = b->snapshot.decoded_samples - b->snapshot.submitted_samples;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_metadata(const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata, void *userdata)
{
    music_decoder_t *b = userdata;
    const FLAC__StreamMetadata_StreamInfo *info;
    (void)decoder;
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) return;
    info = &metadata->data.stream_info;
    if (!supported_rate(info->sample_rate) ||
        (info->channels != 1 && info->channels != 2) ||
        (info->bits_per_sample != 16 && info->bits_per_sample != 24) ||
        !info->max_blocksize || info->min_blocksize > info->max_blocksize) {
        fail(b, MUSIC_BACKEND_UNSUPPORTED, 0);
        return;
    }
    b->snapshot.sample_rate = info->sample_rate;
    b->output_rate = info->sample_rate == 96000 ? 48000 : info->sample_rate;
    b->snapshot.channels = (uint8_t)info->channels;
    b->snapshot.bits_per_sample = (uint8_t)info->bits_per_sample;
    b->total_samples = info->total_samples;
    b->max_blocksize = info->max_blocksize;
    b->snapshot.duration_known = info->total_samples != 0;
    b->snapshot.duration_ms = samples_to_ms(info->total_samples, info->sample_rate);
    b->metadata_seen = true;
}

static void flac_error(const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus error, void *userdata)
{
    (void)decoder;
    fail(userdata, MUSIC_BACKEND_DECODE, (int)error);
}

static int close_output(music_decoder_t *b)
{
    int rc = 0;
    callback_gate(b, false);
    if (b->output) {
        rc = epd_pcm_output_close(b->output);
        if (rc == 0) b->output = NULL;
    }
    b->draining = false;
    b->tail_bytes = 0;
    b->drain_ticks = 0;
    b->suspended = false;
    return rc ? fail(b, MUSIC_BACKEND_OUTPUT, rc) : 0;
}

static int open_output(music_decoder_t *b)
{
    callback_gate(b, true);
    b->output = epd_pcm_output_open(AUDIO_TYPE_LOCAL_MUSIC, b->output_rate,
                                   audio_callback, b);
    if (!b->output) {
        callback_gate(b, false);
        return fail(b, MUSIC_BACKEND_OUTPUT, 0);
    }
    return consume_events(b);
}

static int update_flac_position(music_decoder_t *b)
{
    uint32_t cached_ms = 0;
    uint64_t cached_samples;
    if (!b->output || b->snapshot.state == MUSIC_BACKEND_ENDED ||
        b->snapshot.state == MUSIC_BACKEND_ERROR) return 0;
    cached_ms = epd_pcm_output_cached_ms(b->output);
    uint32_t silence_ms = (uint64_t)b->tail_bytes * 1000 /
                         (b->output_rate * sizeof(int16_t));
    cached_ms = cached_ms > silence_ms ? cached_ms - silence_ms : 0;
    cached_samples = (uint64_t)cached_ms * b->snapshot.sample_rate / 1000;
    if (cached_samples > b->snapshot.submitted_samples)
        cached_samples = b->snapshot.submitted_samples;
    /* Cache time remains in media units when the selected output resamples.
     * Hardware and remote-headset playback latency are not exposed here. */
    b->resume_sample = b->base_sample + b->snapshot.submitted_samples - cached_samples;
    b->snapshot.position_ms = samples_to_ms(b->resume_sample, b->snapshot.sample_rate);
    return 0;
}

static int seek_flac_samples(music_decoder_t *b, uint64_t sample)
{
    bool ok;
    if (close_output(b) < 0) return -(int)b->snapshot.error;
    b->pcm_bytes = b->pcm_offset = 0;
    b->snapshot.pending_samples = 0;
    b->snapshot.decoded_samples = b->snapshot.submitted_samples = 0;
    /* seek_absolute may deliver target PCM synchronously. */
    memset(&b->resample, 0, sizeof(b->resample));
    b->eof = false;
    b->resume_seek = false;
    if (cancelled(b)) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
    /* libFLAC rejects total_samples as a seek target. End is a valid prepared
     * media position and requires no target audio. */
    if (b->snapshot.duration_known && sample == b->total_samples) {
        b->eof = true;
        ok = true;
    } else {
        ok = FLAC__stream_decoder_seek_absolute(b->decoder, sample) != 0;
    }
    if (cancelled(b)) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
    if (b->snapshot.state == MUSIC_BACKEND_ERROR) return -(int)b->snapshot.error;
    if (!ok) return fail(b, MUSIC_BACKEND_SEEK,
                         b->decoder ? (int)FLAC__stream_decoder_get_state(b->decoder) : 0);
    b->base_sample = b->resume_sample = sample;
    b->snapshot.position_ms = samples_to_ms(sample, b->snapshot.sample_rate);
    return 0;
}

music_decoder_t *music_decoder_create(music_backend_wake_fn wake, void *userdata)
{
    music_decoder_t *b = epd_app_alloc(sizeof(*b), EPD_APP_PSRAM);
    if (b) {
        memset(b, 0, sizeof(*b));
        b->file_fd = -1;
        b->wake = wake;
        b->wake_userdata = userdata;
    }
    return b;
}

void music_decoder_request_cancel(music_decoder_t *b)
{
    rt_base_t level;
    if (!b) return;
    level = rt_hw_interrupt_disable();
    b->cancelled = true;
    rt_hw_interrupt_enable(level);
    wake_service(b);
}

int music_decoder_close(music_decoder_t *b)
{
    uint64_t session;
    rt_base_t level;
    if (!b) return -MUSIC_BACKEND_INVALID;
    music_decoder_request_cancel(b);
    callback_gate(b, false);
    if (close_output(b) < 0) return -(int)b->snapshot.error;
    if (b->decoder) {
        FLAC__stream_decoder_finish(b->decoder);
        FLAC__stream_decoder_delete(b->decoder);
        b->decoder = NULL;
    }
    if (b->file_fd >= 0) close(b->file_fd);
    epd_app_free(b->pcm);
    epd_app_free(b->filename);
    session = b->snapshot.session_id;
    level = rt_hw_interrupt_disable();
    /* Cancellation may signal while close resets the session. The wake target
     * remains immutable until all producers and the worker have stopped. */
    memset(&b->snapshot, 0, sizeof(*b) - offsetof(music_decoder_t, snapshot));
    b->file_fd = -1;
    b->snapshot.session_id = session;
    rt_hw_interrupt_enable(level);
    return 0;
}

void music_decoder_destroy(music_decoder_t *b)
{
    if (b && music_decoder_close(b) == 0) epd_app_free(b);
}

int music_decoder_open(music_decoder_t *b, const char *path)
{
    unsigned char signature[12];
    size_t length;
    ssize_t count;
    int rc;
    off_t file_bytes;
    FLAC__StreamDecoderInitStatus init;
    if (!b || !path || !*path) return -MUSIC_BACKEND_INVALID;
    if (b->filename || b->decoder || b->output || b->file_fd >= 0) {
        rc = music_decoder_close(b);
        if (rc < 0) return rc;
    }
    if (cancelled(b)) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
    {
        uint64_t session = b->snapshot.session_id + 1;
        memset(&b->snapshot, 0, sizeof(b->snapshot));
        b->snapshot.session_id = session;
    }
    b->snapshot.state = MUSIC_BACKEND_PREPARED;
    length = strlen(path);
    b->filename = epd_app_alloc(length + 1, EPD_APP_PSRAM);
    if (!b->filename) return fail(b, MUSIC_BACKEND_NO_MEMORY, 0);
    memcpy(b->filename, path, length + 1);
    b->file_fd = open(b->filename, O_RDONLY | O_BINARY);
    if (b->file_fd < 0) return fail(b, MUSIC_BACKEND_IO, errno);
    count = read(b->file_fd, signature, sizeof(signature));
    if (count < 0) return fail(b, MUSIC_BACKEND_IO, errno);
    if (count < 4) return fail(b, MUSIC_BACKEND_UNSUPPORTED, 0);
    if (memcmp(signature, "fLaC", 4) != 0)
        return fail(b, MUSIC_BACKEND_UNSUPPORTED, 0);
    b->snapshot.format = MUSIC_BACKEND_FORMAT_FLAC;
    file_bytes = lseek(b->file_fd, 0, SEEK_END);
    if (file_bytes < 0 || lseek(b->file_fd, 0, SEEK_SET) != 0)
        return fail(b, MUSIC_BACKEND_IO, errno);
    b->file_bytes = (uint64_t)file_bytes;
    b->decoder = FLAC__stream_decoder_new();
    if (!b->decoder) return fail(b, MUSIC_BACKEND_NO_MEMORY, 0);
    init = FLAC__stream_decoder_init_stream(b->decoder, flac_read, flac_file_seek,
        flac_tell, flac_length, flac_eof, flac_write, flac_metadata, flac_error, b);
    if (init != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        return fail(b, MUSIC_BACKEND_DECODE, (int)init);
    rc = FLAC__stream_decoder_process_until_end_of_metadata(b->decoder);
    if (cancelled(b)) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
    if (b->snapshot.state == MUSIC_BACKEND_ERROR) return -(int)b->snapshot.error;
    if (!rc || !b->metadata_seen)
        return fail(b, MUSIC_BACKEND_DECODE, (int)FLAC__stream_decoder_get_state(b->decoder));
    return 0;
}

int music_decoder_play(music_decoder_t *b)
{
    int rc;
    if (!b || (!b->decoder)) return -MUSIC_BACKEND_INVALID;
    if (consume_events(b) < 0) return -(int)b->snapshot.error;
    if (b->snapshot.state == MUSIC_BACKEND_ENDED) return -MUSIC_BACKEND_INVALID;
    if (b->snapshot.want_play) return 0;
    if (b->resume_seek) {
        rc = seek_flac_samples(b, b->resume_sample);
        if (rc < 0) return rc;
    }
    if (cancelled(b)) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
    b->snapshot.want_play = true;
    update_state(b);
    wake_service(b);
    return consume_events(b);
}

int music_decoder_pause(music_decoder_t *b)
{
    if (!b || (!b->decoder)) return -MUSIC_BACKEND_INVALID;
    if (consume_events(b) < 0) return -(int)b->snapshot.error;
    if (b->snapshot.state == MUSIC_BACKEND_ENDED) return 0;
    b->snapshot.want_play = false;
    if (b->output) {
        if (update_flac_position(b) < 0) return -(int)b->snapshot.error;
        /* Keep the decoded frame while paused. If closing drops queued samples,
         * rebuild from the confirmed cache-adjusted position on explicit play. */
        b->resume_seek = b->resume_sample != b->base_sample + b->snapshot.submitted_samples;
        if (close_output(b) < 0) return -(int)b->snapshot.error;
    }
    b->snapshot.state = MUSIC_BACKEND_PAUSED;
    return consume_events(b);
}

int music_decoder_seek(music_decoder_t *b, uint64_t position_ms)
{
    int rc;
    uint64_t sample;
    if (!b || (!b->decoder)) return -MUSIC_BACKEND_INVALID;
    if (consume_events(b) < 0) return -(int)b->snapshot.error;
    {
        if (b->snapshot.duration_known && !b->snapshot.duration_estimated &&
            position_ms > b->snapshot.duration_ms) {
            sample = b->total_samples;
        } else {
            if (position_ms / 1000 > UINT64_MAX / b->snapshot.sample_rate)
                return fail(b, MUSIC_BACKEND_SEEK, EOVERFLOW);
            sample = position_ms / 1000 * b->snapshot.sample_rate;
            if (UINT64_MAX - sample < position_ms % 1000 * b->snapshot.sample_rate / 1000)
                return fail(b, MUSIC_BACKEND_SEEK, EOVERFLOW);
            sample += position_ms % 1000 * b->snapshot.sample_rate / 1000;
        }
        rc = seek_flac_samples(b, sample);
    }
    if (rc < 0) return rc;
    if (b->snapshot.state == MUSIC_BACKEND_ENDED) b->snapshot.state = MUSIC_BACKEND_PAUSED;
    update_state(b);
    return 0;
}

int music_decoder_pump(music_decoder_t *b)
{
    int rc;
    size_t bytes;
    FLAC__StreamDecoderState state;
    if (!b) return -MUSIC_BACKEND_INVALID;
    if (consume_events(b) < 0) return -(int)b->snapshot.error;
    if ((!b->decoder) || !b->snapshot.want_play ||
        b->suspended || b->snapshot.state == MUSIC_BACKEND_ENDED) return 0;
    if (!b->output) {
        if (open_output(b) < 0) return -(int)b->snapshot.error;
        return b->suspended ? 0 : 1;
    }
    if (update_flac_position(b) < 0) return -(int)b->snapshot.error;
    if (b->draining) {
        if ((rt_tick_t)(rt_tick_get() - b->drain_start) < b->drain_ticks) return 0;
        b->snapshot.position_ms = samples_to_ms(b->base_sample +
            b->snapshot.submitted_samples, b->snapshot.sample_rate);
        b->snapshot.state = MUSIC_BACKEND_ENDED;
        b->snapshot.want_play = false;
        return 0;
    }
    if (b->pcm_offset < b->pcm_bytes) {
        bytes = b->pcm_bytes - b->pcm_offset;
        /* SDK FLAC example feeds small PCM blocks and retries when full. */
        if (bytes > 320) bytes = 320;
        bytes -= bytes % sizeof(int16_t);
        if (!bytes) return 0;
        if (cancelled(b)) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
        rc = epd_pcm_output_write(b->output, (uint8_t *)b->pcm + b->pcm_offset, (uint32_t)bytes);
        if (rc == -1) {
            b->suspended = true;
            update_state(b);
            return 0;
        }
        if (rc < 0) return fail(b, MUSIC_BACKEND_OUTPUT, rc);
        if ((size_t)rc > bytes || (unsigned)rc % sizeof(int16_t))
            return fail(b, MUSIC_BACKEND_OUTPUT, EINVAL);
        b->pcm_offset += (size_t)rc;
        b->snapshot.submitted_samples += (unsigned)rc / sizeof(int16_t) *
            (b->snapshot.sample_rate / b->output_rate);
        /* The last downsampled output can contain one repeated padding input. */
        if (b->snapshot.submitted_samples > b->snapshot.decoded_samples)
            b->snapshot.submitted_samples = b->snapshot.decoded_samples;
        b->snapshot.pending_samples = b->snapshot.decoded_samples - b->snapshot.submitted_samples;
        return rc ? 1 : 0;
    }
    if (b->eof) {
        /* Push the last short block through the SDK cache with silence. This
         * is not media data and does not advance the saved playback position. */
        uint32_t tail = b->output_rate / 10 * sizeof(int16_t);
        if (b->tail_bytes < tail) {
            int16_t silence[160] = {0};
            uint32_t count = tail - b->tail_bytes;
            if (count > sizeof(silence)) count = sizeof(silence);
            rc = epd_pcm_output_write(b->output, silence, count);
            if (rc == -1) { b->suspended = true; update_state(b); return 0; }
            if (rc < 0 || (uint32_t)rc > count || rc % 2)
                return fail(b, MUSIC_BACKEND_OUTPUT, rc);
            b->tail_bytes += (uint32_t)rc;
            return rc ? 1 : 0;
        }
        uint32_t cached_ms = epd_pcm_output_cached_ms(b->output);
        b->drain_start = rt_tick_get();
        b->drain_ticks = rt_tick_from_millisecond(cached_ms + 20);
        b->draining = true;
        update_state(b);
        return 0;
    }
    b->pcm_offset = b->pcm_bytes = 0;
    rc = FLAC__stream_decoder_process_single(b->decoder);
    if (cancelled(b)) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
    if (b->snapshot.state == MUSIC_BACKEND_ERROR) return -(int)b->snapshot.error;
    state = FLAC__stream_decoder_get_state(b->decoder);
    if (!rc) return fail(b, MUSIC_BACKEND_DECODE, (int)state);
    if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
        if (b->snapshot.duration_known &&
            b->base_sample + b->snapshot.decoded_samples != b->total_samples)
            return fail(b, MUSIC_BACKEND_IO, 0);
        if (b->snapshot.sample_rate == 96000)
            b->pcm_bytes = music_resample_finish(&b->resample, b->pcm) * sizeof(int16_t);
        b->eof = true;
        b->snapshot.state = MUSIC_BACKEND_DRAINING;
    }
    return 1;
}

void music_decoder_snapshot(music_decoder_t *b, music_backend_snapshot_t *snapshot)
{
    if (!b || !snapshot) return;
    consume_events(b);
    if ((b->decoder) && b->snapshot.want_play)
        update_flac_position(b);
    *snapshot = b->snapshot;
}
