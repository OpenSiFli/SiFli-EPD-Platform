/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_core.h"
#include "speech_audio.h"
#include "speech_media.h"
#include "speech_text.h"
#include "net/network.h"
#include <string.h>

#define PCM_SECOND_BYTES (EPD_SPEECH_SAMPLE_RATE * 2u)
#define OUTPUT_STACK_BYTES 8192u

static bool route_lost(uint32_t generation)
{
    network_snapshot_t network;
    network_snapshot(&network);
    return !network.ready || network.generation != generation;
}
static uint32_t current_route(void)
{
    network_snapshot_t network;
    network_snapshot(&network);
    return network.generation;
}
static void provider_error(speech_session_t *s, speech_provider_session_t *connection, epd_speech_error_t error)
{
    speech_failure_t failure;
    memset(&failure, 0, sizeof(failure));
    speech_provider_failure(connection, &failure);
    speech_lock();
    if (!s->failure.error) {
        s->failure = failure;
        s->failure.error = error;
    }
    speech_unlock();
}

void speech_asr_run(speech_session_t *s)
{
    epd_speech_error_t error = EPD_SPEECH_OK;
    speech_provider_session_t *connection = NULL;
    speech_audio_input_t *input = NULL;
    unsigned char *packet = NULL;
    uint32_t paused_tts = 0, paused_intent = 0;
    bool lease = false, sent_end = false, done = false;
    speech_tts_suspend(s, &paused_tts, &paused_intent);
    uint32_t started = speech_now_ms();
    while (!speech_tts_quiet()) {
        if (speech_session_cancelled(s)) { error = EPD_SPEECH_CANCELLED; goto cleanup; }
        if (speech_now_ms() - started >= 2000) { error = EPD_SPEECH_OUTPUT_TIMEOUT; goto cleanup; }
        rt_thread_mdelay(10);
    }
    error = speech_media_acquire(EPD_SPEECH_ASR, speech_session_cancelled, s);
    if (error) goto cleanup;
    lease = true;
    packet = speech_alloc(SPEECH_AUDIO_PACKET_BYTES);
    if (!packet) { error = EPD_SPEECH_NO_MEMORY; goto cleanup; }
    speech_provider_sink_t sink = {.text=speech_session_text, .user=s};
    s->network_generation = current_route();
    speech_failure_t opening;
    memset(&opening, 0, sizeof(opening));
    error = speech_provider_open(&s->account, EPD_SPEECH_ASR, s->request.language,
        speech_session_cancelled, s, &sink, &connection, &opening);
    if (error) { speech_lock(); s->failure = opening; speech_unlock(); goto cleanup; }
    if (!speech_media_can_start(EPD_SPEECH_ASR)) { error = EPD_SPEECH_OUTPUT_INTERRUPTED; goto cleanup; }
    error = speech_audio_input_open(speech_session_cancelled, s, &input);
    if (error) goto cleanup;
    speech_session_state(s, EPD_SPEECH_RECORDING);
    size_t buffered = 0;
    uint32_t last_packet = speech_now_ms(), final_deadline = 0, last_update = 0;
    while (!done) {
        if (speech_session_cancelled(s)) { error = EPD_SPEECH_CANCELLED; break; }
        if (route_lost(s->network_generation)) { error = EPD_SPEECH_NO_NETWORK; break; }
        if (!speech_media_can_start(EPD_SPEECH_ASR)) { error = EPD_SPEECH_OUTPUT_INTERRUPTED; break; }
        speech_lock(); bool finish = s->finished; speech_unlock();
        if (finish && !sent_end) speech_audio_input_finish(input);
        speech_audio_input_status_t status;
        speech_audio_input_status(input, &status);
        if (status.error) { error = status.error; break; }
        if (status.duration_limit) {
            speech_lock();
            if (!s->cancelled) { s->view.reason = EPD_SPEECH_DURATION_LIMIT; s->finished = s->view.input_finished = true; }
            speech_unlock();
        }
        uint32_t now = speech_now_ms();
        if (now - last_update >= 1000) {
            speech_lock(); s->view.elapsed_ms = status.elapsed_ms; ++s->view.revision; s->state_pending = true; speech_unlock();
            speech_session_wake(s); last_update = now;
        }
        int received = 0;
        bool read_empty = false;
        if (!sent_end && buffered < SPEECH_AUDIO_PACKET_BYTES) {
            received = speech_audio_input_read(input, packet + buffered, SPEECH_AUDIO_PACKET_BYTES - buffered);
            if (received < 0) { error = (epd_speech_error_t)-received; break; }
            buffered += (size_t)received;
            read_empty = received == 0;
        }
        bool final_packet = status.finished && read_empty;
        if (!sent_end && buffered && (buffered == SPEECH_AUDIO_PACKET_BYTES || final_packet) && now - last_packet >= 200) {
            /* Keep normal scheduler jitter on the capture timebase. Missed
             * whole intervals are not replayed as a burst of queued packets. */
            last_packet += 200u;
            if (now - last_packet >= 200u) last_packet = now;
            error = speech_provider_audio(connection, packet, buffered);
            if (error) break;
            speech_audio_input_ack(input, buffered);
            speech_lock(); s->view.submitted_samples += buffered / 2; speech_unlock();
            buffered = 0;
        }
        if (!sent_end && final_packet && !buffered) {
            error = speech_provider_finish(connection);
            if (error) break;
            sent_end = true; final_deadline = speech_now_ms();
            speech_session_state(s, EPD_SPEECH_FINALIZING);
        }
        error = speech_provider_poll(connection, 10, &done);
        if (error) break;
        if (done && !sent_end) { error = EPD_SPEECH_PROTOCOL; break; }
        if (sent_end && speech_now_ms() - final_deadline >= 10000 && !done) { error = EPD_SPEECH_TIMEOUT; break; }
    }
cleanup:
    if (connection && error && error != EPD_SPEECH_CANCELLED) provider_error(s, connection, error);
    if (input) speech_audio_input_close(input);
    if (connection) speech_provider_close(connection);
    speech_free(packet);
    if (lease) speech_media_release(EPD_SPEECH_ASR, error != EPD_SPEECH_OUTPUT_INTERRUPTED);
    if (speech_media_can_start(EPD_SPEECH_TTS)) speech_tts_restore(paused_tts, paused_intent);
    speech_session_complete(s, error);
}

static void media_commands(speech_session_t *s)
{
    epd_media_command_t command;
    while (speech_media_poll(s->worker, &command)) {
        if (command == EPD_MEDIA_PAUSE) (void)epd_speech_pause(s->owner, s->view.session);
        else if (command == EPD_MEDIA_PLAY) (void)epd_speech_resume(s->owner, s->view.session);
        else if (command == EPD_MEDIA_STOP) (void)epd_speech_stop(s->owner, s->view.session);
        /* Dedicated session guard owns subsequent work, not this command. */
        epd_service_sleep_release(s->worker);
    }
}

/* Only the network worker consumes media commands, including while the
 * provider is waiting inside connect/send/receive. Audio callbacks use the
 * side-effect-free cancellation check instead. */
static bool tts_network_cancelled(void *session)
{
    speech_session_t *s = session;
    media_commands(s);
    return speech_session_cancelled(s);
}

typedef struct { speech_session_t *session; uint32_t intent; } resume_context_t;
static bool resume_cancelled(void *context)
{
    resume_context_t *resume = context;
    return tts_network_cancelled(resume->session) ||
        !speech_tts_resume_current(resume->session, resume->intent);
}
static bool resume_handoff(void *context)
{
    (void)context;
    return speech_tts_asr_pending();
}
static void resume_requested(speech_session_t *s)
{
    resume_context_t resume = {.session=s};
    if (!speech_tts_resume_begin(s, &resume.intent)) return;
    epd_speech_error_t error = speech_media_reacquire_tts(resume_cancelled, &resume, resume_handoff);
    speech_tts_resume_finish(s, resume.intent, error);
}

static size_t queued_audio(speech_session_t *s)
{
    size_t bytes = 0;
    for (unsigned i = 0; i < 2; ++i) bytes += s->slots[i].produced - s->slots[i].consumed;
    return bytes;
}

static void supply_wait(speech_session_t *s, rt_int32_t timeout)
{
    rt_uint32_t events;
    rt_event_recv(&s->output_wakeup, 1u, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, timeout, &events);
}

static void output_cleanup(rt_thread_t thread)
{
    /* RT-Thread idle invokes cleanup with scheduling locked and interrupts
     * disabled, then detaches this static object before allowing the woken
     * worker to run. No blocking operation is allowed here. */
    speech_session_t *s = thread->parameter;
    s->output_thread_done = true;
    epd_service_wake(s->worker);
}

static void tts_supply(void *parameter)
{
    speech_session_t *s = parameter;
    speech_audio_output_t *output = NULL;
    epd_speech_error_t error = EPD_SPEECH_OK;
    const void *pending = NULL;
    size_t pending_bytes = 0;
    uint32_t pending_started = 0;
    bool started = false, buffering = true;
    for (;;) {
        if (speech_session_cancelled(s)) { error = EPD_SPEECH_CANCELLED; break; }
        speech_lock();
        bool pause = s->pause;
        bool net_done = s->output_done;
        speech_slot_t *slot = &s->slots[s->read_slot];
        size_t available = slot->produced - slot->consumed;
        bool complete = slot->complete;
        bool empty = !slot->busy;
        bool all_empty = queued_audio(s) == 0;
        speech_unlock();
        if (pause && !pending) {
            if (output) {
                speech_session_state(s, EPD_SPEECH_PAUSING);
                error = speech_audio_output_drain(output);
                speech_audio_output_close(output); output = NULL;
                speech_lock(); s->output_busy = false; speech_unlock();
                if (error) break;
            }
            speech_lock(); pause = s->pause; speech_unlock();
            if (!pause) continue;
            speech_media_publish(s->worker, s->owner, s->request.title, false, true);
            speech_lock();
            s->pause_confirmed = s->pause && !s->output_busy;
            if (s->pause_confirmed && !s->network_busy && !s->resume_requested && !s->resume_busy)
                sleep_guard_release(&s->sleep);
            pause = s->pause_confirmed;
            speech_unlock();
            if (!pause) continue;
            speech_session_state(s, EPD_SPEECH_PAUSED);
            epd_service_wake(s->worker);
            supply_wait(s, RT_WAITING_FOREVER); continue;
        }
        if (net_done && all_empty && !pending) {
            if (output) {
                speech_session_state(s, EPD_SPEECH_DRAINING);
                error = speech_audio_output_drain(output);
                speech_audio_output_close(output); output = NULL;
                speech_lock(); s->output_busy = false; pause = s->pause; speech_unlock();
                if (error) break;
                if (speech_session_cancelled(s)) { error = EPD_SPEECH_CANCELLED; break; }
                if (pause) continue;
            }
            break;
        }
        if (!pending && complete && !available) {
            speech_lock();
            slot->produced = slot->consumed = 0; slot->busy = slot->complete = false;
            s->read_slot ^= 1u;
            speech_unlock();
            epd_service_wake(s->worker);
            continue;
        }
        size_t threshold = (started ? 1000u : 500u) * PCM_SECOND_BYTES / 1000u;
        if (!pending && (empty || !available || (buffering && available < threshold && !complete))) {
            if (output && !available) {
                error = speech_audio_output_drain(output);
                speech_audio_output_close(output); output = NULL;
                speech_lock(); s->output_busy = false; speech_unlock();
                if (error) break;
            }
            buffering = true;
            speech_session_state(s, EPD_SPEECH_BUFFERING);
            supply_wait(s, rt_tick_from_millisecond(20)); continue;
        }
        if (!output) {
            speech_lock();
            bool allowed = !s->pause;
            if (allowed) { s->output_busy = true; s->pause_confirmed = false; }
            speech_unlock();
            if (!allowed) continue;
            if (!speech_media_can_start(EPD_SPEECH_TTS)) {
                speech_lock(); s->output_busy = false; speech_unlock();
                error = EPD_SPEECH_OUTPUT_INTERRUPTED; break;
            }
            error = speech_audio_output_open(speech_session_cancelled, s, &output);
            if (error) { speech_lock(); s->output_busy = false; speech_unlock(); break; }
            started = true; buffering = false;
            speech_media_publish(s->worker, s->owner, s->request.title, true, false);
            speech_session_state(s, EPD_SPEECH_PLAYING);
        }
        if (!speech_media_can_start(EPD_SPEECH_TTS)) { error = EPD_SPEECH_OUTPUT_INTERRUPTED; break; }
        if (!pending) {
            pending = slot->pcm + slot->consumed;
            pending_bytes = available > 1024 ? 1024 : available;
            pending_started = speech_now_ms();
        }
        if (speech_now_ms() - pending_started >= 2000) { error = EPD_SPEECH_OUTPUT_TIMEOUT; break; }
        int written = speech_audio_output_write(output, pending, pending_bytes);
        if (written == -EPD_SPEECH_BUSY) {
            error = speech_audio_output_handoff(output);
            if (error) break;
            continue;
        }
        if (written < 0) { error = (epd_speech_error_t)-written; break; }
        if (written > 0) {
            if ((size_t)written > pending_bytes || (written & 1)) { error = EPD_SPEECH_PROTOCOL; break; }
            speech_lock(); slot->consumed += (size_t)written;
            s->view.submitted_samples += (unsigned)written / 2;
            s->view.unit = slot->unit;
            if (queued_audio(s) <= 4u * PCM_SECOND_BYTES) epd_service_wake(s->worker);
            speech_unlock();
            pending = NULL; pending_bytes = 0;
        } else rt_thread_mdelay(5);
    }
    if (output) {
        if (!error) { speech_session_state(s, EPD_SPEECH_DRAINING); error = speech_audio_output_drain(output); }
        speech_audio_output_close(output);
    }
    if (!error && speech_session_cancelled(s)) error = EPD_SPEECH_CANCELLED;
    speech_media_publish(s->worker, s->owner, s->request.title, false, false);
    speech_lock(); s->output_busy = false; s->output_error = error; speech_unlock();
}

void speech_tts_run(speech_session_t *s)
{
    epd_speech_error_t error = EPD_SPEECH_OK;
    speech_provider_session_t *connection = NULL;
    bool lease = false, supply_started = false, drain_on_error = false;
    /* Text admission owns the first 12 KiB; the remaining 4 KiB keeps the
     * currently submitted unit inside the single 16 KiB text budget. */
    char *unit = s->text + 12u * 1024u;
    for (unsigned i = 0; i < 2; ++i) {
        s->slots[i].pcm = speech_alloc(EPD_SPEECH_PCM_SLOT_BYTES);
        if (!s->slots[i].pcm) { error = EPD_SPEECH_NO_MEMORY; goto cleanup; }
    }
    s->output_stack = speech_alloc(OUTPUT_STACK_BYTES);
    if (!s->output_stack) { error = EPD_SPEECH_NO_MEMORY; goto cleanup; }
    error = speech_media_acquire(EPD_SPEECH_TTS, tts_network_cancelled, s);
    if (error) goto cleanup;
    lease = true;
    speech_media_publish(s->worker, s->owner, s->request.title, false, false);
    if (rt_event_init(&s->output_wakeup, "speech_pcm", RT_IPC_FLAG_FIFO) != RT_EOK) {
        error = EPD_SPEECH_NO_MEMORY; goto cleanup;
    }
    s->output_event_ready = true;
    if (rt_thread_init(&s->output_thread, "speech_pcm", tts_supply, s, s->output_stack,
        OUTPUT_STACK_BYTES, RT_THREAD_PRIORITY_HIGH + 1, 10) != RT_EOK) { error = EPD_SPEECH_NO_MEMORY; goto cleanup; }
    s->output_thread.cleanup = output_cleanup;
    if (rt_thread_startup(&s->output_thread) != RT_EOK) {
        s->output_thread.cleanup = NULL; rt_thread_detach(&s->output_thread);
        error = EPD_SPEECH_NO_MEMORY; goto cleanup;
    }
    supply_started = true;
    speech_provider_sink_t sink = {.pcm=speech_session_pcm, .user=s};
    bool high_water = false, offline_waiting = false;
    uint32_t offline_started = 0;
    for (;;) {
        media_commands(s);
        resume_requested(s);
        if (speech_session_cancelled(s)) { error = EPD_SPEECH_CANCELLED; break; }
        bool online = network_ready();
        speech_lock();
        epd_speech_error_t output_error = s->output_error;
        bool pause = s->pause;
        speech_slot_t *slot = &s->slots[s->write_slot];
        size_t audio = queued_audio(s);
        if (audio >= 12u * PCM_SECOND_BYTES) high_water = true;
        else if (audio <= 4u * PCM_SECOND_BYTES) high_water = false;
        bool free_slot = !slot->busy;
        size_t count = 0;
        speech_text_result_t text_result = speech_text_unit(s->text, s->text_used, s->finished, &count);
        if (text_result == SPEECH_TEXT_SKIP) {
            memmove(s->text, s->text + count, s->text_used - count); s->text_used -= count;
            if (s->text_used <= 8u*1024u) { s->text_blocked = false; s->writable_pending = true; }
        }
        bool done = s->finished && !s->text_used;
        if (online && !pause && !high_water && free_slot && text_result == SPEECH_TEXT_READY) {
            memcpy(unit, s->text, count); unit[count] = 0;
            memmove(s->text, s->text + count, s->text_used - count); s->text_used -= count;
            if (s->text_used <= 8u*1024u) { s->text_blocked = false; s->writable_pending = true; }
            slot->busy = true; slot->complete = false;
            slot->produced = slot->consumed = 0; slot->unit = ++s->next_unit;
            s->network_busy = true;
        } else count = 0;
        speech_unlock();
        if (output_error) { error = output_error; break; }
        if (text_result == SPEECH_TEXT_INVALID || text_result == SPEECH_TEXT_LONG) {
            error = text_result == SPEECH_TEXT_LONG ? EPD_SPEECH_TEXT_TOO_LONG : EPD_SPEECH_INVALID; break;
        }
        if (done) break;
        if (text_result == SPEECH_TEXT_SKIP) continue;
        if (!online && !pause && !audio && text_result == SPEECH_TEXT_READY) {
            uint32_t now = speech_now_ms();
            if (!offline_waiting) { offline_started = now; offline_waiting = true; }
            if ((uint32_t)(now - offline_started) >= 20000) { error = EPD_SPEECH_NO_NETWORK; break; }
        } else offline_waiting = false;
        if (!count) {
            epd_service_wait(s->worker, offline_waiting ? rt_tick_from_millisecond(100) : RT_WAITING_FOREVER);
            continue;
        }
        speech_session_wake(s);
        if (!sleep_guard_acquire(&s->sleep)) { error = EPD_SPEECH_BUSY; break; }
        speech_failure_t opening;
        memset(&opening, 0, sizeof(opening));
        s->network_generation = current_route();
        error = speech_provider_open(&s->account, EPD_SPEECH_TTS, s->request.language,
            tts_network_cancelled, s, &sink, &connection, &opening);
        if (error) {
            speech_lock();
            s->failure = opening;
            /* This unit has not been submitted. A failed prefetch connection
             * must not discard an earlier, already complete playable unit. */
            drain_on_error = (error == EPD_SPEECH_CONNECT || error == EPD_SPEECH_NO_NETWORK ||
                              error == EPD_SPEECH_TIMEOUT) && queued_audio(s) != 0;
            slot->busy = slot->complete = false;
            speech_unlock();
            break;
        }
        error = speech_provider_text(connection, unit, count);
        if (!error) error = speech_provider_finish(connection);
        bool task_done = false;
        uint32_t started = speech_now_ms();
        while (!error && !task_done) {
            media_commands(s);
            resume_requested(s);
            if (speech_session_cancelled(s)) { error = EPD_SPEECH_CANCELLED; break; }
            if (route_lost(s->network_generation)) { error = EPD_SPEECH_NO_NETWORK; break; }
            speech_lock(); output_error = s->output_error; speech_unlock();
            if (output_error) { error = output_error; break; }
            if (speech_now_ms() - started >= 120000) { error = EPD_SPEECH_TIMEOUT; break; }
            /* Pausing does not cancel an already billed, bounded unit. */
            error = speech_provider_poll(connection, 20, &task_done);
        }
        if (error) {
            provider_error(s, connection, error);
            /* A failed unit must never acquire the short-unit playback
             * exemption while transport close is still in progress. */
            speech_lock(); s->cancelled = true; speech_output_wake(s); speech_unlock();
        }
        speech_provider_close(connection); connection = NULL;
        speech_lock();
        s->network_busy = false;
        if (!error) { slot->complete = true; s->write_slot ^= 1u; }
        speech_output_wake(s);
        speech_unlock();
        if (error) break;
    }
cleanup:
    if (connection) speech_provider_close(connection);
    if (supply_started) {
        speech_lock();
        s->network_busy = false; s->output_done = true;
        if (error && !drain_on_error) s->cancelled = true;
        speech_output_wake(s);
        speech_unlock();
        while (!s->output_thread_done) {
            media_commands(s);
            resume_requested(s);
            if (epd_service_cancelled(s->worker)) rt_thread_mdelay(10);
            else epd_service_wait(s->worker, RT_WAITING_FOREVER);
        }
        if (!error) error = s->output_error;
    }
    if (lease) {
        speech_media_publish(s->worker, s->owner, NULL, false, false);
        speech_media_release(EPD_SPEECH_TTS, error != EPD_SPEECH_OUTPUT_INTERRUPTED);
    }
    speech_session_complete(s, error);
}
