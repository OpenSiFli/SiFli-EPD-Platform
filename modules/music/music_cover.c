#include "music_cover.h"
#include "music_artwork.h"
#include "../common/image/raster.h"
#include "storage.h"
#include "epd_memory.h"
#include <dfs_posix.h>
#include <limits.h>
#include <string.h>

#define COVER_SIDE 180u
#define COVER_STACK_BYTES 8192u
#define COVER_PRIORITY 25
#define COVER_WAKE 1u

typedef struct {
    uint64_t serial;
    uint32_t track_id, session;
    bool card, embedded;
    size_t fallback;
    char path[];
} cover_job_t;

static struct {
    struct rt_mutex mutex;
    struct rt_event event;
    struct rt_thread thread;
    void *stack;
    cover_job_t *pending, *ready;
    music_cover_image_t image;
    uint64_t serial;
    uint32_t revision;
    bool initialized, started, stopping, active;
    void (*wake)(void *);
    void *wake_context;
} cover;

static void lock(void) { rt_mutex_take(&cover.mutex, RT_WAITING_FOREVER); }
static void unlock(void) { rt_mutex_release(&cover.mutex); }

void music_cover_image_free(music_cover_image_t *image)
{
    if (!image) return;
    epd_app_free(image->pixels);
    memset(image, 0, sizeof(*image));
}

static bool available(const cover_job_t *job)
{
    return storage_path_available(job->path) &&
        (!job->card || storage_session_valid(STORAGE_SD, job->session));
}

static bool cancelled(void *context)
{
    const cover_job_t *job = context;
    lock();
    bool stale = cover.stopping || job->serial != cover.serial;
    unlock();
    return stale || !available(job);
}

typedef struct {
    cover_job_t *job;
    int fd;
    size_t size;
} cover_source_t;

static int source_read(void *context, size_t offset, void *data, size_t size)
{
    cover_source_t *source = context;
    if (offset > source->size || size > source->size - offset || size > INT_MAX) return -1;
    size_t done = 0;
    while (done < size) {
        if (cancelled(source->job)) return -1;
        size_t n = size - done;
        if (n > 4096) n = 4096;
        storage_lock();
        int got = -1;
        if (available(source->job) && lseek(source->fd, offset + done, SEEK_SET) == (off_t)(offset + done))
            got = read(source->fd, (uint8_t *)data + done, n);
        storage_unlock();
        if (got <= 0 || (size_t)got > n) return -1;
        done += got;
    }
    return (int)done;
}

static bool artwork_cancelled(void *context)
{
    music_artwork_reader_t *reader = context;
    cover_source_t *source = reader->source.context;
    return cancelled(source->job);
}

static bool decode_image(cover_job_t *job, const char *path,
                         music_artwork_reader_t *reader, size_t bytes,
                         music_cover_image_t *image)
{
    raster_decoder_t decoder = {0};
    decoder.fd = -1;
    decoder.context = reader ? (void *)reader : (void *)job;
    decoder.path = path;
    decoder.cancel = reader ? artwork_cancelled : cancelled;
    if (reader) {
        decoder.read_at = music_artwork_read_at;
        decoder.file_size = bytes;
    }
    decoder.background = true;
    decoder.max_width = decoder.max_height = COVER_SIDE;
    bool ok = !cancelled(job) && raster_io_open(&decoder) && raster_decode(&decoder);
    if (ok && decoder.gray && decoder.width && decoder.height && !cancelled(job)) {
        raster_fit(decoder.width, decoder.height, COVER_SIDE, COVER_SIDE,
                   &image->width, &image->height, true);
        size_t bytes = (size_t)image->width * image->height * sizeof(*image->pixels);
        image->pixels = epd_app_alloc(bytes, EPD_APP_PSRAM);
        ok = image->pixels != NULL;
        for (unsigned y = 0; ok && y < image->height; ++y) {
            if (cancelled(job)) { ok = false; break; }
            unsigned sy = (uint64_t)y * decoder.height / image->height;
            for (unsigned x = 0; x < image->width; ++x) {
                unsigned sx = (uint64_t)x * decoder.width / image->width;
                uint8_t gray = decoder.gray[(size_t)sy * decoder.width + sx];
                image->pixels[(size_t)y * image->width + x] =
                    (uint16_t)((gray >> 3) << 11 | (gray >> 2) << 5 | (gray >> 3));
            }
        }
    } else ok = false;
    raster_decoder_clear(&decoder);
    if (!ok) music_cover_image_free(image);
    return ok;
}

static bool decode(cover_job_t *job, music_cover_image_t *image)
{
    if (!job->embedded) return decode_image(job, job->path, NULL, 0, image);
    cover_source_t source = {.job = job, .fd = -1};
    struct stat st;
    storage_lock();
    if (available(job)) source.fd = open(job->path, O_RDONLY);
    bool valid = source.fd >= 0 && fstat(source.fd, &st) == 0 &&
                 S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= SIZE_MAX;
    if (valid) source.size = st.st_size;
    storage_unlock();
    bool ok = false;
    if (valid && !cancelled(job)) {
        music_artwork_source_t input = {&source, source_read, source.size};
        music_artwork_t artwork;
        if (music_artwork_find(&input, &artwork) && !cancelled(job)) {
            music_artwork_reader_t reader;
            music_artwork_reader_init(&reader, &input, &artwork);
            ok = decode_image(job, job->path, &reader, artwork.decoded_bytes, image);
        }
    }
    if (source.fd >= 0) {
        storage_lock();
        close(source.fd);
        storage_unlock();
    }
    if (!ok && !cancelled(job) && job->path[job->fallback])
        ok = decode_image(job, job->path + job->fallback, NULL, 0, image);
    return ok;
}

static void run(void *parameter)
{
    (void)parameter;
    for (;;) {
        lock();
        bool stopping = cover.stopping;
        cover_job_t *job = cover.pending;
        cover.pending = NULL;
        cover.active = job != NULL;
        unlock();
        if (stopping) { epd_app_free(job); return; }
        if (!job) {
            rt_uint32_t received;
            rt_event_recv(&cover.event, COVER_WAKE, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, &received);
            continue;
        }
        music_cover_image_t image = {0};
        bool ok = decode(job, &image);
        lock();
        if (!cover.stopping && job->serial == cover.serial) {
            if (ok && available(job)) {
                cover.image = image;
                cover.ready = job;
                memset(&image, 0, sizeof(image));
                job = NULL;
            }
            ++cover.revision;
        }
        unlock();
        music_cover_image_free(&image);
        epd_app_free(job);
        lock();
        cover.active = false;
        void (*wake)(void *) = cover.wake;
        void *context = cover.wake_context;
        unlock();
        if (wake) wake(context);
    }
}

/* Called with the mutex held; the new thread cannot consume the job until
 * request has finished publishing it. Static threads detach on normal exit. */
static bool start(void)
{
    if (cover.started) return true;
    cover.stack = epd_app_alloc(COVER_STACK_BYTES, EPD_APP_PSRAM);
    if (!cover.stack) return false;
    int result = rt_thread_init(&cover.thread, "muscover", run, NULL, cover.stack,
                               COVER_STACK_BYTES, COVER_PRIORITY, 10);
    if (result == RT_EOK) {
        result = rt_thread_startup(&cover.thread);
        if (result != RT_EOK) rt_thread_detach(&cover.thread);
    }
    if (result != RT_EOK) {
        epd_app_free(cover.stack);
        cover.stack = NULL;
        return false;
    }
    cover.started = true;
    return true;
}

int music_cover_init(void)
{
    if (cover.initialized) return 0;
    memset(&cover, 0, sizeof(cover));
    if (rt_mutex_init(&cover.mutex, "muscover", RT_IPC_FLAG_PRIO) != RT_EOK) return -1;
    if (rt_event_init(&cover.event, "muscover", RT_IPC_FLAG_PRIO) != RT_EOK) {
        rt_mutex_detach(&cover.mutex);
        return -1;
    }
    cover.initialized = true;
    return 0;
}

void music_cover_set_wake(void (*wake)(void *), void *context)
{
    lock();
    cover.wake = wake;
    cover.wake_context = context;
    unlock();
}

bool music_cover_busy(void)
{
    if (!cover.initialized) return false;
    lock();
    bool busy = cover.pending || cover.active;
    unlock();
    return busy;
}

void music_cover_trim(void)
{
    if (!cover.initialized) return;
    lock();
    if (cover.active || cover.pending || cover.stopping) { unlock(); return; }
    cover_job_t *ready = cover.ready;
    music_cover_image_t image = cover.image;
    cover.ready = NULL;
    memset(&cover.image, 0, sizeof(cover.image));
    if (ready || image.pixels) ++cover.revision;
    unlock();
    epd_app_free(ready);
    music_cover_image_free(&image);
}

bool music_cover_request(uint32_t track_id, const char *audio_path, const char *fallback)
{
    if (!cover.initialized) return false;
    bool embedded = audio_path && *audio_path;
    const char *path = embedded ? audio_path : fallback;
    bool clear = !path || !*path;
    cover_job_t *job = NULL;
    if (!clear) {
        size_t length = strlen(path);
        size_t extra = embedded && fallback ? strlen(fallback) : 0;
        if (extra < SIZE_MAX - sizeof(*job) - 2 && length <= SIZE_MAX - sizeof(*job) - extra - 2) {
            job = epd_app_alloc(sizeof(*job) + length + extra + 2, EPD_APP_PSRAM);
            if (job) {
                memset(job, 0, sizeof(*job));
                memcpy(job->path, path, length + 1);
                job->embedded = embedded;
                job->fallback = length + 1;
                if (extra) memcpy(job->path + job->fallback, fallback, extra);
                job->path[job->fallback + extra] = 0;
                job->track_id = track_id;
                job->session = storage_card_session();
                const char *root = storage_path_root(path);
                job->card = root && !strcmp(root, STORAGE_SD_ROOT);
            }
        }
    }
    lock();
    if (cover.stopping) { unlock(); epd_app_free(job); return false; }
    cover_job_t *old_pending = cover.pending, *old_ready = cover.ready;
    music_cover_image_t old_image = cover.image;
    cover.pending = cover.ready = NULL;
    memset(&cover.image, 0, sizeof(cover.image));
    ++cover.serial;
    ++cover.revision;
    bool accepted = clear;
    if (job && available(job) && start()) {
        job->serial = cover.serial;
        cover.pending = job;
        job = NULL;
        accepted = true;
    }
    if (cover.started) rt_event_send(&cover.event, COVER_WAKE);
    unlock();
    epd_app_free(job);
    epd_app_free(old_pending);
    epd_app_free(old_ready);
    music_cover_image_free(&old_image);
    return accepted;
}

static bool thread_closed(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool closed = (cover.thread.stat & RT_THREAD_STAT_MASK) == RT_THREAD_CLOSE;
    rt_hw_interrupt_enable(level);
    return closed;
}

void music_cover_stop(void)
{
    if (!cover.initialized) return;
    lock();
    cover.stopping = true;
    ++cover.serial;
    ++cover.revision;
    cover_job_t *pending = cover.pending, *ready = cover.ready;
    music_cover_image_t image = cover.image;
    cover.pending = cover.ready = NULL;
    memset(&cover.image, 0, sizeof(cover.image));
    bool started = cover.started;
    if (started) rt_event_send(&cover.event, COVER_WAKE);
    unlock();
    epd_app_free(pending);
    epd_app_free(ready);
    music_cover_image_free(&image);
    if (started) while (!thread_closed()) rt_thread_mdelay(1);
    lock();
    epd_app_free(cover.stack);
    cover.stack = NULL;
    cover.started = cover.stopping = cover.active = false;
    unlock();
}

void music_cover_cleanup(void)
{
    if (!cover.initialized) return;
    music_cover_stop();
    cover.initialized = false;
    rt_event_detach(&cover.event);
    rt_mutex_detach(&cover.mutex);
}

/* A ready image can outlive a card removal until the next UI poll. */
static void invalidate(void)
{
    if (cover.ready && !available(cover.ready)) {
        epd_app_free(cover.ready);
        cover.ready = NULL;
        music_cover_image_free(&cover.image);
        ++cover.revision;
    }
}

uint32_t music_cover_revision(void)
{
    if (!cover.initialized) return 0;
    lock();
    invalidate();
    uint32_t revision = cover.revision;
    unlock();
    return revision;
}

bool music_cover_copy(uint32_t track_id, music_cover_image_t *out)
{
    if (!cover.initialized || !out || out->pixels) return false;
    memset(out, 0, sizeof(*out));
    lock();
    invalidate();
    if (cover.ready && cover.ready->track_id == track_id) {
        size_t bytes = (size_t)cover.image.width * cover.image.height * sizeof(*out->pixels);
        out->pixels = epd_app_alloc(bytes, EPD_APP_PSRAM);
        if (out->pixels) {
            memcpy(out->pixels, cover.image.pixels, bytes);
            out->width = cover.image.width;
            out->height = cover.image.height;
        }
    }
    unlock();
    return out->pixels != NULL;
}
