#include "app_service.h"
#include "storage.h"
#include "storage_file.h"
#include "epd_memory.h"
#include "gallery.h"
#include <dfs_posix.h>
#include <dlmodule.h>
#include <string.h>
#include <stdio.h>

static gallery_job_t requested;
static uint32_t generation;
static bool pending;
static gallery_result_t *completed;
static epd_service_t *worker;
static epd_service_t *active_service;
static uint32_t failed_serial;
static gallery_result_t *prefetched;
static gallery_job_t prefetch_job;
static bool prefetch_pending;
static char prefetch_failed_path[STORAGE_PATH_MAX];
static uint32_t prefetch_failed_session, prefetch_failed_revision;

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
    rt_hw_interrupt_enable(level);
}

void gallery_result_free(gallery_result_t *result)
{
    if (!result) return;
    rt_base_t level = rt_hw_interrupt_disable();
    bool released = --result->references == 0;
    rt_hw_interrupt_enable(level);
    if (!released) return;
    epd_app_free(result->pixels);
    epd_app_free(result);
}

void gallery_cancel(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    ++generation;
    pending = false;
    gallery_result_t *old = completed;
    completed = NULL;
    rt_hw_interrupt_enable(level);
    gallery_result_free(old);
}

void gallery_cancel_request(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    gallery_result_t *old = NULL;
    if (serial && serial == generation)
    {
        ++generation;
        pending = false;
        old = completed;
        completed = NULL;
    }
    rt_hw_interrupt_enable(level);
    gallery_result_free(old);
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
    rt_base_t level = rt_hw_interrupt_disable();
    if (!worker || epd_service_cancelled(worker)) { rt_hw_interrupt_enable(level); return 0; }
    job.serial = generation;
    requested = job;
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

static void prefetch_clear(void)
{
    gallery_result_free(prefetched);
    prefetched = NULL;
    prefetch_pending = false;
}

static bool prefetch_current(const char *path)
{
    return prefetched && !strcmp(prefetched->path, path) &&
        storage_session_valid(STORAGE_SD, prefetched->source_session) &&
        storage_path_revision(path) == prefetched->source_revision && !storage_file_writers(path);
}

static gallery_result_t *render_job(const gallery_job_t *job, bool speculative)
{
    if (!speculative && job->kind == GALLERY_IMAGE && prefetch_current(job->path)) {
        gallery_result_t *result = prefetched;
        prefetched = NULL;
        prefetch_pending = false;
        result->serial = job->serial;
        result->kind = job->kind;
        return result;
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

static void process_job(const gallery_job_t *job)
{
    gallery_result_t *result = render_job(job, false);
    if (!result) { failed_serial = job->serial; return; }
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
            bool failed = !strcmp(prefetch_failed_path, neighbor) &&
                prefetch_failed_session == storage_card_session() &&
                prefetch_failed_revision == storage_path_revision(neighbor);
            prefetch_pending = neighbor[0] && !failed && !prefetch_current(neighbor);
            if (prefetch_current(neighbor)) prefetched->serial = job->serial;
            if (prefetch_pending) {
                prefetch_job = (gallery_job_t){.kind = GALLERY_IMAGE, .serial = job->serial, .speculative = true};
                snprintf(prefetch_job.path, sizeof(prefetch_job.path), "%s", neighbor);
            }
        }
    }
    gallery_result_free(result);
}

static bool background_step(void)
{
    bool scanning = gallery_catalog_step();
    bool saving = gallery_cache_step();
    if (prefetched && (prefetched->serial != generation || !prefetch_current(prefetched->path))) prefetch_clear();
    if (!saving && prefetch_pending) {
        gallery_job_t job = prefetch_job;
        prefetch_pending = false;
        if (!gallery_cancelled(job.serial)) {
            gallery_result_t *result = render_job(&job, true);
            if (result && !result->error_code && !gallery_cancelled(job.serial)) {
                prefetched = result;
                gallery_cache_schedule(&job, result);
            } else {
                if (!gallery_cancelled(job.serial)) {
                    memcpy(prefetch_failed_path, job.path, sizeof(prefetch_failed_path));
                    prefetch_failed_session = storage_card_session();
                    prefetch_failed_revision = storage_path_revision(job.path);
                }
                gallery_result_free(result);
            }
        }
    }
    return scanning || saving || prefetch_pending;
}

static void run(epd_service_t *service)
{
    active_service = service;
    while (!epd_service_cancelled(service)) {
        rt_base_t level = rt_hw_interrupt_disable();
        bool ready = pending;
        gallery_job_t job = requested;
        if (ready) pending = false;
        rt_hw_interrupt_enable(level);
        if (ready) {
            process_job(&job);
            (void)gallery_catalog_step();
        } else {
            bool busy = background_step();
            epd_service_wait(service, rt_tick_from_millisecond(busy ? 5 : 100));
        }
    }
    gallery_cancel();
    gallery_cache_clear();
    prefetch_clear();
    gallery_catalog_clear();
    /* Failed descriptors now belong to resident storage, without module callbacks. */
    rt_base_t level = rt_hw_interrupt_disable();
    worker = NULL;
    active_service = NULL;
    rt_hw_interrupt_enable(level);
}

static const epd_background_t definition = {.run = run, .stack_size = 16384, .priority = 25};

bool gallery_start(void)
{
    worker = app_service_start("gallery", &definition, dlmodule_find("gallery"));
    return worker != NULL;
}

void gallery_stop(void)
{
    gallery_cancel();
    app_service_stop("gallery");
}
