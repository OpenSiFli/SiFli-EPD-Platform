/* SPDX-License-Identifier: Apache-2.0 */
#include "frontlight_store.h"
#include "app_service.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "platform/epd_system_internal.h"
#include "platform/epd_i18n.h"
#include <dfs_posix.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define FRONTLIGHT_MAGIC 0x464c4954u
#define FRONTLIGHT_VERSION 2u
static bool valid_percent(uint8_t percent) { return percent <= 100; }

/* storage_record_load returns only bool. Decode the same record envelope to
 * distinguish missing/invalid records from I/O errors, as system_time does. */
static frontlight_store_status_t read_preference(uint8_t *percent, frontlight_mode_t *mode)
{
    uint32_t header[4];
    uint8_t payload[4];
    uint8_t record[sizeof(header) + sizeof(payload)];
    size_t used = 0;
    struct stat st;
    frontlight_store_status_t result = FRONTLIGHT_STORE_IO_ERROR;
    if (!storage_path_available(FRONTLIGHT_SETTINGS_PATH) ||
        !storage_file_recover(FRONTLIGHT_SETTINGS_PATH)) return result;
    int fd = open(FRONTLIGHT_SETTINGS_PATH, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT || errno == -ENOENT ? FRONTLIGHT_STORE_MISSING : result;
    if (fstat(fd, &st) != 0) goto done;
    if (!S_ISREG(st.st_mode) || st.st_size != (int64_t)sizeof(record))
    {
        result = FRONTLIGHT_STORE_INVALID;
        goto done;
    }
    while (used < sizeof(record))
    {
        int n = read(fd, record + used, sizeof(record) - used);
        /* A short/failed transfer is not evidence that a preference is absent. */
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    memcpy(header, record, sizeof(header));
    memcpy(payload, record + sizeof(header), sizeof(payload));
    result = FRONTLIGHT_STORE_INVALID;
    if (header[0] == FRONTLIGHT_MAGIC && header[1] == FRONTLIGHT_VERSION &&
        header[2] == sizeof(payload) && header[3] == storage_crc32(payload, sizeof(payload)) &&
        valid_percent(payload[0]) && !payload[2] && !payload[3] &&
        payload[1] < FRONTLIGHT_MODE_COUNT)
    {
        *percent = payload[0];
        *mode = (frontlight_mode_t)payload[1];
        result = FRONTLIGHT_STORE_READY;
    }
done:
    if (close(fd) != 0 || !storage_path_available(FRONTLIGHT_SETTINGS_PATH))
        result = FRONTLIGHT_STORE_IO_ERROR;
    return result;
}


/* Only this worker performs filesystem operations. The mutex protects small
 * memory snapshots and is never held while waiting for storage. */
static struct rt_mutex jobs_lock;
static bool lock_ready;
static epd_service_t *owner;
static frontlight_store_snapshot_t store;
static bool load_queued, save_queued, working;
static uint8_t requested_percent;
static frontlight_mode_t requested_mode;
static bool notice_visible;

static void take(void) { rt_mutex_take(&jobs_lock, RT_WAITING_FOREVER); }
static void give(void) { rt_mutex_release(&jobs_lock); }

void frontlight_store_get(frontlight_store_snapshot_t *out)
{
    if (!lock_ready) { memset(out, 0, sizeof(*out)); return; }
    take();
    *out = store;
    out->pending = load_queued || save_queued || working;
    give();
}

static bool flush(void)
{
    /* A completed first load can enqueue the first-use default. Admit that
     * write before reporting the barrier complete. Hardware stays UI-owned. */
    frontlight_process();
    frontlight_store_snapshot_t snapshot;
    frontlight_store_get(&snapshot);
    /* Completed failures remain reported, but do not hold shutdown forever. */
    return !snapshot.pending;
}

static bool ui_process(epd_service_t *service, bool stopping)
{
    (void)service;
    frontlight_process();
    frontlight_store_snapshot_t snapshot;
    frontlight_store_get(&snapshot);
    if (snapshot.failed && !notice_visible) {
        epd_notification_t notice = {0};
        snprintf(notice.app_id, sizeof(notice.app_id), "settings");
        snprintf(notice.id, sizeof(notice.id), "frontlight-save");
        snprintf(notice.title, sizeof(notice.title), "%s", epd_get_str(platform_settings_save_failed));
        snprintf(notice.text, sizeof(notice.text), "%s", epd_get_str(platform_settings_save_failed_detail));
        notice_visible = epd_notification_publish_resident(&notice);
    } else if (!snapshot.failed && !snapshot.pending && notice_visible) {
        (void)epd_notification_dismiss("settings", "frontlight-save");
        notice_visible = false;
    }
    if (stopping && !snapshot.pending) {
        take(); owner = NULL; give();
        return true;
    }
    return false;
}

static void run(epd_service_t *service)
{
    bool cached = false;
    uint8_t cached_percent = 0;
    frontlight_mode_t cached_mode = FRONTLIGHT_MIXED;
    uint32_t cached_path = 0, cached_volume = 0;
    for (;;) {
        epd_service_wait(service, RT_WAITING_FOREVER);
        for (;;) {
            take();
            bool load = load_queued;
            if (!load && !save_queued) { give(); break; }
            uint32_t revision = store.revision;
            uint8_t percent = requested_percent;
            frontlight_mode_t mode = requested_mode;
            if (load) load_queued = false;
            else save_queued = false;
            working = true;
            give();

            storage_lock();
            frontlight_store_status_t result = FRONTLIGHT_STORE_IO_ERROR;
            if (load) {
                uint32_t path = storage_path_revision(FRONTLIGHT_SETTINGS_PATH);
                uint32_t volume = storage_volume_revision(STORAGE_FLASH);
                if (!storage_file_writers(FRONTLIGHT_SETTINGS_PATH)) result = read_preference(&percent, &mode);
                if (path != storage_path_revision(FRONTLIGHT_SETTINGS_PATH) ||
                    volume != storage_volume_revision(STORAGE_FLASH)) result = FRONTLIGHT_STORE_IO_ERROR;
            } else {
                uint8_t payload[4] = {percent, (uint8_t)mode, 0, 0};
                bool ok = system_reset_write_allowed() && !storage_file_writers(FRONTLIGHT_SETTINGS_PATH);
                bool unchanged = cached && cached_percent == percent && cached_mode == mode &&
                    cached_path == storage_path_revision(FRONTLIGHT_SETTINGS_PATH) &&
                    cached_volume == storage_volume_revision(STORAGE_FLASH);
                if (ok && !unchanged)
                    ok = storage_record_save(FRONTLIGHT_SETTINGS_PATH, FRONTLIGHT_MAGIC,
                                             FRONTLIGHT_VERSION, payload, sizeof(payload));
                result = ok ? FRONTLIGHT_STORE_READY : FRONTLIGHT_STORE_SAVE_ERROR;
            }
            cached = result == FRONTLIGHT_STORE_READY;
            if (cached) {
                cached_percent = percent; cached_mode = mode;
                cached_path = storage_path_revision(FRONTLIGHT_SETTINGS_PATH);
                cached_volume = storage_volume_revision(STORAGE_FLASH);
            }
            storage_unlock();

            take();
            working = false;
            if (revision == store.revision) {
                store.status = result;
                if (load) {
                    store.load_revision = revision;
                    store.percent = percent;
                    store.mode = mode;
                } else {
                    store.failed = result != FRONTLIGHT_STORE_READY;
                    store.percent = percent;
                    store.mode = mode;
                }
            }
            if (!load_queued && !save_queued) epd_service_sleep_release(service);
            give();
            epd_service_ui_request(service, 0);
        }
        if (epd_service_cancelled(service)) return;
    }
}

static bool start(void)
{
    if (!lock_ready) {
        if (rt_mutex_init(&jobs_lock, "flsave", RT_IPC_FLAG_PRIO) != RT_EOK) return false;
        lock_ready = true;
    }
    if (owner) return !epd_service_cancelled(owner);
    static const epd_background_t definition = {
        .run = run, .stack_size = 3072, .priority = 23,
        .ui_process = ui_process, .flush = flush
    };
    owner = app_service_start("frontlight", &definition, NULL);
    return owner != NULL;
}

bool frontlight_store_load(void)
{
    if (!start()) return false;
    take();
    /* Unsaved user intent wins over an external reload, including a failure
     * awaiting explicit retry. A later reload may run after saving settles. */
    if (load_queued || save_queued || working || store.failed || !epd_service_sleep_acquire(owner)) {
        give(); return false;
    }
    if (++store.revision == 0) ++store.revision;
    store.status = FRONTLIGHT_STORE_PENDING;
    load_queued = true;
    epd_service_wake(owner);
    give();
    return true;
}

bool frontlight_store_save(uint8_t percent, frontlight_mode_t mode)
{
    if (!start()) return false;
    take();
    if (!epd_service_sleep_acquire(owner)) { give(); return false; }
    if (++store.revision == 0) ++store.revision;
    requested_percent = percent;
    requested_mode = mode;
    load_queued = false;
    save_queued = true;
    if (!store.failed) store.status = FRONTLIGHT_STORE_PENDING;
    epd_service_wake(owner);
    give();
    return true;
}
