#include "app_service.h"
#include "storage.h"
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

static bool allocation_cancelled(void *serial)
{
    return gallery_cancelled(*(const uint32_t *)serial);
}

bool gallery_cancelled(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancelled = serial != generation;
    rt_hw_interrupt_enable(level);
    return cancelled || epd_service_cancelled(active_service);
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
    gallery_cancel();
    gallery_job_t job = {.kind = kind, .first = first};
    snprintf(job.path, sizeof(job.path), "%s", path);
    rt_base_t level = rt_hw_interrupt_disable();
    if (!worker || epd_service_cancelled(worker)) { rt_hw_interrupt_enable(level); return 0; }
    job.serial = generation;
    requested = job;
    pending = true;
    rt_hw_interrupt_enable(level);
    epd_service_wake(worker);
    return job.serial;
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

static bool image_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext || strlen(ext) > 5) return false;
    char lower[6];
    unsigned i;
    for (i = 0; ext[i]; ++i)
        lower[i] = ext[i] >= 'A' && ext[i] <= 'Z' ? ext[i] + ('a' - 'A') : ext[i];
    lower[i] = 0;
    return !strcmp(lower, ".png") || !strcmp(lower, ".jpg") || !strcmp(lower, ".jpeg");
}

static void scan(const gallery_job_t *job, gallery_result_t *result)
{
    storage_lock();
    DIR *directory = storage_path_available(job->path) ? opendir(job->path) : NULL;
    storage_unlock();
    if (!directory) { result->error_code = GALLERY_STATUS_STORAGE; return; }
    unsigned skipped = 0;
    while (!gallery_cancelled(job->serial))
    {
        storage_lock();
        if (!storage_path_available(job->path))
        {
            storage_unlock();
            result->error_code = GALLERY_STATUS_REMOVED;
            break;
        }
        struct dirent *entry = readdir(directory);
        if (!entry) { storage_unlock(); break; }
        char path[STORAGE_PATH_MAX];
        struct stat st;
        int n = snprintf(path, sizeof(path), "%s/%s", job->path, entry->d_name);
        bool valid = entry->d_name[0] != '.' && strlen(entry->d_name) < 256 &&
                     n > 0 && (size_t)n < sizeof(path) && stat(path, &st) == 0;
        valid = valid && (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && image_name(entry->d_name)));
        if (valid && skipped++ >= job->first)
        {
            if (result->count == GALLERY_ROWS) result->more = true;
            else
            {
                gallery_entry_t *item = &result->entries[result->count++];
                strcpy(item->name, entry->d_name);
                item->directory = S_ISDIR(st.st_mode);
            }
        }
        storage_unlock();
        if (result->more) break;
    }
    storage_lock();
    closedir(directory);
    storage_unlock();
}

static void run(epd_service_t *service)
{
    active_service = service;
    while (!epd_service_cancelled(service))
    {
        gallery_job_t job;
        rt_base_t level = rt_hw_interrupt_disable();
        bool ready = pending;
        if (ready) { job = requested; pending = false; }
        rt_hw_interrupt_enable(level);
        if (!ready) { epd_service_wait(service, RT_WAITING_FOREVER); continue; }
        epd_memory_retry_t memory_retry = {0};
        gallery_result_t *result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
        if (!result && epd_memory_retry_wait(&memory_retry, sizeof(*result),
                                             allocation_cancelled, &job.serial))
            result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
        if (!result)
        {
            rt_kprintf("[gallery] result allocation failed\n");
            level = rt_hw_interrupt_disable();
            failed_serial = job.serial;
            rt_hw_interrupt_enable(level);
            continue;
        }
        memset(result, 0, sizeof(*result));
        result->memory_retry = memory_retry;
        result->references = 1;
        result->serial = job.serial;
        result->kind = job.kind;
        void *cache = NULL;
        size_t cache_size = 0;
        gallery_decoder_t decoder = {.fd = -1};
        bool image_ready = false;
        if (job.kind == GALLERY_SCAN)
        {
            scan(&job, result);
        }
        else
        {
            bool ok = gallery_decode(&job, &decoder, result);
            if (ok && !decoder.cache_hit) ok = gallery_render(&decoder, result, &job);
            epd_app_free(decoder.gray);
            decoder.gray = NULL;
            if (!ok)
            {
                if (!result->error_code) result->error_code = GALLERY_STATUS_DECODE;
                snprintf(result->error, sizeof(result->error), "%s", decoder.error);
            }
            image_ready = ok;
        }
        level = rt_hw_interrupt_disable();
        gallery_result_t *old = NULL;
        bool published = false;
        if (job.serial == generation && !epd_service_cancelled(service))
        {
            old = completed;
            /* UI owns one reference; the worker retains its reference until
             * cache packing stops reading the immutable display pixels. */
            ++result->references;
            completed = result;
            published = true;
        }
        rt_hw_interrupt_enable(level);
        gallery_result_free(old);
        if (published && image_ready) cache = gallery_cache_pack(&decoder, result, &cache_size);
        gallery_decoder_clear(&decoder);
        gallery_result_free(result);
        gallery_cache_save(&job, cache, cache_size);
        epd_app_free(cache);
    }
    gallery_cancel();
    rt_base_t level = rt_hw_interrupt_disable();
    worker = NULL;
    active_service = NULL;
    rt_hw_interrupt_enable(level);
}

static const epd_background_t definition = {.run = run, .stack_size = 8192, .priority = 25};

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
