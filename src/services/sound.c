/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/epd_sound_internal.h"
#include "../ui/platform/epd_sound.h"
#include "sound_tone.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "sleep_guard.h"
#include "epd_memory.h"
#include "epd_shared_service.h"
#include <dfs_posix.h>
#include <rtm.h>
#include <errno.h>
#include <string.h>
#ifdef AUDIO_USING_MANAGER
#include <audio_server.h>
#endif

#define SOUND_PATH STORAGE_FLASH_ROOT "/system/sound.bin"
#define SOUND_MAGIC 0x534e4421u
#define SOUND_VERSION 2u

typedef struct
{
    uint8_t volume[EPD_SOUND_CHANNEL_COUNT];
    bool muted;
    uint8_t notification_tone, touch_tone;
} preference_t;
typedef enum { RECORD_LOADED, RECORD_MISSING, RECORD_INVALID, RECORD_IO } record_result_t;

static struct rt_mutex sound_lock;
static bool lock_ready, started, preference_ready, defaults_ready, use_defaults;
static bool process_scheduled, reload_pending;
static struct rt_semaphore jobs_wake, reload_done;
static bool worker_ready, load_queued, loading, save_queued, writing, save_failed, reload_barrier;
static epd_sound_reload_result_t reload_result = EPD_SOUND_RELOAD_IO_ERROR;
static sleep_guard_owner_t storage_hold = {.id = "sound-settings"};
static rt_tick_t last_process;
static preference_t preference, defaults;
static epd_sound_snapshot_t state;
static uint8_t published_notification_tone, published_touch_tone;

bool epd_sound_tone_valid(epd_sound_channel_t channel, epd_sound_tone_t tone)
{
    if (channel == EPD_SOUND_NOTIFICATION)
        return tone >= EPD_TONE_NONE && tone <= EPD_TONE_CETI;
    if (channel == EPD_SOUND_TOUCH)
        return tone == EPD_TONE_NONE || tone == EPD_TONE_TICK || tone == EPD_TONE_KEYPRESS;
    return false;
}

static bool take_lock(void)
{
    rt_enter_critical();
    if (!lock_ready)
        lock_ready = rt_mutex_init(&sound_lock, "sound", RT_IPC_FLAG_PRIO) == RT_EOK;
    rt_exit_critical();
    return lock_ready && rt_mutex_take(&sound_lock, RT_WAITING_FOREVER) == RT_EOK;
}

static bool equal_preference(const preference_t *a, const preference_t *b)
{
    return !memcmp(a->volume, b->volume, sizeof(a->volume)) && a->muted == b->muted &&
        a->notification_tone == b->notification_tone && a->touch_tone == b->touch_tone;
}

/* Same envelope as storage_record_save; the separate read distinguishes an
 * absent record from unavailable storage, including failure after open(). */
static record_result_t read_record(preference_t *out)
{
    uint32_t header[4];
    uint8_t payload[8], record[sizeof(header) + sizeof(payload)];
    struct stat st;
    size_t used = 0;
    record_result_t result = RECORD_IO;
    if (!storage_path_available(SOUND_PATH) || !storage_file_recover(SOUND_PATH)) return result;
    if (storage_file_writers(SOUND_PATH)) return RECORD_IO;
    uint32_t path_revision = storage_path_revision(SOUND_PATH);
    uint32_t volume_revision = storage_volume_revision(STORAGE_FLASH);
    int fd = open(SOUND_PATH, O_RDONLY);
    if (fd < 0) return storage_path_available(SOUND_PATH) && (errno == ENOENT || errno == -ENOENT) ?
        RECORD_MISSING : RECORD_IO;
    if (fstat(fd, &st) != 0) goto done;
    if (!S_ISREG(st.st_mode) || st.st_size != (int64_t)sizeof(record))
    { result = RECORD_INVALID; goto done; }
    while (used < sizeof(record))
    {
        int n = read(fd, record + used, sizeof(record) - used);
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    memcpy(header, record, sizeof(header));
    memcpy(payload, record + sizeof(header), sizeof(payload));
    result = RECORD_INVALID;
    if (header[0] != SOUND_MAGIC || (header[1] != 1u && header[1] != SOUND_VERSION) ||
        header[2] != sizeof(payload) || header[3] != storage_crc32(payload, sizeof(payload)) ||
        payload[4] > 1 || payload[7]) goto done;
    if (header[1] == 1u)
    {
        if (payload[5] || payload[6]) goto done;
        out->notification_tone = EPD_TONE_ARGON;
        out->touch_tone = EPD_TONE_TICK;
    }
    else
    {
        if (!epd_sound_tone_valid(EPD_SOUND_NOTIFICATION, (epd_sound_tone_t)payload[5]) ||
            !epd_sound_tone_valid(EPD_SOUND_TOUCH, (epd_sound_tone_t)payload[6])) goto done;
        out->notification_tone = payload[5];
        out->touch_tone = payload[6];
    }
    for (unsigned i = 0; i < EPD_SOUND_CHANNEL_COUNT; ++i)
        if (payload[i] > 100) goto done;
    memcpy(out->volume, payload, sizeof(out->volume));
    out->muted = payload[4] != 0;
    result = RECORD_LOADED;
done:
    if (close(fd) != 0 || !storage_path_available(SOUND_PATH) || storage_file_writers(SOUND_PATH) ||
        path_revision != storage_path_revision(SOUND_PATH) ||
        volume_revision != storage_volume_revision(STORAGE_FLASH)) result = RECORD_IO;
    return result;
}

static bool save_record(const preference_t *value)
{
    uint8_t payload[8] = {0};
    memcpy(payload, value->volume, sizeof(value->volume));
    payload[4] = value->muted ? 1 : 0;
    payload[5] = value->notification_tone;
    payload[6] = value->touch_tone;
    return storage_record_save(SOUND_PATH, SOUND_MAGIC, SOUND_VERSION, payload, sizeof(payload));
}

static void publish(const epd_sound_snapshot_t *before)
{
    state.initialized = preference_ready;
    memcpy(state.volume, preference.volume, sizeof(state.volume));
    state.muted = preference.muted;
    bool output_changed = memcmp(before->volume, state.volume, sizeof(state.volume)) ||
        memcmp(before->effective, state.effective, sizeof(state.effective)) ||
        before->max_level != state.max_level || before->initialized != state.initialized ||
        before->available != state.available || before->muted != state.muted ||
        before->output_ready != state.output_ready ||
        published_notification_tone != preference.notification_tone ||
        published_touch_tone != preference.touch_tone;
    published_notification_tone = preference.notification_tone;
    published_touch_tone = preference.touch_tone;
    bool changed = output_changed || before->persisted != state.persisted;
    /* Persistence completion must not interrupt a tone using unchanged output. */
    if (output_changed) sound_tone_cancel();
    if (changed && ++state.revision == 0) ++state.revision;
}

#ifdef AUDIO_USING_MANAGER
static const audio_type_t types[] = {AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TYPE_BT_MUSIC,
                                    AUDIO_TYPE_NOTIFY, AUDIO_TYPE_LOCAL_RING};
static const unsigned channels[] = {EPD_SOUND_MEDIA, EPD_SOUND_MEDIA,
                                    EPD_SOUND_NOTIFICATION, EPD_SOUND_TOUCH};
static unsigned dirty;
static bool public_known;
static uint8_t public_level;
#define DIRTY_PUBLIC (1u << 4)
#define DIRTY_MUTE (1u << 5)

static uint8_t level_for(const preference_t *value, unsigned channel)
{
    uint32_t product = (uint32_t)value->volume[EPD_SOUND_MASTER] * state.max_level;
    if (channel == EPD_SOUND_MASTER) return (uint8_t)((product + 50u) / 100u);
    return (uint8_t)((product * value->volume[channel] + 5000u) / 10000u);
}

static bool backend_ready(void)
{
    if (!state.available)
    {
        /* The SDK getters return defaults before audio_server_init. Its mute
         * setter reports readiness. Writing back the same mute is non-changing,
         * even though the SDK stores it before reporting initialization failure. */
        uint8_t mute = audio_server_get_public_speaker_mute();
        if (audio_server_set_public_speaker_mute(mute) != 0) return false;
        state.available = true;
        public_known = false;
    }
    uint8_t maximum = audio_server_get_max_volume();
    if (!maximum) { state.available = false; return false; }
    state.max_level = maximum;
    if (!defaults_ready)
    {
        preference_t initial = {{100, 100, 0, 0}, false, EPD_TONE_ARGON, EPD_TONE_TICK};
        for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        {
            uint8_t level = audio_server_get_private_volume(types[i]);
            if (level > maximum) { state.available = false; return false; }
            /* Only notification volume inherits the SDK startup level. */
            if (channels[i] == EPD_SOUND_NOTIFICATION) initial.volume[channels[i]] =
                (uint8_t)(((uint32_t)level * 100u + maximum / 2u) / maximum);
        }
        initial.muted = audio_server_get_public_speaker_mute() != 0;
        defaults = initial;
        defaults_ready = true;
    }
    return true;
}

static bool write_mute(bool target)
{
    if (!(dirty & DIRTY_MUTE) && (audio_server_get_public_speaker_mute() != 0) == target)
        return true;
    bool ok = audio_server_set_public_speaker_mute(target ? 1 : 0) == 0;
    if (ok) ok = (audio_server_get_public_speaker_mute() != 0) == target;
    if (ok) dirty &= ~DIRTY_MUTE;
    else dirty |= DIRTY_MUTE;
    return ok;
}

static bool apply_output(const preference_t *value)
{
    bool mute = value->muted || value->volume[EPD_SOUND_MASTER] == 0;
    bool ok = true;
    /* Mute before lowering/changing levels; unmute only after all levels apply. */
    if (mute) ok = write_mute(true);
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
    {
        uint8_t target = level_for(value, channels[i]);
        /* A getter can be returning the public fallback (private == 0xff).
         * Pin every category before first changing that shared public value. */
        if (!public_known || (dirty & (1u << i)) || audio_server_get_private_volume(types[i]) != target)
        {
            bool done = audio_server_set_private_volume(types[i], target) == 0;
            if (done) done = audio_server_get_private_volume(types[i]) == target;
            if (done) dirty &= ~(1u << i);
            else { dirty |= 1u << i; ok = false; }
        }
    }
    uint8_t master = level_for(value, EPD_SOUND_MASTER);
    if (!public_known || (dirty & DIRTY_PUBLIC) || public_level != master)
    {
        if (audio_server_set_public_volume(master) == 0)
        { public_known = true; public_level = master; dirty &= ~DIRTY_PUBLIC; }
        else { dirty |= DIRTY_PUBLIC; ok = false; }
    }
    if (!mute && ok) ok = write_mute(false);
    state.output_ready = ok;
    if (ok)
        for (unsigned i = 0; i < EPD_SOUND_CHANNEL_COUNT; ++i)
            state.effective[i] = mute ? 0 : level_for(value, i);
    else
    {
        /* A failed setter can mean that the SDK lost its initialized state.
         * Its public volume has no getter; reassert it during recovery. */
        public_known = false;
        memset(state.effective, 0, sizeof(state.effective));
    }
    return ok;
}
#else
static bool backend_ready(void) { return false; }
static bool apply_output(const preference_t *value) { (void)value; return false; }
#endif

static void reconcile(void)
{
    state.output_ready = false;
    if (!backend_ready())
    { memset(state.effective, 0, sizeof(state.effective)); return; }
    if (use_defaults && defaults_ready)
    {
        preference = defaults;
        preference_ready = true;
        use_defaults = false;
    }
    if (preference_ready) apply_output(&preference);
}

/* The worker alone accesses storage. sound_lock is never held across I/O. */
static void storage_worker(void *unused)
{
    (void)unused;
    for (;;)
    {
        rt_sem_take(&jobs_wake, RT_WAITING_FOREVER);
        for (;;)
        {
            take_lock();
            bool load = load_queued;
            if (!load && !save_queued) { rt_mutex_release(&sound_lock); break; }
            preference_t value = preference;
            if (load) { load_queued = false; loading = true; }
            else { save_queued = false; writing = true; }
            rt_mutex_release(&sound_lock);

            storage_lock();
            record_result_t result = RECORD_IO;
            bool saved = false;
            if (load) result = read_record(&value);
            else if (system_reset_write_allowed() && !storage_file_writers(SOUND_PATH))
                saved = save_record(&value);
            storage_unlock();

            take_lock();
            epd_sound_snapshot_t before = state;
            if (load)
            {
                loading = false;
                reload_pending = result == RECORD_IO;
                reload_result = result == RECORD_IO ? EPD_SOUND_RELOAD_IO_ERROR :
                    result == RECORD_LOADED ? EPD_SOUND_RELOAD_LOADED : EPD_SOUND_RELOAD_DEFAULTED;
                if (result != RECORD_IO)
                {
                    use_defaults = result != RECORD_LOADED;
                    if (result == RECORD_LOADED) { preference = value; preference_ready = true; }
                    state.persisted = result == RECORD_LOADED;
                    reconcile();
                }
                else state.persisted = false;
            }
            else
            {
                writing = false;
                /* A newer snapshot owns the eventual completion status. */
                if (!save_queued) { state.persisted = saved; save_failed = !saved; }
            }
            publish(&before);
            if (!save_queued && !load_queued) sleep_guard_release(&storage_hold);
            if (load && reload_barrier) rt_sem_release(&reload_done);
            rt_mutex_release(&sound_lock);
        }
    }
}

/* Called under sound_lock, including concurrent initialization attempts. */
static bool start_worker(void)
{
    if (worker_ready) return true;
    epd_shared_service_guard_t guard = {0};
    if (!epd_shared_service_enter(&guard)) return false;
    bool ready = false;
    if (rt_sem_init(&jobs_wake, "sndsave", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto done;
    if (rt_sem_init(&reload_done, "sndload", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto wake_failed;
    rt_thread_t thread = epd_thread_create_psram("sound_save", storage_worker, NULL, 3072, 23, 10);
    if (!thread) goto done_failed;
    if (rt_thread_startup(thread) != RT_EOK) { rt_thread_delete(thread); goto done_failed; }
    worker_ready = ready = true;
    goto done;
done_failed:
    rt_sem_detach(&reload_done);
wake_failed:
    rt_sem_detach(&jobs_wake);
done:
    epd_shared_service_leave(&guard);
    return ready;
}

static bool queue_load(void)
{
    if (!start_worker() || !sleep_guard_acquire(&storage_hold)) return false;
    load_queued = true;
    reload_pending = true;
    state.persisted = false;
    rt_sem_release(&jobs_wake);
    return true;
}

bool epd_sound_init(void)
{
    if (!take_lock()) return false;
    epd_sound_snapshot_t before = state;
    if (!started)
    {
        started = true;
        reload_pending = true;
        queue_load();
        last_process = rt_tick_get();
        process_scheduled = true;
    }
    publish(&before);
    sound_tone_init();
    bool ok = state.initialized && state.available && state.output_ready;
    rt_mutex_release(&sound_lock);
    return ok;
}

bool epd_sound_read(epd_sound_snapshot_t *out)
{
    if (!out || !take_lock()) return false;
    *out = state;
    rt_mutex_release(&sound_lock);
    return out->initialized;
}

typedef enum { SET_VOLUME, SET_MUTE, SET_TONE } preference_change_t;

static bool set_preference(epd_sound_channel_t channel, unsigned value, preference_change_t change)
{
    if (!take_lock()) return false;
    epd_sound_snapshot_t before = state;
    bool ok = false;
    if (!started || !preference_ready || reload_pending || reload_barrier ||
        system_reset_blocks_apps() || storage_changing() || !backend_ready()) goto done;
    preference_t candidate = preference;
    if (change == SET_MUTE) candidate.muted = value != 0;
    else if (change == SET_TONE)
    {
        if (channel == EPD_SOUND_NOTIFICATION) candidate.notification_tone = (uint8_t)value;
        else candidate.touch_tone = (uint8_t)value;
    }
    else candidate.volume[channel] = (uint8_t)value;
    if (equal_preference(&candidate, &preference) && (state.persisted || save_queued || writing))
    { ok = apply_output(&preference); goto done; }
    if (!start_worker() || !sleep_guard_acquire(&storage_hold)) goto done;
    ok = apply_output(&candidate);
    if (ok)
    {
        preference = candidate;
        state.persisted = false;
        bool wake = !save_queued && !writing;
        save_queued = true;
        if (wake) rt_sem_release(&jobs_wake);
    }
    else
    {
        apply_output(&preference);
        if (!save_queued && !writing) sleep_guard_release(&storage_hold);
    }
done:
    publish(&before);
    rt_mutex_release(&sound_lock);
    return ok;
}

bool epd_sound_set_volume(epd_sound_channel_t channel, unsigned percent)
{
    if ((unsigned)channel >= EPD_SOUND_CHANNEL_COUNT || percent > 100) return false;
    return set_preference(channel, percent, SET_VOLUME);
}

bool epd_sound_set_muted(bool muted)
{
    return set_preference(EPD_SOUND_MASTER, muted ? 1u : 0u, SET_MUTE);
}

epd_sound_tone_t epd_sound_get_tone(epd_sound_channel_t channel)
{
    if (!epd_sound_tone_valid(channel, EPD_TONE_NONE) || !take_lock()) return EPD_TONE_NONE;
    epd_sound_tone_t tone = channel == EPD_SOUND_NOTIFICATION ?
        (preference_ready ? (epd_sound_tone_t)preference.notification_tone : EPD_TONE_ARGON) :
        (preference_ready ? (epd_sound_tone_t)preference.touch_tone : EPD_TONE_TICK);
    rt_mutex_release(&sound_lock);
    return tone;
}

bool epd_sound_set_tone(epd_sound_channel_t channel, epd_sound_tone_t tone)
{
    if (!epd_sound_tone_valid(channel, tone)) return false;
    return set_preference(channel, (unsigned)tone, SET_TONE);
}

bool epd_sound_preview(epd_sound_channel_t channel, epd_sound_tone_t tone)
{
    if (!epd_sound_tone_valid(channel, tone)) return false;
    /* Even a suppressed new selection cancels the preceding preview. */
    sound_tone_stop_preview();
    return tone == EPD_TONE_NONE || sound_tone_play(channel, tone, true);
}

void epd_sound_preview_stop(void) { sound_tone_stop_preview(); }
void epd_sound_notify(void)
{ sound_tone_play(EPD_SOUND_NOTIFICATION, epd_sound_get_tone(EPD_SOUND_NOTIFICATION), false); }
void epd_sound_touch(void)
{ sound_tone_play(EPD_SOUND_TOUCH, epd_sound_get_tone(EPD_SOUND_TOUCH), false); }

epd_sound_save_status_t epd_sound_save_status(void)
{
    if (!take_lock()) return EPD_SOUND_SAVE_FAILED;
    epd_sound_save_status_t result = save_queued || writing ? EPD_SOUND_SAVE_PENDING :
        save_failed ? EPD_SOUND_SAVE_FAILED : EPD_SOUND_SAVE_OK;
    rt_mutex_release(&sound_lock);
    return result;
}

/* Explicit lifecycle reload waits outside sound_lock. Refuse to discard an
 * accepted unsaved preference; startup and polling use asynchronous loads. */
epd_sound_reload_result_t epd_sound_reload(void)
{
    if (!take_lock()) return EPD_SOUND_RELOAD_IO_ERROR;
    if (reload_barrier || load_queued || loading || save_queued || writing || save_failed)
    { rt_mutex_release(&sound_lock); return EPD_SOUND_RELOAD_IO_ERROR; }
    epd_sound_snapshot_t before = state;
    if (!queue_load()) { rt_mutex_release(&sound_lock); return EPD_SOUND_RELOAD_IO_ERROR; }
    started = true;
    reload_barrier = true;
    publish(&before);
    sound_tone_cancel();
    rt_mutex_release(&sound_lock);
    rt_sem_take(&reload_done, RT_WAITING_FOREVER);
    take_lock();
    epd_sound_reload_result_t result = reload_result;
    reload_barrier = false;
    rt_mutex_release(&sound_lock);
    return result;
}

void epd_sound_process(void)
{
    if (!take_lock()) return;
    rt_tick_t now = rt_tick_get();
    if (started && (!process_scheduled ||
        (rt_tick_t)(now - last_process) >= (rt_tick_t)rt_tick_from_millisecond(1000)))
    {
        epd_sound_snapshot_t before = state;
        last_process = now;
        process_scheduled = true;
        if (reload_pending)
        {
            if (!load_queued && !loading && !reload_barrier) queue_load();
        }
        else reconcile();
        publish(&before);
        sound_tone_init();
    }
    rt_mutex_release(&sound_lock);
}

RTM_EXPORT(epd_sound_read);
RTM_EXPORT(epd_sound_save_status);
RTM_EXPORT(epd_sound_set_volume);
RTM_EXPORT(epd_sound_set_muted);
RTM_EXPORT(epd_sound_tone_valid);
RTM_EXPORT(epd_sound_get_tone);
RTM_EXPORT(epd_sound_set_tone);
RTM_EXPORT(epd_sound_preview);
RTM_EXPORT(epd_sound_preview_stop);
RTM_EXPORT(epd_sound_notify);
RTM_EXPORT(epd_sound_touch);
