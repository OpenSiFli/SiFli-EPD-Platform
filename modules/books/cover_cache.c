/* SPDX-License-Identifier: Apache-2.0 */
#include "cover_cache.h"
#include "app_service.h"
#include "epd_memory.h"
#include "epd_resource_runtime.h"
#include "epd_activity_work.h"
#include "epd_cold_budget.h"
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define COVER_MAGIC 0x43544842u
#define COVER_VERSION 1u
#define COVER_SLOTS 16u
#define COVER_CHUNK 4096u
#define COVER_MAX_BYTES (DOCUMENT_COVER_WIDTH * DOCUMENT_COVER_HEIGHT * 2u)
#define COVER_ALLOCATION ((COVER_MAX_BYTES + 31u) / 32u * 32u + 31u)
#define COVER_IO_BUDGET (2u * COVER_ALLOCATION)
#define COVER_DISK_RESERVE (2u * 1024u * 1024u)

typedef struct {
    char path[BOOKSHELF_PATH_MAX];
    uint32_t size, modified, format, width_limit, height_limit;
} cover_key_t;
typedef struct {
    uint32_t magic, version, checksum, bytes, width, height, stride;
    cover_key_t key;
    char title[BOOKSHELF_NAME_MAX];
} cover_header_t;
typedef enum { COVER_OPEN, COVER_HEADER, COVER_PIXELS, COVER_SEAL,
               COVER_SYNC, COVER_CLOSE, COVER_COMMIT, COVER_ABORT } cover_phase_t;
typedef struct cover_job {
    epd_activity_work_t *work;
    epd_cold_lease_t disk_lease;
    cover_header_t header;
    cover_key_t key;
    book_file_version_t source, cached;
    document_bitmap_t bitmap;
    char path[STORAGE_PATH_MAX], temporary[STORAGE_PATH_MAX];
    size_t offset, charged;
    uint32_t epoch, session, checksum;
    storage_volume_t volume;
    cover_phase_t phase;
    int fd;
    bool writing, temporary_owned, hit;
} cover_job_t;
static struct {
    cover_job_t *read, *write, *active, *ready;
    epd_service_t *service;
    uint32_t epoch, hits, misses, saved, errors, cancelled;
    size_t bytes;
} cache;
static epd_resource_cache_t cover_resource;
static uint32_t cover_publication;
static void *cover_allocate(size_t bytes)
{
    if (epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM, epd_app_allocation_peak(bytes)) != EPD_RESOURCE_OK)
        return NULL;
    return epd_app_alloc(bytes, EPD_APP_PSRAM);
}
static void cover_wake(void *unused) { (void)unused; epd_service_wake(cache.service); }
static int cover_resource_shrink(void *, uint32_t, size_t, size_t *);
static void cover_resource_publish(void)
{
    if (!cover_resource.provider) return;
    for (;;) {
        rt_base_t level = rt_hw_interrupt_disable();
        uint32_t revision = ++cover_publication;
        size_t total = cache.bytes, reclaimable = 0;
        if (cache.read) reclaimable += cache.read->charged;
        if (cache.write) reclaimable += cache.write->charged;
        if (cache.ready) reclaimable += cache.ready->charged;
        rt_hw_interrupt_enable(level);
        epd_resource_cache_update_region(&cover_resource, EPD_MEMORY_REGION_HCPU_PSRAM, total, reclaimable, 3, 0);
        level = rt_hw_interrupt_disable(); bool current = revision == cover_publication; rt_hw_interrupt_enable(level);
        if (current) return;
    }
}

static uint32_t cover_checksum(uint32_t value, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    while (bytes--) value = (value ^ *p++) * 16777619u;
    return value;
}
static void cover_key(cover_key_t *key, const bookshelf_item_t *file)
{
    memset(key, 0, sizeof(*key));
    snprintf(key->path, sizeof(key->path), "%s", file->path);
    key->size = file->size; key->modified = file->modified; key->format = file->format;
    key->width_limit = DOCUMENT_COVER_WIDTH; key->height_limit = DOCUMENT_COVER_HEIGHT;
}
static bool cover_path(const cover_key_t *key, storage_volume_t volume, char *out, size_t size)
{
    unsigned slot = cover_checksum(2166136261u, key->path, strlen(key->path)) % COVER_SLOTS;
    int n = snprintf(out, size, "%s/thumb-%u.rgb", volume == STORAGE_SD ?
        STORAGE_SD_ROOT "/.epd/cache/books/covers" : STORAGE_FLASH_ROOT "/cache/books/covers", slot);
    return n > 0 && (size_t)n < size;
}
static bool cover_source_valid(const cover_job_t *job)
{
    return book_file_version_valid(job->key.path, &job->source) &&
        (job->key.format != DOCUMENT_SPLIT_EPUB ||
         book_file_version(job->key.path).storage == job->source.storage);
}
static void cover_own_change(cover_job_t *job)
{
    if (job->key.format == DOCUMENT_SPLIT_EPUB)
        job->source.storage = book_file_version(job->key.path).storage;
}
static void cover_pixels_release(cover_job_t *job)
{
    epd_app_free(job->bitmap.memory); memset(&job->bitmap, 0, sizeof(job->bitmap));
    rt_base_t level = rt_hw_interrupt_disable();
    cache.bytes -= job->charged; job->charged = 0;
    rt_hw_interrupt_enable(level);
    cover_resource_publish();
}
static void cover_free(cover_job_t *job)
{
    if (!job) return;
    cover_pixels_release(job); epd_activity_work_close(job->work); epd_app_free(job);
}
static bool cover_pixels(cover_job_t *job, unsigned bytes)
{
    size_t charge = (bytes + 31u) / 32u * 32u + 31u;
    if (!bytes || bytes > COVER_MAX_BYTES) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    bool room = cache.bytes <= COVER_IO_BUDGET - charge;
    if (room) { cache.bytes += charge; job->charged = charge; }
    rt_hw_interrupt_enable(level);
    if (!room) return false;
    job->bitmap.memory = cover_allocate(charge);
    if (!job->bitmap.memory) { cover_pixels_release(job); return false; }
    job->bitmap.image.data = (uint8_t *)(((uintptr_t)job->bitmap.memory + 31u) & ~(uintptr_t)31u);
    job->bitmap.image.data_size = bytes;
    cover_resource_publish();
    return true;
}
static cover_job_t *cover_new(const bookshelf_item_t *file, const book_file_version_t *version)
{
    epd_resource_cache_open(&cover_resource, EPD_RESOURCE_UI, cover_resource_shrink, NULL);
    cover_job_t *job = cover_allocate(sizeof(*job));
    if (!job) return NULL;
    memset(job, 0, sizeof(*job)); job->fd = -1;
    cover_key(&job->key, file); job->source = *version;
    return job;
}
bool books_cover_cache_request(const bookshelf_item_t *file, const book_file_version_t *version)
{
    cover_job_t *job = cover_new(file, version);
    if (!job) return false;
    job->work = epd_activity_work_queue(EPD_RESOURCE_OPTIONAL, 0, COVER_ALLOCATION + 4096u,
        epd_resource_file_io_peak(STORAGE_PATH_MAX), cover_wake, NULL);
    if (!job->work) { cover_free(job); return false; }
    rt_base_t level = rt_hw_interrupt_disable();
    cover_job_t *old = cache.read, *ready = cache.ready;
    cache.read = job; cache.ready = NULL; job->epoch = cache.epoch;
    epd_service_wake(cache.service);
    rt_hw_interrupt_enable(level);
    cover_free(old); cover_free(ready);
    cover_resource_publish();
    return true;
}
int books_cover_cache_take(char *title, size_t capacity, document_bitmap_t *cover)
{
    rt_base_t level = rt_hw_interrupt_disable();
    cover_job_t *job = cache.ready; cache.ready = NULL;
    bool pending = cache.read || (cache.active && !cache.active->writing && cache.active->epoch == cache.epoch);
    rt_hw_interrupt_enable(level);
    if (!job) return pending ? 0 : -1;
    bool hit = job->hit && cover_source_valid(job);
    if (hit) {
        snprintf(title, capacity, "%s", job->header.title);
        *cover = job->bitmap;
        /* The shelf now owns these pixels; the I/O budget no longer owns them. */
        memset(&job->bitmap, 0, sizeof(job->bitmap));
        level = rt_hw_interrupt_disable(); ++cache.hits; rt_hw_interrupt_enable(level);
    } else { level = rt_hw_interrupt_disable(); ++cache.misses; rt_hw_interrupt_enable(level); }
    cover_free(job);
    return hit ? 1 : -1;
}
void books_cover_cache_save(const bookshelf_item_t *file, const book_file_version_t *version,
                           const char *title, const document_bitmap_t *cover)
{
    const lv_image_dsc_t *image = &cover->image;
    if (!cover->memory || !image->data || image->header.cf != LV_COLOR_FORMAT_RGB565 ||
        !image->header.w || image->header.w > DOCUMENT_COVER_WIDTH ||
        !image->header.h || image->header.h > DOCUMENT_COVER_HEIGHT ||
        image->header.stride != ((image->header.w * 2u + 3u) & ~3u) ||
        image->data_size != image->header.stride * image->header.h) return;
    cover_job_t *job = cover_new(file, version);
    if (!job) return;
    if (!cover_pixels(job, image->data_size)) { cover_free(job); return; }
    /* A bounded snapshot lets the shelf retire or replace its image independently. */
    memcpy((void *)job->bitmap.image.data, image->data, image->data_size);
    job->writing = true;
    job->header.magic = COVER_MAGIC; job->header.version = COVER_VERSION;
    job->header.key = job->key; job->header.bytes = image->data_size;
    job->header.width = image->header.w; job->header.height = image->header.h; job->header.stride = image->header.stride;
    snprintf(job->header.title, sizeof(job->header.title), "%s", title);
    job->work = epd_activity_work_queue(EPD_RESOURCE_OPTIONAL, 0, 4096u,
        epd_resource_file_io_peak(STORAGE_PATH_MAX), cover_wake, NULL);
    if (!job->work) { cover_free(job); return; }
    rt_base_t level = rt_hw_interrupt_disable();
    cover_job_t *old = cache.write; cache.write = job; job->epoch = cache.epoch;
    epd_service_wake(cache.service);
    rt_hw_interrupt_enable(level);
    cover_free(old);
    cover_resource_publish();
}
void books_cover_cache_cancel(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    cover_job_t *read = cache.read, *write = cache.write, *ready = cache.ready;
    bool cancel = read || write || ready || (cache.active && cache.active->epoch == cache.epoch);
    cache.read = cache.write = cache.ready = NULL;
    /* The worker returns active descriptor ownership to resident storage. */
    if (cancel) { ++cache.epoch; epd_service_wake(cache.service); }
    rt_hw_interrupt_enable(level);
    cover_free(read); cover_free(write); cover_free(ready);
    cover_resource_publish();
}
bool books_cover_cache_busy(void)
{
    rt_base_t level = rt_hw_interrupt_disable(); bool busy = cache.active != NULL;
    rt_hw_interrupt_enable(level); return busy;
}
static bool cover_cancelled(const cover_job_t *job, epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable(); bool stale = job->epoch != cache.epoch;
    rt_hw_interrupt_enable(level);
    return stale || (service && epd_service_cancelled(service));
}
static bool cover_header_valid(const cover_job_t *job)
{
    const cover_header_t *h = &job->header;
    return h->magic == COVER_MAGIC && h->version == COVER_VERSION &&
        !memcmp(&h->key, &job->key, sizeof(job->key)) && memchr(h->title, 0, sizeof(h->title)) &&
        h->width && h->width <= DOCUMENT_COVER_WIDTH && h->height && h->height <= DOCUMENT_COVER_HEIGHT &&
        h->stride == ((h->width * 2u + 3u) & ~3u) && h->bytes == h->stride * h->height && h->bytes <= COVER_MAX_BYTES;
}
static bool cover_cleanup(cover_job_t *job)
{
    storage_lock();
    if (job->fd >= 0) {
        bool closed = storage_file_owned_close(job->fd);
        job->fd = -1;
        if (!closed) job->temporary_owned = false;
    }
    if (job->temporary_owned && storage_session_valid(job->volume, job->session))
        (void)unlink(job->temporary);
    job->temporary_owned = false;
    storage_unlock(); return true;
}
bool books_cover_cache_worker_step(epd_service_t *service, bool reader_active)
{
    epd_activity_guard_t guard;
    bool entered = false;
    rt_base_t level = rt_hw_interrupt_disable(); cache.service = service;
    cover_job_t *job = cache.active;
    rt_hw_interrupt_enable(level);
    if (reader_active || (service && epd_service_cancelled(service)))
        books_cover_cache_cancel();
    if (job && cover_cancelled(job, service)) {
        if (job->phase != COVER_ABORT) ++cache.cancelled;
        job->phase = COVER_ABORT;
    }
    if (job && job->phase == COVER_ABORT) goto abort;
    if (reader_active || (service && epd_service_cancelled(service))) return false;
    if (!job) {
        level = rt_hw_interrupt_disable();
        if (cache.read) { job = cache.read; cache.read = NULL; }
        else if (cache.write) { job = cache.write; cache.write = NULL; }
        cache.active = job;
        rt_hw_interrupt_enable(level);
        cover_resource_publish();
        if (!job) return false;
    }
    if (cover_cancelled(job, service) || !cover_source_valid(job)) goto abort;
    int admission = epd_activity_work_ready(job->work);
    if (!admission) return false;
    if (admission < 0 || !(entered = epd_activity_work_enter(job->work, &guard))) goto abort;
    if ((job->phase == COVER_OPEN || job->phase == COVER_COMMIT) &&
        epd_resource_context_reserve(EPD_MEMORY_REGION_SRAM,
        epd_resource_file_io_peak(STORAGE_PATH_MAX)) != EPD_RESOURCE_OK) goto abort;
    if (job->phase == COVER_OPEN) {
        job->volume = storage_available(STORAGE_SD) ? STORAGE_SD : STORAGE_FLASH;
        job->session = storage_card_session();
        storage_info_t info;
        if (job->writing && (!storage_info(job->volume, &info) || info.used > info.total ||
            info.total - info.used < job->header.bytes + sizeof(job->header) + COVER_DISK_RESERVE)) goto abort;
        if (!cover_path(&job->key, job->volume, job->path, sizeof(job->path))) goto abort;
        if (epd_cold_budget_begin(job->path, EPD_COLD_DERIVED,
            job->writing ? sizeof(job->header) + (size_t)job->header.bytes : 0,
            job->session, &job->disk_lease) != EPD_COLD_BUDGET_OK) goto abort;
        cover_own_change(job);
        storage_lock();
        bool ok = cover_source_valid(job) && storage_session_valid(job->volume, job->session);
        if (ok && job->writing) {
            char directory[STORAGE_PATH_MAX]; strcpy(directory, job->path); *strrchr(directory, '/') = 0;
            ok = epd_cold_budget_temporary(job->path, job->temporary, sizeof(job->temporary)) &&
                storage_mkdirs(directory) && storage_file_recover(job->path);
            if (ok) { job->fd = storage_file_owned_open(job->temporary, O_CREAT | O_TRUNC | O_RDWR, true); job->temporary_owned = job->fd >= 0; }
            cover_own_change(job);
        } else if (ok) {
            if (storage_file_recover(job->path)) job->fd = storage_file_owned_open(job->path, O_RDONLY, false);
            cover_own_change(job); job->cached = book_file_version(job->path);
        }
        storage_unlock();
        if (!ok || job->fd < 0) goto abort;
        job->phase = COVER_HEADER; goto progressed;
    }
    storage_lock();
    bool valid = cover_source_valid(job) && storage_session_valid(job->volume, job->session) &&
        (job->writing || book_file_version_valid(job->path, &job->cached));
    if (!valid) { storage_unlock(); goto abort; }
    bool ok = true, done = false;
    switch (job->phase) {
    case COVER_HEADER:
        if (job->writing) ok = write(job->fd, &job->header, sizeof(job->header)) == sizeof(job->header);
        else ok = read(job->fd, &job->header, sizeof(job->header)) == sizeof(job->header) && cover_header_valid(job);
        if (ok && !job->writing) ok = cover_pixels(job, job->header.bytes);
        if (ok) {
            uint32_t checksum = job->header.checksum; job->header.checksum = 0;
            job->checksum = cover_checksum(2166136261u, &job->header, sizeof(job->header));
            job->header.checksum = checksum; job->phase = COVER_PIXELS;
        }
        break;
    case COVER_PIXELS: {
        unsigned bytes = job->header.bytes - (unsigned)job->offset;
        if (bytes > COVER_CHUNK) bytes = COVER_CHUNK;
        uint8_t *pixels = (uint8_t *)job->bitmap.image.data + job->offset;
        ok = job->writing ? write(job->fd, pixels, bytes) == (int)bytes : read(job->fd, pixels, bytes) == (int)bytes;
        if (ok) { job->checksum = cover_checksum(job->checksum, pixels, bytes); job->offset += bytes; }
        if (ok && job->offset == job->header.bytes) job->phase = COVER_SEAL;
        break;
    }
    case COVER_SEAL:
        if (job->writing) {
            job->header.checksum = job->checksum;
            ok = lseek(job->fd, 0, SEEK_SET) == 0 && write(job->fd, &job->header, sizeof(job->header)) == sizeof(job->header);
            job->phase = COVER_SYNC;
        } else {
            uint8_t extra; ok = read(job->fd, &extra, 1) == 0 && job->checksum == job->header.checksum;
            job->phase = COVER_CLOSE;
        }
        break;
    case COVER_SYNC: ok = fsync(job->fd) == 0; job->phase = COVER_CLOSE; break;
    case COVER_CLOSE:
        ok = storage_file_owned_close(job->fd);
        job->fd = -1;
        if (!ok) { job->temporary_owned = false; break; }
        if (job->writing) job->phase = COVER_COMMIT; else { job->hit = true; done = true; }
        break;
    case COVER_COMMIT:
        ok = !cover_cancelled(job, service) && storage_file_commit(job->temporary, job->path);
        if (ok) { job->temporary_owned = false; ++cache.saved; done = true; }
        break;
    default: ok = false; break;
    }
    if (job->writing) cover_own_change(job);
    storage_unlock();
    if (!ok) { ++cache.errors; goto abort; }
    if (!done) goto progressed;
    goto finished;
abort:
    job->phase = COVER_ABORT; job->hit = false; cover_pixels_release(job);
    (void)cover_cleanup(job);
finished:
    if (job->disk_lease) {
        (void)epd_cold_budget_finish(job->disk_lease, EPD_COLD_DERIVED, job->phase != COVER_ABORT);
        job->disk_lease = 0;
        cover_own_change(job);
    }
    if (entered) { epd_activity_work_leave(&guard); entered = false; }
    epd_activity_work_close(job->work); job->work = NULL;
    if (job->hit) {
        job->bitmap.image.header.magic = LV_IMAGE_HEADER_MAGIC;
        job->bitmap.image.header.cf = LV_COLOR_FORMAT_RGB565;
        job->bitmap.image.header.w = job->header.width; job->bitmap.image.header.h = job->header.height;
        job->bitmap.image.header.stride = job->header.stride;
    }
    level = rt_hw_interrupt_disable(); cache.active = NULL;
    if (!job->writing && job->epoch == cache.epoch && !cache.ready) { cache.ready = job; job = NULL; }
    rt_hw_interrupt_enable(level);
    cover_free(job); cover_resource_publish(); epd_service_ui_request(service, 0);
progressed:
    if (entered) epd_activity_work_leave(&guard);
    return true;
}
void books_cover_cache_shutdown(void)
{
    books_cover_cache_cancel();
    if (books_cover_cache_busy()) books_cover_cache_worker_step(cache.service, true);
    cache.service = NULL;
    cover_resource_publish(); epd_resource_cache_close(&cover_resource);
}
void books_cover_cache_diagnostic(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    unsigned hits = cache.hits, misses = cache.misses, saved = cache.saved, errors = cache.errors, cancelled = cache.cancelled;
    unsigned bytes = (unsigned)cache.bytes, active = cache.active != NULL, queued = (cache.read != NULL) + (cache.write != NULL);
    rt_hw_interrupt_enable(level);
    rt_kprintf("bookshelf cover_cache: hits=%u misses=%u saved=%u errors=%u cancelled=%u io_bytes=%u active=%u queued=%u slots=%u\n",
        hits, misses, saved, errors, cancelled, bytes, active, queued, COVER_SLOTS);
}

static int cover_resource_shrink(void *context, uint32_t backend, size_t requested, size_t *released)
{
    (void)context; (void)backend;
    *released = 0;
    for (unsigned n = 0; n < 3 && *released < requested; ++n) {
        rt_base_t level = rt_hw_interrupt_disable();
        cover_job_t **at = cache.write ? &cache.write : cache.read ? &cache.read : &cache.ready;
        cover_job_t *job = *at; *at = NULL;
        size_t bytes = job ? job->charged : 0;
        rt_hw_interrupt_enable(level);
        if (!job) break;
        cover_free(job); *released += bytes;
    }
    cover_resource_publish();
    return EPD_RESOURCE_OK;
}
