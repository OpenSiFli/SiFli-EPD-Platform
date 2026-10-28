/* SPDX-License-Identifier: Apache-2.0 */
#include "sound_tone.h"
#include <rtthread.h>

#ifdef AUDIO_USING_MANAGER
#include "../ui/platform/epd_system.h"
#include "../boards/board_service.h"
#include "system_reset_guard.h"
#include "../platform/epd_pcm_output.h"
#include <string.h>

#define TONE_WAKE 1u
#define TONE_POLL_MS 10
#define TONE_STALL_MS 1000
#define TONE_CHUNK_BYTES 320u
#define TONE_TOUCH_STARTUP_SILENCE_MS 20u
#define TONE_SILENCE_MS 100u
#define TONE_TAIL_MARGIN_MS 20u

typedef struct
{
    epd_sound_channel_t channel;
    const epd_tone_asset_t *asset;
    bool preview;
    uint32_t generation, preview_generation;
} tone_request_t;

typedef struct { bool failed; } tone_callback_t;
static struct rt_event wake;
static bool event_ready, worker_ready, pending, active;
static bool suppressed, output_unclosed;
static uint32_t generation, preview_generation;
static tone_request_t request;
/* This object also remains alive if audio_close reports an error. */
static tone_callback_t callback_state;

static void signal_worker(void)
{
    rt_enter_critical();
    bool ready = event_ready;
    rt_exit_critical();
    if (ready) rt_event_send(&wake, TONE_WAKE);
}

static void wait_worker(int milliseconds)
{
    rt_uint32_t received;
    rt_event_recv(&wake, TONE_WAKE, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
        milliseconds < 0 ? RT_WAITING_FOREVER : rt_tick_from_millisecond(milliseconds), &received);
}

static bool allowed(epd_sound_channel_t channel)
{
    rt_enter_critical();
    bool blocked = suppressed || (output_unclosed && !active);
    rt_exit_critical();
    if (blocked) return false;
    epd_sound_snapshot_t sound;
    if (board_power_pending() || system_reset_blocks_apps() ||
        !epd_sound_read(&sound) || !sound.available || !sound.output_ready || sound.muted ||
        !sound.volume[EPD_SOUND_MASTER] || !sound.volume[channel] || !sound.effective[channel])
        return false;
    return !board_power_pending() && !system_reset_blocks_apps();
}

static bool current(const tone_request_t *value)
{
    rt_enter_critical();
    bool valid = !suppressed && value->generation == generation && !callback_state.failed &&
        (!value->preview || value->preview_generation == preview_generation);
    rt_exit_critical();
    if (!valid || !allowed(value->channel)) return false;
    rt_enter_critical();
    valid = !suppressed && value->generation == generation && !callback_state.failed &&
        (!value->preview || value->preview_generation == preview_generation);
    rt_exit_critical();
    return valid;
}

static int audio_callback(audio_server_callback_cmt_t cmd, void *userdata, uint32_t reserved)
{
    tone_callback_t *state = userdata;
    (void)reserved;
    rt_enter_critical();
    /* A suspended short effect is stale even if immediately resumed. */
    if (cmd == as_callback_cmd_suspended || cmd == as_callback_cmd_closed)
        state->failed = true;
    rt_exit_critical();
    signal_worker();
    return 0;
}

static void play(const tone_request_t *value)
{
    epd_pcm_output_t *client;
    uint32_t offset = 0;
    /* The first DMA block can reapply DAC volume after unmute. Let its
     * hardware ramp settle on silence before a short effect's attack. */
    uint32_t startup_ms = value->channel == EPD_SOUND_TOUCH ?
        TONE_TOUCH_STARTUP_SILENCE_MS : EPD_PCM_STARTUP_SILENCE_MS;
    uint32_t primer_left = value->asset->sample_rate * 2u * startup_ms / 1000u;
    uint32_t silence_left = value->asset->sample_rate * 2u * TONE_SILENCE_MS / 1000u;
    int16_t pcm[TONE_CHUNK_BYTES / sizeof(int16_t)];
    rt_enter_critical();
    memset(&callback_state, 0, sizeof(callback_state));
    rt_exit_critical();
    if (!current(value)) return;
    client = epd_pcm_output_open(value->channel == EPD_SOUND_NOTIFICATION ?
        AUDIO_TYPE_NOTIFY : AUDIO_TYPE_LOCAL_RING, value->asset->sample_rate,
        audio_callback, &callback_state);
    if (!client) return;
    rt_enter_critical();
    output_unclosed = true;
    rt_exit_critical();

    rt_tick_t last_progress = rt_tick_get();
    while ((primer_left || offset < value->asset->bytes || silence_left) && current(value))
    {
        bool priming = primer_left != 0;
        bool silence = priming || offset == value->asset->bytes;
        uint32_t bytes = priming ? primer_left : silence ? silence_left : value->asset->bytes - offset;
        if (bytes > sizeof(pcm)) bytes = sizeof(pcm);
        /* SDK fades and some routes modify input, including on retries. */
        if (silence) memset(pcm, 0, bytes);
        else memcpy(pcm, value->asset->pcm + offset, bytes);
        if (!current(value)) goto close;
        int written = epd_pcm_output_write(client, pcm, bytes);
        if (written < 0 || (uint32_t)written > bytes || written % 2) goto close;
        if (priming) primer_left -= (uint32_t)written;
        else if (silence) silence_left -= (uint32_t)written;
        else offset += (uint32_t)written;
        if (written) last_progress = rt_tick_get();
        else
        {
            if ((rt_tick_t)(rt_tick_get() - last_progress) >=
                (rt_tick_t)rt_tick_from_millisecond(TONE_STALL_MS))
            {
                rt_kprintf("[sound] write timeout: accepted=%u/%u\n",
                    (unsigned)offset, (unsigned)value->asset->bytes);
                goto close;
            }
            wait_worker(TONE_POLL_MS);
        }
    }
    if (offset == value->asset->bytes && !silence_left && current(value))
    {
        uint32_t cache_ms = epd_pcm_output_cached_ms(client);
        /* Silence carries the final samples through block-sized caches. This
         * timed estimate includes scheduling margin, not hardware completion. */
        if (cache_ms > (uint32_t)INT32_MAX - TONE_TAIL_MARGIN_MS) goto close;
        rt_tick_t tail_ticks = rt_tick_from_millisecond((int)(cache_ms + TONE_TAIL_MARGIN_MS));
        rt_tick_t tail_start = rt_tick_get();
        rt_tick_t poll_ticks = rt_tick_from_millisecond(TONE_POLL_MS);
        while (current(value))
        {
            rt_tick_t elapsed = rt_tick_get() - tail_start;
            if (elapsed >= tail_ticks) break;
            rt_tick_t remaining = tail_ticks - elapsed;
            rt_uint32_t received;
            rt_event_recv(&wake, TONE_WAKE, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                remaining < poll_ticks ? remaining : poll_ticks, &received);
        }
    }
close:
    if (epd_pcm_output_close(client) == 0)
    {
        rt_enter_critical();
        output_unclosed = false;
        rt_exit_critical();
    }
}

static void tone_worker(void *argument)
{
    (void)argument;
    for (;;)
    {
        rt_enter_critical();
        bool have_request = pending;
        tone_request_t value = request;
        if (have_request)
        {
            pending = false;
            active = true;
        }
        rt_exit_critical();
        if (!have_request) { wait_worker(-1); continue; }
        play(&value);
        rt_enter_critical();
        active = false;
        rt_exit_critical();
    }
}

void sound_tone_init(void)
{
    if (worker_ready) return;
    if (!event_ready)
    {
        if (rt_event_init(&wake, "tone", RT_IPC_FLAG_PRIO) != RT_EOK) return;
        rt_enter_critical();
        event_ready = true;
        rt_exit_critical();
    }
    rt_thread_t thread = rt_thread_create("tone", tone_worker, NULL, 3072,
                                        RT_THREAD_PRIORITY_HIGH + 1, 10);
    if (!thread) return;
    if (rt_thread_startup(thread) != RT_EOK)
    {
        rt_thread_delete(thread);
        return;
    }
    rt_enter_critical();
    worker_ready = true;
    rt_exit_critical();
}

bool sound_tone_play(epd_sound_channel_t channel, epd_sound_tone_t tone, bool preview)
{
    rt_enter_critical();
    uint32_t observed_generation = generation, observed_preview = preview_generation;
    rt_exit_critical();
    if (!epd_sound_tone_valid(channel, tone) || tone == EPD_TONE_NONE || !allowed(channel))
        return false;
    const epd_tone_asset_t *asset = epd_tone_asset((unsigned)tone);
    if (!asset || !asset->pcm || !asset->bytes || asset->bytes % 2 ||
        asset->sample_rate != 32000) return false;
    rt_enter_critical();
    bool accepted = worker_ready && !suppressed && generation == observed_generation &&
        (!preview || preview_generation == observed_preview) &&
        !(channel == EPD_SOUND_TOUCH && !preview && (active || pending));
    if (accepted)
    {
        request.channel = channel;
        request.asset = asset;
        request.preview = preview;
        request.generation = ++generation;
        request.preview_generation = observed_preview;
        pending = true;
    }
    rt_exit_critical();
    if (accepted) signal_worker();
    return accepted;
}

void sound_tone_stop_preview(void)
{
    rt_enter_critical();
    /* Also invalidate a preview still reading its admission snapshots. */
    ++preview_generation;
    if (pending && request.preview) pending = false;
    rt_exit_critical();
    signal_worker();
}

void sound_tone_cancel(void)
{
    rt_enter_critical();
    ++generation;
    pending = false;
    rt_exit_critical();
    signal_worker();
}

void sound_tone_suppress(bool suppress)
{
    rt_enter_critical();
    suppressed = suppress;
    if (suppress)
    {
        ++generation;
        pending = false;
    }
    rt_exit_critical();
    signal_worker();
}

bool sound_tone_quiet(void)
{
    rt_enter_critical();
    bool quiet = !pending && !active && !output_unclosed;
    rt_exit_critical();
    return quiet;
}
#else
void sound_tone_init(void) {}
bool sound_tone_play(epd_sound_channel_t channel, epd_sound_tone_t tone, bool preview)
{ (void)channel; (void)tone; (void)preview; return false; }
void sound_tone_stop_preview(void) {}
void sound_tone_cancel(void) {}
void sound_tone_suppress(bool suppress) { (void)suppress; }
bool sound_tone_quiet(void) { return true; }
#endif
