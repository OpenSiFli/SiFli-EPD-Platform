#include "music_flac_worker.h"
#include "music_decoder.h"
#include "epd_memory.h"
#include <rtthread.h>
#include <rthw.h>
#include <string.h>

#define AUDIO_STACK_BYTES 20480u
#define AUDIO_PRIORITY (RT_THREAD_PRIORITY_HIGH + 1)
#define AUDIO_STEPS 32u
#define AUDIO_WAKE 1u
#define AUDIO_DONE 2u

typedef enum {
    COMMAND_NONE, COMMAND_OPEN, COMMAND_PLAY, COMMAND_PAUSE,
    COMMAND_SEEK, COMMAND_CLOSE, COMMAND_STOP
} backend_command_t;

struct music_flac_worker {
    struct rt_mutex api;
    struct rt_event event;
    struct rt_thread thread;
    void *stack;
    music_decoder_t *decoder;
    music_backend_wake_fn wake;
    void *userdata;
    music_backend_snapshot_t snapshot;
    backend_command_t command;
    const char *path;
    uint64_t position;
    int result;
};

/* Also used from Audio Manager callbacks. No decoder or API lock is taken. */
static void wake_audio(void *context)
{
    music_flac_worker_t *b = context;
    rt_event_send(&b->event, AUDIO_WAKE);
}

static void publish(music_flac_worker_t *b)
{
    music_backend_snapshot_t value;
    music_decoder_snapshot(b->decoder, &value);
    rt_base_t level = rt_hw_interrupt_disable();
    bool changed = value.state != b->snapshot.state ||
                   value.session_id != b->snapshot.session_id ||
                   value.position_ms / 1000 != b->snapshot.position_ms / 1000 ||
                   value.error != b->snapshot.error;
    b->snapshot = value;
    rt_hw_interrupt_enable(level);
    if (changed && b->wake) b->wake(b->userdata);
}

static backend_command_t pending(music_flac_worker_t *b)
{
    rt_base_t level = rt_hw_interrupt_disable();
    backend_command_t command = b->command;
    rt_hw_interrupt_enable(level);
    return command;
}

static void audio_run(void *context)
{
    music_flac_worker_t *b = context;
    for (;;) {
        backend_command_t command = pending(b);
        int result = 0;
        switch (command) {
        case COMMAND_OPEN: result = music_decoder_open(b->decoder, b->path); break;
        case COMMAND_PLAY: result = music_decoder_play(b->decoder); break;
        case COMMAND_PAUSE: result = music_decoder_pause(b->decoder); break;
        case COMMAND_SEEK: result = music_decoder_seek(b->decoder, b->position); break;
        case COMMAND_CLOSE: result = music_decoder_close(b->decoder); break;
        case COMMAND_STOP:
            /* A failed close still owns its callback resources. Keep
             * the service and module alive until the owner confirms release. */
            do {
                result = music_decoder_close(b->decoder);
                if (result) {
                    rt_thread_mdelay(20);
                }
            } while (result);
            break;
        default: break;
        }
        if (command != COMMAND_NONE) {
            publish(b);
            rt_base_t level = rt_hw_interrupt_disable();
            b->result = result;
            b->command = COMMAND_NONE;
            rt_hw_interrupt_enable(level);
            rt_event_send(&b->event, AUDIO_DONE);
            if (command == COMMAND_STOP) return;
        }

        int more = 0;
        for (unsigned step = 0; step < AUDIO_STEPS && pending(b) == COMMAND_NONE; ++step) {
            more = music_decoder_pump(b->decoder);
            if (more <= 0) break;
        }
        publish(b);
        if (pending(b) != COMMAND_NONE) continue;
        music_backend_state_t state = b->snapshot.state;
        /* Full output caches wait for callbacks. The timed fallback also
         * advances the tail deadline if no further DMA callback arrives. */
        rt_int32_t timeout = more > 0 ? 1 :
            state == MUSIC_BACKEND_PLAYING || state == MUSIC_BACKEND_DRAINING ?
            (rt_int32_t)rt_tick_from_millisecond(10) : RT_WAITING_FOREVER;
        rt_uint32_t received;
        int wait_result = rt_event_recv(&b->event, AUDIO_WAKE,
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, timeout, &received);
        if (wait_result != RT_EOK && wait_result != -RT_ETIMEOUT)
            rt_thread_mdelay(1);
    }
}

music_flac_worker_t *music_flac_create(music_backend_wake_fn wake, void *userdata)
{
    music_flac_worker_t *b = epd_app_alloc(sizeof(*b), EPD_APP_PSRAM);
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->wake = wake;
    b->userdata = userdata;
    if (rt_mutex_init(&b->api, "musapi", RT_IPC_FLAG_PRIO) != RT_EOK) goto free_backend;
    if (rt_event_init(&b->event, "flacaudio", RT_IPC_FLAG_PRIO) != RT_EOK) goto free_mutex;
    b->decoder = music_decoder_create(wake_audio, b);
    b->stack = epd_app_alloc(AUDIO_STACK_BYTES, EPD_APP_PSRAM);
    if (!b->decoder || !b->stack) goto free_resources;
    if (rt_thread_init(&b->thread, "flacaudio", audio_run, b, b->stack,
                       AUDIO_STACK_BYTES, AUDIO_PRIORITY, 10) != RT_EOK) goto free_resources;
    if (rt_thread_startup(&b->thread) != RT_EOK) {
        rt_thread_detach(&b->thread);
        goto free_resources;
    }
    return b;

free_resources:
    music_decoder_destroy(b->decoder);
    epd_app_free(b->stack);
    rt_event_detach(&b->event);
free_mutex:
    rt_mutex_detach(&b->api);
free_backend:
    epd_app_free(b);
    return NULL;
}

static int call(music_flac_worker_t *b, backend_command_t command, const char *path,
                uint64_t position)
{
    if (!b) return -MUSIC_BACKEND_INVALID;
    rt_mutex_take(&b->api, RT_WAITING_FOREVER);
    rt_base_t level = rt_hw_interrupt_disable();
    b->path = path;
    b->position = position;
    b->command = command;
    rt_hw_interrupt_enable(level);
    wake_audio(b);
    rt_uint32_t received;
    /* A wait error does not complete the command or transfer ownership back
     * to the caller. In particular, STOP must still await its acknowledgement. */
    while (rt_event_recv(&b->event, AUDIO_DONE, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                         RT_WAITING_FOREVER, &received) != RT_EOK)
        rt_thread_mdelay(1);
    int result = b->result;
    rt_mutex_release(&b->api);
    return result;
}

void music_flac_request_cancel(music_flac_worker_t *b)
{
    if (b) music_decoder_request_cancel(b->decoder);
}

void music_flac_destroy(music_flac_worker_t *b)
{
    if (!b) return;
    music_flac_request_cancel(b);
    call(b, COMMAND_STOP, NULL, 0);
    /* The completion event precedes the RTOS exit trampoline. Static thread
     * storage and module code must outlive both the entry and the trampoline. */
    for (;;) {
        rt_base_t level = rt_hw_interrupt_disable();
        bool exited = (b->thread.stat & RT_THREAD_STAT_MASK) == RT_THREAD_CLOSE;
        rt_hw_interrupt_enable(level);
        if (exited) break;
        rt_thread_mdelay(1);
    }
    music_decoder_destroy(b->decoder);
    rt_event_detach(&b->event);
    rt_mutex_detach(&b->api);
    epd_app_free(b->stack);
    epd_app_free(b);
}

int music_flac_open(music_flac_worker_t *b, const char *path)
{
    if (!path || !*path) return -MUSIC_BACKEND_INVALID;
    return call(b, COMMAND_OPEN, path, 0);
}
int music_flac_play(music_flac_worker_t *b) { return call(b, COMMAND_PLAY, NULL, 0); }
int music_flac_pause(music_flac_worker_t *b) { return call(b, COMMAND_PAUSE, NULL, 0); }
int music_flac_seek(music_flac_worker_t *b, uint64_t position)
{ return call(b, COMMAND_SEEK, NULL, position); }
int music_flac_close(music_flac_worker_t *b)
{
    music_flac_request_cancel(b);
    return call(b, COMMAND_CLOSE, NULL, 0);
}

void music_flac_snapshot(music_flac_worker_t *b, music_backend_snapshot_t *snapshot)
{
    if (!b || !snapshot) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *snapshot = b->snapshot;
    rt_hw_interrupt_enable(level);
}

int music_flac_pump(music_flac_worker_t *b)
{
    if (!b) return -MUSIC_BACKEND_INVALID;
    music_backend_snapshot_t snapshot;
    music_flac_snapshot(b, &snapshot);
    return snapshot.state == MUSIC_BACKEND_ERROR ? -(int)snapshot.error : 0;
}
