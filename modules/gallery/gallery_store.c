#include "gallery.h"
#include "storage_file.h"
#include "epd_memory.h"
#include "epd_resource_runtime.h"
#include "epd_cold_budget.h"
#include <dfs_posix.h>
#include <stdio.h>
#include <string.h>

#define THUMB_SLOTS 128u
#define IMAGE_SLOTS 16u
#define CACHE_MAGIC 0x474c5232u
#define THUMB_CACHE_VERSION 2u
#define IMAGE_CACHE_VERSION 3u

static uint32_t cache_version(gallery_job_kind_t kind)
{
    return kind == GALLERY_THUMB ? THUMB_CACHE_VERSION : IMAGE_CACHE_VERSION;
}

typedef struct {
    uint32_t magic, version, signature, width, height, source_width, source_height, checksum;
    char path[STORAGE_PATH_MAX];
} cache_header_t;

/* Storage watches include cache writes. Track external changes separately so
 * our own bounded cache traffic cannot continuously restart discovery. */
static uint32_t observed_revision, content_revision;
static bool observing;
static void observe_changes(void)
{
    uint32_t value = storage_volume_revision(STORAGE_SD);
    if (!observing || observed_revision != value) ++content_revision;
    observed_revision = value;
    observing = true;
}
static void own_change_done(void)
{
    observed_revision = storage_volume_revision(STORAGE_SD);
}
uint32_t gallery_storage_revision(void)
{
    storage_lock();
    observe_changes();
    uint32_t value = content_revision;
    storage_unlock();
    return value;
}
bool gallery_store_ready(uint32_t session)
{
    struct stat st;
    storage_lock();
    observe_changes();
    bool ok = storage_session_valid(STORAGE_SD, session) &&
        ((stat(GALLERY_STORE, &st) == 0 && S_ISDIR(st.st_mode)) || mkdir(GALLERY_STORE, 0777) == 0);
    own_change_done();
    storage_unlock();
    return ok;
}

bool gallery_sd_path(const char *path)
{
    size_t root = strlen(STORAGE_SD_ROOT);
    if (!path || strncmp(path, STORAGE_SD_ROOT, root) || (path[root] && path[root] != '/') ||
        strlen(path) >= STORAGE_PATH_MAX) return false;
    for (const char *part = path + root; *part; )
    {
        if (*part++ != '/') return false;
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
        part += n;
    }
    return true;
}

int gallery_file_open(const char *path, int flags, uint32_t session)
{
    if (epd_resource_context_reserve(EPD_MEMORY_REGION_SRAM,
        epd_resource_file_io_peak(STORAGE_PATH_MAX)) != EPD_RESOURCE_OK) return -1;
    storage_lock();
    observe_changes();
    int fd = storage_session_valid(STORAGE_SD, session) ?
        storage_file_owned_open(path, flags, strlen(path) > 6 && !strcmp(path + strlen(path) - 6, ".stage")) : -1;
    own_change_done();
    storage_unlock();
    return fd;
}

bool gallery_file_close(int fd)
{
    if (fd < 0) return true;
    storage_lock();
    observe_changes();
    bool ok = storage_file_owned_close(fd);
    own_change_done();
    storage_unlock();
    return ok;
}

bool gallery_file_io(int fd, void *data, size_t bytes, bool writing, uint32_t serial, uint32_t session)
{
    uint8_t *p = data;
    while (bytes && !gallery_cancelled(serial))
    {
        size_t chunk = bytes > 4096 ? 4096 : bytes;
        storage_lock();
        observe_changes();
        int n = !storage_session_valid(STORAGE_SD, session) ? -1 :
                writing ? write(fd, p, chunk) : read(fd, p, chunk);
        own_change_done();
        storage_unlock();
        if (n <= 0 || (size_t)n > chunk) return false;
        bytes -= n;
        p += n;
    }
    return !bytes && !gallery_cancelled(serial) && storage_session_valid(STORAGE_SD, session);
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static bool signature_update(void *user, const void *data, size_t size, uint32_t *hash)
{
    const gallery_job_t *job = user;
    if (gallery_cancelled(job->serial)) return false;
    *hash = hash_bytes(*hash, data, size);
    return true;
}

static void cache_path(const gallery_job_t *job, char *path, size_t size)
{
    bool thumb = job->kind == GALLERY_THUMB;
    unsigned slot = hash_bytes(2166136261u, job->path, strlen(job->path)) % (thumb ? THUMB_SLOTS : IMAGE_SLOTS);
    snprintf(path, size, GALLERY_STORE "/%c%03u", thumb ? 't' : 'i', slot);
}

static bool cache_load(const gallery_job_t *job, gallery_result_t *result, uint32_t signature, uint32_t session)
{
    char path[STORAGE_PATH_MAX];
    cache_path(job, path, sizeof(path));
    epd_cold_lease_t lease;
    if (epd_cold_budget_begin(path, EPD_COLD_DERIVED, 0, session, &lease) != EPD_COLD_BUDGET_OK) return false;
    storage_lock(); own_change_done(); storage_unlock();
    int fd = gallery_file_open(path, O_RDONLY, session);
    if (fd < 0) {
        (void)epd_cold_budget_finish(lease, EPD_COLD_DERIVED, false);
        storage_lock(); own_change_done(); storage_unlock();
        return false;
    }
    cache_header_t header;
    bool ok = gallery_file_io(fd, &header, sizeof(header), false, job->serial, session);
    unsigned max_width = job->kind == GALLERY_THUMB ? GALLERY_THUMB_SIZE : GALLERY_WIDTH;
    unsigned max_height = job->kind == GALLERY_THUMB ? GALLERY_THUMB_SIZE : GALLERY_HEIGHT;
    ok = ok && header.magic == CACHE_MAGIC && header.version == cache_version(job->kind) &&
         header.signature == signature && memchr(header.path, 0, sizeof(header.path)) && !strcmp(header.path, job->path) &&
         header.width && header.width <= max_width && header.height && header.height <= max_height &&
         header.source_width && header.source_height;
    if (ok) ok = gallery_image_alloc(result, header.width, header.height);
    uint32_t hash = 2166136261u;
    if (ok) ok = epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM,
        epd_app_allocation_peak(4096)) == EPD_RESOURCE_OK;
    uint8_t *packed = ok ? epd_app_alloc(4096, EPD_APP_PSRAM) : NULL;
    ok = ok && packed;
    size_t row_bytes = ok ? (header.width + 1) / 2 : 1;
    for (unsigned y = 0; ok && y < header.height; )
    {
        unsigned rows = 4096 / row_bytes;
        if (rows > header.height - y) rows = header.height - y;
        size_t bytes = rows * row_bytes;
        ok = gallery_file_io(fd, packed, bytes, false, job->serial, session);
        if (!ok) break;
        hash = hash_bytes(hash, packed, bytes);
        for (unsigned r = 0; r < rows; ++r, ++y)
        {
            uint16_t *row = (uint16_t *)(result->image.data + y * result->image.header.stride);
            for (unsigned x = 0; x < header.width; ++x)
                row[x] = gallery_gray16_colors[(packed[r * row_bytes + x / 2] >> (x % 2 ? 0 : 4)) & 15];
        }
    }
    epd_app_free(packed);
    ok = gallery_file_close(fd) && ok;
    ok = ok && hash == header.checksum;
    (void)epd_cold_budget_finish(lease, EPD_COLD_DERIVED, ok);
    storage_lock(); own_change_done(); storage_unlock();
    if (!ok)
    {
        epd_app_free(result->pixels);
        result->pixels = NULL;
        memset(&result->image, 0, sizeof(result->image));
        return false;
    }
    result->source_width = header.source_width;
    result->source_height = header.source_height;
    gallery_image_clean(result);
    return true;
}

typedef struct {
    gallery_job_t job;
    gallery_result_t *result;
    cache_header_t header;
    int fd;
    unsigned y;
    uint32_t hash;
    bool pinned;
    bool committed;
    epd_cold_lease_t lease;
    char path[EPD_COLD_BUDGET_PATH], temporary[EPD_COLD_BUDGET_PATH + 8u];
    uint8_t packed[4096];
} cache_write_t;
static cache_write_t *saving;

void gallery_cache_clear(void)
{
    if (!saving) return;
    gallery_file_close(saving->fd);
    if (saving->lease) {
        (void)epd_cold_budget_finish(saving->lease, EPD_COLD_DERIVED, saving->committed);
        storage_lock(); own_change_done(); storage_unlock();
    }
    if (saving->pinned) storage_path_unpin(saving->job.path);
    gallery_result_free(saving->result);
    epd_app_free(saving);
    saving = NULL;
}

static bool source_current(const gallery_job_t *job, const gallery_result_t *result)
{
    return storage_session_valid(STORAGE_SD, result->source_session) &&
           storage_path_revision(job->path) == result->source_revision && !storage_file_writers(job->path);
}

void gallery_cache_schedule(const gallery_job_t *job, gallery_result_t *result)
{
    gallery_cache_clear();
    if (!result->cache_pending || !source_current(job, result)) return;
    if (epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM,
        epd_app_allocation_peak(sizeof(*saving))) != EPD_RESOURCE_OK) return;
    saving = epd_app_alloc(sizeof(*saving), EPD_APP_PSRAM);
    if (!saving) return;
    memset(saving, 0, sizeof(*saving));
    saving->fd = -1;
    saving->job = *job;
    saving->job.serial = 0;
    saving->result = result;
    gallery_result_retain(result);
    saving->pinned = storage_path_pin(job->path);
    saving->hash = 2166136261u;
    saving->header = (cache_header_t){.magic = CACHE_MAGIC, .version = cache_version(job->kind),
        .signature = result->source_signature, .width = result->image.header.w,
        .height = result->image.header.h, .source_width = result->source_width,
        .source_height = result->source_height};
    memcpy(saving->header.path, job->path, sizeof(saving->header.path));
}

/* At most one 4 KiB pixel block per turn. The immutable result has its own
 * reference, so navigation can release the displayed image during a write. */
bool gallery_cache_step(void)
{
    if (!saving) return false;
    cache_write_t *write = saving;
    const gallery_result_t *result = write->result;
    uint32_t session = result->source_session;
    const char *temporary = write->temporary;
    if (gallery_cancelled(0) || !source_current(&write->job, result)) goto done;
    if (write->fd < 0) {
        cache_path(&write->job, write->path, sizeof(write->path));
        size_t bytes = sizeof(write->header) +
            ((write->header.width + 1u) / 2u) * (size_t)write->header.height;
        if (epd_cold_budget_begin(write->path, EPD_COLD_DERIVED, bytes, session,
            &write->lease) != EPD_COLD_BUDGET_OK) goto done;
        storage_lock(); own_change_done(); storage_unlock();
        if (!epd_cold_budget_temporary(write->path, write->temporary, sizeof(write->temporary))) goto done;
        if (!gallery_store_ready(session)) goto done;
        write->fd = gallery_file_open(temporary, O_CREAT | O_TRUNC | O_RDWR, session);
        if (write->fd < 0 || !gallery_file_io(write->fd, &write->header, sizeof(write->header), true, 0, session)) goto done;
        return true;
    }
    if (write->y < write->header.height) {
        unsigned width = write->header.width;
        size_t row_bytes = (width + 1) / 2;
        unsigned rows = sizeof(write->packed) / row_bytes;
        if (rows > write->header.height - write->y) rows = write->header.height - write->y;
        size_t bytes = rows * row_bytes;
        memset(write->packed, 0, bytes);
        for (unsigned r = 0; r < rows; ++r, ++write->y) {
            const uint16_t *row = (const uint16_t *)(result->image.data + write->y * result->image.header.stride);
            for (unsigned x = 0; x < width; ++x)
                write->packed[r * row_bytes + x / 2] |= ((row[x] >> 11) / 2) << (x % 2 ? 0 : 4);
        }
        write->hash = hash_bytes(write->hash, write->packed, bytes);
        if (!gallery_file_io(write->fd, write->packed, bytes, true, 0, session)) goto done;
        return true;
    }
    write->header.checksum = write->hash;
    storage_lock();
    bool positioned = source_current(&write->job, result) && lseek(write->fd, 0, SEEK_SET) == 0;
    storage_unlock();
    if (!positioned || !gallery_file_io(write->fd, &write->header, sizeof(write->header), true, 0, session)) goto done;
    bool closed = gallery_file_close(write->fd);
    write->fd = -1;
    if (!closed) goto done;
    storage_lock();
    observe_changes();
    if (!gallery_cancelled(0) && source_current(&write->job, result)) {
        unlink(write->path);
        write->committed = rename(temporary, write->path) == 0;
        if (!write->committed) unlink(temporary);
    }
    own_change_done();
    storage_unlock();
done:
    gallery_cache_clear();
    return false;
}

bool gallery_cached_render(const gallery_job_t *job, gallery_decoder_t *decoder, gallery_result_t *result)
{
    if (!gallery_sd_path(job->path)) { result->error_code = GALLERY_STATUS_PATH; return false; }
    uint32_t session = storage_card_session(), signature = 0;
    bool pinned = storage_path_pin(job->path);
    uint32_t revision = storage_path_revision(job->path);
    bool fingerprint = storage_file_signature_with(job->path, &signature, signature_update, (void *)job);
    bool ok = fingerprint && cache_load(job, result, signature, session);
    bool cached = ok;
    if (!ok && !gallery_cancelled(job->serial) && storage_session_valid(STORAGE_SD, session))
    {
        ok = gallery_decode(job, decoder, result) && gallery_render(decoder, result, job);

    }
    if (!storage_session_valid(STORAGE_SD, session) || revision != storage_path_revision(job->path) || storage_file_writers(job->path))
    { result->error_code = GALLERY_STATUS_REMOVED; ok = false; }
    result->source_session = session;
    result->source_revision = revision;
    result->source_signature = signature;
    result->cache_pending = ok && fingerprint && !cached;
    if (pinned) storage_path_unpin(job->path);
    return ok && !gallery_cancelled(job->serial);
}
