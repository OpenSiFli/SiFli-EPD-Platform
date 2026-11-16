/* SPDX-License-Identifier: Apache-2.0 */
#include "preference_store.h"
#include "app_service.h"
#include "storage_file.h"
#include "storage_internal.h"
#include "system_reset_guard.h"
#include "boards/board_service.h"
#include "mem_section.h"
#include "platform/epd_system_internal.h"
#include "platform/epd_i18n.h"
#include <string.h>
#include <stdio.h>

#define RECORD_CAPACITY 256u
#define SYNC_WAIT_MS 2000u
typedef struct {
    const char *path;
    uint32_t magic, version, revision;
    uint32_t result_revision, notified_revision;
    size_t size;
    bool queued, writing, exclusive, failed, notified;
    uint8_t data[RECORD_CAPACITY];
} record_t;
L2_RET_BSS_SECT_BEGIN(preference_records)
static record_t records[PREFERENCE_COUNT] L2_RET_BSS_SECT(preference_records);
L2_RET_BSS_SECT_END
static struct rt_mutex jobs_lock;
static bool lock_ready;
static epd_service_t *owner;

static int records_init(void)
{
    memset(records, 0, sizeof(records));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(records_init);

static bool prepare_lock(void)
{
    rt_enter_critical();
    if (!lock_ready) lock_ready = rt_mutex_init(&jobs_lock, "prefsave", RT_IPC_FLAG_PRIO) == RT_EOK;
    rt_exit_critical();
    return lock_ready;
}
static void take(void) { rt_mutex_take(&jobs_lock, RT_WAITING_FOREVER); }
static void give(void) { rt_mutex_release(&jobs_lock); }
static bool pending(void)
{
    for (unsigned i = 0; i < PREFERENCE_COUNT; ++i)
        if (records[i].queued || records[i].writing) return true;
    return false;
}

/* Called with jobs_lock held; only a completed write changes the failure. */
static void write_result(record_t *record, bool saved)
{
    record->failed = !saved;
    ++record->result_revision;
}

preference_status_t preference_store_status(preference_id_t id)
{
    preference_status_t status = {0};
    if ((unsigned)id >= PREFERENCE_COUNT || !lock_ready) return status;
    take();
    status.pending = records[id].queued || records[id].writing || records[id].exclusive;
    status.failed = records[id].failed;
    give();
    return status;
}

static bool flush(void)
{
    take();
    bool done = !pending();
    for (unsigned i = 0; i < PREFERENCE_COUNT; ++i) done &= !records[i].exclusive;
    give();
    /* Completed errors remain visible; only in-flight I/O holds retirement. */
    return done;
}

static bool ui_process(epd_service_t *service, bool stopping)
{
    (void)service;
    static const char *const ids[] = {"identity-save", "time-save", "language-save"};
    for (unsigned i = 0; i < PREFERENCE_COUNT; ++i) {
        take();
        preference_status_t status = {
            records[i].queued || records[i].writing || records[i].exclusive, records[i].failed
        };
        uint32_t result_revision = records[i].result_revision;
        give();
        /* Notification bookkeeping is UI-owned. A new completed failure may
         * notify again after dismissal; retry admission alone preserves it. */
        if (status.failed && (!records[i].notified || records[i].notified_revision != result_revision)) {
            epd_notification_t notice = {0};
            snprintf(notice.app_id, sizeof(notice.app_id), "settings");
            snprintf(notice.id, sizeof(notice.id), "%s", ids[i]);
            snprintf(notice.title, sizeof(notice.title), "%s", epd_get_str(platform_settings_save_failed));
            snprintf(notice.text, sizeof(notice.text), "%s", epd_get_str(platform_settings_save_failed_detail));
            records[i].notified = epd_notification_publish_resident(&notice);
            records[i].notified_revision = result_revision;
        } else if (!status.failed && !status.pending && records[i].notified) {
            (void)epd_notification_dismiss("settings", ids[i]);
            records[i].notified = false;
        }
    }
    if (stopping && flush()) { take(); if (owner == service) owner = NULL; give(); return true; }
    return false;
}

static void run(epd_service_t *service)
{
    unsigned next = 0;
    for (;;) {
        epd_service_wait(service, RT_WAITING_FOREVER);
        for (;;) {
            take();
            unsigned id;
            for (id = 0; id < PREFERENCE_COUNT; ++id)
                if (records[(next + id) % PREFERENCE_COUNT].queued) break;
            if (id == PREFERENCE_COUNT) {
                /* Serialize exit with acceptance. A submitter that acquired
                 * the sleep hold before cancellation must publish its queued
                 * value before this final empty-queue check can succeed. */
                bool stopping = epd_service_cancelled(service);
                give();
                if (stopping) return;
                break;
            }
            id = (next + id) % PREFERENCE_COUNT;
            next = (id + 1) % PREFERENCE_COUNT;
            record_t *record = &records[id];
            uint8_t data[RECORD_CAPACITY];
            memcpy(data, record->data, record->size);
            const char *path = record->path;
            size_t size = record->size;
            uint32_t magic = record->magic, version = record->version, revision = record->revision;
            record->queued = false;
            record->writing = true;
            give();

            storage_lock();
            bool ok = system_reset_write_allowed() && !storage_file_writers(path) &&
                      storage_record_save(path, magic, version, data, size);
            storage_unlock();

            take();
            record->writing = false;
            if (record->revision == revision) write_result(record, ok);
            if (!pending()) epd_service_sleep_release(service);
            give();
            epd_service_ui_request(service, 0);
        }
    }
}

static bool start(void)
{
    /* jobs_lock serializes startup, publication and owner retirement. */
    if (owner) return !epd_service_cancelled(owner);
    static const epd_background_t definition = {
        .run = run, .stack_size = 3072, .priority = 23,
        .ui_process = ui_process, .flush = flush
    };
    epd_service_t *created = app_service_start("preferences", &definition, NULL);
    if (created) owner = created;
    return owner != NULL;
}

bool preference_store_prepare(void)
{
    if (system_reset_blocks_apps() || storage_changing() || storage_release_requested() ||
        board_power_pending() || !system_reset_write_allowed() || !prepare_lock()) return false;
    take();
    bool ready = start();
    give();
    return ready;
}

bool preference_store_submit(preference_id_t id, const char *path, uint32_t magic,
                             uint32_t version, const void *data, size_t size)
{
    if ((unsigned)id >= PREFERENCE_COUNT || !path || !data || !size || size > RECORD_CAPACITY ||
        system_reset_blocks_apps() || storage_changing() || storage_release_requested() ||
        board_power_pending() || !system_reset_write_allowed() || !prepare_lock()) return false;
    take();
    record_t *record = &records[id];
    if (record->exclusive || !start() || !epd_service_sleep_acquire(owner)) { give(); return false; }
    record->path = path; record->magic = magic; record->version = version; record->size = size;
    memcpy(record->data, data, size);
    ++record->revision;
    record->queued = true;
    epd_service_wake(owner);
    give();
    return true;
}

bool preference_store_begin_sync(preference_id_t id)
{
    if ((unsigned)id >= PREFERENCE_COUNT || !prepare_lock()) return false;
    uint32_t started = (uint32_t)rt_tick_get();
    uint32_t timeout = (uint32_t)rt_tick_from_millisecond(SYNC_WAIT_MS);
    for (;;) {
        uint32_t elapsed = (uint32_t)rt_tick_get() - started;
        if (elapsed >= timeout || rt_mutex_take(&jobs_lock, (rt_int32_t)(timeout - elapsed)) != RT_EOK)
            return false;
        record_t *record = &records[id];
        if (!record->queued && !record->writing && !record->exclusive) {
            record->exclusive = true;
            give(); return true;
        }
        give();
        rt_thread_mdelay(1);
    }
}

void preference_store_end_sync(preference_id_t id, preference_sync_result_t result)
{
    if ((unsigned)id >= PREFERENCE_COUNT || !lock_ready ||
        (unsigned)result > PREFERENCE_SYNC_FAILED) return;
    take();
    if (!records[id].exclusive) { give(); return; }
    records[id].exclusive = false;
    if (result != PREFERENCE_SYNC_UNCHANGED) write_result(&records[id], result == PREFERENCE_SYNC_SAVED);
    if (owner) epd_service_ui_request(owner, 0);
    give();
}
