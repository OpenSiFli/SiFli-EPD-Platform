/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service_internal.h"
#include "platform/epd_system_internal.h"
#include "speech_media.h"
#include "app_service.h"
#include "../sound_tone.h"
#include <rthw.h>
#include <string.h>
#ifdef AUDIO_USING_MANAGER
#include <audio_server.h>
#endif

#define MEDIA_WAIT_MS 2000u
#define MEDIA_POLL_MS 10

static struct rt_mutex guard;
static bool initialized, held[2], acquiring, restore_music;
static epd_speech_kind_t acquiring_kind;
static bool tts_quiet = true;
static epd_service_t *tts_service;

static bool lock(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!initialized)
        initialized = rt_mutex_init(&guard, "speechmedia", RT_IPC_FLAG_PRIO) == RT_EOK;
    bool ready = initialized;
    rt_hw_interrupt_enable(level);
    return ready && rt_mutex_take(&guard, RT_WAITING_FOREVER) == RT_EOK;
}
static void unlock(void) { rt_mutex_release(&guard); }
static bool valid_kind(epd_speech_kind_t kind)
{ return kind == EPD_SPEECH_ASR || kind == EPD_SPEECH_TTS; }

static bool expired(rt_tick_t start)
{
    return (rt_tick_t)(rt_tick_get() - start) >=
        (rt_tick_t)rt_tick_from_millisecond(MEDIA_WAIT_MS);
}

static void release_locked(epd_speech_kind_t kind, bool restore)
{
    held[kind] = false;
    restore_music = restore_music && restore;
    if (kind == EPD_SPEECH_ASR) sound_tone_suppress(false);
    else
    {
        if (tts_service) epd_media_publish_overlay(tts_service, NULL, NULL, true);
        tts_service = NULL;
        tts_quiet = true;
    }
    if (!held[0] && !held[1]) epd_media_lease_end(restore_music);
}

static bool begin_locked(epd_speech_kind_t kind, bool paused)
{
    if (!held[0] && !held[1])
    {
        if (!epd_media_lease_begin()) return false;
        restore_music = true;
    }
    held[kind] = true;
    acquiring = true;
    acquiring_kind = kind;
    if (kind == EPD_SPEECH_ASR) sound_tone_suppress(true);
    else tts_quiet = paused;
    return true;
}

static epd_speech_error_t wait_acquired(epd_speech_kind_t kind, speech_cancel_fn cancel,
                                      void *user, rt_tick_t start, bool rollback,
                                      speech_cancel_fn handoff_to_asr)
{
    epd_speech_error_t result = EPD_SPEECH_OUTPUT_TIMEOUT;
    for (;;)
    {
        if (cancel && cancel(user)) { result = EPD_SPEECH_CANCELLED; break; }
        if (!lock()) { result = EPD_SPEECH_BUSY; break; }
        bool valid = epd_media_lease_valid();
        bool ready = valid && epd_media_lease_ready() &&
            (kind != EPD_SPEECH_ASR || (tts_quiet && sound_tone_quiet()));
        unlock();
        if (!valid) { result = EPD_SPEECH_OUTPUT_INTERRUPTED; break; }
        if (ready)
        { result = EPD_SPEECH_OK; break; }
        if (expired(start)) break;
        rt_thread_mdelay(MEDIA_POLL_MS);
    }
    bool handoff = rollback && result == EPD_SPEECH_CANCELLED &&
        handoff_to_asr && handoff_to_asr(user);
    if (lock())
    {
        acquiring = false;
        if (result != EPD_SPEECH_OK && rollback && !handoff) release_locked(kind, true);
        unlock();
    }
    return result;
}

epd_speech_error_t speech_media_acquire(epd_speech_kind_t kind,
                                      speech_cancel_fn cancel, void *user)
{
    if (!valid_kind(kind)) return EPD_SPEECH_INVALID;
    rt_tick_t start = rt_tick_get();
    for (;;)
    {
        if (cancel && cancel(user)) return EPD_SPEECH_CANCELLED;
        if (!lock()) return EPD_SPEECH_BUSY;
        if (held[kind]) { unlock(); return EPD_SPEECH_BUSY; }
        if (kind == EPD_SPEECH_ASR &&
            ((acquiring && acquiring_kind == EPD_SPEECH_TTS) ||
             (held[EPD_SPEECH_TTS] && !tts_quiet)))
        {
            unlock();
            if (expired(start)) return EPD_SPEECH_OUTPUT_TIMEOUT;
            rt_thread_mdelay(MEDIA_POLL_MS);
            continue;
        }
        if (acquiring || (kind == EPD_SPEECH_TTS && held[EPD_SPEECH_ASR]))
        { unlock(); return EPD_SPEECH_BUSY; }
        bool begun = begin_locked(kind, false);
        unlock();
        if (!begun) return EPD_SPEECH_BUSY;
        return wait_acquired(kind, cancel, user, start, true, NULL);
    }
}

epd_speech_error_t speech_media_reacquire_tts(speech_cancel_fn cancel, void *user,
                                            speech_cancel_fn handoff_to_asr)
{
    if (cancel && cancel(user)) return EPD_SPEECH_CANCELLED;
    rt_tick_t start = rt_tick_get();
    if (!lock()) return EPD_SPEECH_BUSY;
    if (acquiring || held[EPD_SPEECH_ASR] || !tts_quiet)
    { unlock(); return EPD_SPEECH_BUSY; }
    bool retained = held[EPD_SPEECH_TTS] && epd_media_lease_valid();
    if (retained)
    {
        acquiring = true;
        acquiring_kind = EPD_SPEECH_TTS;
    }
    else
    {
        if (held[EPD_SPEECH_TTS]) release_locked(EPD_SPEECH_TTS, false);
        if (!begin_locked(EPD_SPEECH_TTS, true)) { unlock(); return EPD_SPEECH_BUSY; }
    }
    unlock();
    return wait_acquired(EPD_SPEECH_TTS, cancel, user, start, !retained, handoff_to_asr);
}

bool speech_media_can_start(epd_speech_kind_t kind)
{
    if (!valid_kind(kind) || !lock()) return false;
    bool ready = held[kind] && !acquiring &&
        (kind != EPD_SPEECH_TTS || !held[EPD_SPEECH_ASR]) &&
        (kind != EPD_SPEECH_ASR || (tts_quiet && sound_tone_quiet())) &&
        epd_media_lease_ready();
    unlock(); return ready;
}

bool speech_media_publish(epd_service_t *service, epd_service_t *requester, const char *title,
                          bool playing, bool paused)
{
    if (!service || (playing && paused) || !lock()) return false;
    if ((tts_service && tts_service != service) ||
        (title && (!held[EPD_SPEECH_TTS] ||
                   (playing && (held[EPD_SPEECH_ASR] || !epd_media_lease_ready())))))
    { unlock(); return false; }
    epd_media_t value = {0};
    if (title)
    {
        size_t length = strlen(title);
        if (length >= sizeof(value.title)) length = sizeof(value.title) - 1;
        memcpy(value.title, title, length);
        value.playing = playing;
        value.capabilities = EPD_MEDIA_CAN_STOP |
            (paused ? EPD_MEDIA_CAN_PLAY : EPD_MEDIA_CAN_PAUSE);
#ifdef AUDIO_USING_MANAGER
        value.audio_type = AUDIO_TYPE_LOCAL_MUSIC;
#endif
    }
    if (title && paused)
    {
        char id[EPD_SYSTEM_APP_ID_SIZE];
        if (epd_service_identity(service, id, sizeof(id)))
        {
            tts_service = service;
            tts_quiet = true;
        }
    }
    bool ok = epd_media_publish_overlay(service, requester, title ? &value : NULL, paused || !title);
    if (ok)
    {
        tts_service = title ? service : NULL;
        tts_quiet = !title || paused;
    }
    unlock(); return ok;
}

bool speech_media_poll(epd_service_t *service, epd_media_command_t *command)
{
    if (!service || !command || !lock()) return false;
    bool ok = service == tts_service && epd_media_poll(service, command);
    unlock(); return ok;
}

void speech_media_release(epd_speech_kind_t kind, bool restore)
{
    if (!valid_kind(kind) || !lock()) return;
    if (!held[kind] || (acquiring && acquiring_kind == kind)) { unlock(); return; }
    release_locked(kind, restore);
    unlock();
}
