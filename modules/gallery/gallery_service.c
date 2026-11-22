#include "app_service.h"
#include "storage.h"
#include "storage_file.h"
#include "epd_memory.h"
#include "epd_activity_work.h"
#include "epd_resource_runtime.h"
#include "gallery.h"
#include <dfs_posix.h>
#include <dlmodule.h>
#include <string.h>
#include <stdio.h>

static gallery_job_t requested;
static uint32_t generation;
static bool pending;
static epd_activity_work_t *requested_work;
static gallery_result_t *completed;
static epd_service_t *worker;
static epd_service_t *active_service;
static uint32_t failed_serial;
static gallery_result_t *prefetched;
static gallery_job_t prefetch_job;
static bool prefetch_pending;
static epd_activity_work_t *prefetch_work, *prefetch_parent;
static epd_resource_cache_t prefetch_resource;
static size_t prefetch_bytes;
static size_t prefetch_published_total, prefetch_published_idle;
static uint32_t prefetch_publication;
static void prefetch_publish(void);

static void activity_wake(void *unused)
{
    (void)unused;
    rt_base_t level = rt_hw_interrupt_disable();
    if (worker) epd_service_wake(worker);
    rt_hw_interrupt_enable(level);
}

static epd_activity_work_t *activity_new(epd_resource_kind_t kind, epd_resource_activity_t parent)
{
    size_t result = epd_app_allocation_peak(sizeof(gallery_result_t));
    size_t buffer = epd_app_allocation_peak(4096);
    size_t io = epd_resource_file_io_peak(STORAGE_PATH_MAX);
    if (result == SIZE_MAX || buffer > SIZE_MAX - result || io == SIZE_MAX) return NULL;
    return epd_activity_work_queue(kind, parent, result + buffer, io, activity_wake, NULL);
}

static bool allocation_cancelled(void *serial)
{
    return gallery_cancelled(*(const uint32_t *)serial);
}

bool gallery_cancelled(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancelled = serial && serial != generation;
    rt_hw_interrupt_enable(level);
    return cancelled || epd_service_cancelled(active_service);
}

void gallery_result_retain(gallery_result_t *result)
{
    rt_base_t level = rt_hw_interrupt_disable();
    ++result->references;
    bool changed = result == prefetched;
    rt_hw_interrupt_enable(level);
    if (changed) prefetch_publish();
}

void gallery_result_free(gallery_result_t *result)
{
    if (!result) return;
    rt_base_t level = rt_hw_interrupt_disable();
    bool released = --result->references == 0;
    bool changed = result == prefetched;
    rt_hw_interrupt_enable(level);
    if (changed) prefetch_publish();
    if (!released) return;
    epd_app_free(result->pixels);
    epd_app_free(result);
}

static void prefetch_publish(void)
{
    if (!prefetch_resource.provider) return;
    bool retry = false;
    for (;;) {
        rt_base_t level = rt_hw_interrupt_disable();
        size_t total = prefetch_bytes;
        size_t idle = prefetched && prefetched->references == 1 ? total : 0;
        uint32_t revision = ++prefetch_publication;
        if (!retry && total == prefetch_published_total && idle == prefetch_published_idle) {
            rt_hw_interrupt_enable(level); return;
        }
        rt_hw_interrupt_enable(level);
        int status = epd_resource_cache_update_region(&prefetch_resource, EPD_MEMORY_REGION_HCPU_PSRAM,
            total, idle, 3, 1);
        level = rt_hw_interrupt_disable();
        bool current = revision == prefetch_publication;
        if (current && status == EPD_RESOURCE_OK) {
            prefetch_published_total = total; prefetch_published_idle = idle;
        }
        rt_hw_interrupt_enable(level);
        if (current) return;
        retry = true;
    }
}

static int prefetch_shrink(void *unused, uint32_t backend, size_t target, size_t *released)
{
    (void)unused;
    epd_memory_backend_t psram;
    *released = 0;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &psram) || backend != psram)
        return EPD_RESOURCE_INVALID;
    rt_base_t level = rt_hw_interrupt_disable();
    gallery_result_t *victim = target && prefetched && prefetched->references == 1 ? prefetched : NULL;
    if (victim) { prefetched = NULL; *released = prefetch_bytes; prefetch_bytes = 0; }
    rt_hw_interrupt_enable(level);
    gallery_result_free(victim);
    prefetch_publish();
    return EPD_RESOURCE_OK;
}

/* The resource worker may take the idle slot. Borrow before inspecting pixels
 * or source metadata; a disk writer and a UI consumer use the same references. */
static gallery_result_t *prefetch_borrow(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    gallery_result_t *result = prefetched;
    if (result) ++result->references;
    rt_hw_interrupt_enable(level);
    return result;
}

void gallery_cancel(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    ++generation;
    pending = false;
    epd_activity_work_t *work = requested_work;
    requested_work = NULL;
    gallery_result_t *old = completed;
    completed = NULL;
    rt_hw_interrupt_enable(level);
    epd_activity_work_close(work);
    gallery_result_free(old);
    activity_wake(NULL);
}

void gallery_cancel_request(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    gallery_result_t *old = NULL;
    epd_activity_work_t *work = NULL;
    if (serial && serial == generation)
    {
        ++generation;
        pending = false;
        work = requested_work;
        requested_work = NULL;
        old = completed;
        completed = NULL;
    }
    rt_hw_interrupt_enable(level);
    epd_activity_work_close(work);
    gallery_result_free(old);
    activity_wake(NULL);
}

bool gallery_failed(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool failed = serial && failed_serial == serial;
    rt_hw_interrupt_enable(level);
    return failed;
}

uint32_t gallery_submit(gallery_job_kind_t kind, const char *path, unsigned first)
{
    return gallery_submit_view(kind, path, first, false);
}

static uint32_t submit(gallery_job_t job)
{
    gallery_cancel();
    if (!gallery_sd_path(job.path)) return 0;
    epd_activity_work_t *work = activity_new(EPD_RESOURCE_INTERACTIVE, 0);
    if (!work) return 0;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!worker || epd_service_cancelled(worker)) {
        rt_hw_interrupt_enable(level); epd_activity_work_close(work); return 0;
    }
    job.serial = generation;
    requested = job;
    requested_work = work;
    pending = true;
    rt_hw_interrupt_enable(level);
    epd_service_wake(worker);
    return job.serial;
}

uint32_t gallery_submit_view(gallery_job_kind_t kind, const char *path, unsigned first, bool recursive)
{
    return gallery_submit_page(kind, path, first, recursive, NULL);
}

uint32_t gallery_submit_page(gallery_job_kind_t kind, const char *path, unsigned first, bool recursive, const char *anchor)
{
    gallery_job_t job = {.kind = kind, .first = first, .recursive = recursive};
    if (!gallery_sd_path(path)) return 0;
    snprintf(job.path, sizeof(job.path), "%s", path);
    if (anchor) snprintf(job.anchor, sizeof(job.anchor), "%s", anchor);
    return submit(job);
}

uint32_t gallery_submit_location(const char *root, const char *path, bool recursive, int direction)
{
    gallery_job_t job = {.kind = GALLERY_LOCATE, .recursive = recursive, .direction = direction};
    if (!gallery_sd_path(root) || !gallery_sd_path(path)) return 0;
    snprintf(job.path, sizeof(job.path), "%s", root);
    snprintf(job.anchor, sizeof(job.anchor), "%s", path);
    return submit(job);
}

gallery_result_t *gallery_take(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    gallery_result_t *result = completed;
    completed = NULL;
    rt_hw_interrupt_enable(level);
    if (result && result->serial != serial)
    {
        gallery_result_free(result);
        return NULL;
    }
    return result;
}

static void prefetch_work_clear(void)
{
    epd_activity_work_close(prefetch_work);
    epd_activity_work_close(prefetch_parent);
    prefetch_work = prefetch_parent = NULL;
    prefetch_pending = false;
}

static void prefetch_clear(void)
{
    prefetch_work_clear();
    rt_base_t level = rt_hw_interrupt_disable();
    gallery_result_t *old = prefetched;
    prefetched = NULL;
    prefetch_bytes = 0;
    rt_hw_interrupt_enable(level);
    gallery_result_free(old);
    prefetch_publish();
}

static bool prefetch_current(const gallery_result_t *result, const char *path)
{
    return result && !strcmp(result->path, path) &&
        storage_session_valid(STORAGE_SD, result->source_session) &&
        storage_path_revision(path) == result->source_revision && !storage_file_writers(path);
}

static gallery_result_t *render_job(const gallery_job_t *job, bool speculative)
{
    if (!speculative && job->kind == GALLERY_IMAGE) {
        gallery_result_t *result = prefetch_borrow();
        if (prefetch_current(result, job->path)) {
            prefetch_clear();
            result->serial = job->serial;
            result->kind = job->kind;
            return result;
        }
        gallery_result_free(result);
    }
    if (job->kind != GALLERY_LOCATE) prefetch_clear();
    gallery_result_t *result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
    epd_memory_retry_t retry = {0};
    if (!result && !speculative && epd_memory_retry_wait(&retry, sizeof(*result), allocation_cancelled, (void *)&job->serial))
        result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
    if (!result) return NULL;
    memset(result, 0, sizeof(*result));
    result->references = 1;
    result->memory_retry = retry;
    if (speculative) result->memory_retry.used = true;
    result->serial = job->serial;
    result->kind = job->kind;
    if (job->kind == GALLERY_SCAN || job->kind == GALLERY_ALBUMS) gallery_catalog_page(job, result);
    else if (job->kind == GALLERY_LOCATE) gallery_catalog_locate(job, result);
    else {
        gallery_job_t image_job = *job;
        gallery_decoder_t decoder = {.fd = -1};
        bool ok = job->kind != GALLERY_INDEX_IMAGE || gallery_catalog_picture(job, result, &image_job);
        if (ok) {
            memcpy(result->path, image_job.path, sizeof(result->path));
            ok = gallery_cached_render(&image_job, &decoder, result);
        }
        if (!ok && !result->error_code) result->error_code = GALLERY_STATUS_DECODE;
        if (!ok) snprintf(result->error, sizeof(result->error), "%s", decoder.error);
        gallery_decoder_clear(&decoder);
    }
    return result;
}

static bool process_job(const gallery_job_t *job, epd_activity_work_t *work)
{
    if (gallery_cancelled(job->serial)) return true;
    /* A superseded neighbor must not retain the credit this request awaits.
     * Keep completed pixels available for a foreground cache hit. */
    if (prefetch_pending && gallery_cancelled(prefetch_job.serial)) prefetch_work_clear();
    int ready = epd_activity_work_ready(work);
    if (!ready) return false;
    epd_activity_guard_t guard;
    if (ready < 0 || !epd_activity_work_enter(work, &guard)) {
        failed_serial = job->serial; return true;
    }
    gallery_result_t *result = render_job(job, false);
    if (!result) {
        epd_activity_work_leave(&guard); failed_serial = job->serial; return true;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    gallery_result_t *old = NULL;
    bool published = job->serial == generation && !epd_service_cancelled(active_service);
    if (published) {
        old = completed;
        ++result->references;
        completed = result;
    }
    rt_hw_interrupt_enable(level);
    gallery_result_free(old);
    if (published && !result->error_code) {
        if (result->pixels) {
            gallery_job_t save = *job;
            memcpy(save.path, result->path, sizeof(save.path));
            gallery_cache_schedule(&save, result);
        } else if (job->kind == GALLERY_LOCATE && result->located) {
            const char *neighbor = job->direction < 0 ? result->previous : result->next;
            prefetch_work_clear();
            gallery_result_t *near = prefetch_borrow();
            bool hit = prefetch_current(near, neighbor);
            prefetch_pending = prefetch_resource.provider && neighbor[0] && !hit;
            if (hit) near->serial = job->serial;
            gallery_result_free(near);
            if (prefetch_pending) {
                prefetch_job = (gallery_job_t){.kind = GALLERY_IMAGE, .serial = job->serial, .speculative = true};
                snprintf(prefetch_job.path, sizeof(prefetch_job.path), "%s", neighbor);
                prefetch_work = activity_new(EPD_RESOURCE_NEARBY, epd_activity_work_activity(work));
                if (prefetch_work && epd_activity_work_retain(work)) prefetch_parent = work;
                else {
                    epd_activity_work_close(prefetch_work); prefetch_work = NULL;
                    prefetch_pending = false;
                }
            }
        }
    }
    gallery_result_free(result);
    epd_activity_work_leave(&guard);
    if (prefetch_parent == work && !epd_activity_work_settle(work)) prefetch_clear();
    return true;
}

static bool background_step(void)
{
    bool scanning = gallery_catalog_step();
    bool saving = gallery_cache_step();
    if (prefetch_pending && gallery_cancelled(prefetch_job.serial)) prefetch_clear();
    gallery_result_t *near = prefetch_borrow();
    if (near && (near->serial != generation || !prefetch_current(near, near->path))) prefetch_clear();
    gallery_result_free(near);
    if (!saving && prefetch_pending) {
        int ready = epd_activity_work_ready(prefetch_work);
        if (!ready) return scanning || saving;
        gallery_job_t job = prefetch_job;
        epd_activity_work_t *work = prefetch_work, *parent = prefetch_parent;
        prefetch_work = prefetch_parent = NULL;
        prefetch_pending = false;
        epd_activity_guard_t guard;
        if (ready > 0 && !gallery_cancelled(job.serial) && epd_activity_work_enter(work, &guard)) {
            gallery_result_t *result = render_job(&job, true);
            if (result && !result->error_code && !gallery_cancelled(job.serial)) {
                epd_memory_block_snapshot_t object = {0}, pixels = {0};
                bool tracked = prefetch_resource.provider &&
                    epd_memory_block_get(result, &object) == EPD_MEMORY_OK &&
                    epd_memory_block_get(result->pixels, &pixels) == EPD_MEMORY_OK;
                /* Schedule the disk reference before exposing an idle slot. */
                gallery_cache_schedule(&job, result);
                if (tracked) {
                    rt_base_t level = rt_hw_interrupt_disable();
                    prefetched = result;
                    prefetch_bytes = object.physical + pixels.physical;
                    rt_hw_interrupt_enable(level);
                    prefetch_publish();
                } else gallery_result_free(result);
            } else {
                gallery_result_free(result);
            }
            epd_activity_work_leave(&guard);
        }
        epd_activity_work_close(work);
        epd_activity_work_close(parent);
    }
    return scanning || saving || prefetch_pending;
}

static void run(epd_service_t *service)
{
    active_service = service;
    gallery_job_t job = {0};
    epd_activity_work_t *work = NULL;
    while (!epd_service_cancelled(service)) {
        rt_base_t level = rt_hw_interrupt_disable();
        bool ready = pending;
        epd_activity_work_t *old = NULL;
        if (ready) {
            old = work; work = requested_work; requested_work = NULL;
            job = requested; pending = false;
        }
        rt_hw_interrupt_enable(level);
        epd_activity_work_close(old);
        if (work) {
            if (process_job(&job, work)) {
                epd_activity_work_close(work); work = NULL;
                (void)gallery_catalog_step();
                epd_service_ui_request(service, 0);
            } else epd_service_wait(service, RT_WAITING_FOREVER);
        } else {
            uint32_t revision = gallery_catalog_revision(), generation = gallery_catalog_generation();
            bool busy = background_step();
            if (revision != gallery_catalog_revision() || generation != gallery_catalog_generation())
                epd_service_ui_request(service, 0);
            epd_service_wait(service, busy ? rt_tick_from_millisecond(5) : RT_WAITING_FOREVER);
        }
    }
    epd_activity_work_close(work);
    gallery_cancel();
    gallery_cache_clear();
    prefetch_clear();
    epd_resource_cache_close(&prefetch_resource);
    gallery_catalog_clear();
    /* Failed descriptors now belong to resident storage, without module callbacks. */
    rt_base_t level = rt_hw_interrupt_disable();
    worker = NULL;
    active_service = NULL;
    rt_hw_interrupt_enable(level);
}

static bool ui_process(epd_service_t *service, bool stopping)
{
    if (stopping) return true;
    epd_service_ui_watch(service, EPD_SERVICE_UI_STORAGE | EPD_SERVICE_UI_FILES |
                         EPD_SERVICE_UI_SETTINGS | EPD_SERVICE_UI_DISPLAY);
    gallery_ui_update();
    return false;
}

static const epd_background_t definition = {
    .run = run, .stack_size = 16384, .priority = 25, .ui_process = ui_process
};

bool gallery_start(void)
{
    epd_resource_cache_open(&prefetch_resource, EPD_RESOURCE_WORKER, prefetch_shrink, NULL);
    worker = app_service_start("gallery", &definition, dlmodule_find("gallery"));
    if (!worker) epd_resource_cache_close(&prefetch_resource);
    return worker != NULL;
}

void gallery_stop(void)
{
    gallery_cancel();
    app_service_stop("gallery");
}
