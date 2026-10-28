#include "storage_internal.h"
#include "storage_file_internal.h"
#include "storage_file.h"
#include "epd_memory.h"
#include "system_reset_guard.h"
#include <dfs_posix.h>
#include <dfs_fs.h>
#include <string.h>
#include <rtm.h>
#include <stdio.h>
#include <errno.h>
#include "platform/app_module.h"

typedef struct { uint32_t magic, version, size, crc; } record_header_t;

/* Reserve ownership before open, including while segmented I/O drops the
 * storage lock. Exhaustion rejects new opens instead of losing a failed fd. */
#define STORAGE_CLOSE_SLOTS 8u
typedef struct
{
    bool used, pending, retryable, temporary;
    unsigned later_attempts;
    int fd;
    storage_volume_t volume;
    uint32_t session, release_id;
    char path[STORAGE_PATH_MAX];
} owned_file_t;
static owned_file_t owned_files[STORAGE_CLOSE_SLOTS];

static void close_retired(owned_file_t *file)
{
    /* Closing an invalidated SD object is allowed; deleting by pathname on a
     * different/unavailable session is not. Never commit an abandoned import. */
    if (file->temporary && storage_session_valid(file->volume, file->session) &&
        system_reset_write_allowed()) (void)unlink(file->path);
    memset(file, 0, sizeof(*file));
}

/* One opportunistic retry in the object's lifetime, or one attempt per new
 * release request. Repeated release polling performs no additional close I/O.
 * Caller holds storage_lock; DFS/watch ownership ends only on close success. */
static bool close_pending(storage_volume_t volume, uint32_t release_id)
{
    bool done = true;
    for (unsigned i = 0; i < STORAGE_CLOSE_SLOTS; ++i)
    {
        owned_file_t *file = &owned_files[i];
        if (!file->used || ((unsigned)volume < STORAGE_COUNT && file->volume != volume)) continue;
        if (file->pending && file->retryable)
        {
            bool retry = release_id ? file->release_id != release_id : file->later_attempts != 0;
            if (retry)
            {
                if (release_id) file->release_id = release_id;
                else --file->later_attempts;
                if (close(file->fd) == 0) { close_retired(file); continue; }
            }
        }
        done = false;
    }
    return done;
}

bool storage_file_release(storage_volume_t volume, uint32_t release_id)
{
    if ((unsigned)volume >= STORAGE_COUNT || !release_id) return false;
    storage_lock();
    bool done = close_pending(volume, release_id);
    storage_unlock();
    return done;
}

static int owned_open(const char *path, int flags, bool temporary)
{
    (void)close_pending(STORAGE_COUNT, 0);
    if (strlen(path) >= STORAGE_PATH_MAX) return -1;
    for (unsigned i = 0; i < STORAGE_CLOSE_SLOTS; ++i)
    {
        owned_file_t *file = &owned_files[i];
        if (file->used) continue;
        /* A failed LittleFS close frees its private object in this SDK. Keep
         * the DFS handle quarantined rather than retrying through freed data.
         * FatFS retains its FIL on error and permits a later close. */
        struct dfs_filesystem *fs = dfs_filesystem_lookup(path);
        file->retryable = fs && fs->ops && fs->ops->name && !strcmp(fs->ops->name, "elm");
        file->fd = open(path, flags, 0);
        if (file->fd < 0) { memset(file, 0, sizeof(*file)); return -1; }
        file->used = true;
        file->volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
        file->session = storage_card_session();
        file->temporary = temporary;
        strcpy(file->path, path);
        return file->fd;
    }
    return -1;
}

static owned_file_t *owned_find(int fd)
{
    for (unsigned i = 0; i < STORAGE_CLOSE_SLOTS; ++i)
        if (owned_files[i].used && owned_files[i].fd == fd) return &owned_files[i];
    return NULL;
}

static bool owned_close(int fd)
{
    owned_file_t *file = owned_find(fd);
    if (!file) return false;
    if (close(fd) == 0) { memset(file, 0, sizeof(*file)); return true; }
    /* An immediate retry only retires resources; the original operation still
     * fails even if that retry succeeds. No result or replacement is published. */
    if (file->retryable && close(fd) == 0) { memset(file, 0, sizeof(*file)); return false; }
    file->pending = true;
    file->later_attempts = 1;
    return false;
}

static bool managed_path(const char *path)
{
    return path && (!strncmp(path, STORAGE_FLASH_ROOT "/", sizeof(STORAGE_FLASH_ROOT)) ||
                    !strncmp(path, STORAGE_SD_ROOT "/", sizeof(STORAGE_SD_ROOT)));
}

const char *storage_app_directory(storage_volume_t volume)
{
    return volume == STORAGE_SD ? STORAGE_SD_APPS : STORAGE_FLASH_APPS;
}

static bool app_id_valid(const char *id)
{
    if (!id || !*id || strlen(id) >= RT_NAME_MAX) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

bool storage_app_locate(const char *id, storage_volume_t *volume)
{
    if (!app_id_valid(id) || !volume) return false;
    char path[96];
    struct stat st;
    bool found = false;
    storage_lock();
    /* A duplicate copied in by a PC never replaces the internal installation. */
    for (storage_volume_t v = STORAGE_FLASH; v < STORAGE_COUNT; ++v)
    {
        rt_snprintf(path, sizeof(path), "%s/%s/app.json", storage_app_directory(v), id);
        if (storage_available(v) && stat(path, &st) == 0 && S_ISREG(st.st_mode))
        { *volume = v; found = true; break; }
    }
    storage_unlock();
    return found;
}

static bool app_location(const char *id, storage_volume_t *volume)
{
    uint32_t session;
    if (app_module_storage(id, volume, &session)) return storage_session_valid(*volume, session);
    return storage_app_locate(id, volume);
}

bool storage_app_available(const char *id)
{
    storage_volume_t volume;
    return app_location(id, &volume);
}
RTM_EXPORT(storage_app_available);

bool storage_app_path(char *out, size_t size, const char *id,
                      storage_app_area_t area, const char *relative)
{
    static const char *const areas[] = {"apps", "data", "cache"};
    if (out && size) out[0] = 0;
    if (!out || !size || !app_id_valid(id) || (unsigned)area >= 3 || !relative) return false;
    const char *segment = relative;
    for (const char *p = relative; ; ++p)
    {
        if (*p && ((unsigned char)*p < 32 || *p == '\\' || *p == ':')) return false;
        if (*p == '/' || !*p)
        {
            size_t n = p - segment;
            if ((!n && *relative) || (n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) return false;
            segment = p + 1;
        }
        if (!*p) break;
    }
    int n;
    storage_volume_t volume;
    if (!app_location(id, &volume)) return false;
    if (area == STORAGE_APP_CODE)
    {
        n = snprintf(out, size, "%s/%s%s%s", storage_app_directory(volume), id,
                        *relative ? "/" : "", relative);
    }
    else
        n = snprintf(out, size, "%s/%s/%s%s%s",
                        volume == STORAGE_SD ? STORAGE_SD_ROOT "/.epd" : STORAGE_FLASH_ROOT, areas[area], id,
                        *relative ? "/" : "", relative);
    if (n <= 0 || (size_t)n >= size) { out[0] = 0; return false; }
    return true;
}
RTM_EXPORT(storage_app_path);

bool storage_mkdirs(const char *path)
{
    char directory[STORAGE_PATH_MAX];
    struct stat st;
    if (!path || !*path) return false;
    const char *root = storage_path_root(path);
    if (!strcmp(path, root)) return storage_path_available(path);
    if (!managed_path(path) || strlen(path) >= sizeof(directory)) return false;
    strcpy(directory, path);
    bool ok = true;
    storage_lock();
    if (!system_reset_write_allowed() || !storage_path_available(path)) ok = false;
    for (char *p = directory + strlen(root) + 1; ok; ++p)
    {
        if (*p != '/' && *p) continue;
        char separator = *p;
        if (!storage_path_available(path)) { ok = false; break; }
        *p = 0;
        if (stat(directory, &st) == 0) ok = S_ISDIR(st.st_mode);
        else ok = mkdir(directory, 0) == 0;
        *p = separator;
        if (!separator) break;
    }
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_mkdirs);

void storage_prepare_directories(void)
{
    static const char *const dirs[] = {"apps", "data", "cache", "system", "fonts", "update"};
    char path[32];
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
    {
        rt_snprintf(path, sizeof(path), STORAGE_FLASH_ROOT "/%s", dirs[i]);
        if (!storage_mkdirs(path)) rt_kprintf("[storage] mkdir failed: %s\n", path);
    }
}

uint32_t storage_crc32(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (size--)
    {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
RTM_EXPORT(storage_crc32);

static bool transfer_write(int fd, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size)
    {
        int n = write(fd, p, size);
        if (n <= 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

static int replace_begin(const char *path, char temporary[STORAGE_PATH_MAX])
{
    (void)close_pending(STORAGE_COUNT, 0);
    if (!system_reset_write_allowed() || !managed_path(path) || strlen(path) + 5 >= STORAGE_PATH_MAX ||
        !storage_path_available(path) || !storage_file_recover(path)) return -1;
    strcpy(temporary, path);
    char *slash = strrchr(temporary, '/');
    *slash = 0;
    if (!storage_mkdirs(temporary)) return -1;
    rt_snprintf(temporary, STORAGE_PATH_MAX, "%s.tmp", path);
    if (storage_file_writers(temporary)) return -1;
    return owned_open(temporary, O_CREAT | O_TRUNC | O_WRONLY, true);
}

static int file_state(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == -ENOENT ? 0 : -1;
}

static storage_replace_mode_t replace_mode(const char *path)
{
    return storage_replace_mode(!strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH);
}

bool storage_file_recover(const char *path)
{
    if (!managed_path(path) || strlen(path) + 5 >= STORAGE_PATH_MAX) return false;
    storage_lock();
    storage_replace_mode_t mode = replace_mode(path);
    bool ok = mode != STORAGE_REPLACE_UNAVAILABLE;
    if (ok && mode == STORAGE_REPLACE_BACKUP)
    {
        char backup[STORAGE_PATH_MAX];
        snprintf(backup, sizeof(backup), "%s.old", path);
        int saved = file_state(backup), current = file_state(path);
        ok = saved >= 0 && current >= 0;
        if (ok && saved && !current) ok = system_reset_write_allowed() && rename(backup, path) == 0;
    }
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_file_recover);

bool storage_file_commit(const char *temporary, const char *path)
{
    if (!managed_path(temporary) || !managed_path(path) || !strcmp(temporary, path) ||
        strlen(path) + 5 >= STORAGE_PATH_MAX ||
        strcmp(storage_path_root(temporary), storage_path_root(path))) return false;
    storage_lock();
    storage_replace_mode_t mode = replace_mode(path);
    bool ok = system_reset_write_allowed() && mode != STORAGE_REPLACE_UNAVAILABLE;
    bool needs_backup = mode == STORAGE_REPLACE_BACKUP;
    char backup[STORAGE_PATH_MAX];
    snprintf(backup, sizeof(backup), "%s.old", path);
    if (ok && needs_backup)
    {
        ok = storage_file_recover(path);
        int saved = ok ? file_state(backup) : -1;
        int current = ok ? file_state(path) : -1;
        ok = saved >= 0 && current >= 0;
        if (ok && saved) ok = unlink(backup) == 0;
        if (ok && current) ok = rename(path, backup) == 0;
    }
    if (ok) ok = storage_path_available(path) && rename(temporary, path) == 0;
    if (needs_backup && storage_path_available(path))
    {
        if (ok)
        {
            /* The new main file is committed. Cleanup failure must not make
             * callers roll back their value or keep an obsolete save cache. */
            int saved = file_state(backup);
            if (saved > 0) unlink(backup);
        }
        else storage_file_recover(path);
    }
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_file_commit);

static bool replace_finish(int fd, const char *temporary, const char *path, bool ok)
{
    if (ok) ok = storage_path_available(path) && fsync(fd) == 0;
    if (!owned_close(fd)) ok = false;
    if (ok) ok = storage_file_commit(temporary, path);
    if (!ok && !owned_find(fd) && storage_path_available(path)) unlink(temporary);
    return ok;
}

bool storage_file_replace(const char *path, const void *data, size_t size)
{
    char temporary[STORAGE_PATH_MAX];
    storage_lock();
    int fd = replace_begin(path, temporary);
    bool ok = fd >= 0 && replace_finish(fd, temporary, path, transfer_write(fd, data, size));
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_file_replace);

bool storage_record_save(const char *path, uint32_t magic, uint32_t version, const void *data, size_t size)
{
    record_header_t header = {magic, version, size, storage_crc32(data, size)};
    char temporary[STORAGE_PATH_MAX];
    storage_lock();
    int fd = replace_begin(path, temporary);
    bool ok = fd >= 0 && replace_finish(fd, temporary, path,
                    transfer_write(fd, &header, sizeof(header)) && transfer_write(fd, data, size));
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_record_save);

bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size)
{
    record_header_t header;
    struct stat st;
    storage_lock();
    int fd = storage_file_recover(path) ? owned_open(path, O_RDONLY, false) : -1;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_size == sizeof(header) + size &&
              read(fd, &header, sizeof(header)) == sizeof(header) &&
              header.magic == magic && header.version == version && header.size == size &&
              read(fd, data, size) == size && header.crc == storage_crc32(data, size);
    if (fd >= 0 && !owned_close(fd)) ok = false;
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_record_load);

bool storage_file_import_session(const char *source, const char *destination,
                                 storage_volume_t volume, uint32_t expected_session)
{
    char temporary[STORAGE_PATH_MAX];
    struct stat st = {0};
    if (!source || !destination || !strcmp(source, destination) ||
        !managed_path(destination) || strlen(destination) + 14 >= sizeof(temporary)) return false;
    uint8_t *buffer = epd_app_alloc(4096, EPD_APP_PSRAM);
    if (!buffer) return false;
    storage_lock();
    if ((unsigned)volume < STORAGE_COUNT && !storage_session_valid(volume, expected_session))
    { storage_unlock(); epd_app_free(buffer); return false; }
    uint32_t session = storage_card_session();
    storage_volume_t source_volume = !strcmp(storage_path_root(source), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    storage_volume_t target_volume = !strcmp(storage_path_root(destination), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    uint32_t revision = storage_path_revision(source);
    int input = storage_path_available(source) ? owned_open(source, O_RDONLY, false) : -1;
    bool ok = input >= 0 && fstat(input, &st) == 0 && S_ISREG(st.st_mode) &&
              !storage_file_writers(source) && system_reset_write_allowed() && storage_path_available(destination);
    strcpy(temporary, destination);
    char *slash = strrchr(temporary, '/');
    *slash = 0;
    if (ok) ok = storage_mkdirs(temporary) && storage_file_recover(destination);
    static uint32_t transfer_id;
    int output = -1;
    for (unsigned attempt = 0; ok && output < 0 && attempt < 8; ++attempt)
    {
        snprintf(temporary, sizeof(temporary), "%s.%08lx.tmp", destination, (unsigned long)++transfer_id);
        output = owned_open(temporary, O_CREAT | O_EXCL | O_WRONLY, true);
    }
    ok = output >= 0;
    size_t remaining = ok ? st.st_size : 0;
    storage_unlock();
    while (ok && remaining)
    {
        storage_lock();
        ok = storage_session_valid(source_volume, session) && storage_session_valid(target_volume, session) &&
             revision == storage_path_revision(source) && !storage_file_writers(source) && system_reset_write_allowed();
        int n = ok ? read(input, buffer, remaining < 4096 ? remaining : 4096) : -1;
        ok = n > 0 && transfer_write(output, buffer, (size_t)n);
        if (ok) remaining -= n;
        storage_unlock();
        if (ok && remaining) rt_thread_yield();
    }
    storage_lock();
    ok = ok && storage_session_valid(source_volume, session) && storage_session_valid(target_volume, session) &&
         revision == storage_path_revision(source) && !storage_file_writers(source);
    if (input >= 0 && !owned_close(input)) ok = false;
    if (output >= 0)
    {
        if (storage_session_valid(target_volume, session)) ok = replace_finish(output, temporary, destination, ok);
        else { (void)owned_close(output); ok = false; }
    }
    storage_unlock();
    epd_app_free(buffer);
    return ok;
}

bool storage_file_import(const char *source, const char *destination)
{ return storage_file_import_session(source, destination, STORAGE_COUNT, 0); }

bool storage_file_signature(const char *path, uint32_t *signature)
{
    typedef struct
    {
        char path[STORAGE_PATH_MAX];
        uint32_t revision, session, hash, use;
        off_t size;
        time_t modified;
    } signature_entry_t;
    enum { SIGNATURE_ENTRIES = 64 };
    static signature_entry_t *cache;
    static uint32_t use;
    if (!path || !signature || strlen(path) >= STORAGE_PATH_MAX) return false;
    storage_volume_t volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    struct stat before, after;
    uint32_t hash = 2166136261u;
    storage_lock();
    (void)close_pending(STORAGE_COUNT, 0);
    uint32_t session = volume == STORAGE_SD ? storage_card_session() : 0;
    uint32_t revision = storage_path_revision(path);
    bool ok = storage_session_valid(volume, session) && !storage_file_writers(path) &&
              stat(path, &before) == 0 && S_ISREG(before.st_mode) &&
              storage_session_valid(volume, session) && revision == storage_path_revision(path) &&
              !storage_file_writers(path);
    if (ok && revision && cache)
        for (unsigned i = 0; i < SIGNATURE_ENTRIES; ++i)
            if (!strcmp(cache[i].path, path) && cache[i].revision == revision && cache[i].session == session &&
                cache[i].size == before.st_size && cache[i].modified == before.st_mtime)
            {
                *signature = cache[i].hash;
                cache[i].use = ++use;
                storage_unlock();
                return true;
            }
    int fd = ok ? owned_open(path, O_RDONLY, false) : -1;
    storage_unlock();
    uint8_t *buffer = fd >= 0 ? epd_app_alloc(4096, EPD_APP_PSRAM) : NULL;
    int n = -1;
    size_t bytes = 0;
    if (buffer)
    {
        do
        {
            storage_lock();
            ok = storage_session_valid(volume, session) && revision == storage_path_revision(path) &&
                 !storage_file_writers(path);
            n = ok ? read(fd, buffer, 4096) : -1;
            storage_unlock();
            for (int i = 0; i < n; ++i) hash = (hash ^ buffer[i]) * 16777619u;
            if (n > 0) { bytes += (size_t)n; rt_thread_yield(); }
        } while (n > 0);
    }
    epd_app_free(buffer);
    storage_lock();
    ok = n == 0 && bytes == (size_t)before.st_size && storage_session_valid(volume, session) &&
         revision == storage_path_revision(path) && !storage_file_writers(path) &&
         stat(path, &after) == 0 && after.st_size == before.st_size && after.st_mtime == before.st_mtime;
    if (fd >= 0 && !owned_close(fd)) ok = false;
    ok = ok && storage_session_valid(volume, session) && revision == storage_path_revision(path) &&
         !storage_file_writers(path);
    if (ok && revision)
    {
        if (!cache)
        {
            cache = epd_app_alloc(sizeof(*cache) * SIGNATURE_ENTRIES, EPD_APP_PSRAM);
            if (cache) memset(cache, 0, sizeof(*cache) * SIGNATURE_ENTRIES);
        }
        if (cache)
        {
            unsigned victim = 0;
            for (unsigned i = 0; i < SIGNATURE_ENTRIES; ++i)
            {
                if (!cache[i].path[0] || !strcmp(cache[i].path, path)) { victim = i; break; }
                if (cache[i].use < cache[victim].use) victim = i;
            }
            signature_entry_t *entry = &cache[victim];
            strcpy(entry->path, path);
            entry->revision = revision; entry->session = session; entry->hash = hash;
            entry->size = before.st_size; entry->modified = before.st_mtime; entry->use = ++use;
        }
    }
    storage_unlock();
    if (!ok) return false;
    *signature = hash;
    return true;
}
RTM_EXPORT(storage_file_import);
RTM_EXPORT(storage_file_import_session);
RTM_EXPORT(storage_file_signature);
