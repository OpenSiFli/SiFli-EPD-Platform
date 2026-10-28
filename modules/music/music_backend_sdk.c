#include "music_backend_sdk.h"
#include "music_flac_worker.h"
#include "epd_memory.h"
#include <audio_mp3ctrl.h>
#include <dfs_posix.h>
#include <errno.h>
#include <rthw.h>
#include <rtthread.h>
#include <string.h>

struct music_backend {
    music_backend_wake_fn wake;
    void *userdata;
    mp3ctrl_handle controller;
    char *filename;
    music_flac_worker_t *flac;
    bool flac_active;
    bool callbacks;
    bool cancelled;
    bool started;
    bool progress_seen;
    bool suspended;
    music_backend_snapshot_t snapshot;
};

static void wake_service(music_backend_t *b)
{
    if (b->wake) b->wake(b->userdata);
}

static int fail(music_backend_t *b, music_backend_error_t error, int detail)
{
    rt_base_t level = rt_hw_interrupt_disable();
    b->snapshot.error = error;
    b->snapshot.error_detail = detail;
    b->snapshot.state = MUSIC_BACKEND_ERROR;
    b->snapshot.want_play = false;
    rt_hw_interrupt_enable(level);
    wake_service(b);
    return -(int)error;
}

/* SDK controller callbacks only publish state and wake the service. Calling
 * play/pause/seek/close here would wait for the callback's own thread. */
static int controller_event(audio_server_callback_cmt_t command, void *context,
                            uint32_t value)
{
    music_backend_t *b = context;
    bool changed = false;
    rt_base_t level = rt_hw_interrupt_disable();
    if (b->callbacks && !b->cancelled) {
        switch (command) {
        case as_callback_cmd_user:
            /* The implementation passes elapsed seconds in reserved. */
            b->snapshot.position_ms = (uint64_t)value * 1000;
            b->progress_seen = true;
            changed = true;
            break;
        case as_callback_cmd_suspended:
        case as_callback_cmd_resumed:
            b->suspended = command == as_callback_cmd_suspended;
            if (b->snapshot.want_play)
                b->snapshot.state = b->suspended ?
                    MUSIC_BACKEND_SUSPENDED : MUSIC_BACKEND_PLAYING;
            changed = true;
            break;
        case as_callback_cmd_play_to_end:
            b->snapshot.state = MUSIC_BACKEND_ENDED;
            b->snapshot.want_play = false;
            changed = true;
            break;
        default: break;
        }
    }
    rt_hw_interrupt_enable(level);
    if (changed) wake_service(b);
    return 0;
}

music_backend_t *music_backend_create(music_backend_wake_fn wake, void *userdata)
{
    music_backend_t *b = epd_app_alloc(sizeof(*b), EPD_APP_PSRAM);
    if (b) {
        memset(b, 0, sizeof(*b));
        b->wake = wake;
        b->userdata = userdata;
    }
    return b;
}

int music_backend_close(music_backend_t *b)
{
    if (!b) return -MUSIC_BACKEND_INVALID;
    rt_base_t level = rt_hw_interrupt_disable();
    b->callbacks = false;
    rt_hw_interrupt_enable(level);
    int rc = b->flac_active ? music_flac_close(b->flac) :
             b->controller ? mp3ctrl_close(b->controller) : 0;
    if (rc) return fail(b, MUSIC_BACKEND_OUTPUT, rc);
    b->controller = NULL;
    b->flac_active = false;
    epd_app_free(b->filename);
    b->filename = NULL;
    level = rt_hw_interrupt_disable();
    uint64_t session = b->snapshot.session_id;
    memset(&b->snapshot, 0, sizeof(b->snapshot));
    b->snapshot.session_id = session;
    b->started = b->progress_seen = b->suspended = b->cancelled = false;
    rt_hw_interrupt_enable(level);
    return 0;
}

void music_backend_destroy(music_backend_t *b)
{
    if (!b || music_backend_close(b)) return;
    music_flac_destroy(b->flac);
    epd_app_free(b);
}

int music_backend_open(music_backend_t *b, const char *path)
{
    if (!b || !path || !*path) return -MUSIC_BACKEND_INVALID;
    int rc = music_backend_close(b);
    if (rc) return rc;
    unsigned char signature[12];
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return fail(b, MUSIC_BACKEND_IO, errno);
    ssize_t count = read(fd, signature, sizeof(signature));
    int read_error = errno;
    close(fd);
    if (count < 0) return fail(b, MUSIC_BACKEND_IO, read_error);
    if (count < 4) return fail(b, MUSIC_BACKEND_UNSUPPORTED, 0);

    rt_base_t level = rt_hw_interrupt_disable();
    ++b->snapshot.session_id;
    rt_hw_interrupt_enable(level);
    if (!memcmp(signature, "fLaC", 4)) {
        if (!b->flac) {
            music_flac_worker_t *flac = music_flac_create(b->wake, b->userdata);
            if (!flac) return fail(b, MUSIC_BACKEND_NO_MEMORY, 0);
            level = rt_hw_interrupt_disable();
            b->flac = flac;
            rt_hw_interrupt_enable(level);
        }
        b->flac_active = true;
        return music_flac_open(b->flac, path);
    }

    bool wav = count == sizeof(signature) &&
        !memcmp(signature, "RIFF", 4) && !memcmp(signature + 8, "WAVE", 4);
    mp3_info_t info = {0};
    rc = mp3ctrl_getinfo(path, &info);
    if (rc || !info.samplerate || (info.channels != 1 && info.channels != 2))
        return fail(b, MUSIC_BACKEND_UNSUPPORTED, rc);
    b->filename = epd_app_alloc(strlen(path) + 1, EPD_APP_PSRAM);
    if (!b->filename) return fail(b, MUSIC_BACKEND_NO_MEMORY, 0);
    strcpy(b->filename, path);
    level = rt_hw_interrupt_disable();
    b->snapshot.format = wav ? MUSIC_BACKEND_FORMAT_WAV : MUSIC_BACKEND_FORMAT_MP3;
    b->snapshot.state = MUSIC_BACKEND_PREPARED;
    b->snapshot.sample_rate = info.samplerate;
    b->snapshot.channels = info.channels;
    b->snapshot.bits_per_sample = 16;
    b->snapshot.duration_ms = (uint64_t)info.total_time_in_seconds * 1000;
    b->snapshot.duration_known = info.total_time_in_seconds != 0;
    b->snapshot.duration_estimated = !wav;
    b->callbacks = true;
    rt_hw_interrupt_enable(level);
    b->controller = mp3ctrl_open(AUDIO_TYPE_LOCAL_MUSIC, b->filename, controller_event, b);
    if (!b->controller) {
        level = rt_hw_interrupt_disable();
        b->callbacks = false;
        rt_hw_interrupt_enable(level);
        return fail(b, MUSIC_BACKEND_IO, 0);
    }
    return 0;
}

int music_backend_play(music_backend_t *b)
{
    if (!b) return -MUSIC_BACKEND_INVALID;
    if (b->flac_active) return music_flac_play(b->flac);
    if (!b->controller) return -MUSIC_BACKEND_INVALID;
    rt_base_t level = rt_hw_interrupt_disable();
    if (b->cancelled || b->snapshot.state == MUSIC_BACKEND_ERROR ||
        b->snapshot.state == MUSIC_BACKEND_ENDED) {
        rt_hw_interrupt_enable(level);
        return -MUSIC_BACKEND_INVALID;
    }
    if (b->snapshot.want_play) {
        rt_hw_interrupt_enable(level);
        return 0;
    }
    b->snapshot.want_play = true;
    b->snapshot.state = b->suspended ? MUSIC_BACKEND_SUSPENDED : MUSIC_BACKEND_PLAYING;
    rt_hw_interrupt_enable(level);
    int rc = b->started ? mp3ctrl_resume(b->controller) : mp3ctrl_play(b->controller);
    if (rc) return fail(b, MUSIC_BACKEND_OUTPUT, rc);
    b->started = true;
    return 0;
}

int music_backend_pause(music_backend_t *b)
{
    if (!b) return -MUSIC_BACKEND_INVALID;
    if (b->flac_active) return music_flac_pause(b->flac);
    if (!b->controller) return -MUSIC_BACKEND_INVALID;
    rt_base_t level = rt_hw_interrupt_disable();
    b->snapshot.want_play = false;
    rt_hw_interrupt_enable(level);
    int rc = mp3ctrl_pause(b->controller);
    if (rc) return fail(b, MUSIC_BACKEND_OUTPUT, rc);
    level = rt_hw_interrupt_disable();
    if (b->snapshot.state != MUSIC_BACKEND_ENDED)
        b->snapshot.state = MUSIC_BACKEND_PAUSED;
    rt_hw_interrupt_enable(level);
    return 0;
}

int music_backend_seek(music_backend_t *b, uint64_t position_ms)
{
    if (!b) return -MUSIC_BACKEND_INVALID;
    if (b->flac_active) return music_flac_seek(b->flac, position_ms);
    music_backend_snapshot_t value;
    music_backend_snapshot(b, &value);
    /* SDK seek needs initialized frame information and resumes decoding.
     * Only an already-playing controller is a valid caller here. */
    if (!b->controller || !value.can_seek || position_ms / 1000 > UINT32_MAX)
        return -MUSIC_BACKEND_SEEK;
    int rc = mp3ctrl_seek(b->controller, (uint32_t)(position_ms / 1000));
    return rc ? fail(b, MUSIC_BACKEND_SEEK, rc) : 0;
}

int music_backend_pump(music_backend_t *b)
{
    if (!b) return -MUSIC_BACKEND_INVALID;
    if (b->flac_active) return music_flac_pump(b->flac);
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancelled = b->cancelled;
    music_backend_error_t error = b->snapshot.error;
    rt_hw_interrupt_enable(level);
    if (cancelled) return fail(b, MUSIC_BACKEND_CANCELLED, 0);
    return -(int)error;
}

void music_backend_snapshot(music_backend_t *b, music_backend_snapshot_t *value)
{
    if (!b || !value) return;
    if (b->flac_active) {
        music_flac_snapshot(b->flac, value);
        value->can_seek = value->state != MUSIC_BACKEND_IDLE &&
                          value->state != MUSIC_BACKEND_ERROR;
        value->session_id = b->snapshot.session_id;
        return;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    *value = b->snapshot;
    value->can_seek = b->progress_seen && !b->cancelled &&
                      value->state == MUSIC_BACKEND_PLAYING;
    rt_hw_interrupt_enable(level);
}

void music_backend_request_cancel(music_backend_t *b)
{
    if (!b) return;
    rt_base_t level = rt_hw_interrupt_disable();
    b->cancelled = true;
    music_flac_worker_t *flac = b->flac;
    rt_hw_interrupt_enable(level);
    if (flac) music_flac_request_cancel(flac);
    wake_service(b);
}
